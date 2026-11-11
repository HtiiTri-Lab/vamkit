// Sampler replacement after fholger/vrperfkit (MIT), d3d11_post_processor.cpp.
// Unlike vrperfkit, every filtering sampler that can reach a lower mip is
// biased (VaM's textures are not necessarily anisotropic), comparison and
// min/max samplers are not, and the original sampler is held while its copy
// is in use, so a released address can never map to another's copy.
#include "stdafx.h"
#include "dx11_vam_mip_bias.h"
#include <detours.h>
#include <TlHelp32.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace {
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
using SetSamplers = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*);
SetSamplers originalSamplers = nullptr;

struct State {
    Ptr<ID3D11Device> device;
    ID3D11DeviceContext* context = nullptr;
    // Set by the compositor, read by the hook (both on the immediate context's thread).
    std::atomic<float> wanted{0.f};
    float bias = 0.f;
    struct Copy { Ptr<ID3D11SamplerState> original, biased; };
    // Null `biased`: the game's sampler is left as it is.
    std::unordered_map<ID3D11SamplerState*, Copy> copies;
    uint64_t replaced = 0;
    bool failed = false;
};
State* state = nullptr;

ID3D11SamplerState* Replacement(State& s, ID3D11SamplerState* original) {
    auto found = s.copies.find(original);
    if (found != s.copies.end()) return found->second.biased ? found->second.biased.Get() : original;
    D3D11_SAMPLER_DESC desc{};
    original->GetDesc(&desc);
    State::Copy& copy = s.copies[original];
    copy.original = original;
    // Comparison (shadow) and min/max filters, samplers with a bias of their
    // own and ones that never leave mip 0 are not the game's textures.
    const bool reduction = (desc.Filter & 0x180) != 0;
    if (reduction || desc.MipLODBias != 0.f || desc.MaxLOD < 1.f) return original;
    desc.MipLODBias = s.bias;
    if (FAILED(s.device->CreateSamplerState(&desc, &copy.biased))) {
        copy.biased.Reset();
        return original;
    }
    ++s.replaced;
    return copy.biased.Get();
}

void STDMETHODCALLTYPE HookSamplers(ID3D11DeviceContext* context, UINT start, UINT count, ID3D11SamplerState* const* samplers) {
    State* s = state;
    if (!s || context != s->context || s->failed || !samplers || count == 0 || count > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        originalSamplers(context, start, count, samplers);
        return;
    }
    const float wanted = s->wanted.load(std::memory_order_relaxed);
    if (wanted != s->bias) {
        // New render scale (or DLSS SR off): new copies from here on.
        s->copies.clear();
        s->bias = wanted;
        static int changes = 0;
        if (++changes <= 40 || changes % 200 == 0)
            OOVR_LOGF("VAM DLSS mip bias %.2f for the game's textures (change %d)", wanted, changes);
    }
    if (s->bias == 0.f) {
        originalSamplers(context, start, count, samplers);
        return;
    }
    ID3D11SamplerState* swapped[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
    try {
        for (UINT i = 0; i < count; ++i) swapped[i] = samplers[i] ? Replacement(*s, samplers[i]) : nullptr;
    } catch (...) {
        s->failed = true;
        OOVR_LOG("VAM DLSS mip bias off for this run: sampler replacement failed");
        originalSamplers(context, start, count, samplers);
        return;
    }
    originalSamplers(context, start, count, swapped);
}

void InstallHook(ID3D11DeviceContext* context) {
    void** table = *reinterpret_cast<void***>(context);
    originalSamplers = reinterpret_cast<SetSamplers>(table[10]); // ID3D11DeviceContext::PSSetSamplers
    // Collect handles before suspending threads (see dx11_vam_ffr.cpp).
    std::vector<HANDLE> threads;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{sizeof(entry)};
        for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId()) continue;
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
            if (thread) threads.push_back(thread);
        }
        CloseHandle(snapshot);
    }
    if (DetourTransactionBegin() != NO_ERROR) {
        for (HANDLE thread : threads) CloseHandle(thread);
        throw std::runtime_error("cannot begin mip bias hook transaction");
    }
    LONG error = DetourUpdateThread(GetCurrentThread());
    for (HANDLE thread : threads) {
        LONG result = DetourUpdateThread(thread);
        if (result && result != ERROR_INVALID_HANDLE) error = result;
    }
    if (!error) error = DetourAttach(reinterpret_cast<PVOID*>(&originalSamplers), HookSamplers);
    if (error) DetourTransactionAbort(); else error = DetourTransactionCommit();
    for (HANDLE thread : threads) CloseHandle(thread);
    if (error) throw std::runtime_error("mip bias D3D11 hook installation failed");
}
}

void VamMipBias::Install(ID3D11Device* device) {
    static std::mutex mutex;
    static bool attempted = false;
    std::lock_guard lock(mutex);
    if (attempted) return;
    attempted = true;
    try {
        auto* s = new State;
        s->device = device;
        Ptr<ID3D11DeviceContext> immediate;
        device->GetImmediateContext(&immediate);
        s->context = immediate.Get(); // the device holds it for the process
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&HookSamplers), &pinned)) {
            delete s;
            throw std::runtime_error("cannot keep the mip bias hook module loaded");
        }
        state = s;
        try { InstallHook(s->context); }
        catch (...) { state = nullptr; delete s; throw; }
        OOVR_LOG("VAM DLSS mip bias: PSSetSamplers hooked; the game's textures get (log2(render/output) - 1) x DLSSMipBias% while DLSS SR upscales");
    } catch (const std::exception& e) {
        OOVR_LOGF("VAM DLSS mip bias unavailable: %s", e.what());
    }
}

void VamMipBias::Set(float bias) {
    if (State* s = state) s->wanted.store(bias, std::memory_order_relaxed);
}
