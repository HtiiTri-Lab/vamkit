#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>

// GPU time of a chain of stages on the D3D11 immediate context, measured with
// timestamp queries that are read back a few frames later without flushing or
// waiting; a sample still in flight when its ring slot comes round is dropped.
// Averages are logged every 10 s per group (eye). Never throws: a failed query
// turns the timer off for the run.
//
// Times are elapsed GPU time on this queue. The NR prepass and OFXR's DLSS-G
// run on D3D12 queues that share the GPU, so a stage here includes any time the
// GPU spent on those queues meanwhile ("nr" includes the wait for the NR queue).
template <unsigned StageCount>
class GpuStageTimer {
public:
	GpuStageTimer(const char* what, const char* const (&names)[StageCount])
	    : what(what)
	{
		std::copy(names, names + StageCount, stageNames);
	}

	void Begin(ID3D11Device* device, ID3D11DeviceContext* immediate, unsigned group) noexcept
	{
		if (disabled || active || group >= kGroups)
			return;
		if (!created && !Create(device))
			return;
		context = immediate;
		Collect();
		if (writeSeq - readSeq >= kRing) {
			// Oldest sample is still in flight: drop it rather than wait.
			++stats[ring[readSeq % kRing].group].dropped;
			ring[readSeq % kRing].pending = false;
			++readSeq;
			chain = false;
		}
		auto& r = ring[writeSeq % kRing];
		r.group = group;
		r.next = 0;
		r.pending = false;
		context->Begin(r.disjoint.Get());
		context->End(r.stamps[0].Get());
		active = true;
	}

	// End of `stage`; stages skipped on this path are marked here as zero.
	void Mark(unsigned stage) noexcept
	{
		if (!active)
			return;
		auto& r = ring[writeSeq % kRing];
		for (; r.next <= stage && r.next < StageCount; ++r.next)
			context->End(r.stamps[r.next + 1].Get());
	}

	void End() noexcept
	{
		if (!active)
			return;
		Mark(StageCount - 1);
		auto& r = ring[writeSeq % kRing];
		context->End(r.disjoint.Get());
		r.pending = true;
		++writeSeq;
		active = false;
		MaybeLog();
	}

	// The submission failed part way: close the bracket and forget the sample.
	void Abandon() noexcept
	{
		if (!active)
			return;
		auto& r = ring[writeSeq % kRing];
		context->End(r.disjoint.Get());
		r.pending = false;
		++writeSeq;
		active = false;
		chain = false;
	}

private:
	static constexpr unsigned kRing = 16;
	static constexpr unsigned kGroups = 2; // eyes
	struct Record {
		Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
		Microsoft::WRL::ComPtr<ID3D11Query> stamps[StageCount + 1];
		unsigned group = 0, next = 0;
		bool pending = false;
	};
	struct Stats {
		double stage[StageCount] = {};
		double total = 0, maxTotal = 0, before = 0;
		uint32_t samples = 0, beforeSamples = 0, dropped = 0, disjoint = 0;
	};

	bool Create(ID3D11Device* device) noexcept
	{
		created = true;
		D3D11_QUERY_DESC dd{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
		D3D11_QUERY_DESC td{ D3D11_QUERY_TIMESTAMP, 0 };
		for (auto& r : ring) {
			if (FAILED(device->CreateQuery(&dd, &r.disjoint))) {
				Disable("disjoint query");
				return false;
			}
			for (auto& s : r.stamps) {
				if (FAILED(device->CreateQuery(&td, &s))) {
					Disable("timestamp query");
					return false;
				}
			}
		}
		nextLog = GetTickCount64() + 10000;
		return true;
	}

	void Disable(const char* why) noexcept
	{
		disabled = true;
		OOVR_LOGF("VAM GPU timing (%s) off: could not create a %s", what, why);
	}

	// Reads finished samples in order; stops at the first one not ready yet.
	void Collect() noexcept
	{
		while (readSeq < writeSeq) {
			auto& r = ring[readSeq % kRing];
			if (r.pending) {
				D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
				if (context->GetData(r.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
					return;
				UINT64 t[StageCount + 1]{};
				for (unsigned i = 0; i <= StageCount; ++i)
					if (context->GetData(r.stamps[i].Get(), &t[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
						return;
				auto& s = stats[r.group];
				if (dj.Disjoint || dj.Frequency == 0) {
					++s.disjoint;
					chain = false;
				} else {
					const double ms = 1000.0 / static_cast<double>(dj.Frequency);
					double total = 0;
					for (unsigned i = 0; i < StageCount; ++i) {
						const double d = t[i + 1] > t[i] ? static_cast<double>(t[i + 1] - t[i]) * ms : 0.0;
						s.stage[i] += d;
						total += d;
					}
					s.total += total;
					s.maxTotal = (std::max)(s.maxTotal, total);
					++s.samples;
					if (chain && t[0] > lastEnd) {
						s.before += static_cast<double>(t[0] - lastEnd) * ms;
						++s.beforeSamples;
					}
					lastEnd = t[StageCount];
					chain = true;
				}
			} else {
				chain = false;
			}
			r.pending = false;
			++readSeq;
		}
	}

	void MaybeLog() noexcept
	{
		const ULONGLONG now = GetTickCount64();
		if (now < nextLog)
			return;
		nextLog = now + 10000;
		for (unsigned g = 0; g < kGroups; ++g) {
			auto& s = stats[g];
			if (!s.samples && !s.dropped)
				continue;
			char line[512];
			int n = sprintf_s(line, "VAM GPU timing %s eye=%u: %u samples (%u dropped, %u disjoint), avg ms:", what, g,
			    s.samples, s.dropped, s.disjoint);
			const double count = s.samples ? static_cast<double>(s.samples) : 1.0;
			for (unsigned i = 0; i < StageCount && n > 0 && n < static_cast<int>(sizeof(line)); ++i)
				n += sprintf_s(line + n, sizeof(line) - n, " %s %.3f", stageNames[i], s.stage[i] / count);
			if (n > 0 && n < static_cast<int>(sizeof(line)))
				sprintf_s(line + n, sizeof(line) - n, " = %.3f (max %.3f); gpu time since the previous sample ended %.3f",
				    s.total / count, s.maxTotal, s.beforeSamples ? s.before / s.beforeSamples : 0.0);
			OOVR_LOGF("%s", line);
			s = Stats{};
		}
	}

	const char* what;
	const char* stageNames[StageCount] = {};
	Record ring[kRing];
	Stats stats[kGroups];
	ID3D11DeviceContext* context = nullptr;
	uint64_t writeSeq = 0, readSeq = 0;
	UINT64 lastEnd = 0;
	ULONGLONG nextLog = 0;
	bool created = false, disabled = false, active = false, chain = false;
};
