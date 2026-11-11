// VRS target selection/pattern design follows fholger/vrperfkit (MIT).
// See performance-licenses/vrperfkit-LICENSE.txt in the deployed bundle.
// This integration restricts VRS to game stereo geometry targets, excludes
// depth-disabled/UI passes, and explicitly suspends it around all composition.
#include "stdafx.h"
#include "dx11_vam_ffr.h"
#include "vam_performance_config.h"
#include <nvapi.h>
#include <detours.h>
#include <TlHelp32.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
using SetTargets = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
using SetTargetsUav = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
using SetDepth = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilState*, UINT);
using ClearContext = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
SetTargets originalTargets = nullptr;
SetTargetsUav originalTargetsUav = nullptr;
SetDepth originalDepth = nullptr;
ClearContext originalClear = nullptr;
std::atomic<VamFfr::Impl*> hooked{nullptr};
thread_local bool inHook = false;
struct HookScope { bool prior = inHook; HookScope() { inHook = true; } ~HookScope() { inHook = prior; } };
}

struct VamFfr::Impl {
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext> context;
    Ptr<ID3D11Texture2D> pattern;
    Ptr<ID3D11NvShadingRateResourceView> patternView;
    VamPerformanceSettings settings = VamPerformanceStartup();
    std::atomic<unsigned> suspended{0};
    bool available = false, active = false, targetEligible = false, depthAttached = false, logged = false;
    UINT targetWidth = 0, targetHeight = 0, targetArray = 0;
    struct Eye { bool known = false; float x = .5f, y = .5f, spanX = 1, spanY = 1; } eyes[2];
    bool dirty = true;
    ULONGLONG nextSettingsRead = 0;

    explicit Impl(ID3D11Device* d) : device(d) {
        d->GetImmediateContext(&context);
        if (NvAPI_Initialize() != NVAPI_OK) throw std::runtime_error("NVAPI initialization unavailable");
        NV_D3D1x_GRAPHICS_CAPS caps{};
        auto result = NvAPI_D3D1x_GetGraphicsCapabilities(d, NV_D3D1x_GRAPHICS_CAPS_VER, &caps);
        if (result != NVAPI_OK || !caps.bVariablePixelRateShadingSupported)
            throw std::runtime_error("NVAPI D3D11 VRS is unsupported on this device/driver");
        available = true;
    }
    void Disable() {
        if (!available && !active) return;
        NV_D3D11_VIEWPORTS_SHADING_RATE_DESC desc{};
        desc.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
        // NVAPI specifies numViewports=0 as disable for ALL viewports.
        auto status = NvAPI_D3D11_RSSetViewportsPixelShadingRates(context.Get(), &desc);
        NvAPI_D3D11_RSSetShadingRateResourceView(context.Get(), nullptr);
        active = false;
        if (status != NVAPI_OK) {
            available = false;
            OOVR_LOGF("VAM FFR disable failed: NVAPI %d", status);
        }
    }
    void Fail(const char* reason) {
        OOVR_LOGF("VAM FFR disabled: %s; original shading remains enabled", reason);
        Disable();
        available = false;
    }
    void ReadSettings() {
        auto now = GetTickCount64();
        if (now < nextSettingsRead) return;
        auto s = ReadVamPerformanceSettings();
        if (s.fullRadius != settings.fullRadius || s.halfRadius != settings.halfRadius) dirty = true;
        settings.ffr = s.ffr;
        settings.fullRadius = s.fullRadius;
        settings.halfRadius = s.halfRadius;
        nextSettingsRead = now + 250;
    }
    void BuildPattern() {
        if (!dirty && patternView) return;
        Disable();
        patternView.Reset(); pattern.Reset();
        UINT w = (targetWidth + NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH - 1) / NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH;
        UINT h = (targetHeight + NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT - 1) / NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT;
        std::vector<uint8_t> data(size_t(w) * h * targetArray, 0);
        for (UINT slice = 0; slice < targetArray; ++slice) {
            for (UINT y = 0; y < h; ++y) for (UINT x = 0; x < w; ++x) {
                // Use tile center and protect any tile crossing a ring edge.
                float px = (x + .5f) * NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH / targetWidth;
                float py = (y + .5f) * NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT / targetHeight;
                float distance = 100.f;
                for (UINT i = 0; i < 2; ++i) {
                    if (targetArray == 2 && i != slice) continue;
                    const auto& eye = eyes[i];
                    float dx = 2 * (px - eye.x) / eye.spanX;
                    float dy = 2 * (py - eye.y) / eye.spanY;
                    float tileRadius = std::hypot(float(NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH) / targetWidth / eye.spanX,
                        float(NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT) / targetHeight / eye.spanY);
                    distance = std::min(distance, std::hypot(dx, dy) - tileRadius);
                }
                data[(slice * h + y) * w + x] = distance < settings.fullRadius ? 0 : distance < settings.halfRadius ? 1 : 2;
            }
        }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.ArraySize = targetArray;
        td.MipLevels = td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R8_UINT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sub[2]{};
        for (UINT i = 0; i < targetArray; ++i) {
            sub[i].pSysMem = data.data() + size_t(w) * h * i;
            sub[i].SysMemPitch = w;
        }
        if (FAILED(device->CreateTexture2D(&td, sub, &pattern))) throw std::runtime_error("VRS pattern allocation failed");
        NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC vd{};
        vd.version = NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_VER;
        vd.Format = td.Format;
        vd.ViewDimension = targetArray == 2 ? NV_SRRV_DIMENSION_TEXTURE2DARRAY : NV_SRRV_DIMENSION_TEXTURE2D;
        if (targetArray == 2) vd.Texture2DArray.ArraySize = 2;
        if (NvAPI_D3D11_CreateShadingRateResourceView(device.Get(), pattern.Get(), &vd, &patternView) != NVAPI_OK)
            throw std::runtime_error("VRS pattern view creation failed");
        dirty = false;
        OOVR_LOGF("VAM FFR pattern: target=%ux%u array=%u radii=%.2f/%.2f rates=1x1,1x2,2x2 centers=%.3f,%.3f/%.3f,%.3f",
            targetWidth, targetHeight, targetArray, settings.fullRadius, settings.halfRadius, eyes[0].x, eyes[0].y, eyes[1].x, eyes[1].y);
    }
    void Apply() {
        if (!available) return;
        ReadSettings();
        if (suspended || !settings.ffr || !targetEligible || !depthAttached || !eyes[0].known || !eyes[1].known) {
            if (active) Disable();
            return;
        }
        Ptr<ID3D11DepthStencilState> depth;
        UINT reference = 0;
        context->OMGetDepthStencilState(&depth, &reference);
        if (depth) {
            D3D11_DEPTH_STENCIL_DESC desc{};
            depth->GetDesc(&desc);
            if (!desc.DepthEnable) { if (active) Disable(); return; }
        }
        if (active && !dirty) return;
        BuildPattern();
        if (!available) return;
        if (NvAPI_D3D11_RSSetShadingRateResourceView(context.Get(), patternView.Get()) != NVAPI_OK)
            throw std::runtime_error("binding VRS view failed");
        NV_D3D11_VIEWPORT_SHADING_RATE_DESC views[NV_MAX_NUM_VIEWPORTS]{};
        for (auto& v : views) {
            v.enableVariablePixelShadingRate = true;
            std::fill(std::begin(v.shadingRateTable), std::end(v.shadingRateTable), NV_PIXEL_X1_PER_RASTER_PIXEL);
            v.shadingRateTable[1] = NV_PIXEL_X1_PER_1X2_RASTER_PIXELS;
            v.shadingRateTable[2] = NV_PIXEL_X1_PER_2X2_RASTER_PIXELS;
        }
        NV_D3D11_VIEWPORTS_SHADING_RATE_DESC desc{};
        desc.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
        desc.numViewports = NV_MAX_NUM_VIEWPORTS;
        desc.pViewports = views;
        if (NvAPI_D3D11_RSSetViewportsPixelShadingRates(context.Get(), &desc) != NVAPI_OK)
            throw std::runtime_error("setting VRS rates failed");
        active = true;
        if (!logged) { OOVR_LOG("VAM FFR enabled on matching game stereo geometry render target"); logged = true; }
    }
    void Targets(UINT count, ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth) {
        targetEligible = false;
        depthAttached = depth != nullptr;
        if (!suspended && count && views && views[0]) {
            D3D11_RENDER_TARGET_VIEW_DESC vd{};
            views[0]->GetDesc(&vd);
            bool mipZero = (vd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D && vd.Texture2D.MipSlice == 0)
                || (vd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY && vd.Texture2DArray.MipSlice == 0 && vd.Texture2DArray.FirstArraySlice == 0 && vd.Texture2DArray.ArraySize == targetArray)
                || vd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMS
                || (vd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY && vd.Texture2DMSArray.FirstArraySlice == 0 && vd.Texture2DMSArray.ArraySize == targetArray);
            if (mipZero) {
                Ptr<ID3D11Resource> resource;
                Ptr<ID3D11Texture2D> texture;
                views[0]->GetResource(&resource);
                if (SUCCEEDED(resource.As(&texture))) {
                    D3D11_TEXTURE2D_DESC td{};
                    texture->GetDesc(&td);
                    targetEligible = td.Width == targetWidth && td.Height == targetHeight && td.ArraySize == targetArray;
                }
            }
        }
        Apply();
    }
};

namespace {
template<class F> void OnContext(ID3D11DeviceContext* context, F action) noexcept {
    if (inHook) return;
    auto* p = hooked.load();
    if (!p || !p->available || context != p->context.Get()) return;
    HookScope guard;
    try { action(*p); }
    catch (const std::exception& e) { p->Fail(e.what()); }
    catch (...) { p->Fail("unexpected D3D11 hook error"); }
}
void STDMETHODCALLTYPE HookTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth) {
    originalTargets(c, n, views, depth);
    OnContext(c, [&](auto& p) { p.Targets(n, views, depth); });
}
void STDMETHODCALLTYPE HookTargetsUav(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth,
    UINT start, UINT count, ID3D11UnorderedAccessView* const* uavs, const UINT* initial) {
    originalTargetsUav(c, n, views, depth, start, count, uavs, initial);
    OnContext(c, [&](auto& p) {
        if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) p.Targets(n, views, depth);
    });
}
void STDMETHODCALLTYPE HookDepth(ID3D11DeviceContext* c, ID3D11DepthStencilState* state, UINT reference) {
    originalDepth(c, state, reference);
    OnContext(c, [](auto& p) { p.Apply(); });
}
void STDMETHODCALLTYPE HookClear(ID3D11DeviceContext* c) {
    OnContext(c, [](auto& p) { p.Disable(); p.targetEligible = false; p.depthAttached = false; });
    originalClear(c);
}
void InstallHooks(ID3D11DeviceContext* context) {
    void** table = *reinterpret_cast<void***>(context);
    originalTargets = reinterpret_cast<SetTargets>(table[33]);
    originalTargetsUav = reinterpret_cast<SetTargetsUav>(table[34]);
    originalDepth = reinterpret_cast<SetDepth>(table[36]);
    originalClear = reinterpret_cast<ClearContext>(table[110]);
    // Collect handles before suspending threads: allocating a growing vector
    // after another thread has been suspended could wait on its heap lock.
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
        throw std::runtime_error("cannot begin FFR hook transaction");
    }
    LONG error = DetourUpdateThread(GetCurrentThread());
    for (HANDLE thread : threads) {
        LONG result = DetourUpdateThread(thread);
        if (result && result != ERROR_INVALID_HANDLE) error = result;
    }
    if (!error) error = DetourAttach(reinterpret_cast<PVOID*>(&originalTargets), HookTargets);
    if (!error) error = DetourAttach(reinterpret_cast<PVOID*>(&originalTargetsUav), HookTargetsUav);
    if (!error) error = DetourAttach(reinterpret_cast<PVOID*>(&originalDepth), HookDepth);
    if (!error) error = DetourAttach(reinterpret_cast<PVOID*>(&originalClear), HookClear);
    if (error) DetourTransactionAbort(); else error = DetourTransactionCommit();
    for (HANDLE thread : threads) CloseHandle(thread);
    if (error) throw std::runtime_error("FFR D3D11 hook installation failed");
}
}

VamFfr::VamFfr(ID3D11Device* device) : impl(std::make_unique<Impl>(device)) {}
VamFfr* VamFfr::GetIfEnabled(ID3D11Device* device) {
    if (!VamPerformanceStartup().enabled) return nullptr;
    static std::mutex mutex;
    static VamFfr* instance = nullptr;
    static bool attempted = false;
    std::lock_guard lock(mutex);
    if (attempted) return instance && instance->impl->device.Get() == device ? instance : nullptr;
    attempted = true;
    try {
        auto* candidate = new VamFfr(device);
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&HookTargets), &pinned)) {
            delete candidate;
            throw std::runtime_error("cannot keep FFR hook module loaded");
        }
        try { InstallHooks(candidate->impl->context.Get()); }
        catch (...) { delete candidate; throw; }
        instance = candidate;
        hooked.store(instance->impl.get());
        OOVR_LOG("VAM FFR D3D11 hooks installed; waiting for both submitted eye regions");
    } catch (const std::exception& e) { OOVR_LOGF("VAM FFR initialization skipped: %s", e.what()); }
    return instance;
}
void VamFfr::Observe(ID3D11Texture2D* submitted, const float* bounds, unsigned eye, float cx, float cy) {
    auto& p = *impl;
    if (!p.available || eye > 1) return;
    D3D11_TEXTURE2D_DESC td{};
    submitted->GetDesc(&td);
    if (td.ArraySize != 1 && td.ArraySize != 2) return;
    if (td.Width != p.targetWidth || td.Height != p.targetHeight || td.ArraySize != p.targetArray) {
        p.Disable();
        p.targetWidth = td.Width; p.targetHeight = td.Height; p.targetArray = td.ArraySize;
        p.eyes[0].known = p.eyes[1].known = false;
        p.dirty = true;
    }
    float sx = std::abs(bounds[2] - bounds[0]), sy = std::abs(bounds[3] - bounds[1]);
    if (sx < .1f || sy < .1f) return;
    float x = bounds[0] + (bounds[2] - bounds[0]) * std::clamp(cx, .2f, .8f);
    float y = bounds[1] + (bounds[3] - bounds[1]) * std::clamp(cy, .2f, .8f);
    auto& e = p.eyes[eye];
    if (!e.known || std::abs(e.x - x) > .001f || std::abs(e.y - y) > .001f || e.spanX != sx || e.spanY != sy) {
        e = {true, x, y, sx, sy};
        p.dirty = true;
    }
}
void VamFfr::Suspend() { ++impl->suspended; impl->Disable(); }
void VamFfr::Resume() {
    --impl->suspended;
    // Leave VRS disabled across runtime/OFXR work. The next GAME target bind
    // re-enables it, rather than restoring it onto a compositor target.
    impl->targetEligible = false;
}
