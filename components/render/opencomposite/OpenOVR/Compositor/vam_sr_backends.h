#pragma once
// The two temporal upscalers of the raster route that are not NVIDIA's: Intel XeSS (libxess.dll) and AMD FSR through
// the FSR API (amd_fidelityfx_loader_dx12.dll, which loads the upscaler DLL beside it: FSR 4 on the Radeons that have it,
// FSR 3.1 on every other card). Neither is linked. Each library is loaded from the folder the launcher names
// (VAM_XESS_RUNTIME_PATH, VAM_FSR_RUNTIME_PATH); a missing or refused one only means that upscaler is not offered.
// Both record D3D12 work into a command list the caller owns, so both exist only on the D3D11On12 device, where they
// take the place of NGX in VamDlss's hand-over (EvaluateOn12). docs/knowledge/upscalers-other-vendors.md.
#include <Windows.h>
#include <d3d12.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <xess/xess.h>
#include <xess/xess_d3d12.h>
#include <api/include/ffx_api.h>
#include <api/include/dx12/ffx_api_dx12.h>
#include <upscalers/include/ffx_upscale.h>

inline HMODULE VamLoadBeside(const std::wstring& folder, const wchar_t* name) {
    if (folder.empty()) return nullptr;
    const std::wstring path = folder + L"\\" + name;
    // The library's own folder first: FSR's loader finds its effect DLLs there, XeSS its dependencies.
    return LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

struct VamXessApi {
    HMODULE module = nullptr;
    decltype(&xessGetVersion) GetVersion = nullptr;
    decltype(&xessD3D12CreateContext) CreateContext = nullptr;
    decltype(&xessD3D12Init) Init = nullptr;
    decltype(&xessD3D12Execute) Execute = nullptr;
    decltype(&xessDestroyContext) DestroyContext = nullptr;
    decltype(&xessGetOptimalInputResolution) GetOptimalInputResolution = nullptr;
    decltype(&xessGetIntelXeFXVersion) GetIntelXeFXVersion = nullptr;
    decltype(&xessIsOptimalDriver) IsOptimalDriver = nullptr;

    // False with the reason in `why` when the library is absent or is not the API this was built for.
    bool Load(const std::wstring& folder, std::string& why) {
        if (module) return true;
        HMODULE m = VamLoadBeside(folder, L"libxess.dll");
        if (!m) { why = folder.empty() ? "VAM_XESS_RUNTIME_PATH is not set" : "libxess.dll could not be loaded (error " + std::to_string(GetLastError()) + ")"; return false; }
        auto bind = [&](auto& f, const char* name) { f = reinterpret_cast<std::remove_reference_t<decltype(f)>>(GetProcAddress(m, name)); return f != nullptr; };
        const bool ok = bind(GetVersion, "xessGetVersion") & bind(CreateContext, "xessD3D12CreateContext") & bind(Init, "xessD3D12Init") &
            bind(Execute, "xessD3D12Execute") & bind(DestroyContext, "xessDestroyContext") &
            bind(GetOptimalInputResolution, "xessGetOptimalInputResolution") & bind(GetIntelXeFXVersion, "xessGetIntelXeFXVersion") &
            bind(IsOptimalDriver, "xessIsOptimalDriver");
        if (!ok) { why = "libxess.dll lacks an export of the XeSS 2 API"; FreeLibrary(m); return false; }
        module = m;
        return true;
    }
};

struct VamFsrApi {
    HMODULE module = nullptr;
    PfnFfxCreateContext CreateContext = nullptr;
    PfnFfxDestroyContext DestroyContext = nullptr;
    PfnFfxConfigure Configure = nullptr;
    PfnFfxQuery Query = nullptr;
    PfnFfxDispatch Dispatch = nullptr;

    bool Load(const std::wstring& folder, std::string& why) {
        if (module) return true;
        HMODULE m = VamLoadBeside(folder, L"amd_fidelityfx_loader_dx12.dll");
        if (!m) { why = folder.empty() ? "VAM_FSR_RUNTIME_PATH is not set" : "amd_fidelityfx_loader_dx12.dll could not be loaded (error " + std::to_string(GetLastError()) + ")"; return false; }
        CreateContext = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(m, "ffxCreateContext"));
        DestroyContext = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(m, "ffxDestroyContext"));
        Configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(m, "ffxConfigure"));
        Query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(m, "ffxQuery"));
        Dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(m, "ffxDispatch"));
        if (!CreateContext || !DestroyContext || !Configure || !Query || !Dispatch) { why = "amd_fidelityfx_loader_dx12.dll lacks an FSR API export"; FreeLibrary(m); return false; }
        module = m;
        return true;
    }

    // The versions the upscaler DLL (and the driver) offer on this device, newest first as the API lists them:
    // "4.1.2, 3.1.6, 2.3.4" on a Radeon with FSR 4. Empty when the upscaler DLL is absent.
    std::string UpscalerVersions(ID3D12Device* device) const {
        uint64_t count = 0;
        ffxQueryDescGetVersions q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        q.device = device;
        q.outputCount = &count;
        if (Query(nullptr, &q.header) != FFX_API_RETURN_OK || !count) return {};
        const char* names[16]{};
        uint64_t ids[16]{};
        count = std::min<uint64_t>(count, 16);
        q.versionIds = ids;
        q.versionNames = names;
        if (Query(nullptr, &q.header) != FFX_API_RETURN_OK) return {};
        std::string text;
        for (uint64_t i = 0; i < count; ++i) if (names[i]) text += (text.empty() ? "" : ", ") + std::string(names[i]);
        return text;
    }

    // The version a context runs ("3.1.6"; a trailing " *" marks one supplied by the driver).
    std::string ContextVersion(ffxContext context) const {
        ffxQueryGetProviderVersion q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (Query(&context, &q.header) != FFX_API_RETURN_OK || !q.versionName) return "unknown";
        return q.versionName;
    }
};

inline const char* VamXessResult(xess_result_t r) {
    switch (r) {
    case XESS_RESULT_SUCCESS: return "success";
    case XESS_RESULT_WARNING_OLD_DRIVER: return "old driver";
    case XESS_RESULT_ERROR_UNSUPPORTED_DEVICE: return "unsupported device (shader model 6.4 with DP4a is required)";
    case XESS_RESULT_ERROR_UNSUPPORTED_DRIVER: return "unsupported driver";
    case XESS_RESULT_ERROR_UNINITIALIZED: return "not initialized";
    case XESS_RESULT_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY: return "out of GPU memory";
    case XESS_RESULT_ERROR_DEVICE: return "device error";
    case XESS_RESULT_ERROR_UNSUPPORTED: return "not supported in this configuration";
    case XESS_RESULT_ERROR_CANT_LOAD_LIBRARY: return "a library could not be loaded";
    default: return "error";
    }
}

inline void VamCheckXess(xess_result_t r, const char* what) {
    if (r == XESS_RESULT_SUCCESS || r == XESS_RESULT_WARNING_OLD_DRIVER) return;
    char text[192];
    snprintf(text, sizeof(text), "%s failed: %s (%d)", what, VamXessResult(r), static_cast<int>(r));
    throw std::runtime_error(text);
}

inline void VamCheckFsr(ffxReturnCode_t r, const char* what) {
    if (r == FFX_API_RETURN_OK) return;
    static const char* const names[] = {"ok", "error", "unknown structure type", "runtime error", "no provider", "out of memory",
        "invalid parameter", "structure too new for this provider"};
    char text[192];
    snprintf(text, sizeof(text), "%s failed: %s (%u)", what, r < _countof(names) ? names[r] : "error", r);
    throw std::runtime_error(text);
}

// XeSS's quality setting for an input / output pair: the one whose own ratio is nearest, as DLSS's QualityFor. The
// library accepts an input inside a range around each setting's own size; XessInit steps to the neighbours when the
// nearest one refuses the size.
inline xess_quality_settings_t VamXessQuality(UINT w, UINT ow) {
    static const struct { xess_quality_settings_t setting; float ratio; } modes[] = {
        {XESS_QUALITY_SETTING_AA, 1.f}, {XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS, 1.3f}, {XESS_QUALITY_SETTING_ULTRA_QUALITY, 1.5f},
        {XESS_QUALITY_SETTING_QUALITY, 1.7f}, {XESS_QUALITY_SETTING_BALANCED, 2.f}, {XESS_QUALITY_SETTING_PERFORMANCE, 2.3f},
        {XESS_QUALITY_SETTING_ULTRA_PERFORMANCE, 3.f}};
    const float ratio = w ? static_cast<float>(ow) / static_cast<float>(w) : 1.f;
    xess_quality_settings_t best = XESS_QUALITY_SETTING_AA;
    float distance = 1e9f;
    for (const auto& m : modes) {
        const float d = std::fabs(std::log(ratio / m.ratio));
        if (d < distance) { distance = d; best = m.setting; }
    }
    return best;
}

inline const char* VamXessQualityName(xess_quality_settings_t q) {
    switch (q) {
    case XESS_QUALITY_SETTING_AA: return "Native AA";
    case XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS: return "Ultra Quality Plus";
    case XESS_QUALITY_SETTING_ULTRA_QUALITY: return "Ultra Quality";
    case XESS_QUALITY_SETTING_QUALITY: return "Quality";
    case XESS_QUALITY_SETTING_BALANCED: return "Balanced";
    case XESS_QUALITY_SETTING_PERFORMANCE: return "Performance";
    default: return "Ultra Performance";
    }
}
