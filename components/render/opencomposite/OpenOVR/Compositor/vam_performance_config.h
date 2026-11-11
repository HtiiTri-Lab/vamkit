#pragma once
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <mutex>
#include <string>

// Only the PreFG launcher sets this environment variable. Projection, poses and
// all legacy launchers are deliberately independent of these pixel settings.
struct VamPerformanceSettings {
    bool enabled = false;
    bool ffr = true;
    float scale = .75f;
    float sharpness = .20f;
    bool fsr = false;
    bool upscale = true;
    float fsrSharpness = .40f;
    float nrFeather = .10f;
    float fullRadius = .60f;
    float halfRadius = .85f;
    // Upscaler=DLSS: real DLSS SR fed by the BepInEx bridge; FSR1 stays the
    // per-frame fallback whenever a bridge capture is missing.
    bool dlss = false;
    int dlssPreset = 0;          // NVSDK_NGX_DLSS_Hint_Render_Preset value, 0 = driver default
    // DLSSPreset=Off: no DLSS SR at all; the bridge renders native frames at
    // the output size (v18: the only way to switch SR off; 100% with a model
    // is DLAA). Live, as the preset.
    bool dlssOff = false;
    // [Performance] OutputScale, 75-150 (DLSS only): the output and the XR
    // swapchain are the runtime's recommendation times this; the render scale
    // is a share of it. Live (VamLiveRenderSettings); this is the launch's.
    int outputPercent = 100;
    float dlssJitterSignX = 1.f; // diagnostic only: sign of the offsets passed to DLSS
    float dlssJitterSignY = 1.f;
    bool nrDepth = true;         // feed bridge depth / motion to NR when available
    bool nrMotion = true;
    bool dlssObjectMotion = true; // per-object motion for VaM GPU-skinned skin and clothing
    bool dlssHairMotion = true;  // motion of VaM physics hair from its guide strands
    // Moving props, CUAs and toys (MeshRenderers of non-Person atoms) from their
    // vertex buffers and this and the previous frame's matrix; needs DLSSObjectMotion. Live.
    bool dlssRigidMotion = true;
    int dlssHairBias = 0;        // 0-100: DLSS bias-current-colour mask value on hair pixels (0 = off)
    // While DLSS SR upscales, the game's textures are sampled with mip bias
    // (log2(render / output) - 1) x DLSSMipBias / 100: 100 = the DLSS guide's
    // value (3.5; -2 at 50%), 0 = off (dx11_vam_mip_bias.cpp). Live. Default 50:
    // 100 turned thin seams into dotted lines that change every frame (SimXR, 2026-10-16).
    int dlssMipBias = 50;
    // DLSSSharpness 0-100: NVSharpen on DLSS SR + optional NR output;
    // 0 = off (dx11_vam_nis.cpp). Live. Historical RCAS default 25: on a
    // dancing character +20% detail (generated frames as sharp as the real ones
    // were), +12% shimmer when still; 50 +19%, 75 +59% shimmer (SimXR, 2026-10-17).
    int dlssSharpness = 25;
    bool dlssSharpenLast = true; // DLSSSharpenLast: 0 after SR, 1 after optional post-SR NR. Live.
    // Depth and motion of VaM's world-space menus from their rectangles:
    // removed 2026-10-15 (no use; the bridge 2.5.0 sends no rectangles),
    // DLSSUIMotion is ignored.
    bool dlssUiMotion = false;
    bool dlssUiBypass = false;   // diagnostic: those menus' pixels from this frame's EASU instead of DLSS SR (still shakes with the jitter)
    // While VaM's menus are open (main HUD, start screen / scene browser) the
    // bridge stops the jitter and those frames skip DLSS SR (FSR1, as SR off).
    bool dlssMenuNative = true;
    int dlssFgEyes = 3;          // eyes given to DLSS Frame Generation: bit 0 left, bit 1 right; the others use optical flow
    int dlssSrEyes = 3;          // diagnostic: eyes given to DLSS SR (others FSR1); anything but both also stops the jitter
    // NR on the upscaler's output (output size, after DLSS SR / FSR / NIS)
    // instead of on the render before it; 0 = before (cheaper).
    bool nrAfterUpscale = false;
    // NRAfterScale, 25-100 (with NRAfterUpscale): NR runs on a copy of the
    // output reduced to this percent per axis and its edit (NR's result minus
    // its input) is carried back to the output, so NR sees the same pixel
    // scale whatever SR's render size (dx11_vam_nis.cpp). 100 = NR on the
    // output itself.
    int nrAfterScale = 100;
    // [NR] NasolabialReduction, 0-100: inside the nasolabial-fold band of
    // female VaM faces NR's result is blended back toward its input by this
    // percentage, which lowers its local tone and structure there (needs the
    // DLSS bridge's skin draws; see dx11_nr_prepass.cpp). Removed 2026-10-15
    // (no use; the bridge 2.5.0 sends no bands): always 0, the key is ignored.
    int nrFold = 0;
    // [Performance] RRTransparentGuides (2026-11-04): Legacy RR, the skin's guides fade to the raster fallback where
    // transparents drawn after the opaque pass (sheer clothing) change the pixel. 0 = the skin guides as before.
    bool rrTransparentGuides = true;
    // [Performance] PTReference (2026-11-07, diagnostic): PT shows the running mean of its raw input while the camera
    // is still, upscaled, instead of RR's output (RR still runs). Ctrl+Alt+R toggles it in game.
    bool ptReference = false;
    // PT VR only: confidence-gated cross-eye resolve, fused into RR's existing composite.
    // 0 = original composite; 100 = at most 50% of the other eye (before confidence gates).
    int rrStereo = 0;
};

inline std::wstring VamPerformanceConfigPath() {
    wchar_t path[32768]{};
    DWORD n = GetEnvironmentVariableW(L"VAM_PERFORMANCE_CONFIG", path, _countof(path));
    return n && n < _countof(path) ? std::wstring(path, n) : std::wstring{};
}

// The profile's last write time and size. GetPrivateProfile* opens and parses
// the whole file for every key (~45 us each, ~1.5 ms for the block below), so
// the readers parse again only when this changes (~2 us): the VR menu, the
// launch panel and the tray replace the file whole. A missing file is zero.
struct VamProfileStamp {
    ULONGLONG time = 0, size = 0;
    bool operator==(const VamProfileStamp& o) const { return time == o.time && size == o.size; }
};
inline VamProfileStamp VamProfileStampOf(const std::wstring& path) {
    VamProfileStamp s;
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!path.empty() && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) {
        s.time = (ULONGLONG(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
        s.size = (ULONGLONG(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    }
    return s;
}
// True the first time and whenever the profile changed since `seen`, which it
// then takes; always true without a profile (older launchers: the registry).
inline bool VamProfileChanged(VamProfileStamp& seen, bool& read) {
    static const std::wstring path = VamPerformanceConfigPath();
    if (path.empty()) return true;
    const VamProfileStamp now = VamProfileStampOf(path);
    if (read && now == seen) return false;
    seen = now;
    read = true;
    return true;
}

// The in-game path tracing switch ([RayTracing] Renderer in the skin plugin's cfg, ST_SetPathTracingV1). A switched-off or
// failed PT turns this false on its own. This is where the renderer learns which reconstruction the frame needs.
inline bool VamPathRequested() {
    static int(__cdecl* requested)()=nullptr;
    if(!requested)if(HMODULE m=GetModuleHandleW(L"VaMSkinTransmissionNative.bin"))
        requested=reinterpret_cast<int(__cdecl*)()>(GetProcAddress(m,"ST_PathTracingRequestedV1"));
    return requested && requested()!=0;
}

inline VamPerformanceSettings ParseVamPerformanceSettings(bool pathRequested) {
    VamPerformanceSettings s;
    const auto path = VamPerformanceConfigPath();
    if (path.empty()) return s;
    auto read = [&](const wchar_t* name, int fallback, int low, int high) {
        return std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Performance", name, fallback, path.c_str())), low, high);
    };
    s.enabled = read(L"Enabled", 1, 0, 1) != 0;
    s.ffr = read(L"FFREnabled", 1, 0, 1) != 0;
    // NIS upscales at most 2x per axis (NVScalerUpdateConfig refuses more):
    // 50% for it, 33% for DLSS and FSR 1.
    wchar_t chosen[32]{};
    GetPrivateProfileStringW(L"Performance", L"Upscaler", L"NIS", chosen, _countof(chosen), path.c_str());
    const bool nisChosen = _wcsicmp(chosen, L"DLSS") != 0 && _wcsicmp(chosen, L"FSR") != 0 && _wcsicmp(chosen, L"None") != 0;
    s.scale = read(L"RenderScale", 75, nisChosen ? 50 : 33, 100) / 100.f;
    s.sharpness = read(L"NISSharpness", 20, 0, 100) / 100.f;
    wchar_t upscaler[32]{};
    GetPrivateProfileStringW(L"Performance", L"Upscaler", L"NIS", upscaler, _countof(upscaler), path.c_str());
    s.dlss = _wcsicmp(upscaler, L"DLSS") == 0;
    s.fsr = s.dlss || _wcsicmp(upscaler, L"FSR") == 0;
    s.upscale = _wcsicmp(upscaler, L"None") != 0;
    wchar_t preset[16]{};
    GetPrivateProfileStringW(L"Performance", L"DLSSPreset", L"Default", preset, _countof(preset), path.c_str());
    // J..M are the transformer presets shipped by DLSS 310.x (A-F were removed).
    if (preset[0] && !preset[1] && towupper(preset[0]) >= L'J' && towupper(preset[0]) <= L'M')
        s.dlssPreset = 10 + (towupper(preset[0]) - L'J');
    if (_wcsicmp(preset, L"RR") == 0) s.dlssPreset = 6; // RR F, distinct from SR presets
    s.dlssOff = s.dlss && _wcsicmp(preset, L"Off") == 0;
    // PT on: RR F, with its own input scale, whatever the file holds for K/L/M (the menu keeps those for when PT goes off).
    if(pathRequested) {
        s.dlss=s.fsr=s.upscale=true;s.dlssOff=false;s.dlssPreset=6;
        s.scale=read(L"RenderScaleRR",33,33,100)/100.f;
    }
    // PT off: no RR. A live INI edit selecting RR gets K at K's own input scale.
    else if(s.dlssPreset==6) {
        s.dlssPreset=11;
        s.scale=read(L"RenderScaleK",75,33,100)/100.f;
    }
    s.outputPercent = s.dlss ? read(L"OutputScale", 100, 75, 150) : 100;
    // GetPrivateProfileInt does not return negative values: use 0/1 flips.
    s.dlssJitterSignX = read(L"DLSSJitterFlipX", 0, 0, 1) ? -1.f : 1.f;
    s.dlssJitterSignY = read(L"DLSSJitterFlipY", 0, 0, 1) ? -1.f : 1.f;
    s.nrDepth = read(L"NRUseDepth", 1, 0, 1) != 0;
    s.nrMotion = read(L"NRUseMotion", 1, 0, 1) != 0;
    s.dlssObjectMotion = read(L"DLSSObjectMotion", 1, 0, 1) != 0;
    s.dlssHairMotion = read(L"DLSSHairMotion", 1, 0, 1) != 0;
    s.dlssRigidMotion = read(L"DLSSRigidMotion", 1, 0, 1) != 0;
    s.dlssHairBias = read(L"DLSSHairBias", 0, 0, 100);
    s.dlssMipBias = read(L"DLSSMipBias", 50, 0, 100);
    s.dlssSharpness = read(L"DLSSSharpness", 25, 0, 100);
    s.dlssSharpenLast = read(L"DLSSSharpenLast", 1, 0, 1) != 0;
    s.dlssUiBypass = read(L"DLSSUIBypass", 0, 0, 1) != 0;
    s.dlssMenuNative = read(L"DLSSMenuNative", 1, 0, 1) != 0;
    s.dlssFgEyes = read(L"DLSSFGEyes", 3, 0, 3);
    s.dlssSrEyes = read(L"DLSSSREyes", 3, 0, 3);
    s.nrAfterUpscale = read(L"NRAfterUpscale", 0, 0, 1) != 0;
    s.nrAfterScale = read(L"NRAfterScale", 100, 25, 100);
    s.rrTransparentGuides = read(L"RRTransparentGuides", 1, 0, 1) != 0;
    s.ptReference = read(L"PTReference", 0, 0, 1) != 0;
    s.rrStereo = read(L"RRStereo", 0, 0, 100);
    if (!s.upscale) s.scale = 1.f;
    s.fsrSharpness = read(L"FSRSharpness", 40, 0, 100) / 100.f;
    s.nrFeather = read(L"NRFeatherPercent", 10, 1, 45) / 100.f;
    s.fullRadius = read(L"FFRFullRateRadius", 60, 20, 150) / 100.f;
    s.halfRadius = std::max(s.fullRadius, read(L"FFRHalfRateRadius", 85, 20, 150) / 100.f);
    return s;
}

// [Performance] as the profile holds it now, for every thread that asks (NR,
// DLSS SR, the upscaler stage and FFR each every 250 ms, VamLiveRenderSettings):
// parsed again only after the file changed (VamProfileChanged) or PT was switched.
inline VamPerformanceSettings ReadVamPerformanceSettings() {
    static std::mutex mutex;
    static VamProfileStamp seen;
    static bool read = false, path = false;
    static VamPerformanceSettings cached;
    const bool pathNow = VamPathRequested();
    std::lock_guard lock(mutex);
    if (VamProfileChanged(seen, read) || pathNow != path) { path = pathNow; cached = ParseVamPerformanceSettings(pathNow); }
    return cached;
}

// The render settings as they are now: [Performance] RenderScale in percent
// (100 with Upscaler=None, DLSSPreset=Off or without the performance profile),
// OutputScale (DLSS only, else 100) and DLSSPreset=Off, from the block above.
struct VamLiveRender {
    int renderPercent = 100;
    int outputPercent = 100;
    bool srOff = false;
};
inline VamLiveRender ReadVamLiveRender() {
    VamLiveRender r;
    const auto s = ReadVamPerformanceSettings();
    if (!s.enabled || !s.upscale) return r;
    r.renderPercent = static_cast<int>(std::lround(s.scale * 100.f));
    if (s.dlss) {
        r.outputPercent = s.outputPercent;
        r.srOff = s.dlssOff;
        if (r.srOff) r.renderPercent = 100;
    }
    return r;
}

// ReadVamLiveRender at most every 250 ms, for every thread that asks (the
// bridge on VaM's main thread, the swapchain on the render thread). A new
// OutputScale is taken only once it has stayed the same for 0.5 s: each one
// rebuilds the swapchains, DLSS SR and DLSS-G, so a slider being dragged
// applies when it rests.
inline VamLiveRender VamLiveRenderSettings() {
    static std::mutex mutex;
    static ULONGLONG next = 0, outputSince = 0;
    static bool read = false;
    static int outputSeen = 100;
    static VamLiveRender live;
    std::lock_guard lock(mutex);
    const ULONGLONG now = GetTickCount64();
    if (!read || now >= next) {
        const VamLiveRender r = ReadVamLiveRender();
        live.renderPercent = r.renderPercent;
        live.srOff = r.srOff;
        if (!read) live.outputPercent = outputSeen = r.outputPercent;
        else if (r.outputPercent != outputSeen) { outputSeen = r.outputPercent; outputSince = now; }
        else if (live.outputPercent != outputSeen && now - outputSince >= 500) live.outputPercent = outputSeen;
        read = true;
        next = now + 250;
    }
    return live;
}

// One output dimension: the runtime's recommendation at `percent`.
inline uint32_t VamOutputDimension(uint32_t recommended, int percent) {
    return std::max(4u, static_cast<uint32_t>((static_cast<uint64_t>(recommended) * static_cast<uint64_t>(percent) + 50) / 100));
}

// Freeze scale for the whole process: changing the game's render recommendation
// independently of its current targets would produce mismatched swapchains.
// (In the DLSS launcher RenderScale is still live: the VaM bridge scales
// Unity's eye texture from that recommendation, see VaMDLSS_GetEyeTextureScale.)
inline const VamPerformanceSettings& VamPerformanceStartup() {
    static const auto s = ReadVamPerformanceSettings();
    return s;
}

// The size VaM was told to render and the runtime's size it was reduced
// from; zero until the game asked.
struct VamRecommendation {
    std::atomic<uint32_t> width{0}, height{0}, fullWidth{0}, fullHeight{0};
};
inline VamRecommendation& VamRecommendedSize() {
    static VamRecommendation r;
    return r;
}

inline void VamScaleRecommendedSize(uint32_t* width, uint32_t* height) {
    const auto& s = VamPerformanceStartup();
    if (!s.enabled) return;
    const uint32_t fullWidth = *width, fullHeight = *height;
    // DLSS (v18): VaM gets the runtime's size itself and the bridge scales
    // Unity's eye texture from it (0.33 to 1.5); FSR 1 and NIS render at the
    // launch's scale from the start.
    const float scale = s.dlss ? 1.f : s.scale;
    *width = std::max(4u, static_cast<uint32_t>(*width * scale + 2) & ~3u);
    *height = std::max(4u, static_cast<uint32_t>(*height * scale + 2) & ~3u);
    auto& r = VamRecommendedSize();
    r.fullWidth = fullWidth; r.fullHeight = fullHeight;
    r.width = *width; r.height = *height;
}

