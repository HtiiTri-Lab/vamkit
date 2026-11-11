// Native versions of VaM 1.22.0.3 character hot paths, bit-identical to the game's own C# as VaM's Mono runs it.
//
// The rules that make it bit-identical (measured in VaM's Mono, harness\floattest):
//   - float operands are widened to double, every operation runs in double, and the value is rounded to float only
//     where the IL stores it (a local, a field, an array element). Comparisons of computed values happen in double.
//   - Math.Sin / Math.Cos are the x87 fsin / fcos instructions on the double argument (x87.asm).
//   - no FMA contraction anywhere (built with /fp:strict).
// Every function below names the C# method it replaces and follows its IL statement by statement.
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <vector>
#include <immintrin.h>
#include <windows.h>

extern "C" double cb_fsin(double);
extern "C" double cb_fcos(double);
extern "C" void cb_fsincos(double, double*, double*);
extern "C" unsigned cb_get_x87cw();
extern "C" void cb_set_x87cw(unsigned);

#define CB_API extern "C" __declspec(dllexport)

static const int kApiVersion = 5;
CB_API int CB_ApiVersion() { return kApiVersion; }
CB_API unsigned CB_X87ControlWord() { return cb_get_x87cw(); }

// ---------------------------------------------------------------------------------------------------------------
// Worker pool, shared by every native plugin in the process (CB_Run). A job is a range cut into chunks; the calling
// thread takes chunks too and returns when all of its chunks are done. Several callers (VaM's character and skin
// threads, a plugin's solve thread) can have jobs in at once; a worker takes the chunks of the highest-priority job
// first, the oldest among equals, and leaves a lower one between chunks when a higher one comes in. Each chunk runs with the caller's x87 control word and MXCSR, so rounding and
// denormal handling are the caller's. A worker out of chunks looks for the next job for 50 us before it sleeps: a
// solver's substeps follow each other within microseconds, and waking a sleeping worker takes ~20 us.
// Placement: CB_SetWorkerCpus can give each worker its own CPU mask (our worker threads only; the process and VaM's
// threads are never touched); without masks Windows places the workers. CB_PinThread puts a plugin's own long-running
// thread on a core of its own among them.
// ---------------------------------------------------------------------------------------------------------------

// Tasks: what each job is for, by name, with a priority (2: VaM's frame paths, which the frame waits for; 1: plugin
// work the frame waits for; 0: background solves). Per task: calls, the callers' time in Run, and the CPU time of its
// chunks on all threads (CB_TaskStats; the plugin logs them with LogStats).
struct TaskInfo {
    char name[48] = {};
    int priority = 0;
    std::atomic<long long> calls{0}, wall{0}, cpu{0};
};
static const int kMaxTasks = 64;
static TaskInfo g_tasks[kMaxTasks];
static std::atomic<int> g_taskCount{0};
static std::mutex g_taskLock;
static int TaskId(const char* name, int priority) {
    std::lock_guard<std::mutex> g(g_taskLock);
    int n = g_taskCount.load();
    for (int i = 0; i < n; i++) if (strncmp(g_tasks[i].name, name, sizeof g_tasks[i].name - 1) == 0) return i;
    if (n == kMaxTasks) return kMaxTasks - 1;   // the last one takes the rest
    strncpy_s(g_tasks[n].name, name, _TRUNCATE);
    g_tasks[n].priority = priority < 0 ? 0 : priority > 2 ? 2 : priority;
    g_taskCount.store(n + 1);
    return n;
}
static inline long long Ticks() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static const int kTaskOther = TaskId("other", 0), kTaskSkin = TaskId("skinning", 2), kTaskMorph = TaskId("morphs", 2),
                 kTaskNormals = TaskId("normals", 2), kTaskSoft = TaskId("soft vertices", 2);

// a native path run on its caller's thread only, counted as its task
struct TaskTime {
    int task; long long t0;
    explicit TaskTime(int t) : task(t), t0(Ticks()) { g_tasks[t].calls.fetch_add(1, std::memory_order_relaxed); }
    ~TaskTime() { long long d = Ticks() - t0; g_tasks[task].wall.fetch_add(d, std::memory_order_relaxed); g_tasks[task].cpu.fetch_add(d, std::memory_order_relaxed); }
};

// L3 cache (CCD) of each logical CPU of processor group 0, -1 if unknown; and each L3's CPU mask.
static int g_l3Of[64];
static unsigned long long g_l3Mask[8];
static int g_l3Count = 0;
static void InitL3() {
    for (int& x : g_l3Of) x = -1;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationCache, nullptr, &len);
    std::vector<char> buf(len);
    if (!len || !GetLogicalProcessorInformationEx(RelationCache, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len)) return;
    for (DWORD off = 0; off < len;) {
        auto* p = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + off);
        if (p->Cache.Level == 3 && p->Cache.GroupMask.Group == 0 && g_l3Count < 8) {
            unsigned long long m = p->Cache.GroupMask.Mask;
            for (int i = 0; i < 64; i++) if (m >> i & 1) g_l3Of[i] = g_l3Count;
            g_l3Mask[g_l3Count++] = m;
        }
        off += p->Size;
    }
}
static const bool g_l3Read = (InitL3(), true);   // once, when the DLL loads
static int CurrentL3() { unsigned lp = GetCurrentProcessorNumber(); return lp < 64 ? g_l3Of[lp] : -1; }

// Where calls and chunks ran: per kind the logical CPU counts, plus worker chunks on the job caller's L3 / another.
enum { kHistSkin, kHistMorph, kHistNormals, kHistSoft, kHistWorker, kHistKinds };
static std::atomic<long long> g_hist[kHistKinds][64];
static std::atomic<long long> g_chunkSame{0}, g_chunkCross{0};
static inline void Count(int kind) { unsigned lp = GetCurrentProcessorNumber(); if (lp < 64) g_hist[kind][lp].fetch_add(1, std::memory_order_relaxed); }

struct Job {
    void (*fn)(void* ctx, int begin, int end) = nullptr;
    void* ctx = nullptr;
    int begin = 0, end = 0, chunk = 1, chunks = 0, task = 0, priority = 0;
    unsigned x87 = 0, mxcsr = 0;
    int callerL3 = -1;            // caller's L3, for the counters
    std::atomic<int> next{0};
    std::atomic<int> left{0};     // chunks not finished
};

class Pool {
public:
    // At process exit Windows ends the workers before the DLL detaches; a joinable std::thread would call terminate.
    ~Pool() { for (auto& t : threads_) if (t.joinable()) t.detach(); }

    void Resize(int n) {
        std::lock_guard<std::mutex> l(layout_);
        want_ = n;
        Layout();
    }
    int Size() { return size_.load(); }

    // The workers' CPU masks (none, or a 0 entry: the process's own mask), shared round robin by the workers.
    void SetCpus(const unsigned long long* masks, int n) {
        std::lock_guard<std::mutex> l(layout_);
        masks_.assign(masks, masks + (n > 0 ? n : 0));
        Layout();
    }

    void Run(int task, void (*fn)(void*, int, int), void* ctx, int begin, int end, int chunk) {
        int count = end - begin;
        if (count <= 0) return;
        TaskInfo& info = g_tasks[task];
        info.calls.fetch_add(1, std::memory_order_relaxed);
        long long t0 = Ticks();
        int chunks = (count + chunk - 1) / chunk;
        int workers = size_.load();
        if (workers == 0 || chunks < 2) {
            fn(ctx, begin, end);
            long long d = Ticks() - t0;
            info.cpu.fetch_add(d, std::memory_order_relaxed); info.wall.fetch_add(d, std::memory_order_relaxed);
            return;
        }
        Job job;
        job.fn = fn; job.ctx = ctx; job.begin = begin; job.end = end; job.chunk = chunk;
        job.chunks = chunks; job.task = task; job.priority = info.priority;
        job.x87 = cb_get_x87cw(); job.mxcsr = _mm_getcsr();
        job.callerL3 = CurrentL3();
        job.left.store(chunks);
        int sleeping;
        {
            std::lock_guard<std::mutex> g(m_);
            jobs_.push_back(&job);
            open_[job.priority].fetch_add(1, std::memory_order_relaxed);
            sleeping = sleeping_;
            gen_.fetch_add(1, std::memory_order_release);
        }
        // Wake only as many sleeping workers as there are chunks to share.
        int want = chunks - 1 < sleeping ? chunks - 1 : sleeping;
        if (want >= sleeping) cv_.notify_all();
        else for (int i = 0; i < want; i++) cv_.notify_one();
        while (Take(job)) {}
        // Our chunks may still be running on workers; wait without taking other callers' work.
        int spins = 0;
        while (job.left.load(std::memory_order_acquire) > 0) {
            if (++spins < 2000) _mm_pause(); else SwitchToThread();
        }
        {
            std::lock_guard<std::mutex> g(m_);
            for (size_t i = 0; i < jobs_.size(); i++) if (jobs_[i] == &job) { jobs_.erase(jobs_.begin() + i); break; }
        }
        info.wall.fetch_add(Ticks() - t0, std::memory_order_relaxed);
    }

    // The calling thread onto the k-th of the workers' masks counted from the last, which the workers then leave to it
    // (they keep the others, one worker fewer per mask taken). A caller that shares a core with a worker crawls after
    // the pool sat idle (both on one logical CPU of a parked core: a 70-job step 9 ms instead of 3). Returns its mask,
    // 0 when the workers have none (Windows places them, and this thread too).
    unsigned long long PinCaller(int k) {
        std::lock_guard<std::mutex> l(layout_);
        if (masks_.empty()) return 0;
        int n = (int)masks_.size();
        k = (k < 0 ? 0 : k) % n;
        if (k + 1 > reserved_) { reserved_ = k + 1; Layout(); }
        unsigned long long want = masks_[n - 1 - k] & ProcessMask();
        if (!want) return 0;
        SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)want);
        return want;
    }

private:
    // Runs one chunk of job j; false when j has no chunks left to hand out (the first to find that closes it).
    bool Take(Job& j) {
        int k = j.next.fetch_add(1);
        int b = j.begin + k * j.chunk;
        if (b >= j.end) { if (k == j.chunks) open_[j.priority].fetch_sub(1, std::memory_order_relaxed); return false; }
        int e = b + j.chunk < j.end ? b + j.chunk : j.end;
        long long t0 = Ticks();
        j.fn(j.ctx, b, e);
        g_tasks[j.task].cpu.fetch_add(Ticks() - t0, std::memory_order_relaxed);
        j.left.fetch_sub(1, std::memory_order_release);
        return true;
    }

    // The highest-priority job with chunks left (the oldest among equals), counted as in use (so its caller does not
    // return under us); m_ held.
    Job* Pick() {
        Job* best = nullptr;
        for (Job* q : jobs_)
            if (q->next.load() * q->chunk < q->end - q->begin && (!best || q->priority > best->priority)) best = q;
        if (best) best->left.fetch_add(1);
        return best;
    }

    // Is a job of higher priority than p handing out chunks?
    bool Above(int p) const {
        for (int q = p + 1; q < 3; q++) if (open_[q].load(std::memory_order_relaxed) > 0) return true;
        return false;
    }

    static unsigned long long ProcessMask() {
        DWORD_PTR proc = 0, sys = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
        return proc;
    }

    // Masks the workers may use: all but the reserved ones at the end (at least one).
    int FreeMasks() const { int f = (int)masks_.size() - reserved_; return f > 1 ? f : 1; }

    // Starts the worker count wanted (fewer when callers took masks: no more workers per mask than without them) and
    // gives each its mask; layout_ held.
    void Layout() {
        int target = want_;
        if (!masks_.empty()) {
            int n = (int)masks_.size(), perMask = (want_ + n - 1) / n, cap = FreeMasks() * perMask;
            if (target > cap) target = cap;
        }
        if (target != size_.load()) {
            Stop();
            std::lock_guard<std::mutex> g(m_);
            quit_ = false;
            for (int i = 0; i < target; i++) threads_.emplace_back([this] { Work(); });
            size_.store(target);
        }
        unsigned long long proc = ProcessMask();
        int free = FreeMasks();
        for (size_t i = 0; i < threads_.size(); i++) {
            unsigned long long want = masks_.empty() ? 0 : masks_[i % free] & proc;
            SetThreadAffinityMask((HANDLE)threads_[i].native_handle(), want ? (DWORD_PTR)want : proc);
        }
    }

    // A worker woken for a job that its caller has already finished (a parked core can take longer to wake than a
    // short job lasts) spins for the next one too instead of going straight back to sleep; otherwise it misses every
    // job of a solver's step after the pool sat idle between frames.
    void Work() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);   // as VaM's own skin sub-threads
        unsigned x87 = cb_get_x87cw(), mxcsr = _mm_getcsr();
        LARGE_INTEGER f; QueryPerformanceFrequency(&f);
        const long long spin = f.QuadPart / 20000;   // 50 us
        for (;;) {
            Job* j = nullptr;
            unsigned long long seen;
            {
                std::lock_guard<std::mutex> g(m_);
                if (quit_) return;
                j = Pick();
                seen = gen_.load(std::memory_order_acquire);
            }
            if (!j) {
                for (long long until = Ticks() + spin; gen_.load(std::memory_order_acquire) == seen && Ticks() < until;) _mm_pause();
                if (gen_.load(std::memory_order_acquire) == seen) {
                    std::unique_lock<std::mutex> g(m_);
                    sleeping_++;
                    cv_.wait(g, [&] { return quit_ || gen_.load(std::memory_order_acquire) != seen; });
                    sleeping_--;
                }
                continue;
            }
            if (j->x87 != x87) { cb_set_x87cw(j->x87); x87 = j->x87; }
            if (j->mxcsr != mxcsr) { _mm_setcsr(j->mxcsr); mxcsr = j->mxcsr; }
            int l3 = CurrentL3();
            long long n = 0;
            // leave a job between chunks when one of higher priority has chunks to hand out
            while (Take(*j)) { n++; if (Above(j->priority)) break; }
            (l3 == j->callerL3 ? g_chunkSame : g_chunkCross).fetch_add(n, std::memory_order_relaxed);
            if (n) Count(kHistWorker);
            j->left.fetch_sub(1, std::memory_order_release);
        }
    }

    void Stop() {
        std::vector<std::thread> old;
        { std::lock_guard<std::mutex> g(m_); quit_ = true; old.swap(threads_); size_.store(0); }
        cv_.notify_all();
        for (auto& t : old) t.join();
    }

    std::mutex layout_;                        // the workers, their masks and the callers' reserved masks
    std::vector<std::thread> threads_;
    std::vector<unsigned long long> masks_;
    int want_ = 0, reserved_ = 0;
    std::mutex m_;                             // the jobs
    std::condition_variable cv_;
    std::vector<Job*> jobs_;
    std::atomic<unsigned long long> gen_{0};   // jobs put in so far
    std::atomic<int> open_[3] = {};            // jobs still handing out chunks, by priority
    std::atomic<int> size_{0};
    int sleeping_ = 0;
    bool quit_ = false;
};

static Pool g_pool;

// Number of worker threads (0 = everything on the calling thread). Call while nothing is running.
CB_API void CB_SetThreads(int n) {
    g_pool.Resize(n < 0 ? 0 : n > 64 ? 64 : n);
}
CB_API int CB_GetThreads() { return g_pool.Size(); }

// Worker placement (see the pool).
CB_API void CB_SetWorkerCpus(const unsigned long long* masks, int n) { g_pool.SetCpus(masks, n); }

// ---- the shared runtime, for every native plugin (found with GetModuleHandle(L"VaMCpuBoostNative.bin")) ----------
// A task id for name (the same name, the same id); priority as in "Tasks".
CB_API int CB_Task(const char* name, int priority) { return TaskId(name ? name : "other", priority); }
// fn(ctx, b, e) over [begin, end) in chunks of `chunk`, on the shared workers and the calling thread; returns when all
// are done. Chunks run in any order on any of those threads, with the caller's rounding mode.
CB_API void CB_Run(int task, void (*fn)(void*, int, int), void* ctx, int begin, int end, int chunk) {
    if (task < 0 || task >= g_taskCount.load()) task = kTaskOther;
    g_pool.Run(task, fn, ctx, begin, end, chunk < 1 ? 1 : chunk);
}
// A plugin's own long-running thread (a solve thread that calls CB_Run) onto a core of its own among the workers': the
// k-th counted from the last, which the workers leave from then on (the same k, the same core). Returns the CPU mask
// set, 0 when the workers have none (then Windows places this thread too).
CB_API unsigned long long CB_PinThread(int k) { return g_pool.PinCaller(k); }
CB_API int CB_TaskCount() { return g_taskCount.load(); }
// Task i: out = calls, ms the callers spent in CB_Run, CPU ms of its chunks on all threads; name into name[len].
// reset: zero the counts after reading them. Returns 0 for an unknown task.
CB_API int CB_TaskStats(int i, char* name, int len, double* out, int reset) {
    if (i < 0 || i >= g_taskCount.load()) return 0;
    TaskInfo& t = g_tasks[i];
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    double ms = 1000.0 / (double)f.QuadPart;
    long long c = reset ? t.calls.exchange(0) : t.calls.load(), w = reset ? t.wall.exchange(0) : t.wall.load(),
              u = reset ? t.cpu.exchange(0) : t.cpu.load();
    out[0] = (double)c; out[1] = w * ms; out[2] = u * ms;
    if (name && len > 0) strncpy_s(name, (size_t)len, t.name, _TRUNCATE);
    return 1;
}
// The CPU mask of L3 cache (CCD) i of processor group 0, 0 if there is none.
CB_API unsigned long long CB_L3Mask(int i) {
    return i >= 0 && i < g_l3Count ? g_l3Mask[i] : 0;
}
// The physical cores of processor group 0 as CPU masks (both logical CPUs of a core), the cores Windows prefers least
// first (lowest scheduling class, then lowest index); returns how many were written.
CB_API int CB_CoreMasks(unsigned long long* out, int max) {
    ULONG n = 0;
    GetSystemCpuSetInformation(nullptr, 0, &n, GetCurrentProcess(), 0);
    std::vector<char> buf(n);
    if (!n || !GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf.data(), n, &n, GetCurrentProcess(), 0)) return 0;
    struct Core { int index, sched; unsigned long long mask; };
    std::vector<Core> cores;
    for (ULONG off = 0; off < n;) {
        auto* s = (PSYSTEM_CPU_SET_INFORMATION)(buf.data() + off);
        if (s->Type == CpuSetInformation && s->CpuSet.Group == 0 && s->CpuSet.LogicalProcessorIndex < 64) {
            int core = s->CpuSet.CoreIndex;
            unsigned long long bit = 1ull << s->CpuSet.LogicalProcessorIndex;
            bool found = false;
            for (Core& c : cores) if (c.index == core) { c.mask |= bit; found = true; }
            if (!found) cores.push_back(Core{ core, s->CpuSet.SchedulingClass, bit });
        }
        off += s->Size;
    }
    for (size_t i = 1; i < cores.size(); i++)
        for (size_t k = i; k > 0 && (cores[k].sched < cores[k - 1].sched ||
                                     (cores[k].sched == cores[k - 1].sched && cores[k].index < cores[k - 1].index)); k--)
            std::swap(cores[k], cores[k - 1]);
    int c = 0;
    for (; c < (int)cores.size() && c < max; c++) out[c] = cores[c].mask;
    return c;
}
// Copies kind's per-CPU counts (64) into out; kind kHistKinds: out[0] = worker chunks on the caller's L3, out[1] on
// another L3. reset: zero them after copying.
CB_API void CB_CpuHist(int kind, long long* out, int reset) {
    if (kind == kHistKinds) {
        out[0] = reset ? g_chunkSame.exchange(0) : g_chunkSame.load();
        out[1] = reset ? g_chunkCross.exchange(0) : g_chunkCross.load();
        return;
    }
    if (kind < 0 || kind > kHistKinds) return;
    for (int i = 0; i < 64; i++) out[i] = reset ? g_hist[kind][i].exchange(0) : g_hist[kind][i].load();
}
// Tests only: pins the calling thread to mask (0 = the process's mask); returns the previous mask.
CB_API unsigned long long CB_TestPinThread(unsigned long long mask) {
    DWORD_PTR proc = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    return SetThreadAffinityMask(GetCurrentThread(), mask ? (DWORD_PTR)mask : proc);
}

typedef double D;
static inline float R(D x) { return (float)x; }   // a store into a float local / field / element

// Unity Matrix4x4 is column-major: m<row><col> at [col * 4 + row].
static inline float M(const float* m, int row, int col) { return m[col * 4 + row]; }

// ---------------------------------------------------------------------------------------------------------------
// DAZSkinV2.SkinMeshPart(int startIndex, int stopIndex, bool[] onlyVerts, bool[] onlyBones)
//
// The C# walks bones in order and, per bone, its weight list and then its fully-weighted list, transforming
// workingVerts[v] in place. Every step reads and writes only vertex v, so running each vertex through its own steps
// in that same global order gives the same bits; the per-vertex step lists are built once per skin here.
// ---------------------------------------------------------------------------------------------------------------

enum : uint8_t { OpWeight = 0, OpFull = 1, OpGeneral = 2 };
struct Op { uint32_t data; uint16_t bone; uint8_t kind; uint8_t pad; };

struct WeightData { int vertex; float xw, yw, zw, xl, xr, yl, yr, zl, zr; };   // DAZSkinV2VertexWeights
struct GeneralData { int vertex; float weight; };                            // DAZSkinV2GeneralVertexWeights

struct Skin {
    int numVerts = 0, numBones = 0;
    std::vector<int> order;                // DAZSkinV2Node.rotationOrder per bone
    std::vector<float> bulge;              // 12 per bone: xposleft xnegleft xposright xnegright, y..., z...
    std::vector<WeightData> weights;
    std::vector<GeneralData> general;
    // Per vertex, the DAZ-mode steps (weights + fully weighted) and the general-mode steps, in the C# order.
    std::vector<uint32_t> dazStart, genStart;   // numVerts + 1
    std::vector<Op> dazOps, genOps;
};

// Per call, per bone values SkinMeshPart computes before its vertex loops.
struct BoneK {
    float c[12];      // V_3..V_14   boneChangeFromOriginalMatrices: m00 m01 m02 m03 m10 ... m23
    float w2l[12];    // V_22..V_33  boneWorldToLocalMatrices
    float l2w[12];    // V_35..V_46  boneLocalToWorldMatrices
    float ax, ay, az; // V_47        boneRotationAngles
    // V_48..V_62: any / posleft / negleft / posright / negright per axis; V_63..V_74 the matching factors.
    bool xAny, xPL, xNL, xPR, xNR, yAny, yPL, yNL, yPR, yNR, zAny, zPL, zNL, zPR, zNR;
    float xPLv, xNLv, xPRv, xNRv, yPLv, yNLv, yPRv, yNRv, zPLv, zNLv, zPRv, zNRv;
    bool enabled;
    int order;
};

static void Rows(const float* m, float* out) {
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) out[r * 4 + c] = M(m, r, c);
}

// x' = (((x * r0) + (y * r1)) + (z * r2)) + r3, in double, one rounding at the store.
static inline float Row(const float* r, float x, float y, float z) {
    return R((((D)x * r[0] + (D)y * r[1]) + (D)z * r[2]) + r[3]);
}

CB_API void* CB_SkinCreate(int numVerts, int numBones, const int* rotationOrder, const float* bulge12,
                           const int* weightCount, const int* weightVertex, const float* weightData9,
                           const int* fullCount, const int* fullVertex,
                           const int* generalCount, const int* generalVertex, const float* generalWeight) {
    Skin* s = new (std::nothrow) Skin();
    if (!s) return nullptr;
    s->numVerts = numVerts; s->numBones = numBones;
    s->order.assign(rotationOrder, rotationOrder + numBones);
    s->bulge.assign(bulge12, bulge12 + (size_t)numBones * 12);
    size_t nw = 0, nf = 0, ng = 0;
    for (int b = 0; b < numBones; b++) { nw += weightCount[b]; nf += fullCount[b]; ng += generalCount[b]; }
    s->weights.resize(nw);
    for (size_t i = 0; i < nw; i++) {
        WeightData& w = s->weights[i]; const float* d = weightData9 + i * 9;
        w.vertex = weightVertex[i]; w.xw = d[0]; w.yw = d[1]; w.zw = d[2];
        w.xl = d[3]; w.xr = d[4]; w.yl = d[5]; w.yr = d[6]; w.zl = d[7]; w.zr = d[8];
    }
    s->general.resize(ng);
    for (size_t i = 0; i < ng; i++) { s->general[i].vertex = generalVertex[i]; s->general[i].weight = generalWeight[i]; }

    // Count, then fill the per-vertex lists in bone order (weights before fully weighted, as the C# loops run).
    std::vector<uint32_t> dazCount(numVerts, 0), genCount(numVerts, 0);
    size_t wi = 0, fi = 0, gi = 0;
    for (int b = 0; b < numBones; b++) {
        for (int k = 0; k < weightCount[b]; k++) { int v = weightVertex[wi++]; if (v < 0 || v >= numVerts) { delete s; return nullptr; } dazCount[v]++; }
        for (int k = 0; k < fullCount[b]; k++) { int v = fullVertex[fi++]; if (v < 0 || v >= numVerts) { delete s; return nullptr; } dazCount[v]++; }
        for (int k = 0; k < generalCount[b]; k++) { int v = generalVertex[gi++]; if (v < 0 || v >= numVerts) { delete s; return nullptr; } genCount[v]++; }
    }
    s->dazStart.assign(numVerts + 1, 0); s->genStart.assign(numVerts + 1, 0);
    for (int v = 0; v < numVerts; v++) { s->dazStart[v + 1] = s->dazStart[v] + dazCount[v]; s->genStart[v + 1] = s->genStart[v] + genCount[v]; }
    s->dazOps.resize(s->dazStart[numVerts]); s->genOps.resize(s->genStart[numVerts]);
    std::vector<uint32_t> dazPos(s->dazStart.begin(), s->dazStart.end() - 1), genPos(s->genStart.begin(), s->genStart.end() - 1);
    wi = fi = gi = 0;
    for (int b = 0; b < numBones; b++) {
        for (int k = 0; k < weightCount[b]; k++, wi++) { int v = weightVertex[wi]; s->dazOps[dazPos[v]++] = Op{ (uint32_t)wi, (uint16_t)b, OpWeight, 0 }; }
        for (int k = 0; k < fullCount[b]; k++, fi++) { int v = fullVertex[fi]; s->dazOps[dazPos[v]++] = Op{ 0, (uint16_t)b, OpFull, 0 }; }
        for (int k = 0; k < generalCount[b]; k++, gi++) { int v = generalVertex[gi]; s->genOps[genPos[v]++] = Op{ (uint32_t)gi, (uint16_t)b, OpGeneral, 0 }; }
    }
    return s;
}

CB_API void CB_SkinDestroy(void* skin) { delete (Skin*)skin; }

static void PrepareBones(const Skin& s, const uint8_t* onlyBones, int useGeneral, float bulgeScale,
                         const float* changeMats, const float* w2lMats, const float* l2wMats, const float* angles,
                         BoneK* k) {
    for (int b = 0; b < s.numBones; b++) {
        BoneK& K = k[b];
        K.enabled = !onlyBones || onlyBones[b];
        if (!K.enabled) continue;
        Rows(changeMats + b * 16, K.c);
        if (useGeneral) continue;
        Rows(w2lMats + b * 16, K.w2l);
        Rows(l2wMats + b * 16, K.l2w);
        K.order = s.order[b];
        K.ax = angles[b * 3 + 0]; K.ay = angles[b * 3 + 1]; K.az = angles[b * 3 + 2];
        const float* f = &s.bulge[b * 12];   // xposleft xnegleft xposright xnegright | y | z
        K.xAny = K.xPL = K.xNL = K.xPR = K.xNR = false;
        K.yAny = K.yPL = K.yNL = K.yPR = K.yNR = false;
        K.zAny = K.zPL = K.zNL = K.zPR = K.zNR = false;
        K.xPLv = K.xNLv = K.xPRv = K.xNRv = K.yPLv = K.yNLv = K.yPRv = K.yNRv = K.zPLv = K.zNLv = K.zPRv = K.zNRv = 0.0f;
        // if (angle > 0.01f) { posleft, posright } else if (angle < -0.01f) { negleft, negright }
        // factor = (bulgeFactor * angle) * bulgeScale; a factor of exactly 0 leaves its flag off.
        if (K.ax > 0.01f) {
            if (!(f[0] == 0.0f)) { K.xAny = K.xPL = true; K.xPLv = R(((D)f[0] * K.ax) * bulgeScale); }
            if (!(f[2] == 0.0f)) { K.xAny = K.xPR = true; K.xPRv = R(((D)f[2] * K.ax) * bulgeScale); }
        } else if (K.ax < -0.01f) {
            if (!(f[1] == 0.0f)) { K.xAny = K.xNL = true; K.xNLv = R(((D)f[1] * K.ax) * bulgeScale); }
            if (!(f[3] == 0.0f)) { K.xAny = K.xNR = true; K.xNRv = R(((D)f[3] * K.ax) * bulgeScale); }
        }
        if (K.ay > 0.01f) {
            if (!(f[4] == 0.0f)) { K.yAny = K.yPL = true; K.yPLv = R(((D)f[4] * K.ay) * bulgeScale); }
            if (!(f[6] == 0.0f)) { K.yAny = K.yPR = true; K.yPRv = R(((D)f[6] * K.ay) * bulgeScale); }
        } else if (K.ay < -0.01f) {
            if (!(f[5] == 0.0f)) { K.yAny = K.yNL = true; K.yNLv = R(((D)f[5] * K.ay) * bulgeScale); }
            if (!(f[7] == 0.0f)) { K.yAny = K.yNR = true; K.yNRv = R(((D)f[7] * K.ay) * bulgeScale); }
        }
        if (K.az > 0.01f) {
            if (!(f[8] == 0.0f)) { K.zAny = K.zPL = true; K.zPLv = R(((D)f[8] * K.az) * bulgeScale); }
            if (!(f[10] == 0.0f)) { K.zAny = K.zPR = true; K.zPRv = R(((D)f[10] * K.az) * bulgeScale); }
        } else if (K.az < -0.01f) {
            if (!(f[9] == 0.0f)) { K.zAny = K.zNL = true; K.zNLv = R(((D)f[9] * K.az) * bulgeScale); }
            if (!(f[11] == 0.0f)) { K.zAny = K.zNR = true; K.zNRv = R(((D)f[11] * K.az) * bulgeScale); }
        }
    }
}

// (float)Math.Sin(a), (float)Math.Cos(a): one fsincos where it is proven bit-identical to fsin + fcos.
static inline void SinCos(float a, float& sn, float& cs) {
    if (a >= -8.0f && a <= 8.0f) { double s, c; cb_fsincos((D)a, &s, &c); sn = R(s); cs = R(c); }
    else { sn = R(cb_fsin((D)a)); cs = R(cb_fcos((D)a)); }
}

// p += p * (factor * bulge) for the two components an axis bulges; factor * bulge is a float local in the C#.
static inline void Bulge(float& p, float& q, float factor, float amount) {
    float k = R((D)factor * amount);
    p = R((D)p + (D)p * k);
    q = R((D)q + (D)q * k);
}

static inline void AxisZ(const BoneK& K, const WeightData& w, float& x, float& y) {
    if (w.zw > 0.0f) {
        float a = R((D)K.az * w.zw);
        float sn, cs; SinCos(a, sn, cs);
        float t = R((D)x * cs - (D)y * sn);
        y = R((D)x * sn + (D)y * cs);
        x = t;
    }
    if (K.zAny) {
        if (K.zPL && w.zl > 0.0f) Bulge(x, y, K.zPLv, w.zl);
        if (K.zPR && w.zr > 0.0f) Bulge(x, y, K.zPRv, w.zr);
        if (K.zNL && w.zl > 0.0f) Bulge(x, y, K.zNLv, w.zl);
        if (K.zNR && w.zr > 0.0f) Bulge(x, y, K.zNRv, w.zr);
    }
}

static inline void AxisY(const BoneK& K, const WeightData& w, float& x, float& z) {
    if (w.yw > 0.0f) {
        float a = R((D)K.ay * w.yw);
        float sn, cs; SinCos(a, sn, cs);
        float t = R((D)x * cs + (D)z * sn);
        z = R((D)z * cs - (D)x * sn);
        x = t;
    }
    if (K.yAny) {
        if (K.yPL && w.yl > 0.0f) Bulge(x, z, K.yPLv, w.yl);
        if (K.yPR && w.yr > 0.0f) Bulge(x, z, K.yPRv, w.yr);
        if (K.yNL && w.yl > 0.0f) Bulge(x, z, K.yNLv, w.yl);
        if (K.yNR && w.yr > 0.0f) Bulge(x, z, K.yNRv, w.yr);
    }
}

static inline void AxisX(const BoneK& K, const WeightData& w, float& y, float& z) {
    if (w.xw > 0.0f) {
        float a = R((D)K.ax * w.xw);
        float sn, cs; SinCos(a, sn, cs);
        float t = R((D)y * cs - (D)z * sn);
        z = R((D)y * sn + (D)z * cs);
        y = t;
    }
    if (K.xAny) {
        if (K.xPL && w.xl > 0.0f) Bulge(y, z, K.xPLv, w.xl);
        if (K.xPR && w.xr > 0.0f) Bulge(y, z, K.xPRv, w.xr);
        if (K.xNL && w.xl > 0.0f) Bulge(y, z, K.xNLv, w.xl);
        if (K.xNR && w.xr > 0.0f) Bulge(y, z, K.xNRv, w.xr);
    }
}

// One partially weighted bone on one vertex (the body of the six rotation-order loops).
static inline void WeightStep(const BoneK& K, const WeightData& w, float* v) {
    if (w.xw > 0.99999f && w.yw > 0.99999f && w.zw > 0.99999f) {
        float x = v[0], y = v[1], z = v[2];
        v[0] = Row(K.c + 0, x, y, z); v[1] = Row(K.c + 4, x, y, z); v[2] = Row(K.c + 8, x, y, z);
        return;
    }
    float ox = v[0], oy = v[1], oz = v[2];
    float x = Row(K.w2l + 0, ox, oy, oz), y = Row(K.w2l + 4, ox, oy, oz), z = Row(K.w2l + 8, ox, oy, oz);
    switch (K.order) {          // Quaternion2Angles.RotationOrder; axes run in reverse of the name
        case 0: AxisZ(K, w, x, y); AxisY(K, w, x, z); AxisX(K, w, y, z); break;   // XYZ
        case 1: AxisY(K, w, x, z); AxisZ(K, w, x, y); AxisX(K, w, y, z); break;   // XZY
        case 2: AxisZ(K, w, x, y); AxisX(K, w, y, z); AxisY(K, w, x, z); break;   // YXZ
        case 3: AxisX(K, w, y, z); AxisZ(K, w, x, y); AxisY(K, w, x, z); break;   // YZX
        case 4: AxisY(K, w, x, z); AxisX(K, w, y, z); AxisZ(K, w, x, y); break;   // ZXY
        case 5: AxisX(K, w, y, z); AxisY(K, w, x, z); AxisZ(K, w, x, y); break;   // ZYX
    }
    v[0] = Row(K.l2w + 0, x, y, z); v[1] = Row(K.l2w + 4, x, y, z); v[2] = Row(K.l2w + 8, x, y, z);
}

static void SkinRange(const Skin& s, const BoneK* k, int start, int stop, const uint8_t* onlyVerts, int useGeneral,
                      float* verts, const float* startVerts) {
    if (start < 0) start = 0;
    if (stop > s.numVerts - 1) stop = s.numVerts - 1;
    if (useGeneral) {
        for (int v = start; v <= stop; v++) {
            float* p = verts + (size_t)v * 3; const float* q = startVerts + (size_t)v * 3;
            for (uint32_t i = s.genStart[v]; i < s.genStart[v + 1]; i++) {
                const Op& op = s.genOps[i]; const BoneK& K = k[op.bone];
                if (!K.enabled) continue;
                float wgt = s.general[op.data].weight;
                float sx = q[0], sy = q[1], sz = q[2];
                p[0] = R((D)p[0] + ((((D)sx * K.c[0] + (D)sy * K.c[1]) + (D)sz * K.c[2]) + K.c[3]) * wgt);
                p[1] = R((D)p[1] + ((((D)sx * K.c[4] + (D)sy * K.c[5]) + (D)sz * K.c[6]) + K.c[7]) * wgt);
                p[2] = R((D)p[2] + ((((D)sx * K.c[8] + (D)sy * K.c[9]) + (D)sz * K.c[10]) + K.c[11]) * wgt);
            }
        }
        return;
    }
    for (int v = start; v <= stop; v++) {
        if (onlyVerts && !onlyVerts[v]) continue;
        float* p = verts + (size_t)v * 3;
        for (uint32_t i = s.dazStart[v]; i < s.dazStart[v + 1]; i++) {
            const Op& op = s.dazOps[i]; const BoneK& K = k[op.bone];
            if (!K.enabled) continue;
            if (op.kind == OpFull) {
                float x = p[0], y = p[1], z = p[2];
                p[0] = Row(K.c + 0, x, y, z); p[1] = Row(K.c + 4, x, y, z); p[2] = Row(K.c + 8, x, y, z);
            } else if (K.order >= 0 && K.order <= 5) {   // the C# has no weight loop for any other rotation order
                WeightStep(K, s.weights[op.data], p);
            }
        }
    }
}

struct SkinJob {
    const Skin* s; const BoneK* k; const uint8_t* onlyVerts; int useGeneral; float* verts; const float* startVerts;
};
static const int kSkinChunk = 256;   // vertices per chunk: ~25 us of work, enough to hide the hand-out
static void SkinChunk(void* ctx, int begin, int end) {
    SkinJob* j = (SkinJob*)ctx;
    SkinRange(*j->s, j->k, begin, end - 1, j->onlyVerts, j->useGeneral, j->verts, j->startVerts);
}

// Returns 0 on success; the caller falls back to the C# for anything else.
CB_API int CB_SkinPart(void* skin, int start, int stop, const uint8_t* onlyVerts, const uint8_t* onlyBones,
                       int useGeneral, float bulgeScale, const float* changeMats, const float* w2lMats,
                       const float* l2wMats, const float* angles, float* workingVerts, const float* startVertsCopy) {
    Count(kHistSkin);
    Skin* s = (Skin*)skin;
    if (!s || !workingVerts || !changeMats) return 1;
    if (useGeneral && !startVertsCopy) return 2;
    if (!useGeneral && (!w2lMats || !l2wMats || !angles)) return 3;
    BoneK stackK[256];
    std::vector<BoneK> heapK;
    BoneK* k = stackK;
    if (s->numBones > 256) { heapK.resize(s->numBones); k = heapK.data(); }
    PrepareBones(*s, onlyBones, useGeneral, bulgeScale, changeMats, w2lMats, l2wMats, angles, k);
    if (start < 0) start = 0;
    if (stop > s->numVerts - 1) stop = s->numVerts - 1;
    SkinJob job{ s, k, onlyVerts, useGeneral, workingVerts, startVertsCopy };
    g_pool.Run(kTaskSkin, SkinChunk, &job, start, stop + 1, kSkinChunk);
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Shared by the paths below. An array argument is a pointer plus a length, length -1 meaning the C# array is null.
// Every entry point checks all the indices the C# would use before it writes anything, and returns non-zero (nothing
// written) for an input the C# would throw on; the caller then runs the C#.
// ---------------------------------------------------------------------------------------------------------------

// Mathf.Sqrt(((x * x) + (y * y)) + (z * z)) into a float local: the sum is rounded to the float argument, Math.Sqrt
// runs on its double, and the result is rounded again.
static inline float Len(float x, float y, float z) {
    return R(std::sqrt((D)R(((D)x * x + (D)y * y) + (D)z * z)));
}

static inline bool In(int i, int len) { return i >= 0 && i < len; }

// ---------------------------------------------------------------------------------------------------------------
// RecalculateNormals.recalculateNormals(int[] trianglesToUse, int[] verticesToUse), which VaM calls for the post-skin
// vertices (DAZSkinV2.SkinMeshPostVertsThreadedFast). It forwards to the static overload with this.triangles,
// this.vertices, this.normals, this.surfaceNormals, this.thisMarkerArray and normalizeSurfaceNormals = true.
// ---------------------------------------------------------------------------------------------------------------

// normals[v] = n on a vertex's first triangle this call (markerArray[v] still false), normals[v] += n after that.
static inline void Accum(float* normals, uint8_t* marker, int v, float x, float y, float z) {
    float* n = normals + (size_t)v * 3;
    if (!marker[v]) { marker[v] = 1; n[0] = x; n[1] = y; n[2] = z; }
    else { n[0] = R((D)n[0] + x); n[1] = R((D)n[1] + y); n[2] = R((D)n[2] + z); }
}

CB_API int CB_NormalsSubset(const int* triangles, int triLen, const float* vertices, int vertLen,
                            float* normals, int normLen, float* surfaceNormals, int surfLen,
                            const int* trianglesToUse, int ttuLen, const int* verticesToUse, int vtuLen,
                            uint8_t* marker, int markerLen, int normalizeSurface) {
    Count(kHistNormals);
    TaskTime timed(kTaskNormals);
    // vertices.Length is read first; a null marker array means the C# shares a static one: both stay with the C#.
    if (vertLen < 0 || markerLen < 0 || ttuLen < 0 || vtuLen < 0) return 1;
    for (int i = 0; i < ttuLen; i++) {
        int t = trianglesToUse[i];
        if (triLen < 0) return 2;
        if (t >= triLen) continue;                          // the C# skips these
        if (t < 0 || (int64_t)t + 2 >= triLen) return 3;
        if (surfLen < 0 || t / 3 >= surfLen || normLen < 0) return 4;
        for (int k = 0; k < 3; k++) {
            int v = triangles[t + k];
            if (!In(v, vertLen) || v >= normLen || v >= markerLen) return 5;
        }
    }
    for (int j = 0; j < vtuLen; j++) {
        int v = verticesToUse[j];
        if (v >= markerLen) continue;                       // the C# skips these
        if (v < 0 || normLen < 0 || v >= normLen) return 6;
    }

    for (int i = 0; i < ttuLen; i++) {
        int t = trianglesToUse[i];
        if (t >= triLen) continue;
        int a = triangles[t], b = triangles[t + 1], c = triangles[t + 2];
        const float* pa = vertices + (size_t)a * 3;
        const float* pb = vertices + (size_t)b * 3;
        const float* pc = vertices + (size_t)c * 3;
        float e1x = R((D)pb[0] - pa[0]), e1y = R((D)pb[1] - pa[1]), e1z = R((D)pb[2] - pa[2]);   // V_7..V_9
        float e2x = R((D)pc[0] - pa[0]), e2y = R((D)pc[1] - pa[1]), e2z = R((D)pc[2] - pa[2]);   // V_10..V_12
        float* sn = surfaceNormals + (size_t)(t / 3) * 3;
        sn[0] = R((D)e1y * e2z - (D)e1z * e2y);
        sn[1] = R((D)e1z * e2x - (D)e1x * e2z);
        sn[2] = R((D)e1x * e2y - (D)e1y * e2x);
        if (sn[0] == 0.0f && sn[1] == 0.0f && sn[2] == 0.0f) sn[2] = 0.001f;
        if (normalizeSurface) {
            float len = Len(sn[0], sn[1], sn[2]);
            float inv = R(1.0 / (D)len);
            sn[0] = R((D)sn[0] * inv); sn[1] = R((D)sn[1] * inv); sn[2] = R((D)sn[2] * inv);
        }
        float nx = sn[0], ny = sn[1], nz = sn[2];                                                  // V_15
        Accum(normals, marker, a, nx, ny, nz);
        Accum(normals, marker, b, nx, ny, nz);
        Accum(normals, marker, c, nx, ny, nz);
    }
    for (int j = 0; j < vtuLen; j++) {
        int v = verticesToUse[j];
        if (v >= markerLen || !marker[v]) continue;
        float* n = normals + (size_t)v * 3;
        if (n[0] == 0.0f && n[1] == 0.0f && n[2] == 0.0f) {
            if (v > 0) { n[0] = n[-3]; n[1] = n[-2]; n[2] = n[-1]; }   // normals[v] = normals[v - 1], a struct copy
            else n[2] = 1.0f;
        }
        float len = Len(n[0], n[1], n[2]);
        float inv = R(1.0 / (D)len);
        n[0] = R((D)n[0] * inv); n[1] = R((D)n[1] * inv); n[2] = R((D)n[2] * inv);
        marker[v] = 0;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// DAZPhysicsMeshSoftVerticesGroup.MorphVerticesThreadedFast(Vector3[] verts): soft-body vertex moves into the skin's
// post-skin morphs. The sets run in list order, as in the C#, so later sets overwrite earlier ones the same way.
//
// Mathf.Pow is (float)Math.Pow, and Math.Pow is a Mono internal call into the C runtime linked inside mono.dll; the
// plugin hands us that same function (mono_lookup_internal_call), so the bits are the game's.
// ---------------------------------------------------------------------------------------------------------------

typedef double (*PowFn)(double, double);
static PowFn g_pow = nullptr;
CB_API void CB_SetPow(void* fn) { g_pow = (PowFn)fn; }
CB_API void CB_PowBatch(const float* f, const float* p, float* out, int n) {   // (float)Math.Pow(f, p), for tests
    for (int i = 0; i < n; i++) out[i] = g_pow ? R(g_pow((D)f[i], (D)p[i])) : 0.0f;
}

// One DAZPhysicsMeshSoftVerticesSet as the plugin gathers it (C# CpuBoostSoft.Set, explicit layout, 128 bytes).
struct SoftSet {
    int target, anchor, count, pad0;     // count = influenceVertices.Length, -1 when null
    const int* infl; float* dist; float* wts;
    int distLen, wtsLen;                 // -1 when null
    float pm[3], move[3];                // primaryMove, move (move is written back when the C# would set it)
    float lastPosT[3], curPos[3], lastKinT[3], curKin[3];
    float interp; int pad1;
};
static_assert(sizeof(SoftSet) == 128, "SoftSet layout");

struct SoftParams {
    int useInterp;               // V_0: _useSimulation && _useInterpolation && useCustomInterpolation
    int lookAtNormal;            // V_2: lookAtOption is VertexNormal / VertexNormalRefUp / VertexNormalAnchorUp
    int center;                  // centerBetweenTargetAndAnchor
    int influenceType;           // 0 Distance, 1 DistanceAlongMoveVector, 2 HardCopy
    int autoInfluenceAnchor;
    float maxInfluenceDistance, oneoverscale, falloffPower, weightBias, weightMultiplier;
};

static inline float Clamp01(float v) { if (v < 0.0f) return 0.0f; if (v > 1.0f) return 1.0f; return v; }

// Vector3.Lerp(a, b, t): t = Clamp01(t); a + (b - a) * t per component, one rounding.
static inline void Lerp(const float* a, const float* b, float t, float* o) {
    t = Clamp01(t);
    o[0] = R((D)a[0] + ((D)b[0] - (D)a[0]) * t);
    o[1] = R((D)a[1] + ((D)b[1] - (D)a[1]) * t);
    o[2] = R((D)a[2] + ((D)b[2] - (D)a[2]) * t);
}

static inline void Flag(uint8_t* flags, int v, int& changed) { if (!flags[v]) { flags[v] = 1; changed = 1; } }

CB_API int CB_SoftMorph(SoftSet* sets, int nSets, const SoftParams* p, const float* verts, int vertsLen,
                        uint8_t* psv, int psvLen, uint8_t* psnv, int psnvLen, float* psm, int psmLen, int* changedOut) {
    Count(kHistSoft);
    TaskTime timed(kTaskSoft);
    *changedOut = 0;
    if (nSets <= 0) return 0;
    if (vertsLen < 0 || psvLen < 0 || psmLen < 0) return 1;
    const bool hard = p->influenceType == 2;
    for (int s = 0; s < nSets; s++) {
        const SoftSet& S = sets[s];
        if (!In(S.target, vertsLen) || (p->center && !In(S.anchor, vertsLen))) return 2;
        if (S.count < 0) return 3;
        if (S.count > 0 && !hard) {
            if (S.distLen < S.count) return 4;
            for (int i = 0; i < S.count; i++) if (!In(S.infl[i], vertsLen)) return 5;
        }
        if (!In(S.anchor, psvLen) || !In(S.target, psvLen) || !In(S.target, psmLen)) return 6;
        if (p->lookAtNormal && !In(S.target, psnvLen)) return 7;
        if (hard) {
            if (p->autoInfluenceAnchor && !In(S.anchor, psmLen)) return 8;
        } else if (S.count > 0) {
            if (S.distLen < S.count || S.wtsLen < S.count || !g_pow) return 9;
        }
        for (int i = 0; i < S.count; i++) if (!In(S.infl[i], psvLen) || !In(S.infl[i], psmLen)) return 10;
    }

    int changed = 0;
    const float invMax = R(1.0 / (D)p->maxInfluenceDistance);                      // V_1
    const float scale = p->oneoverscale, wb = p->weightBias, wm = p->weightMultiplier, fp = p->falloffPower;
    for (int s = 0; s < nSets; s++) {
        SoftSet& S = sets[s];
        const float* vt = verts + (size_t)S.target * 3;
        float c[3];                                                                // V_5
        if (p->center) {
            const float* va = verts + (size_t)S.anchor * 3;
            float sx = R((D)vt[0] + va[0]), sy = R((D)vt[1] + va[1]), sz = R((D)vt[2] + va[2]);
            c[0] = R((D)sx * 0.5f); c[1] = R((D)sy * 0.5f); c[2] = R((D)sz * 0.5f);
        } else { c[0] = vt[0]; c[1] = vt[1]; c[2] = vt[2]; }

        if (S.count > 0 && !hard) {
            for (int i = 0; i < S.count; i++) {
                const float* vi = verts + (size_t)S.infl[i] * 3;
                float dx = R((D)R((D)vi[0] - c[0]) * scale);                       // (verts[iv] - V_5) * oneoverscale
                float dy = R((D)R((D)vi[1] - c[1]) * scale);
                float dz = R((D)R((D)vi[2] - c[2]) * scale);
                if (p->influenceType == 1) {
                    float dot = R(((D)S.pm[0] * dx + (D)S.pm[1] * dy) + (D)S.pm[2] * dz);
                    dx = R((D)S.pm[0] * dot); dy = R((D)S.pm[1] * dot); dz = R((D)S.pm[2] * dot);   // V_8
                }
                S.dist[i] = Len(dx, dy, dz);                                       // .magnitude
            }
        }
        Flag(psv, S.anchor, changed);
        if (p->lookAtNormal) Flag(psnv, S.target, changed);
        Flag(psv, S.target, changed);
        if (p->useInterp) {
            float a[3], b[3];
            Lerp(S.lastPosT, S.curPos, S.interp, a);                              // V_11
            Lerp(S.lastKinT, S.curKin, S.interp, b);                              // V_12
            S.move[0] = R((D)a[0] - b[0]); S.move[1] = R((D)a[1] - b[1]); S.move[2] = R((D)a[2] - b[2]);
        }
        float* mt = psm + (size_t)S.target * 3;
        mt[0] = S.move[0]; mt[1] = S.move[1]; mt[2] = S.move[2];
        if (hard) {
            if (p->autoInfluenceAnchor) {
                float* ma = psm + (size_t)S.anchor * 3;
                ma[0] = S.move[0]; ma[1] = S.move[1]; ma[2] = S.move[2];
            }
            for (int i = 0; i < S.count; i++) {
                int v = S.infl[i];
                Flag(psv, v, changed);
                float* m = psm + (size_t)v * 3;
                m[0] = S.move[0]; m[1] = S.move[1]; m[2] = S.move[2];
            }
        } else {
            for (int i = 0; i < S.count; i++) {
                int v = S.infl[i];
                float x = R((D)S.dist[i] * invMax);
                float f = 1.0f < x ? 1.0f : x;                                     // Mathf.Min(1f, x)
                float w = R(1.0 - (D)R(g_pow((D)f, (D)fp)));                       // 1f - Mathf.Pow(f, falloffPower)
                if (wb > 0.0f) w = R((1.0 - (D)wb) * w + wb);
                else w = R((1.0 + (D)wb) * w);
                S.wts[i] = w;
                Flag(psv, v, changed);
                float* m = psm + (size_t)v * 3;
                float mx = R((D)S.move[0] * w), my = R((D)S.move[1] * w), mz = R((D)S.move[2] * w);
                m[0] = R((D)mx * wm); m[1] = R((D)my * wm); m[2] = R((D)mz * wm);
            }
        }
    }
    *changedOut = changed;
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// DAZMorphBank.ApplyMorphsThreadedFast: the delta loops. The plugin defers each changed morph's loop
// (verts[v] += delta * (value - appliedValue), and the same into visibleNonPoseVerts for visible non-pose morphs) to
// the end of the call, which is safe because nothing else in the method reads those arrays, and hands the whole
// list here in the C# order. Each vertex is owned by one thread, which applies that vertex's deltas in list order,
// so every vertex sees the same additions in the same order as in the C#.
//
// DAZMorphVertex is a class: the native code reads the objects in place (VaM's Mono GC, Boehm, never moves objects).
// ---------------------------------------------------------------------------------------------------------------

struct MorphItem { void* const* slots; int count; float w; int visible; int pad; };   // C# CpuBoostMorph.Item
static_assert(sizeof(MorphItem) == 24, "MorphItem layout");

// verts[v] += delta * w for one component pair and one component, in the C#'s operand order (verts + (delta * w)):
// the same bits, and with two NaN operands the same NaN, as the game's SSE code.
static inline void AddDelta(float* p, const float* d, __m128d w2) {
    __m128d pxy = _mm_cvtps_pd(_mm_castpd_ps(_mm_load_sd((const double*)p)));
    __m128d dxy = _mm_cvtps_pd(_mm_castpd_ps(_mm_load_sd((const double*)d)));
    _mm_store_sd((double*)p, _mm_castps_pd(_mm_cvtpd_ps(_mm_add_pd(pxy, _mm_mul_pd(dxy, w2)))));
    __m128d pz = _mm_cvtss_sd(_mm_setzero_pd(), _mm_load_ss(p + 2));
    __m128d dz = _mm_cvtss_sd(_mm_setzero_pd(), _mm_load_ss(d + 2));
    _mm_store_ss(p + 2, _mm_cvtsd_ss(_mm_setzero_ps(), _mm_add_sd(pz, _mm_mul_sd(dz, w2))));
}

struct FlatChunk { int item, begin, end; };   // work unit of the first two passes, never crossing an item
struct Entry { int v; float d[3]; float w; };  // one delta in its owner's list; v's top bit: visible morph

struct MorphJob {
    const MorphItem* items; int nItems; const int* first;    // first[i]: flat index of item i's first delta
    const FlatChunk* chunks; int nChunks;
    int offVertex, offDelta;
    float* verts; int vertsLen; float* visible; int visibleLen;
    int* vtx;                                                  // per flat entry: vertex, -1 when the C# skips it
    int owners; const uint8_t* ownerOf;                        // vertex block (64 vertices) -> owning thread
    int* counts;                                               // [chunk * owners + owner]: entries, then list positions
    Entry* list;                                               // entries grouped by owner, each group in list order
    const int* ownerBegin;                                     // owners + 1
    std::atomic<int> error{0};
};

static const int kMorphChunk = 4096;   // deltas per chunk
static const int kMorphBlock = 6;      // a thread owns whole blocks of 64 vertices, blocks dealt out in turn

// Pass 1, per chunk: check every delta the way the C# would meet it, note its vertex, count each owner's entries.
static void MorphCheck(void* ctx, int begin, int end) {
    auto* j = (MorphJob*)ctx;
    for (int c = begin; c < end && !j->error.load(std::memory_order_relaxed); c++) {
        const FlatChunk& ch = j->chunks[c];
        const MorphItem& it = j->items[ch.item];
        int count[64] = {};   // counted here, stored once: neighbouring chunks' rows share cache lines
        int base = j->first[ch.item];
        for (int k = ch.begin; k < ch.end; k++) {
            const char* o = (const char*)it.slots[k];
            if (!o) { j->error.store(1); return; }                                // the C# throws NullReference
            int v = *(const int*)(o + j->offVertex);
            if (v < 0) { j->error.store(2); return; }                             // IndexOutOfRange
            if (it.visible && v < j->vertsLen && v >= j->visibleLen) { j->error.store(3); return; }
            if (v >= j->vertsLen) { j->vtx[base + k] = -1; continue; }            // the C# skips these
            j->vtx[base + k] = v;
            count[j->ownerOf[v >> kMorphBlock]]++;
        }
        std::memcpy(j->counts + (size_t)c * j->owners, count, sizeof(int) * j->owners);
    }
}

// Pass 2, per chunk: each delta into its owner's list. Chunks are in list order and so are their deltas, so every
// owner's list keeps the C#'s order.
static void MorphGroup(void* ctx, int begin, int end) {
    auto* j = (MorphJob*)ctx;
    for (int c = begin; c < end; c++) {
        const FlatChunk& ch = j->chunks[c];
        const MorphItem& it = j->items[ch.item];
        int pos[64];
        std::memcpy(pos, j->counts + (size_t)c * j->owners, sizeof(int) * j->owners);
        int base = j->first[ch.item], vis = it.visible ? (int)0x80000000 : 0;
        for (int k = ch.begin; k < ch.end; k++) {
            int v = j->vtx[base + k];
            if (v < 0) continue;
            const float* dd = (const float*)((const char*)it.slots[k] + j->offDelta);
            Entry& e = j->list[pos[j->ownerOf[v >> kMorphBlock]]++];
            e.v = v | vis; e.d[0] = dd[0]; e.d[1] = dd[1]; e.d[2] = dd[2]; e.w = it.w;
        }
    }
}

// Pass 3, per owner: its list in order.
static void MorphApply(void* ctx, int begin, int end) {
    auto* j = (MorphJob*)ctx;
    for (int o = begin; o < end; o++) {
        const Entry* e = j->list + j->ownerBegin[o];
        const Entry* stop = j->list + j->ownerBegin[o + 1];
        for (; e < stop; e++) {
            int v = e->v & 0x7fffffff;
            __m128d w2 = _mm_set1_pd((D)e->w);
            AddDelta(j->verts + (size_t)v * 3, e->d, w2);
            if (e->v < 0) AddDelta(j->visible + (size_t)v * 3, e->d, w2);
        }
    }
}

// One thread: after the check pass, the deltas in list order straight from the objects.
static void MorphApplyInOrder(MorphJob* j) {
    for (int i = 0; i < j->nItems; i++) {
        const MorphItem& it = j->items[i];
        const int* vtx = j->vtx + j->first[i];
        __m128d w2 = _mm_set1_pd((D)it.w);
        for (int k = 0; k < it.count; k++) {
            int v = vtx[k];
            if (v < 0) continue;
            const float* d = (const float*)((const char*)it.slots[k] + j->offDelta);
            AddDelta(j->verts + (size_t)v * 3, d, w2);
            if (it.visible) AddDelta(j->visible + (size_t)v * 3, d, w2);
        }
    }
}

static double g_morphTimes[2];

// Returns 0 when applied; 1-4 (nothing written) for a delta the C# would throw on, or a list too long.
CB_API int CB_MorphApply(const MorphItem* items, int nItems, float* verts, int vertsLen, float* visible, int visibleLen,
                         int offVertex, int offDelta) {
    if (nItems <= 0) return 0;
    Count(kHistMorph);
    thread_local std::vector<int> first, vtx, counts, ownerBegin;
    thread_local std::vector<Entry> list;
    thread_local std::vector<FlatChunk> chunks;
    thread_local std::vector<uint8_t> ownerOf;
    first.resize(nItems);
    chunks.clear();
    int64_t total = 0;
    for (int i = 0; i < nItems; i++) {
        first[i] = (int)total;
        for (int b = 0; b < items[i].count; b += kMorphChunk)
            chunks.push_back(FlatChunk{ i, b, b + kMorphChunk < items[i].count ? b + kMorphChunk : items[i].count });
        total += items[i].count;
        if (total > 0x7fffffff / 3) return 4;
    }
    if (total == 0) return 0;
    int threads = g_pool.Size();
    int owners = (total >= 8192 && threads > 0) ? (threads + 1 < 64 ? threads + 1 : 64) : 1;
    int blocks = (vertsLen >> kMorphBlock) + 1;
    ownerOf.resize(blocks);
    for (int b = 0, o = 0; b < blocks; b++) { ownerOf[b] = (uint8_t)o; if (++o == owners) o = 0; }
    vtx.resize((size_t)total);
    counts.resize(chunks.size() * owners);

    MorphJob job;
    job.items = items; job.nItems = nItems; job.first = first.data();
    job.chunks = chunks.data(); job.nChunks = (int)chunks.size();
    job.offVertex = offVertex; job.offDelta = offDelta;
    job.verts = verts; job.vertsLen = vertsLen; job.visible = visible; job.visibleLen = visibleLen;
    job.vtx = vtx.data();
    job.owners = owners; job.ownerOf = ownerOf.data();
    job.counts = counts.data();

    LARGE_INTEGER t0, t1, t2; QueryPerformanceCounter(&t0);
    g_pool.Run(kTaskMorph, MorphCheck, &job, 0, job.nChunks, 1);
    if (int e = job.error.load()) return e;
    if (owners == 1) {
        QueryPerformanceCounter(&t1);
        MorphApplyInOrder(&job);
    } else {
        // Owner o's list starts after all lower owners' entries; within it, chunk c's entries follow chunk c-1's.
        ownerBegin.resize(owners + 1);
        int pos = 0;
        for (int o = 0; o < owners; o++) {
            ownerBegin[o] = pos;
            for (int c = 0; c < job.nChunks; c++) { int n = counts[(size_t)c * owners + o]; counts[(size_t)c * owners + o] = pos; pos += n; }
        }
        ownerBegin[owners] = pos;
        list.resize((size_t)pos > list.size() ? (size_t)pos : list.size());
        job.list = list.data(); job.ownerBegin = ownerBegin.data();
        g_pool.Run(kTaskMorph, MorphGroup, &job, 0, job.nChunks, 1);
        QueryPerformanceCounter(&t1);
        g_pool.Run(kTaskMorph, MorphApply, &job, 0, owners, 1);
    }
    QueryPerformanceCounter(&t2);
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    g_morphTimes[0] = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    g_morphTimes[1] = (t2.QuadPart - t1.QuadPart) * 1000.0 / f.QuadPart;
    return 0;
}
CB_API void CB_MorphTimes(double* out) { out[0] = g_morphTimes[0]; out[1] = g_morphTimes[1]; }

// Diagnostics: n pool jobs of `chunks` empty chunks each; returns ms per job.
static void NoWork(void*, int, int) {}
CB_API double CB_PoolPing(int n, int chunks) {
    LARGE_INTEGER t0, t1, f; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    for (int i = 0; i < n; i++) g_pool.Run(kTaskOther, NoWork, nullptr, 0, chunks, 1);
    QueryPerformanceCounter(&t1);
    return (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / n;
}
