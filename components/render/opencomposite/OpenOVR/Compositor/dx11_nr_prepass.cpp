// NR parameter integration adapted from mbucchia/VirtualDesktop-OpenXR,
// commit 925dc598c524cb48b37fa4b0a2bff471f75fc5c4, dlssnr.cpp.
// MIT License, Copyright(c) 2026 Matthieu Bucchianeri.
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "stdafx.h"
#include "../../../../shading/rt-core/include/startup_display.h"
#include "dx11_nr_prepass.h"
#include "dx11_vam_dlss.h"
#include "vam_performance_config.h"
#include "vam_nr_region.h"
#include "vam_on12.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <nvsdk_ngx.h>
#include <detours.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace {
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};
void CheckHr(HRESULT hr, const char* call) {
    if (FAILED(hr)) {
        char detail[512];
        snprintf(detail, sizeof(detail), "%s: HRESULT 0x%08lx", call, static_cast<unsigned long>(hr));
        throw std::runtime_error(detail);
    }
}
void CheckNgx(NVSDK_NGX_Result result, const char* call) {
    if (NVSDK_NGX_FAILED(result)) {
        char detail[512];
        snprintf(detail, sizeof(detail), "%s: NGX 0x%08x", call, static_cast<unsigned>(result));
        throw std::runtime_error(detail);
    }
}
#define NR_HR(call) CheckHr((call), #call)
#define NR_NGX(call) CheckNgx((call), #call)

std::wstring Environment(const wchar_t* key) {
    wchar_t value[32768]{};
    DWORD count = GetEnvironmentVariableW(key, value, static_cast<DWORD>(std::size(value)));
    if (count >= std::size(value)) throw std::runtime_error("NR environment path is too long");
    return count ? std::wstring(value, count) : std::wstring();
}

// The author's experimental NR build needs this path even with a signed NR
// DLL: a driver NGX core can reject Reserved18 before loading the feature DLL.
// Keep both modules resident once hooked; the callback must not outlive OC.
std::atomic<HMODULE> manualNrModule{nullptr};
auto originalGetModuleFileNameW = &::GetModuleFileNameW;
DWORD WINAPI NrGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size) {
    HMODULE caller = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCSTR>(_ReturnAddress()), &caller);
    const auto nrModule = manualNrModule.load(std::memory_order_acquire);
    if (nrModule && caller == nrModule) {
        // Match the experimental VDXR compatibility shim; all other callers
        // still receive the actual module filename from Windows.
        if (!filename || !size) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
        constexpr wchar_t ngxName[] = L"nvngx.dll";
        wcsncpy_s(filename, size, ngxName, _TRUNCATE);
        if (size < std::size(ngxName)) SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return size;
    }
    return originalGetModuleFileNameW(module, filename, size);
}

HMODULE LoadManualNr() {
    static std::mutex loadMutex;
    static bool hooked = false;
    std::lock_guard lock(loadMutex);
    if (auto module = manualNrModule.load(std::memory_order_acquire)) return module;
    HMODULE pinnedOc = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&NrGetModuleFileNameW), &pinnedOc))
        NR_HR(HRESULT_FROM_WIN32(GetLastError()));
    if (!hooked) {
        NR_HR(HRESULT_FROM_WIN32(DetourTransactionBegin()));
        LONG result = DetourUpdateThread(GetCurrentThread());
        if (result == NO_ERROR) result = DetourAttach(reinterpret_cast<PVOID*>(&originalGetModuleFileNameW), NrGetModuleFileNameW);
        if (result != NO_ERROR) {
            DetourTransactionAbort();
            NR_HR(HRESULT_FROM_WIN32(result));
        }
        NR_HR(HRESULT_FROM_WIN32(DetourTransactionCommit()));
        hooked = true;
    }
    const auto path = Environment(L"VAM_NR_PREPASS_PATH") + L"\\nvngx_dlssnr.dll";
    HMODULE loaded = LoadLibraryW(path.c_str());
    if (!loaded) NR_HR(HRESULT_FROM_WIN32(GetLastError()));
    // Retain the LoadLibrary reference for the lifetime of the hook.
    manualNrModule.store(loaded, std::memory_order_release);
    return loaded;
}

template<class T> T NrExport(HMODULE module, const char* name) {
    auto symbol = GetProcAddress(module, name);
    if (!symbol) throw std::runtime_error(std::string("Missing NR export: ") + name);
    return reinterpret_cast<T>(symbol);
}

void WriteFailure(const char* message) noexcept {
    try {
        const auto path = Environment(L"VAM_NR_PREPASS_LOG") + L"\\failure.txt";
        Handle file;
        file.value = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file.value == INVALID_HANDLE_VALUE) { file.value = nullptr; return; }
        DWORD written = 0;
        WriteFile(file.value, message, static_cast<DWORD>(strlen(message)), &written, nullptr);
    } catch (...) {}
}

struct Settings {
    bool enabled = true;
    int style = 0;
    // DLSSNR.Hint.Render.Preset, a creation parameter ([NR] RenderPreset): 0 =
    // the DLL's default (310.9.16: its new model), 1 = the 310.8.0 model (same
    // output, same cost), 2 = 310.9.16's new model.
    int preset = 0;
    float intensity = 1.f, tone = .2f, structure = .7f, skin = .5f, scale = .66f, feather = .10f;
    // [NR] RegionShiftY, -30..30 percent of the eye image's height: the region
    // moves up by it (negative: down) from the projection centre.
    float shiftY = 0.f;
    bool useDepth = true, useMotion = true; // bridge guides, only when the DLSS launcher provides them
    // [NR] PackEyes (default on, 0 = each eye on its own): both eyes' regions in one image, one evaluation (ProcessPair).
    bool packEyes = true;
};
Settings ReadSettings() {
    Settings s;
    const auto performance = ReadVamPerformanceSettings();
    s.feather = performance.nrFeather;
    s.useDepth = performance.nrDepth;
    s.useMotion = performance.nrMotion;
    HKEY key = nullptr;
    RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Virtual Desktop, Inc.\\OpenXR", 0,
                 KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    auto get = [key](const wchar_t* name, DWORD fallback) {
        DWORD value = fallback, size = sizeof(value);
        return key && RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS ? value : fallback;
    };
    s.enabled = get(L"DLSSNR_Enabled", 1) != 0;
    s.style = static_cast<int>(get(L"DLSSNR_Style", 0));
    s.intensity = std::clamp(get(L"DLSSNR_Intensity", 100) / 100.f, 0.f, 1.f);
    s.tone = std::clamp(get(L"DLSSNR_LocalToneStrength", 20) / 100.f, 0.f, 2.f);
    s.structure = std::clamp(get(L"DLSSNR_LocalStructureStrength", 70) / 100.f, 0.f, 2.f);
    s.skin = std::clamp(get(L"DLSSNR_SkinStructureStrength", 50) / 100.f, 0.f, 2.f);
    s.scale = std::clamp(get(L"FoveationSize", 66) / 100.f, .1f, 1.f);
    if (key) RegCloseKey(key);
    // The launch panel owns only its per-run INI. Existing entry points have
    // no [NR] section and continue to use the original registry controls.
    const auto profile = VamPerformanceConfigPath();
    if (!profile.empty()) {
        auto local = [&](const wchar_t* name, int fallback, int low, int high) {
            return std::clamp(static_cast<int>(GetPrivateProfileIntW(L"NR", name, fallback, profile.c_str())), low, high);
        };
        auto percent = [](float v) { return static_cast<int>(std::lround(v * 100.f)); };
        s.enabled = local(L"Enabled", s.enabled ? 1 : 0, 0, 1) != 0;
        s.style = local(L"Style", s.style, 0, 2);
        s.preset = local(L"RenderPreset", s.preset, 0, 2);
        s.intensity = local(L"Intensity", percent(s.intensity), 0, 100) / 100.f;
        s.tone = local(L"LocalToneStrength", percent(s.tone), 0, 200) / 100.f;
        s.structure = local(L"LocalStructureStrength", percent(s.structure), 0, 200) / 100.f;
        s.skin = local(L"SkinStructureStrength", percent(s.skin), 0, 200) / 100.f;
        s.scale = local(L"FoveationSize", percent(s.scale), 10, 100) / 100.f;
        s.packEyes = local(L"PackEyes", s.packEyes ? 1 : 0, 0, 1) != 0;
        // GetPrivateProfileInt returns 0 for negative values: parse the text.
        wchar_t shift[16]{};
        GetPrivateProfileStringW(L"NR", L"RegionShiftY", L"0", shift, _countof(shift), profile.c_str());
        s.shiftY = std::clamp(static_cast<int>(wcstol(shift, nullptr, 10)), -30, 30) / 100.f;
    }
    return s;
}

// PackEyes: black columns between the two eyes' regions. NR's answer changes
// with the canvas width in steps, so this is the gap measured off-line for the
// region's width (first study, 2026-10-23: L33 608
// wide good at 80-112, M50 916 wide at 0-128; second study, 2026-11-02, NR after
// the upscaler at 75 % of a 3072-wide output, 80 % region: 1844 wide good on a
// canvas of 3688-3704, off at 3720-3832); -1 = other widths, not packed.
int PackGap(UINT regionWidth) {
    if (regionWidth >= 1840 && regionWidth <= 1852) return 3704 - 2 * static_cast<int>(regionWidth);
    return regionWidth <= 760 ? 96 : regionWidth <= 1100 ? 64 : -1;
}

DXGI_FORMAT ResourceFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    default: throw std::runtime_error("NR prepass requires RGBA8 or RGBA16F eye images");
    }
}

// The nasolabial band ([NR] NasolabialReduction, VamFrameGuides::nrControl):
// NR's result is blended back toward the image it was given by the band's
// keep value (G = 1 - cut * band, any size, read at the scaled pixel). NR's
// visible effect is its local tone and structure (incl. skin structure):
// off-line, a mask with both at 0 left the image within 0.5 of the input,
// and this blend came within 0.4-0.6 of NR run with all three x 0.4. NR
// itself never gets a ControlMask: any mask turns its own skin mask off
// (bit-identical to UseAutoMask=0), and SkinStructureStrength with it.
//
// featherPixels 0: the region is the whole image, no feather. useKeep: the
// nasolabial band's keep value (G) scales NR's weight.
constexpr char featherShader[] = R"(
Texture2D<float4> originalImage : register(t0);
Texture2D<float4> neuralImage : register(t1);
Texture2D<float4> keepMask : register(t2);
cbuffer Region : register(b0) { int2 base; int2 size; float featherPixels; uint useKeep; float2 keepScale; };
float4 vertexMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
}
float4 pixelMain(float4 position : SV_Position) : SV_Target {
    int2 pixel = int2(position.xy);
    int2 local = pixel - base;
    float2 edge = min(local, size - 1 - local);
    float2 t = featherPixels > 0.0 ? saturate(edge / max(featherPixels, 1.0)) : 1.0;
    // Quintic ramps have zero first AND second derivatives at both ends.
    // Multiplying the axes softens corners without min-distance diagonals.
    float2 ramp = t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
    float nrWeight = ramp.x * ramp.y;
    if (useKeep != 0 && nrWeight > 0.0) {
        uint kw, kh;
        keepMask.GetDimensions(kw, kh);
        int2 k = min(int2((float2(pixel) + 0.5) * keepScale), int2(kw, kh) - 1);
        nrWeight *= saturate(keepMask.Load(int3(k, 0)).g);
    }
    float4 original = originalImage.Load(int3(pixel, 0));
    // The outermost pixels must exactly match the untouched original, even
    // if the neural output has invalid values at the crop boundary.
    if (nrWeight <= 0.0) return original;
    float3 neural = neuralImage.Load(int3(pixel, 0)).rgb;
    // Both SRVs and the RTV use the same color interpretation; sRGB views
    // therefore blend in linear light and encode once on the target write.
    return float4(lerp(original.rgb, neural, nrWeight), original.a);
}
)";
struct FeatherConstants { int x, y, w, h; float pixels; uint32_t useKeep; float keepScaleX, keepScaleY; };
static_assert(sizeof(FeatherConstants) == 32);
}

struct VamNrPrepass::Impl {
    explicit Impl(ID3D11Device* device) : device11(device) {}
    Ptr<ID3D11Device> device11;
    Ptr<ID3D11Device5> device5;
    Ptr<ID3D11DeviceContext4> context11;
    Ptr<ID3D12Device> device12;
    Ptr<ID3D12CommandQueue> queue;
    Ptr<ID3D12Fence> inputFence12, outputFence12;
    Ptr<ID3D11Fence> inputFence11, outputFence11;
    Handle completionEvent;
    UINT64 inputValue = 0, outputValue = 0;
    NVSDK_NGX_Parameter* parameters = nullptr;
    decltype(&NVSDK_NGX_D3D12_CreateFeature) createFeature = &NVSDK_NGX_D3D12_CreateFeature;
    decltype(&NVSDK_NGX_D3D12_EvaluateFeature) evaluateFeature = &NVSDK_NGX_D3D12_EvaluateFeature;
    decltype(&NVSDK_NGX_D3D12_ReleaseFeature) releaseFeature = &NVSDK_NGX_D3D12_ReleaseFeature;
    bool manualLoad = false;
    // NGX D3D12 belongs to OFXR DLSS Frame Generation in this process: NR
    // then loads its own DLL directly, as its compatibility path does.
    bool sharedNgx = false;
    bool failed = false;
    // NGX did not come up: a property of the machine (a card without RTX), not a resource failure. No retry.
    bool unavailable = false;
    ULONGLONG retryAt=0;
    bool initialized = false;
    bool deviceReady = false;
    // D3D11On12 (vam_on12.h): `queue` is the 11on12 queue itself. NR's simultaneous-access textures stay as they are
    // (the path that already ran under 11on12); its list is executed after a Flush of the D3D11 context, in order
    // with the frame, instead of the fence round trip between two queues (~0.4 ms of GPU per eye).
    VamOn12 on12;
    std::mutex mutex;
    Settings settings;
    ULONGLONG nextSettingsRead = 0;
    VamProfileStamp profileSeen;           // ReadSettings again only when the profile changed
    bool profileRead = false;

    struct Eye {
        Ptr<ID3D11Texture2D> input11, output11;
        Ptr<ID3D12Resource> input12, output12;
        Ptr<ID3D11ShaderResourceView> originalSrv, neuralSrv;
        NVSDK_NGX_Handle* feature = nullptr;
        // Optional bridge guides (render-size depth + jittered motion), shared to D3D12.
        Ptr<ID3D11Texture2D> depth11, motion11;
        Ptr<ID3D12Resource> depth12, motion12;
        // The nasolabial band's keep mask (render size even for NR after the
        // upscaler), read by the composite; the D3D11 path's view of it.
        Ptr<ID3D11Texture2D> keepTexture;
        Ptr<ID3D11ShaderResourceView> keepSrv;
        bool keepLogged = false;
        bool featureWithDepth = false;
        int featurePreset = 0;
        UINT width = 0, height = 0, rw = 0, rh = 0, x = 0, y = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        bool reset = true;
        ULONGLONG lastSubmit = 0;
        uint64_t evaluations = 0;
        float loggedFeather = -1.f;
    };
    std::array<Eye, 2> eyes;
    // v23: the previous size's eye (feature + textures) is kept, one per eye,
    // so the size change back is a swap: VaM's menus (native frames) and the
    // normal render size each keep their NR feature. Recreating one cost a
    // 60-170 ms frame per eye at the toggle and, ~10 s later, one frame with
    // 20-38 ms more NR GPU time (both NR 310.8.0 and 310.9.16).
    std::array<Eye, 2> spare;
    // PackEyes: the canvas (width = rw = 2 * an eye's rw + gap, x = y = 0) with
    // its own feature; D3D12-only textures, eye regions copied in and out.
    Eye packed;
    UINT packedGap = 0;
    bool packRefusedLogged = false;
    bool packing = false;   // the last evaluation was packed (logged on every switch)
    struct Commands {
        Ptr<ID3D12CommandAllocator> allocator;
        Ptr<ID3D12GraphicsCommandList> list;
        UINT64 completed = 0;
    };
    std::array<Commands, 4> commands;
    unsigned nextCommands = 0;
    Ptr<ID3DDeviceContextState> privateState;
    Ptr<ID3D11VertexShader> featherVs;
    Ptr<ID3D11PixelShader> featherPs;
    Ptr<ID3D11Buffer> featherConstants;
    Ptr<ID3D11RasterizerState> featherRaster;
    Ptr<ID3D11DepthStencilState> featherDepth;

    void WaitCpu(ID3D12Fence* fence, UINT64 value) {
        if (!value) return;
        UINT64 completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX) throw std::runtime_error("NR GPU device removed");
        if (completed >= value) return;
        NR_HR(fence->SetEventOnCompletion(value, completionEvent.value));
        if (WaitForSingleObject(completionEvent.value, 10000) != WAIT_OBJECT_0)
            throw std::runtime_error("NR GPU completion timed out");
        if (fence->GetCompletedValue() == UINT64_MAX) throw std::runtime_error("NR GPU device removed");
    }

    void Drain() {
        if (!deviceReady) return;
        // Also covers D3D11's output copy and feather, not only NR completion.
        if (on12) {
            context11->Flush();
            NR_HR(queue->Signal(outputFence12.Get(), ++outputValue));
            WaitCpu(outputFence12.Get(), outputValue);
            return;
        }
        NR_HR(context11->Signal(inputFence11.Get(), ++inputValue));
        context11->Flush();
        WaitCpu(inputFence12.Get(), inputValue);
    }

    ~Impl() {
        try { Drain(); DrainQueue(); } catch (const std::exception& e) { OOVR_LOGF("NR prepass cleanup: %s", e.what()); }
        for (auto& eye : eyes) if (eye.feature) releaseFeature(eye.feature);
        for (auto& eye : spare) if (eye.feature) releaseFeature(eye.feature);
        if (packed.feature) releaseFeature(packed.feature);
        if (parameters) NVSDK_NGX_D3D12_DestroyParameters(parameters);
        // No global NGX shutdown: other layers may have their own NGX features.
    }

    void EnableManualLoad() {
        const auto module = LoadManualNr();
        using InitExt = NVSDK_NGX_Result (NVSDK_CONV *)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
        const auto init = NrExport<InitExt>(module, "NVSDK_NGX_D3D12_Init_Ext");
        const auto create = NrExport<decltype(createFeature)>(module, "NVSDK_NGX_D3D12_CreateFeature");
        const auto evaluate = NrExport<decltype(evaluateFeature)>(module, "NVSDK_NGX_D3D12_EvaluateFeature");
        const auto release = NrExport<decltype(releaseFeature)>(module, "NVSDK_NGX_D3D12_ReleaseFeature");
        const auto directory = Environment(L"VAM_NR_PREPASS_LOG");
        NR_NGX(init(123456, directory.c_str(), device12.Get(), NVSDK_NGX_Version_API, nullptr));
        createFeature = create; evaluateFeature = evaluate; releaseFeature = release;
        manualLoad = true;
        OOVR_LOG("NR prepass: using the author's direct DLL compatibility initialization");
    }

    // Everything on the CPU side of the queue finished: before releasing
    // what work on it may still be using.
    void DrainQueue() {
        if (!deviceReady) return;
        NR_HR(queue->Signal(outputFence12.Get(), ++outputValue));
        WaitCpu(outputFence12.Get(), outputValue);
    }

    void InitializeDevice() {
        if (deviceReady) return;
        NR_HR(device11.As(&device5));
        Ptr<ID3D11DeviceContext> immediate;
        device11->GetImmediateContext(&immediate);
        NR_HR(immediate.As(&context11));
        on12 = VamOn12For(device11.Get());
        if (on12) {
            device12 = on12.device;
            queue = on12.queue;
            // Only for the CPU: when a command allocator may be reset, and draining before releases.
            NR_HR(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&outputFence12)));
            OOVR_LOG("NR prepass: Unity's device is D3D11On12; NR runs on its queue in order with the frame (no fences, no shared textures)");
        } else {
            Ptr<IDXGIDevice> dxgi;
            Ptr<IDXGIAdapter> adapter;
            NR_HR(device11.As(&dxgi));
            NR_HR(dxgi->GetAdapter(&adapter));
            NR_HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12)));
            D3D12_COMMAND_QUEUE_DESC q{};
            q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            NR_HR(device12->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)));
            NR_HR(device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&inputFence12)));
            NR_HR(device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&outputFence12)));
            Handle inHandle, outHandle;
            NR_HR(device12->CreateSharedHandle(inputFence12.Get(), nullptr, GENERIC_ALL, nullptr, &inHandle.value));
            NR_HR(device12->CreateSharedHandle(outputFence12.Get(), nullptr, GENERIC_ALL, nullptr, &outHandle.value));
            NR_HR(device5->OpenSharedFence(inHandle.value, IID_PPV_ARGS(&inputFence11)));
            NR_HR(device5->OpenSharedFence(outHandle.value, IID_PPV_ARGS(&outputFence11)));
        }
        completionEvent.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!completionEvent.value) NR_HR(HRESULT_FROM_WIN32(GetLastError()));

        const auto directory = Environment(L"VAM_NR_PREPASS_PATH");
        const auto logDirectory = Environment(L"VAM_NR_PREPASS_LOG");
        if (directory.empty() || logDirectory.empty()) throw std::runtime_error("Use the NR prepass launcher to set NGX paths");
        unavailable = true;
        // A test of the path a card without RTX takes, on a card that has it (the same switch as DLSS SR's).
        if (Environment(L"VAMKIT_NO_DLSS") == L"1") throw std::runtime_error("DLSS is switched off for this run (VAMKIT_NO_DLSS=1)");
        // DLSS Frame Generation first: NGX keeps the first D3D12 setup of the
        // process, and this one's path has no nvngx_dlssg.dll.
        sharedNgx = VamDlss::PrepareFrameGenerationNgx();
        if (sharedNgx) OOVR_LOG("NR prepass: NGX D3D12 is set up by OFXR DLSS Frame Generation; NR uses the author's direct DLL loader");
        const wchar_t* paths[] = { directory.c_str() };
        NVSDK_NGX_FeatureCommonInfo common{};
        common.PathListInfo.Path = paths;
        common.PathListInfo.Length = 1;
        common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
        NR_NGX(NVSDK_NGX_D3D12_Init(123456, logDirectory.c_str(), device12.Get(), &common));
        NR_NGX(NVSDK_NGX_D3D12_GetCapabilityParameters(&parameters));
        unavailable = false;
        deviceReady = true;
    }

    void Initialize() {
        InitializeDevice();

        D3D_FEATURE_LEVEL level = device11->GetFeatureLevel();
        UINT flags = (device11->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
            ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
        NR_HR(device5->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &privateState));
        Ptr<ID3DBlob> vs, ps, errors;
        NR_HR(D3DCompile(featherShader, strlen(featherShader), nullptr, nullptr, nullptr, "vertexMain", "vs_5_0",
                         D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors));
        errors.Reset();
        NR_HR(D3DCompile(featherShader, strlen(featherShader), nullptr, nullptr, nullptr, "pixelMain", "ps_5_0",
                         D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors));
        NR_HR(device11->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &featherVs));
        NR_HR(device11->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &featherPs));
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(FeatherConstants);
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        NR_HR(device11->CreateBuffer(&cb, nullptr, &featherConstants));
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        raster.ScissorEnable = TRUE;
        NR_HR(device11->CreateRasterizerState(&raster, &featherRaster));
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        depth.StencilEnable = FALSE;
        depth.StencilReadMask = depth.StencilWriteMask = D3D11_DEFAULT_STENCIL_READ_MASK;
        depth.FrontFace.StencilFailOp = depth.FrontFace.StencilDepthFailOp = depth.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        depth.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        depth.BackFace = depth.FrontFace;
        NR_HR(device11->CreateDepthStencilState(&depth, &featherDepth));
        initialized = true;
        OOVR_LOG("NR prepass initialized: real eye images -> central NR + feather -> OFXR history; no generated-frame NR");
    }

    void SharedTexture(UINT width, UINT height, DXGI_FORMAT format, bool output,
                       Ptr<ID3D12Resource>& resource12, Ptr<ID3D11Texture2D>& texture11) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width; desc.Height = height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc = {1, 0};
        desc.Format = ResourceFormat(format);
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (output) desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        NR_HR(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource12)));
        Handle shared;
        NR_HR(device12->CreateSharedHandle(resource12.Get(), nullptr, GENERIC_ALL, nullptr, &shared.value));
        NR_HR(device5->OpenSharedResource1(shared.value, IID_PPV_ARGS(&texture11)));
    }

    void SharedGuide(UINT width, UINT height, DXGI_FORMAT format, Ptr<ID3D12Resource>& resource12, Ptr<ID3D11Texture2D>& texture11) {
        if (resource12) return;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width; desc.Height = height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc = {1, 0};
        desc.Format = format;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        NR_HR(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource12)));
        Handle shared;
        NR_HR(device12->CreateSharedHandle(resource12.Get(), nullptr, GENERIC_ALL, nullptr, &shared.value));
        NR_HR(device5->OpenSharedResource1(shared.value, IID_PPV_ARGS(&texture11)));
    }

    static bool GuideMatches(ID3D11Texture2D* guide, UINT width, UINT height) {
        if (!guide) return false;
        D3D11_TEXTURE2D_DESC desc{};
        guide->GetDesc(&desc);
        return desc.Width == width && desc.Height == height;
    }

    void EnsureEye(Eye& eye, const D3D11_TEXTURE2D_DESC& desc, DXGI_FORMAT format, unsigned eyeIndex) {
        XrFovf fov{};
        if (xr_gbl) {
            const auto views = xr_gbl->GetCachedViews(xr_gbl->seatedSpace);
            if (eyeIndex < views.viewCount) fov = views.views[eyeIndex].fov;
        }
        const auto region = CalculateVamNrRegion(desc.Width, desc.Height, settings.scale, fov, settings.shiftY);
        if (eye.width == desc.Width && eye.height == desc.Height && eye.rw == region.width && eye.rh == region.height && eye.format == format) {
            if (eye.x != region.x || eye.y != region.y) {
                // A new FOV moves both active subrects together. Their size
                // is unchanged, so keep GPU resources and reset NR history.
                eye.x = region.x; eye.y = region.y;
                eye.reset = true;
                eye.loggedFeather = -1.f;
                OOVR_LOGF("NR prepass alignment eye=%u projection-center=%.6f,%.6f offset=%u,%u region=%ux%u; history reset",
                    eyeIndex, region.centerX, region.centerY, eye.x, eye.y, eye.rw, eye.rh);
            }
            return;
        }
        const UINT rw = region.width, rh = region.height;
        auto& kept = spare[eyeIndex];
        if (kept.width == desc.Width && kept.height == desc.Height && kept.rw == rw && kept.rh == rh && kept.format == format) {
            // Back to the size before: swap the kept eye in, with fresh history.
            std::swap(eye, kept);
            eye.x = region.x; eye.y = region.y;
            eye.reset = true;
            eye.loggedFeather = -1.f;
            OOVR_LOGF("NR prepass eye=%u kept %ux%u region=%ux%u reused (feature %s); %ux%u kept",
                eyeIndex, eye.width, eye.height, eye.rw, eye.rh, eye.feature ? "kept" : "none yet", kept.width, kept.height);
            return;
        }
        // The current eye becomes the kept one; only the one kept before goes.
        if (kept.feature || kept.input12) {
            Drain();
            DrainQueue();
            if (kept.feature) NR_NGX(releaseFeature(kept.feature));
        }
        kept = std::move(eye);
        eye = Eye{};
        eye.width = desc.Width; eye.height = desc.Height; eye.rw = rw; eye.rh = rh;
        eye.x = region.x; eye.y = region.y;
        eye.format = format;
        SharedTexture(desc.Width, desc.Height, format, false, eye.input12, eye.input11);
        // NR treats an output subrect covering the entire allocation as a full
        // output and remaps a cropped Color rect into its original coordinates.
        // A crop-sized output therefore fails rect validation when Color has
        // a nonzero offset. Keep the allocation full-sized and both active
        // rects identical; only rw*rh pixels are evaluated or copied back.
        SharedTexture(desc.Width, desc.Height, format, true, eye.output12, eye.output11);
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = format;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        NR_HR(device11->CreateShaderResourceView(eye.input11.Get(), &srv, &eye.originalSrv));
        NR_HR(device11->CreateShaderResourceView(eye.output11.Get(), &srv, &eye.neuralSrv));
        OOVR_LOGF("NR prepass eye allocation: eye=%u full=%ux%u region=%ux%u offset=%u,%u projection-center=%.6f,%.6f format=%u",
            eyeIndex, desc.Width, desc.Height, rw, rh, eye.x, eye.y, region.centerX, region.centerY, format);
    }

    // keep: the nasolabial band's keep mask (G), keepWidth x keepHeight, or null.
    // A whole-image region (no feather) comes here only with the band.
    void Feather(Eye& eye, ID3D11RenderTargetView* target, ID3D11ShaderResourceView* keep, UINT keepWidth, UINT keepHeight) {
        const bool whole = eye.rw == eye.width && eye.rh == eye.height;
        FeatherConstants constants{static_cast<int>(eye.x), static_cast<int>(eye.y), static_cast<int>(eye.rw), static_cast<int>(eye.rh),
                                  whole ? 0.f : std::max(1.f, std::min(eye.rw, eye.rh) * settings.feather), keep ? 1u : 0u,
                                  keep ? static_cast<float>(keepWidth) / eye.width : 0.f, keep ? static_cast<float>(keepHeight) / eye.height : 0.f};
        Ptr<ID3DDeviceContextState> previous;
        context11->SwapDeviceContextState(privateState.Get(), &previous);
        context11->ClearState();
        VamWriteConstants(context11, featherConstants.Get(), &constants, sizeof(constants));
        context11->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context11->VSSetShader(featherVs.Get(), nullptr, 0);
        context11->PSSetShader(featherPs.Get(), nullptr, 0);
        context11->PSSetConstantBuffers(0, 1, featherConstants.GetAddressOf());
        ID3D11ShaderResourceView* srvs[] = {eye.originalSrv.Get(), eye.neuralSrv.Get(), keep};
        context11->PSSetShaderResources(0, 3, srvs);
        context11->OMSetRenderTargets(1, &target, nullptr);
        context11->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context11->OMSetDepthStencilState(featherDepth.Get(), 0);
        context11->RSSetState(featherRaster.Get());
        D3D11_VIEWPORT viewport{0, 0, static_cast<float>(eye.width), static_cast<float>(eye.height), 0, 1};
        context11->RSSetViewports(1, &viewport);
        // Original pixels remain in the target outside this rectangle. Inside,
        // write the explicit composite once instead of first overwriting the
        // rectangle and depending on four separate fixed-function blends.
        const D3D11_RECT region{static_cast<LONG>(eye.x), static_cast<LONG>(eye.y),
            static_cast<LONG>(eye.x + eye.rw), static_cast<LONG>(eye.y + eye.rh)};
        context11->RSSetScissorRects(1, &region);
        context11->Draw(3, 0);
        context11->ClearState();
        context11->SwapDeviceContextState(previous.Get(), nullptr);
        if (eye.loggedFeather != settings.feather) {
            OOVR_LOGF("NR feather explicit composite submitted: region=%ux%u offset=%u,%u width=%.1fpx (%.0f%%), smooth corners; before NIS/OFXR",
                eye.rw, eye.rh, eye.x, eye.y, constants.pixels, settings.feather * 100);
            eye.loggedFeather = settings.feather;
        }
    }

    void EnsureFeature(Eye& eye, ID3D12GraphicsCommandList* list, bool useDepth, bool useMotion, unsigned eyeIndex) {
        parameters->Set("DLSSNR.Enabled", true);
        // Unity renders reversed Z on D3D11 (1 = near). Only meaningful with depth.
        parameters->Set("DLSSNR.DepthInverted", useDepth);
        if (eye.feature) return;
        parameters->Set("DLSSNR.Width", static_cast<int>(eye.rw));
        parameters->Set("DLSSNR.Height", static_cast<int>(eye.rh));
        parameters->Set("DLSSNR.Hint.Render.Preset", settings.preset);
        parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
        parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
        if (sharedNgx && !manualLoad) EnableManualLoad();
        auto result = createFeature(list, NVSDK_NGX_Feature_Reserved18, parameters, &eye.feature);
        if (NVSDK_NGX_FAILED(result) && !manualLoad &&
            std::none_of(eyes.begin(), eyes.end(), [](const Eye& e) { return e.feature != nullptr; })) {
            OOVR_LOGF("NR prepass: NGX CreateFeature returned 0x%08x; trying author's compatibility loader", static_cast<unsigned>(result));
            EnableManualLoad();
            result = createFeature(list, NVSDK_NGX_Feature_Reserved18, parameters, &eye.feature);
        }
        NR_NGX(result);
        eye.featureWithDepth = useDepth;
        eye.featurePreset = settings.preset;
        OOVR_LOGF("NR prepass eye=%u feature created; bridge depth=%d motion=%d render preset=%d", eyeIndex, useDepth ? 1 : 0, useMotion ? 1 : 0, settings.preset);
    }

    void SetEvaluation(const Eye& eye, ID3D12Resource* input, ID3D12Resource* depth, ID3D12Resource* motion, bool reset) {
        parameters->Set("DLSSNR.Color", input);
        parameters->Set("DLSSNR.Output", eye.output12.Get());
        parameters->Set("DLSSNR.ColorSubrectBaseX", static_cast<int>(eye.x));
        parameters->Set("DLSSNR.ColorSubrectBaseY", static_cast<int>(eye.y));
        parameters->Set("DLSSNR.ColorSubrectWidth", static_cast<int>(eye.rw));
        parameters->Set("DLSSNR.ColorSubrectHeight", static_cast<int>(eye.rh));
        parameters->Set("DLSSNR.OutputSubrectBaseX", static_cast<int>(eye.x));
        parameters->Set("DLSSNR.OutputSubrectBaseY", static_cast<int>(eye.y));
        parameters->Set("DLSSNR.OutputSubrectWidth", static_cast<int>(eye.rw));
        parameters->Set("DLSSNR.OutputSubrectHeight", static_cast<int>(eye.rh));
        if (depth) {
            parameters->Set("DLSSNR.Depth", depth);
            parameters->Set("DLSSNR.DepthSubrectBaseX", static_cast<int>(eye.x));
            parameters->Set("DLSSNR.DepthSubrectBaseY", static_cast<int>(eye.y));
            parameters->Set("DLSSNR.DepthSubrectWidth", static_cast<int>(eye.rw));
            parameters->Set("DLSSNR.DepthSubrectHeight", static_cast<int>(eye.rh));
        } else {
            parameters->Set("DLSSNR.Depth", static_cast<ID3D12Resource*>(nullptr));
        }
        if (motion) {
            // Pixels, current -> previous, INCLUDING the projection jitter change:
            // NR has no jitter input, so it must see how the rendered pixels moved.
            parameters->Set("DLSSNR.MVec", motion);
            parameters->Set("DLSSNR.MVecSubrectBaseX", static_cast<int>(eye.x));
            parameters->Set("DLSSNR.MVecSubrectBaseY", static_cast<int>(eye.y));
            parameters->Set("DLSSNR.MVecSubrectWidth", static_cast<int>(eye.rw));
            parameters->Set("DLSSNR.MVecSubrectHeight", static_cast<int>(eye.rh));
            parameters->Set("DLSSNR.MVecScaleX", 1.f);
            parameters->Set("DLSSNR.MVecScaleY", 1.f);
        } else {
            parameters->Set("DLSSNR.MVec", static_cast<ID3D12Resource*>(nullptr));
        }
        // Never a ControlMask: it turns NR's own skin mask off (see above featherShader).
        parameters->Set("DLSSNR.ControlMask", static_cast<ID3D12Resource*>(nullptr));
        parameters->Set("DLSSNR.Reset", reset);
        parameters->Set("DLSSNR.UseAutoMask", true);
        parameters->Set("DLSSNR.Style", settings.style);
        parameters->Set("DLSSNR.Intensity", settings.intensity);
        parameters->Set("DLSSNR.LocalToneStrength", settings.tone);
        parameters->Set("DLSSNR.LocalStructureStrength", settings.structure);
        parameters->Set("DLSSNR.SkinStructureStrength", settings.skin);
        parameters->Set("DLSSNR.UICorrection", 0);
    }

    void ReadSettingsIfDue(ULONGLONG now) {
        if (now >= nextSettingsRead) {
            if (VamProfileChanged(profileSeen, profileRead)) settings = ReadSettings();
            nextSettingsRead = now + 250;
        }
    }

    // False while NR is off.
    bool Refresh(ULONGLONG now) {
        ReadSettingsIfDue(now);
        if (!settings.enabled) {
            for (auto& e : eyes) e.reset = true;
            return false;
        }
        if (!initialized) Initialize();
        return true;
    }

    static D3D11_TEXTURE2D_DESC EyeDesc(ID3D11Texture2D* image) {
        D3D11_TEXTURE2D_DESC desc{};
        image->GetDesc(&desc);
        if (desc.ArraySize != 1 || desc.SampleDesc.Count != 1 || desc.Width < 4 || desc.Height < 4)
            throw std::runtime_error("NR requires cropped, resolved single-eye textures");
        return desc;
    }

    // eyeIndex 2 = the packed canvas (log only).
    void CheckFeature(Eye& eye, bool useDepth, unsigned eyeIndex) {
        if (eye.feature && useDepth && !eye.featureWithDepth) {
            // DepthInverted is a creation parameter: recreate once guides arrive.
            Drain();
            NR_NGX(releaseFeature(eye.feature));
            eye.feature = nullptr;
            eye.reset = true;
        }
        if (eye.feature && eye.featurePreset != settings.preset) {
            // The model (render preset) is a creation parameter too: the
            // in-game tab's 模型 switch recreates the feature.
            Drain();
            NR_NGX(releaseFeature(eye.feature));
            eye.feature = nullptr;
            eye.reset = true;
            OOVR_LOGF("NR prepass eye=%u render preset %d -> %d; feature recreated", eyeIndex, eye.featurePreset, settings.preset);
        }
    }

    // The guides' D3D12 copies; returns the nasolabial band's keep mask
    // (render size, also for NR after the upscaler), read straight from the
    // guides by the composite, with its size in keepDesc, or null.
    ID3D11Texture2D* PrepareGuides(Eye& eye, const VamFrameGuides* guides, bool useDepth, bool useMotion, D3D11_TEXTURE2D_DESC& keepDesc) {
        if (useDepth) SharedGuide(eye.width, eye.height, DXGI_FORMAT_R32_FLOAT, eye.depth12, eye.depth11);
        // RG16F: D3D11 cannot open a shared RG32F texture (E_INVALIDARG),
        // which stopped NR whenever DLSS supplied guides.
        if (useMotion) SharedGuide(eye.width, eye.height, DXGI_FORMAT_R16G16_FLOAT, eye.motion12, eye.motion11);
        ID3D11Texture2D* keep = guides ? guides->nrControl : nullptr;
        if (keep) {
            keep->GetDesc(&keepDesc);
            if (eye.keepTexture.Get() != keep) {
                eye.keepSrv.Reset();
                eye.keepTexture = keep;
                NR_HR(device11->CreateShaderResourceView(keep, nullptr, &eye.keepSrv));
            }
            if (!eye.keepLogged) {
                eye.keepLogged = true;
                OOVR_LOGF("NR nasolabial band composited (D3D11): %ux%u keep mask for the %ux%u image", keepDesc.Width, keepDesc.Height, eye.width, eye.height);
            }
        } else {
            eye.keepLogged = false;
        }
        return keep;
    }

    Commands& BeginCommands() {
        auto& cmd = commands[nextCommands++ % commands.size()];
        WaitCpu(outputFence12.Get(), cmd.completed);
        if (!cmd.allocator) {
            NR_HR(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmd.allocator)));
            NR_HR(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmd.allocator.Get(), nullptr, IID_PPV_ARGS(&cmd.list)));
        } else {
            NR_HR(cmd.allocator->Reset());
            NR_HR(cmd.list->Reset(cmd.allocator.Get(), nullptr));
        }
        return cmd;
    }

    void CopyIn(Eye& eye, ID3D11Texture2D* image, const VamFrameGuides* guides, bool useDepth, bool useMotion) {
        context11->CopySubresourceRegion(eye.input11.Get(), 0, 0, 0, 0, image, 0, nullptr);
        if (useDepth) context11->CopySubresourceRegion(eye.depth11.Get(), 0, 0, 0, 0, guides->depth, 0, nullptr);
        if (useMotion) context11->CopySubresourceRegion(eye.motion11.Get(), 0, 0, 0, 0, guides->motionJitteredHalf, 0, nullptr);
        if (on12) {
            // Same queue: everything D3D11 recorded so far runs before this list.
            context11->Flush();
        } else {
            NR_HR(context11->Signal(inputFence11.Get(), ++inputValue));
            context11->Flush();
            NR_HR(queue->Wait(inputFence12.Get(), inputValue));
        }
    }

    void Execute(Commands& cmd) {
        NR_HR(cmd.list->Close());
        ID3D12CommandList* lists[] = {cmd.list.Get()};
        queue->ExecuteCommandLists(1, lists);
        cmd.completed = ++outputValue;
        NR_HR(queue->Signal(outputFence12.Get(), outputValue));
        // Same queue under D3D11On12: the D3D11 work recorded from here on runs after NR.
        if (!on12) NR_HR(context11->Wait(outputFence11.Get(), outputValue));
    }

    // The shared simultaneous-access textures return to COMMON at the
    // ExecuteCommandLists boundary, matching the author's NR interop path.
    // Reuse is ordered by the next D3D11 input signal, after composition.
    void Compose(Eye& eye, ID3D11Texture2D* image, ID3D11RenderTargetView* target, ID3D11Texture2D* keep, const D3D11_TEXTURE2D_DESC& keepDesc) {
        if (eye.rw == eye.width && eye.rh == eye.height && !keep) {
            context11->CopySubresourceRegion(image, 0, 0, 0, 0, eye.output11.Get(), 0, nullptr);
        } else {
            Feather(eye, target, keep ? eye.keepSrv.Get() : nullptr, keepDesc.Width, keepDesc.Height);
        }
    }

    void Process(ID3D11Texture2D* image, ID3D11RenderTargetView* target, DXGI_FORMAT format, unsigned eyeIndex, const VamFrameGuides* guides) {
        std::lock_guard lock(mutex);
        if(failed && !unavailable && VamDesktopDisplay() && GetTickCount64()>=retryAt){
            Drain();DrainQueue();
            for(auto& e:eyes){if(e.feature)releaseFeature(e.feature);e.feature=nullptr;e.reset=true;}
            for(auto& e:spare){if(e.feature)releaseFeature(e.feature);e.feature=nullptr;e.reset=true;}
            failed=false;
            auto log=Environment(L"VAM_NR_PREPASS_LOG");if(!log.empty())DeleteFileW((log+L"/failure.txt").c_str());
        }
        if (failed) return;
        if (eyeIndex >= eyes.size() || !image || !target) throw std::runtime_error("Invalid NR eye submission");
        const ULONGLONG now = GetTickCount64();
        if (!Refresh(now)) return;
        const auto desc = EyeDesc(image);
        auto& eye = eyes[eyeIndex];
        EnsureEye(eye, desc, format, eyeIndex);
        const bool useDepth = guides && settings.useDepth && GuideMatches(guides->depth, desc.Width, desc.Height);
        const bool useMotion = guides && settings.useMotion && GuideMatches(guides->motionJitteredHalf, desc.Width, desc.Height);
        CheckFeature(eye, useDepth, eyeIndex);
        D3D11_TEXTURE2D_DESC keepDesc{};
        ID3D11Texture2D* keep = PrepareGuides(eye, guides, useDepth, useMotion, keepDesc);
        auto& cmd = BeginCommands();
        CopyIn(eye, image, guides, useDepth, useMotion);

        EnsureFeature(eye, cmd.list.Get(), useDepth, useMotion, eyeIndex);
        SetEvaluation(eye, eye.input12.Get(), useDepth ? eye.depth12.Get() : nullptr, useMotion ? eye.motion12.Get() : nullptr,
            eye.reset || now - eye.lastSubmit > 500 || (guides && guides->reset));
        NR_NGX(evaluateFeature(cmd.list.Get(), eye.feature, parameters, nullptr));
        Execute(cmd);
        Compose(eye, image, target, keep, keepDesc);
        eye.reset = false;
        eye.lastSubmit = now;
        if (++eye.evaluations == 1) OOVR_LOGF("NR prepass eye=%u evaluation submitted successfully; upstream of OFXR", eyeIndex);
        if (packing) {
            packing = false;
            OOVR_LOGF("NR prepass: each eye evaluated on its own again (eye=%u)", eyeIndex);
        }
    }

    static constexpr D3D12_RESOURCE_STATES kNrRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    void PackedTexture(UINT width, UINT height, DXGI_FORMAT format, bool output, Ptr<ID3D12Resource>& resource) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width; desc.Height = height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc = {1, 0};
        desc.Format = format;
        desc.Flags = output ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        // Created zeroed; the gap between the eyes is never written: black, far depth, no motion.
        NR_HR(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                    output ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : kNrRead, nullptr, IID_PPV_ARGS(&resource)));
    }

    void EnsurePacked(UINT eyeWidth, UINT height, UINT gap, DXGI_FORMAT format) {
        const UINT width = 2 * eyeWidth + gap;
        if (packed.width == width && packed.height == height && packed.format == format) return;
        if (packed.feature || packed.input12) {
            Drain();
            DrainQueue();
            if (packed.feature) NR_NGX(releaseFeature(packed.feature));
        }
        packed = Eye{};
        packed.width = packed.rw = width;
        packed.height = packed.rh = height;
        packed.format = format;
        packedGap = gap;
        PackedTexture(width, height, ResourceFormat(format), false, packed.input12);
        PackedTexture(width, height, DXGI_FORMAT_R32_FLOAT, false, packed.depth12);
        PackedTexture(width, height, DXGI_FORMAT_R16G16_FLOAT, false, packed.motion12);
        PackedTexture(width, height, ResourceFormat(format), true, packed.output12);
        OOVR_LOGF("NR prepass packed both eyes: canvas %ux%u = 2 x %u + gap %u, one feature", width, height, eyeWidth, gap);
    }

    static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* const* resources, UINT count,
                           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barriers[3]{};
        UINT used = 0;
        for (UINT i = 0; i < count && used < 3; ++i) {
            if (!resources[i]) continue;
            auto& b = barriers[used++];
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = resources[i];
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = before;
            b.Transition.StateAfter = after;
        }
        if (used) list->ResourceBarrier(used, barriers);
    }

    static void CopyRegion(ID3D12GraphicsCommandList* list, ID3D12Resource* to, UINT toX, UINT toY,
                           ID3D12Resource* from, UINT fromX, UINT fromY, UINT width, UINT height) {
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = to;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = from;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{fromX, fromY, 0, fromX + width, fromY + height, 1};
        list->CopyTextureRegion(&dst, toX, toY, 0, &src, &box);
    }

    // PackEyes: both eyes' regions side by side in one image, PackGap columns
    // apart, one feature and one evaluation: at these sizes most of NR's cost
    // is per evaluation (off-line, L33 both eyes 2.24 -> 1.29 ms). Eye 1 comes
    // cropped ahead, during eye 0's submission (VamNis). False, with nothing
    // done, when off or not packable: the caller then runs Process per eye.
    bool ProcessPair(ID3D11Texture2D* const images[2], ID3D11RenderTargetView* const targets[2], DXGI_FORMAT format,
                     const VamFrameGuides* const guides[2]) {
        std::lock_guard lock(mutex);
        if (failed) return false;
        for (unsigned e = 0; e < 2; ++e)
            if (!images[e] || !targets[e]) throw std::runtime_error("Invalid NR eye submission");
        const ULONGLONG now = GetTickCount64();
        if (!Refresh(now) || !settings.packEyes) return false;
        const D3D11_TEXTURE2D_DESC desc[2] = {EyeDesc(images[0]), EyeDesc(images[1])};
        if (desc[0].Width != desc[1].Width || desc[0].Height != desc[1].Height) return false;
        for (unsigned e = 0; e < 2; ++e) EnsureEye(eyes[e], desc[e], format, e);
        const UINT rw = eyes[0].rw, rh = eyes[0].rh;
        const int gap = PackGap(rw);
        if (eyes[1].rw != rw || eyes[1].rh != rh || gap < 0) {
            if (!packRefusedLogged) {
                OOVR_LOGF("NR prepass not packed: regions %ux%u and %ux%u (packed up to 1100 and 1840-1852 wide); each eye on its own",
                    eyes[0].rw, eyes[0].rh, eyes[1].rw, eyes[1].rh);
                packRefusedLogged = true;
            }
            return false;
        }
        packRefusedLogged = false;
        bool useDepth = settings.useDepth, useMotion = settings.useMotion;
        for (unsigned e = 0; e < 2; ++e) {
            useDepth = useDepth && guides[e] && GuideMatches(guides[e]->depth, desc[e].Width, desc[e].Height);
            useMotion = useMotion && guides[e] && GuideMatches(guides[e]->motionJitteredHalf, desc[e].Width, desc[e].Height);
        }
        EnsurePacked(rw, rh, static_cast<UINT>(gap), format);
        CheckFeature(packed, useDepth, 2);
        D3D11_TEXTURE2D_DESC keepDesc[2]{};
        ID3D11Texture2D* keep[2] = {};
        for (unsigned e = 0; e < 2; ++e) keep[e] = PrepareGuides(eyes[e], guides[e], useDepth, useMotion, keepDesc[e]);
        auto& cmd = BeginCommands();
        auto* list = cmd.list.Get();
        for (unsigned e = 0; e < 2; ++e) CopyIn(eyes[e], images[e], guides[e], useDepth, useMotion);

        EnsureFeature(packed, list, useDepth, useMotion, 2);
        // In: each eye's region to its place on the canvas. The eyes' own
        // textures are simultaneous-access (promoted from COMMON by the copy).
        ID3D12Resource* canvas[] = {packed.input12.Get(), useDepth ? packed.depth12.Get() : nullptr, useMotion ? packed.motion12.Get() : nullptr};
        Transition(list, canvas, 3, kNrRead, D3D12_RESOURCE_STATE_COPY_DEST);
        for (unsigned e = 0; e < 2; ++e) {
            ID3D12Resource* from[] = {eyes[e].input12.Get(), eyes[e].depth12.Get(), eyes[e].motion12.Get()};
            for (unsigned k = 0; k < 3; ++k)
                if (canvas[k]) CopyRegion(list, canvas[k], e ? rw + gap : 0, 0, from[k], eyes[e].x, eyes[e].y, rw, rh);
        }
        Transition(list, canvas, 3, D3D12_RESOURCE_STATE_COPY_DEST, kNrRead);
        bool reset = packed.reset || now - packed.lastSubmit > 500;
        for (unsigned e = 0; e < 2; ++e) reset = reset || eyes[e].reset || (guides[e] && guides[e]->reset);
        SetEvaluation(packed, packed.input12.Get(), canvas[1], canvas[2], reset);
        NR_NGX(evaluateFeature(list, packed.feature, parameters, nullptr));
        // Out: each eye's part back to its region in its own output.
        ID3D12Resource* result[] = {packed.output12.Get()};
        Transition(list, result, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (unsigned e = 0; e < 2; ++e)
            CopyRegion(list, eyes[e].output12.Get(), eyes[e].x, eyes[e].y, packed.output12.Get(), e ? rw + gap : 0, 0, rw, rh);
        Transition(list, result, 1, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Execute(cmd);
        for (unsigned e = 0; e < 2; ++e) {
            Compose(eyes[e], images[e], targets[e], keep[e], keepDesc[e]);
            // The eyes' own features keep their (now old) lastSubmit: back to
            // separate evaluation they start with a reset.
            eyes[e].reset = false;
        }
        packed.reset = false;
        packed.lastSubmit = now;
        if (++packed.evaluations == 1) OOVR_LOGF("NR prepass packed evaluation submitted successfully; regions %ux%u at %u,%u and %u,%u", rw, rh,
            eyes[0].x, eyes[0].y, eyes[1].x, eyes[1].y);
        if (!packing) {
            packing = true;
            OOVR_LOGF("NR prepass: both eyes packed from now (canvas %ux%u, gap %u)", packed.width, packed.height, packedGap);
        }
        return true;
    }

    bool PackEyes() {
        std::lock_guard lock(mutex);
        ReadSettingsIfDue(GetTickCount64());
        return !failed && settings.enabled && settings.packEyes;
    }
};

VamNrPrepass::VamNrPrepass(ID3D11Device* device) : impl(std::make_unique<Impl>(device)) {}
VamNrPrepass::~VamNrPrepass() = default;

std::shared_ptr<VamNrPrepass> VamNrPrepass::GetIfEnabled(ID3D11Device* device) {
    if (Environment(L"VAM_NR_PREPASS_PATH").empty()) return {};
    static std::mutex sharedMutex;
    static std::weak_ptr<VamNrPrepass> shared;
    std::lock_guard lock(sharedMutex);
    auto current = shared.lock();
    if (current) {
        if (current->impl->device11.Get() != device) OOVR_ABORT("NR prepass: multiple D3D11 devices are unsupported");
    } else {
        current = std::shared_ptr<VamNrPrepass>(new VamNrPrepass(device));
        shared = current;
    }
    return current;
}

void VamNrPrepass::Process(ID3D11Texture2D* image, ID3D11RenderTargetView* target, DXGI_FORMAT format, unsigned eye, const VamFrameGuides* guides) {
    try { impl->Process(image, target, format, eye, guides); }
    catch (const std::exception& error) {
        std::lock_guard lock(impl->mutex);
        impl->failed = true;impl->retryAt=GetTickCount64()+1000;
        // An NR startup failure must not abort VaM's first eye submission.
        // Keep all resources alive and let the normal image release continue.
        if (impl->unavailable) OOVR_LOGF("NR prepass cannot run on this machine (NVIDIA NGX did not come up); frames go on without it: %s", error.what());
        else OOVR_LOGF(VamDesktopDisplay()?"VAM desktop NR resource failure; retry in 1s: %s":"NR prepass disabled after failure; OFXR continues with original real frames: %s", error.what());
        WriteFailure(error.what());
    }
}

bool VamNrPrepass::PackEyes() { return impl->PackEyes(); }

bool VamNrPrepass::ProcessPair(ID3D11Texture2D* const images[2], ID3D11RenderTargetView* const targets[2], DXGI_FORMAT format,
                               const VamFrameGuides* const guides[2]) {
    try { return impl->ProcessPair(images, targets, format, guides); }
    catch (const std::exception& error) {
        std::lock_guard lock(impl->mutex);
        impl->failed = true;impl->retryAt=GetTickCount64()+1000;
        OOVR_LOGF("NR prepass disabled after failure (both eyes packed); OFXR continues with original real frames: %s", error.what());
        WriteFailure(error.what());
        return false;
    }
}
