#include "stdafx.h"
#include "dx11_vam_nis.h"
#include "../../../../shading/rt-core/include/startup_display.h"
#include "vam_on12.h"
#include "dx11_nr_prepass.h"
#include "dx11_vam_dlss.h"
#include "dx11_eye_blit.h"
#include "dx11_gpu_stage_timer.h"
#include "dx11_vam_mip_bias.h"
#include "vam_performance_config.h"
#include <cmath>
#include <NIS_Config.h>
#include "vam_nis_sdr.h"
#include "vam_nis_linear.h"
#include "vam_nis_sharpen.h"
#include "vam_fsr_easu.h"
#include "vam_fsr_rcas.h"
#define A_CPU 1
#include <ffx_a.h>
#include <ffx_fsr1.h>
#include <d3d11_1.h>
#include <algorithm>
#include <functional>
#include <stdexcept>

namespace {
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
void CheckNis(HRESULT hr) {
    if (FAILED(hr)) {
        char message[96];
        sprintf_s(message, "Upscaler D3D11 resource error 0x%08x", static_cast<unsigned>(hr));
        throw std::runtime_error(message);
    }
}
bool IsSrgb(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}
DXGI_FORMAT RawFormat(DXGI_FORMAT f) {
    if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) return DXGI_FORMAT_B8G8R8A8_UNORM;
    if (f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB) return DXGI_FORMAT_B8G8R8X8_UNORM;
    return f;
}
DXGI_FORMAT ResourceFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    default: return f;
    }
}
struct ContextScope {
    ID3D11DeviceContext1* context;
    Ptr<ID3DDeviceContextState> previous;
    ContextScope(ID3D11DeviceContext1* c, ID3DDeviceContextState* state) : context(c) {
        c->SwapDeviceContextState(state, &previous);
        c->ClearState();
    }
    ~ContextScope() {
        context->ClearState();
        context->SwapDeviceContextState(previous.Get(), nullptr);
    }
};
struct FsrConstants {
    uint32_t easu[4][4];
    uint32_t rcas[4];
    uint32_t outputSize[2];
    uint32_t linearInput;
    uint32_t sharpen;
};
static_assert(sizeof(FsrConstants) == 96, "FSR shader constant layout");
// NRAfterScale < 100: NR on the output reduced to that percent, its edit carried
// back. reduce: area average (taps x taps bilinear samples per reduced pixel,
// exact for whole ratios). apply: the output plus (NR's result - its input) on
// the reduced image, interpolated with pixel centres aligned; where NR changed
// nothing that is zero and the output stays as it was. Values as stored (the
// gamma-encoded 8-bit bits, or FP16), as NR and DLSS see them.
struct ReducedConstants {
    uint32_t lowSize[2];
    uint32_t fullSize[2];
    uint32_t taps;
    float maxValue;
    uint32_t pad[2];
};
static_assert(sizeof(ReducedConstants) == 32, "reduced NR constant layout");
constexpr char kReducedShader[] = R"(
cbuffer Reduced : register(b0) { uint2 lowSize; uint2 fullSize; uint taps; float maxValue; uint2 pad; };
Texture2D<float4> full : register(t0);
Texture2D<float4> nrOutput : register(t1);
Texture2D<float4> nrInput : register(t2);
SamplerState linearClamp : register(s0);
RWTexture2D<float4> target : register(u0);
RWTexture2D<float4> unchanged : register(u1);
// reduce also writes NR's input as it was (apply's nrInput): one dispatch instead of a copy after it.
[numthreads(8, 8, 1)] void reduce(uint2 id : SV_DispatchThreadID) {
    if (any(id >= lowSize)) return;
    float4 sum = 0;
    for (uint j = 0; j < taps; ++j)
        for (uint i = 0; i < taps; ++i)
            sum += full.SampleLevel(linearClamp, (float2(id) + (float2(i, j) + 0.5) / taps) / float2(lowSize), 0);
    const float4 low = sum / (taps * taps);
    target[id] = low;
    unchanged[id] = low;
}
[numthreads(8, 8, 1)] void apply(uint2 id : SV_DispatchThreadID) {
    if (any(id >= fullSize)) return;
    const float2 uv = (float2(id) + 0.5) / float2(fullSize);
    const float3 edit = nrOutput.SampleLevel(linearClamp, uv, 0).rgb - nrInput.SampleLevel(linearClamp, uv, 0).rgb;
    const float4 c = full.Load(int3(id, 0));
    target[id] = float4(clamp(c.rgb + edit, 0, maxValue), c.a);
}
)";
// denoise / sharpen: NR after the upscaler and NVSharpen when it runs last (elsewhere their time counts as denoise).
enum SubmitStage : unsigned { StageCrop, StageGuides, StageNr, StageUpscale, StageDenoise, StageSharpen, StageOutput, SubmitStageCount };
const char* const kSubmitStageNames[SubmitStageCount] = {"crop", "guides", "nr", "upscale", "denoise", "sharpen", "output"};
// [NR] PackEyes: eye 0's VamNis also crops eye 1 (the other half of the same
// source) into this image and NR runs once for both; eye 1's VamNis then takes
// it instead of running NR. Shared by the two eyes' VamNis, process lifetime.
struct AheadEye {
    Ptr<ID3D11Texture2D> texture;
    Ptr<ID3D11RenderTargetView> rtv;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    ID3D11Texture2D* source = nullptr;
    UINT slice = 0;
    float bounds[4] = {};
    uint64_t serial = 0;
    bool ready = false;
};
AheadEye& Ahead() {
    static auto* ahead = new AheadEye;
    return *ahead;
}
// NRAfterScale < 100 (DenoiseReduced): work = the reduced output, NR runs on it in
// place; input = work before NR.
struct ReducedImages {
    Ptr<ID3D11Texture2D> work, input;
    Ptr<ID3D11ShaderResourceView> workSrv, inputSrv;
    Ptr<ID3D11UnorderedAccessView> workUav, inputUav;
    Ptr<ID3D11RenderTargetView> workRtv;
};
// [NR] PackEyes with NR after the upscaler on the reduced output, under RR: eye 0's
// VamNis also takes eye 1's output from RR ahead (VamDlss::UpscaledAhead) into
// `full`, reduces it into `low` and runs NR once for both eyes (ProcessPair); eye
// 1's VamNis then adds that edit to its own output instead of reducing it and
// running NR. Shared by the two eyes' VamNis, process lifetime.
struct ReducedAheadEye {
    Ptr<ID3D11Texture2D> full;
    Ptr<ID3D11ShaderResourceView> fullSrv;
    ReducedImages low;
    UINT ow = 0, oh = 0, lw = 0, lh = 0;
    DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
    uint64_t serial = 0;
    bool ready = false;
};
ReducedAheadEye& ReducedAhead() {
    static auto* ahead = new ReducedAheadEye;
    return *ahead;
}
}

struct VamNis::Impl {
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext1> context;
    Ptr<ID3DDeviceContextState> state;
    Ptr<ID3D11ComputeShader> sdr, linear, nisSharpen;
    Ptr<ID3D11ComputeShader> easu, rcas;
    Ptr<ID3D11SamplerState> sampler;
    Ptr<ID3D11Buffer> constants;
    Ptr<ID3D11Buffer> fsrConstants;
    Ptr<ID3D11ShaderResourceView> coefficients[2];
    Ptr<ID3D11Texture2D> work, output;
    Ptr<ID3D11RenderTargetView> workRtv, outputRtv;
    Ptr<ID3D11ShaderResourceView> workSrv;
    Ptr<ID3D11UnorderedAccessView> outputUav;
    Ptr<ID3D11Texture2D> fsrIntermediate;
    Ptr<ID3D11ShaderResourceView> fsrSrv;
    Ptr<ID3D11UnorderedAccessView> fsrUav;
    D3D11EyeBlitter blitter;
    UINT width = 0, height = 0, outWidth = 0, outHeight = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT outputView = DXGI_FORMAT_UNKNOWN;
    bool initialized = false, failed = false, logged = false;
    ULONGLONG retryAt=0;
    float sharpness = .20f;
    bool useFsr = false;
    bool useDlss = false;
    bool dlssLogged = false;
    uint64_t dlssFallbacks = 0;
    bool upscale = true;
    float fsrSharpness = .40f;
    ULONGLONG nextRead = 0;
    struct SubmissionSize {
        UINT width = 0, height = 0, outputWidth = 0, outputHeight = 0;
        bool submitted = false;
    } reported[2];
    bool telemetryChecked = false;
    std::wstring telemetryPrefix;
    GpuStageTimer<SubmitStageCount> timer{"eye submission", kSubmitStageNames};
    bool directLogged = false;
    bool aheadLogged = false;
    bool nrAfter = false, nrAfterLogged = false, nrAfterRefusedLogged = false;
    int nrAfterScale = 100; // NRAfterScale: NR after the upscaler on the output reduced to this percent
    // NRAfterScale < 100 (DenoiseReduced): full = a copy of the output (none when the
    // output can be read in place), result = the output with NR's edit (both output
    // size); low = the reduced output and NR's input (ReducedImages).
    struct Reduced {
        Ptr<ID3D11ComputeShader> reduce, apply;
        Ptr<ID3D11Buffer> constants;
        Ptr<ID3D11Texture2D> full, result;
        Ptr<ID3D11ShaderResourceView> fullSrv, resultSrv;
        Ptr<ID3D11UnorderedAccessView> resultUav;
        ReducedImages low;
        UINT ow = 0, oh = 0, lw = 0, lh = 0;
        DXGI_FORMAT view = DXGI_FORMAT_UNKNOWN;
        bool failed = false, logged = false, packLogged = false;
    } reduced;
    int mipBias = 50; // DLSSMipBias, percent of the DLSS guide's bias
    // DLSSSharpness (0-100, 0 = off): NVSharpen after DLSS SR and optional NR before it goes
    // out (8-bit sRGB eyes only); the output then takes the copy + blit path.
    int dlssSharpness = 25;
    bool dlssSharpenLast = true;
    bool sharpenLogged = false;
    Ptr<ID3D11ShaderResourceView> outputSrv;
    Ptr<ID3D11Texture2D> sharpened;
    Ptr<ID3D11UnorderedAccessView> sharpenedUav;
    // Native frames (bridge flag) per eye: the size of the last one, and how
    // many frames since then came at that size unflagged (Unity applies a new
    // eye texture scale a frame or two after the bridge sets it).
    struct NativeTrack {
        UINT width = 0, height = 0;
        unsigned tail = 0;
        bool active = false;
    } native[2];
    uint64_t nativeSwitches = 0;

    // Within 2% of the output in both dimensions: a native frame goes out as
    // a copy (scaled by those few pixels at most) instead of through FSR1/NIS.
    static bool NearNative(UINT w, UINT h, UINT ow, UINT oh) {
        return w * 50 >= ow * 49 && w * 50 <= ow * 51 && h * 50 >= oh * 49 && h * 50 <= oh * 51;
    }

    // NRAfterUpscale: NR runs on the destination image itself once the
    // upscaler has written it. Null (NR stays before the upscaler) unless that
    // is one plain 2D texture of the output size in a format NR takes.
    Ptr<ID3D11Texture2D> NrAfterTarget(ID3D11RenderTargetView* destination, UINT ow, UINT oh, DXGI_FORMAT& view) const {
        Ptr<ID3D11Resource> resource;
        destination->GetResource(&resource);
        Ptr<ID3D11Texture2D> texture;
        if (!resource || FAILED(resource.As(&texture))) return nullptr;
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        destination->GetDesc(&rd);
        D3D11_TEXTURE2D_DESC td{};
        texture->GetDesc(&td);
        const bool nrFormat = rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM || rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
            rd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (rd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || rd.Texture2D.MipSlice != 0 || td.ArraySize != 1 ||
            td.SampleDesc.Count != 1 || td.Width != ow || td.Height != oh || !nrFormat)
            return nullptr;
        view = rd.Format;
        return texture;
    }

    // The swapchain image itself when DLSS's output can be copied straight
    // into it (same size, same 8-bit or FP16 family as `output`): one copy
    // instead of a copy into `output` plus a full-screen blit. A raw copy of
    // UNORM bits into the sRGB image is what the sRGB-view blit produced.
    Ptr<ID3D11Texture2D> DirectTarget(ID3D11RenderTargetView* destination, UINT ow, UINT oh) const {
        Ptr<ID3D11Resource> resource;
        destination->GetResource(&resource);
        Ptr<ID3D11Texture2D> texture;
        if (!resource || FAILED(resource.As(&texture))) return nullptr;
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        destination->GetDesc(&rd);
        D3D11_TEXTURE2D_DESC td{}, od{};
        texture->GetDesc(&td);
        output->GetDesc(&od);
        if (rd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || rd.Texture2D.MipSlice != 0 ||
            td.SampleDesc.Count != 1 || td.Width != ow || td.Height != oh ||
            td.Format == DXGI_FORMAT_R16G16B16A16_UNORM || ResourceFormat(td.Format) != od.Format)
            return nullptr;
        return texture;
    }

    // One small local record per eye, only when dimensions or status change.
    // These sizes come from the actual cropped eye input and destination,
    // not the headset panel resolution or a previous session's recommendation.
    void ReportSubmission(unsigned eye, UINT w, UINT h, UINT ow, UINT oh, bool submitted) noexcept {
        if (eye > 1) return;
        auto& last = reported[eye];
        if (last.width == w && last.height == h && last.outputWidth == ow &&
            last.outputHeight == oh && last.submitted == submitted) return;
        last = {w, h, ow, oh, submitted};
        try {
            if (!telemetryChecked) {
                telemetryChecked = true;
                wchar_t profile[32768];
                const DWORD length = GetEnvironmentVariableW(L"VAM_PERFORMANCE_CONFIG", profile, _countof(profile));
                if (length && length < _countof(profile)) telemetryPrefix.assign(profile, length);
            }
            if (telemetryPrefix.empty()) return;
            const auto path = telemetryPrefix + L".eye" + std::to_wstring(eye) + L".json";
            const auto temporary = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
            char json[256];
            const int length = sprintf_s(json,
                "{\"eye\":%u,\"status\":\"%s\",\"input\":[%u,%u],\"output\":[%u,%u]}\n",
                eye, submitted ? "submitted" : "fallback", w, h, ow, oh);
            HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return;
            DWORD written = 0;
            const bool ok = WriteFile(file, json, static_cast<DWORD>(length), &written, nullptr) &&
                written == static_cast<DWORD>(length);
            CloseHandle(file);
            if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
                DeleteFileW(temporary.c_str());
        } catch (...) {
            // UI reporting must never interrupt eye submission.
        }
    }

    // PackEyes, eye 0 (the left half of Unity's double-wide image): eye 1's
    // crop and guides ahead, then NR once for both. False: not packed, eye 0
    // runs NR on its own (eye 1's guides made ahead still serve eye 1).
    bool PackAhead(ID3D11Texture2D* source, UINT slice, const float* bounds, DXGI_FORMAT f, UINT w, UINT h,
                   VamNrPrepass& nr, VamDlss& dlss, const VamFrameGuides& guides) {
        if (VamViewCount() != 2 || bounds[0] != 0.f || bounds[2] != .5f || !nr.PackEyes()) return false;
        auto& a = Ahead();
        a.ready = false;
        if (a.width != w || a.height != h || a.format != f) {
            a.rtv.Reset();
            a.texture.Reset();
            a.width = 0;
            D3D11_TEXTURE2D_DESC td{};
            work->GetDesc(&td);
            CheckNis(device->CreateTexture2D(&td, nullptr, &a.texture));
            D3D11_RENDER_TARGET_VIEW_DESC rd{};
            workRtv->GetDesc(&rd);
            CheckNis(device->CreateRenderTargetView(a.texture.Get(), &rd, &a.rtv));
            a.width = w; a.height = h; a.format = f;
        }
        const float right[4] = {.5f, bounds[1], 1.f, bounds[3]};
        CheckNis(blitter.Copy(source, a.rtv.Get(), f, w, h, slice, right));
        VamFrameGuides rightGuides;
        if (!dlss.PrepareGuides(source, right, 1, w, h, rightGuides, true)) return false;
        ID3D11Texture2D* images[] = {work.Get(), a.texture.Get()};
        ID3D11RenderTargetView* targets[] = {workRtv.Get(), a.rtv.Get()};
        const VamFrameGuides* both[] = {&guides, &rightGuides};
        if (!nr.ProcessPair(images, targets, f, both)) return false;
        a.source = source;
        a.slice = slice;
        std::copy(right, right + 4, a.bounds);
        a.serial = rightGuides.serial;
        a.ready = true;
        return true;
    }

    explicit Impl(ID3D11Device* d) : device(d) {}
    void Initialize() {
        Ptr<ID3D11DeviceContext> base;
        device->GetImmediateContext(&base);
        CheckNis(base.As(&context));
        Ptr<ID3D11Device1> d1;
        CheckNis(device.As(&d1));
        auto level = device->GetFeatureLevel();
        UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
        CheckNis(d1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &state));
        CheckNis(blitter.Initialize(device.Get(), base.Get()));
        CheckNis(device->CreateComputeShader(vam_nis_sdr, sizeof(vam_nis_sdr), nullptr, &sdr));
        CheckNis(device->CreateComputeShader(vam_nis_linear, sizeof(vam_nis_linear), nullptr, &linear));
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        CheckNis(device->CreateSamplerState(&sd, &sampler));
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(NISConfig);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        CheckNis(device->CreateBuffer(&bd, nullptr, &constants));
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kFilterSize / 4; td.Height = kPhaseCount;
        td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        for (int i = 0; i < 2; ++i) {
            D3D11_SUBRESOURCE_DATA data{};
            data.pSysMem = i ? coef_usm : coef_scale;
            data.SysMemPitch = kFilterSize * sizeof(float);
            Ptr<ID3D11Texture2D> tex;
            CheckNis(device->CreateTexture2D(&td, &data, &tex));
            CheckNis(device->CreateShaderResourceView(tex.Get(), nullptr, &coefficients[i]));
        }
        initialized = true;
    }
    void EnsureFsr() {
        if (!easu) CheckNis(device->CreateComputeShader(vam_fsr_easu, sizeof(vam_fsr_easu), nullptr, &easu));
        if (!rcas) CheckNis(device->CreateComputeShader(vam_fsr_rcas, sizeof(vam_fsr_rcas), nullptr, &rcas));
        if (!fsrConstants) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sizeof(FsrConstants);
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            CheckNis(device->CreateBuffer(&bd, nullptr, &fsrConstants));
        }
        if (!fsrIntermediate) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = outWidth; td.Height = outHeight;
            td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
            // Keep the EASU result in FP16 to avoid an extra 8-bit quantization
            // before RCAS (particularly important for the reversible HDR map).
            td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            CheckNis(device->CreateTexture2D(&td, nullptr, &fsrIntermediate));
            CheckNis(device->CreateShaderResourceView(fsrIntermediate.Get(), nullptr, &fsrSrv));
            CheckNis(device->CreateUnorderedAccessView(fsrIntermediate.Get(), nullptr, &fsrUav));
        }
    }
    // `output` read as stored (raw UNORM: NVSharpen and the reduced NR work on gamma-encoded values).
    ID3D11ShaderResourceView* OutputSrv() {
        if (!outputSrv) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            outputUav->GetDesc(&ud);
            D3D11_SHADER_RESOURCE_VIEW_DESC sr{};
            sr.Format = ud.Format;
            sr.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sr.Texture2D.MipLevels = 1;
            CheckNis(device->CreateShaderResourceView(output.Get(), &sr, &outputSrv));
        }
        return outputSrv.Get();
    }
    // NVSharpen alone over `output` (SR + optional NR, gamma-encoded 8-bit), or over
    // `source` when that holds it (the reduced NR's result), into `sharpened`, which
    // then goes out like `output` does.
    void Sharpen(UINT ow, UINT oh, int percent, ID3D11ShaderResourceView* source = nullptr) {
        if (!nisSharpen) CheckNis(device->CreateComputeShader(vam_nis_sharpen, sizeof(vam_nis_sharpen), nullptr, &nisSharpen));
        if (!sharpened) {
            D3D11_TEXTURE2D_DESC td{};
            output->GetDesc(&td);
            CheckNis(device->CreateTexture2D(&td, nullptr, &sharpened));
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            outputUav->GetDesc(&ud);
            CheckNis(device->CreateUnorderedAccessView(sharpened.Get(), &ud, &sharpenedUav));
        }
        if (!source) source = OutputSrv();
        NISConfig config{};
        if (!NVSharpenUpdateConfig(config, percent / 100.f, 0, 0, ow, oh, ow, oh, 0, 0, NISHDRMode::None))
            throw std::runtime_error("NVSharpen configuration failed");
        ContextScope scope(context.Get(), state.Get());
        VamWriteConstants(context, constants.Get(), &config, sizeof(config));
        context->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
        context->CSSetSamplers(0, 1, sampler.GetAddressOf());
        context->CSSetShaderResources(0, 1, &source);
        context->CSSetUnorderedAccessViews(0, 1, sharpenedUav.GetAddressOf(), nullptr);
        context->CSSetShader(nisSharpen.Get(), nullptr, 0);
        context->Dispatch((ow + 31) / 32, (oh + 31) / 32, 1);
    }
    // ReducedImages for lw x lh in `view`'s family (work: also NR's render target).
    void MakeReducedImages(ReducedImages& images, UINT lw, UINT lh, DXGI_FORMAT view) {
        images = ReducedImages{};
        const DXGI_FORMAT raw = view != DXGI_FORMAT_R16G16B16A16_FLOAT ? RawFormat(view) : view;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = lw; td.Height = lh;
        td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = ResourceFormat(view);
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = raw;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = raw;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        CheckNis(device->CreateTexture2D(&td, nullptr, &images.work));
        CheckNis(device->CreateShaderResourceView(images.work.Get(), &sd, &images.workSrv));
        CheckNis(device->CreateUnorderedAccessView(images.work.Get(), &ud, &images.workUav));
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = view;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        CheckNis(device->CreateRenderTargetView(images.work.Get(), &rd, &images.workRtv));
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        CheckNis(device->CreateTexture2D(&td, nullptr, &images.input));
        CheckNis(device->CreateShaderResourceView(images.input.Get(), &sd, &images.inputSrv));
        CheckNis(device->CreateUnorderedAccessView(images.input.Get(), &ud, &images.inputUav));
    }
    // kReducedShader: reduce (full -> images' work and input) or apply (full plus
    // the edit in images -> reduced.result).
    void ReducedPass(bool apply, ID3D11ShaderResourceView* full, const ReducedImages& images, const ReducedConstants& c) {
        ContextScope scope(context.Get(), state.Get());
        VamWriteConstants(context, reduced.constants.Get(), &c, sizeof(c));
        context->CSSetConstantBuffers(0, 1, reduced.constants.GetAddressOf());
        context->CSSetSamplers(0, 1, sampler.GetAddressOf());
        if (apply) {
            ID3D11ShaderResourceView* sources[] = {full, images.workSrv.Get(), images.inputSrv.Get()};
            context->CSSetShaderResources(0, 3, sources);
            context->CSSetUnorderedAccessViews(0, 1, reduced.resultUav.GetAddressOf(), nullptr);
            context->CSSetShader(reduced.apply.Get(), nullptr, 0);
            context->Dispatch((c.fullSize[0] + 7) / 8, (c.fullSize[1] + 7) / 8, 1);
        } else {
            context->CSSetShaderResources(0, 1, &full);
            ID3D11UnorderedAccessView* low[] = {images.workUav.Get(), images.inputUav.Get()};
            context->CSSetUnorderedAccessViews(0, 2, low, nullptr);
            context->CSSetShader(reduced.reduce.Get(), nullptr, 0);
            context->Dispatch((c.lowSize[0] + 7) / 8, (c.lowSize[1] + 7) / 8, 1);
        }
    }
    // [NR] PackEyes, eye 0 under RR (its reduced image already in reduced.low): eye
    // 1's output from RR ahead, reduced into ReducedAhead(), then NR once for both
    // (one canvas, VamNrPrepass::ProcessPair). guides: eye 0's at the reduced size,
    // or null when NR runs without; eye 1's then come from otherGuides (its
    // render-size guides made ahead). False: eye 1's not made, eye 0 runs NR on its
    // own (and eye 1 later on its own).
    bool PackReducedAhead(VamNrPrepass& nr, VamDlss& dlss, uint64_t serial, const VamFrameGuides* guides,
                          const std::function<bool(VamFrameGuides&)>& otherGuides, DXGI_FORMAT view, const ReducedConstants& c) {
        auto& a = ReducedAhead();
        if (VamViewCount() != 2 || !nr.PackEyes()) return false;
        const UINT ow = c.fullSize[0], oh = c.fullSize[1], lw = c.lowSize[0], lh = c.lowSize[1];
        if (a.ow != ow || a.oh != oh || a.lw != lw || a.lh != lh || a.view != view) {
            a.full.Reset(); a.fullSrv.Reset(); a.low = ReducedImages{};
            a.ow = 0;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = ow; td.Height = oh;
            td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
            td.Format = ResourceFormat(view);
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            CheckNis(device->CreateTexture2D(&td, nullptr, &a.full));
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = view != DXGI_FORMAT_R16G16B16A16_FLOAT ? RawFormat(view) : view;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            CheckNis(device->CreateShaderResourceView(a.full.Get(), &sd, &a.fullSrv));
            MakeReducedImages(a.low, lw, lh, view);
            a.ow = ow; a.oh = oh; a.lw = lw; a.lh = lh; a.view = view;
        }
        if (!dlss.UpscaledAhead(1, serial, a.full.Get(), ow, oh)) return false;
        VamFrameGuides render, other;
        if (guides && !(otherGuides && otherGuides(render) && render.serial == serial && dlss.PrepareOutputGuides(1, render, lw, lh, other)))
            return false;
        ReducedPass(false, a.fullSrv.Get(), a.low, c);
        ID3D11Texture2D* images[] = {reduced.low.work.Get(), a.low.work.Get()};
        ID3D11RenderTargetView* targets[] = {reduced.low.workRtv.Get(), a.low.workRtv.Get()};
        const VamFrameGuides* pair[] = {guides, guides ? &other : nullptr};
        if (!nr.ProcessPair(images, targets, view, pair)) return false;
        a.serial = serial;
        a.ready = true;
        return true;
    }
    // NRAfterUpscale with NRAfterScale < 100: NR on the output (`target`, the
    // swapchain image, `view` its RTV format) reduced to nrAfterScale percent per
    // axis, then the output plus NR's edit there (kReducedShader). NR then sees
    // the same pixel scale whatever SR's render size. False with nothing written
    // (the caller runs NR on the output itself) if it cannot run.
    // targetSrv: `target` read as stored, used in place of a copy of it. resultSrv:
    // the result stays in its own image (this view) and `target` is left as it was,
    // for a caller whose next pass reads it (NVSharpen) instead of a copy back.
    // upscaled: `target` holds this frame's DLSS output as it came ([NR] PackEyes: under
    // RR, eye 0 runs NR for both eyes' reduced images and eye 1 takes its edit, ReducedAheadEye;
    // otherGuides makes eye 1's render-size guides during eye 0's submission).
    bool DenoiseReduced(unsigned eye, ID3D11Texture2D* target, DXGI_FORMAT view, UINT ow, UINT oh,
                        VamNrPrepass& nr, VamDlss* dlss, const VamFrameGuides* guides,
                        ID3D11ShaderResourceView* targetSrv = nullptr, ID3D11ShaderResourceView** resultSrv = nullptr,
                        bool upscaled = false, const std::function<bool(VamFrameGuides&)>& otherGuides = {}) {
        auto& r = reduced;
        auto& ahead = ReducedAhead();
        // Eye 1 takes eye 0's packed evaluation of this very frame only.
        const bool aheadReady = eye == 1 && upscaled && ahead.ready && guides && guides->serial == ahead.serial;
        ahead.ready = false;
        if (r.failed) return false;
        try {
            const UINT lw = std::max(16u, (ow * nrAfterScale / 100 + 1) & ~1u);
            const UINT lh = std::max(16u, (oh * nrAfterScale / 100 + 1) & ~1u);
            const bool eightBit = view != DXGI_FORMAT_R16G16B16A16_FLOAT;
            const DXGI_FORMAT raw = eightBit ? RawFormat(view) : view;
            if (!r.reduce) {
                Ptr<ID3DBlob> blob, errors;
                for (const char* entry : {"reduce", "apply"}) {
                    if (FAILED(D3DCompile(kReducedShader, sizeof(kReducedShader) - 1, "vam_nr_reduced", nullptr, nullptr, entry, "cs_5_0",
                                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors)))
                        throw std::runtime_error(errors ? static_cast<const char*>(errors->GetBufferPointer()) : "reduced NR shader");
                    CheckNis(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                                         entry[0] == 'r' ? &r.reduce : &r.apply));
                }
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = sizeof(ReducedConstants);
                bd.Usage = D3D11_USAGE_DYNAMIC;
                bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                CheckNis(device->CreateBuffer(&bd, nullptr, &r.constants));
            }
            if (r.ow != ow || r.oh != oh || r.lw != lw || r.lh != lh || r.view != view) {
                r.full.Reset(); r.result.Reset(); r.fullSrv.Reset(); r.resultSrv.Reset(); r.resultUav.Reset();
                r.low = ReducedImages{};
                D3D11_TEXTURE2D_DESC td{};
                td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
                td.Format = ResourceFormat(view);
                D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = raw;
                sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sd.Texture2D.MipLevels = 1;
                D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = raw;
                ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
                td.Width = ow; td.Height = oh;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                CheckNis(device->CreateTexture2D(&td, nullptr, &r.full));
                CheckNis(device->CreateShaderResourceView(r.full.Get(), &sd, &r.fullSrv));
                td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
                CheckNis(device->CreateTexture2D(&td, nullptr, &r.result));
                CheckNis(device->CreateUnorderedAccessView(r.result.Get(), &ud, &r.resultUav));
                CheckNis(device->CreateShaderResourceView(r.result.Get(), &sd, &r.resultSrv));
                MakeReducedImages(r.low, lw, lh, view);
                r.ow = ow; r.oh = oh; r.lw = lw; r.lh = lh; r.view = view;
                r.logged = false;
            }
            ReducedConstants c{};
            c.lowSize[0] = lw; c.lowSize[1] = lh;
            c.fullSize[0] = ow; c.fullSize[1] = oh;
            c.taps = std::clamp((std::max(ow * 2 / lw, oh * 2 / lh) + 1) / 2, 1u, 4u);
            c.maxValue = eightBit ? 1.f : 65504.f;
            ID3D11ShaderResourceView* full = targetSrv;
            if (!full) {
                context->CopySubresourceRegion(r.full.Get(), 0, 0, 0, 0, target, 0, nullptr);
                full = r.fullSrv.Get();
            }
            const bool fromAhead = aheadReady && ahead.ow == ow && ahead.oh == oh && ahead.lw == lw && ahead.lh == lh && ahead.view == view;
            bool guided = false, packed = false;
            if (!fromAhead) {
                ReducedPass(false, full, r.low, c);
                VamFrameGuides reducedGuides;
                guided = guides && dlss && dlss->PrepareOutputGuides(eye, *guides, lw, lh, reducedGuides);
                packed = eye == 0 && upscaled && guides && dlss &&
                    PackReducedAhead(nr, *dlss, guides->serial, guided ? &reducedGuides : nullptr, otherGuides, view, c);
                if (!packed) nr.Process(r.low.work.Get(), r.low.workRtv.Get(), view, eye, guided ? &reducedGuides : nullptr);
            }
            ReducedPass(true, full, fromAhead ? ahead.low : r.low, c);
            if (resultSrv) *resultSrv = r.resultSrv.Get();
            else context->CopySubresourceRegion(target, 0, 0, 0, 0, r.result.Get(), 0, nullptr);
            if (!r.logged) {
                OOVR_LOGF("VAM NR after the upscaler eye=%u on the output reduced to %d%%: %ux%u -> %ux%u (%u x %u taps), guides %s; "
                          "the output takes NR's edit there (NRAfterScale)%s", eye, nrAfterScale, ow, oh, lw, lh, c.taps, c.taps,
                          guided ? "resampled to it" : "off", resultSrv ? ", read in place and passed on to NVSharpen without copies" : "");
                r.logged = true;
            }
            if ((packed || fromAhead) && !r.packLogged) {
                OOVR_LOGF("VAM NR after the upscaler eye=%u: %s (PackEyes)", eye, packed ?
                          "NR once for both eyes' reduced images, eye 1's output taken from RR ahead" : "the edit from eye 0's packed NR evaluation");
                r.packLogged = true;
            }
            return true;
        } catch (const std::exception& e) {
            r.failed = true;
            OOVR_LOGF("VAM NR on the reduced output off for this run (NR on the output itself instead): %s", e.what());
            return false;
        }
    }
    void Ensure(UINT w, UINT h, UINT ow, UINT oh, DXGI_FORMAT f) {
        if (w == width && h == height && ow == outWidth && oh == outHeight && f == format) return;
        work.Reset(); workRtv.Reset(); workSrv.Reset(); output.Reset(); outputUav.Reset(); outputRtv.Reset();
        fsrIntermediate.Reset(); fsrSrv.Reset(); fsrUav.Reset();
        outputSrv.Reset(); sharpened.Reset(); sharpenedUav.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h;
        td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = ResourceFormat(f);
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        CheckNis(device->CreateTexture2D(&td, nullptr, &work));
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = f; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        CheckNis(device->CreateRenderTargetView(work.Get(), &rd, &workRtv));
        D3D11_SHADER_RESOURCE_VIEW_DESC sr{};
        // Both SDR upscalers consume gamma-encoded values. Do not decode here.
        sr.Format = RawFormat(f); sr.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sr.Texture2D.MipLevels = 1;
        CheckNis(device->CreateShaderResourceView(work.Get(), &sr, &workSrv));
        td.Width = ow; td.Height = oh;
        td.Format = IsSrgb(f) ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_R16G16B16A16_TYPELESS;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
        CheckNis(device->CreateTexture2D(&td, nullptr, &output));
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = IsSrgb(f) ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        CheckNis(device->CreateUnorderedAccessView(output.Get(), &ud, &outputUav));
        outputView = IsSrgb(f) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R16G16B16A16_FLOAT;
        rd.Format = outputView;
        CheckNis(device->CreateRenderTargetView(output.Get(), &rd, &outputRtv));
        width = w; height = h; outWidth = ow; outHeight = oh; format = f;
        logged = false;
        OOVR_LOGF("VAM upscaler resources: NR/input=%ux%u output=%ux%u format=%u; NR -> %s -> XR submission", w, h, ow, oh, f, !upscale ? "Native" : (useFsr ? "FSR1" : "NIS"));
    }
};

VamNis::VamNis(ID3D11Device* device) : impl(std::make_unique<Impl>(device)) {}
VamNis::~VamNis() = default;

bool VamNis::Process(ID3D11Texture2D* source, UINT slice, const float* bounds,
    DXGI_FORMAT format, UINT inputWidth, UINT inputHeight,
    ID3D11RenderTargetView* destination, UINT outputWidth, UINT outputHeight,
    VamNrPrepass* nr, unsigned eye, VamDlss* dlss, uint64_t frameGenerationSwapchain) {
    auto& p = *impl;
    if(p.failed && VamDesktopDisplay() && GetTickCount64()>=p.retryAt){p.failed=false;p.width=0;}
    if (p.failed) return false;
    try {
        if (!p.initialized) p.Initialize();
        if (GetTickCount64() >= p.nextRead) {
            const auto settings = ReadVamPerformanceSettings();
            if (p.useFsr != settings.fsr || p.upscale != settings.upscale || p.sharpness != settings.sharpness || p.fsrSharpness != settings.fsrSharpness) p.logged = false;
            if (p.useDlss != settings.dlss) p.dlssLogged = false;
            const bool after = settings.nrAfterUpscale || settings.dlssPreset == 6;
            if (p.nrAfter != after) {
                OOVR_LOGF("VAM NR %s the upscaler (NRAfterUpscale=%d)", after ? "after" : "before", after ? 1 : 0);
                p.nrAfterLogged = p.nrAfterRefusedLogged = false;
            }
            p.nrAfter = after;
            if (p.nrAfterScale != settings.nrAfterScale) {
                OOVR_LOGF("VAM NR after the upscaler at %d%% of the output (NRAfterScale)", settings.nrAfterScale);
                p.reduced.logged = false;
            }
            p.nrAfterScale = settings.nrAfterScale;
            p.mipBias = settings.dlssMipBias;
            if (p.dlssSharpness != settings.dlssSharpness || p.dlssSharpenLast != settings.dlssSharpenLast) p.sharpenLogged = false;
            p.dlssSharpness = settings.dlssSharpness;
            p.dlssSharpenLast = settings.dlssSharpenLast;
            p.useFsr = settings.fsr;
            p.useDlss = settings.dlss;
            p.upscale = settings.upscale;
            p.sharpness = settings.sharpness;
            p.fsrSharpness = settings.fsrSharpness;
            p.nextRead = GetTickCount64() + 250;
        }
        p.Ensure(inputWidth, inputHeight, outputWidth, outputHeight, format);
        // FSR upscaling is disabled; its selected/fallback frames use the blitter.
        p.timer.Begin(p.device.Get(), p.context.Get(), eye);
        CheckNis(p.blitter.Copy(source, p.workRtv.Get(), format, inputWidth, inputHeight, slice, bounds));
        p.timer.Mark(StageCrop);
        // Bridge guides exist only in the DLSS launcher; NR uses them too.
        VamFrameGuides guides;
        const bool haveGuides = dlss && dlss->PrepareGuides(source, bounds, eye, inputWidth, inputHeight, guides);
        if (dlss) dlss->SetSelected(p.useDlss && p.upscale);
        p.timer.Mark(StageGuides);
        // DLSS Frame Generation in OFXR reads this frame's render-size guides
        // with the image the compositor is about to release.
        const auto publishGuides = [&] {
            // DLSS-G interpolates output-size images from render-size guides;
            // a render larger than the output gets optical flow instead.
            if (haveGuides && frameGenerationSwapchain && inputWidth <= outputWidth && inputHeight <= outputHeight)
                dlss->PublishFrameGeneration(frameGenerationSwapchain, eye, guides);
        };
        // Native frames (the bridge's FrameInfo flag 4): rendered for the output
        // size without jitter, with DLSSPreset=Off (v18; 100% with a model is
        // DLAA) or while VaM's menus are open. No DLSS SR: copied once Unity renders at that size, FSR1/NIS
        // until then. NR runs on them in either position, as on any frame:
        // frames resized for menus (flag 8) rebuild NR before the upscaler for
        // the native size on opening and back on closing, a pause only then.
        // v11 skipped NR on them to spare that, so with NR before the upscaler
        // NR vanished while VaM's menus were open.
        bool nativeFrame = haveGuides && guides.nativeFrame;
        if (eye < 2) {
            auto& t = p.native[eye];
            if (nativeFrame) {
                t.width = inputWidth; t.height = inputHeight; t.tail = 0;
            } else if (t.active && inputWidth == t.width && inputHeight == t.height && ++t.tail <= 8) {
                nativeFrame = true;     // still the native size: DLSS SR would build a feature for it
            }
            if (nativeFrame != t.active) {
                t.active = nativeFrame;
                if (++p.nativeSwitches <= 40 || p.nativeSwitches % 200 == 0)
                    OOVR_LOGF("VAM native frames %s eye=%u: input=%ux%u output=%ux%u (switch %llu)",
                        nativeFrame ? "on (SR off or VaM menus open: no DLSS SR)" : "off (DLSS SR again)",
                        eye, inputWidth, inputHeight, outputWidth, outputHeight, static_cast<unsigned long long>(p.nativeSwitches));
            }
        }
        // NRAfterUpscale on this path: NR on the destination after the upscaler
        // wrote it, at the output size (with the guides resampled to it).
        DXGI_FORMAT nrAfterView = DXGI_FORMAT_UNKNOWN;
        const auto nrAfterTarget = nr && p.nrAfter && p.upscale ? p.NrAfterTarget(destination, outputWidth, outputHeight, nrAfterView) : nullptr;
        if (nr && p.nrAfter && p.upscale && !nrAfterTarget && !p.nrAfterRefusedLogged) {
            OOVR_LOGF("VAM NR eye=%u stays before the upscaler: the destination is not a single %ux%u 2D RGBA8/RGBA16F image", eye, outputWidth, outputHeight);
            p.nrAfterRefusedLogged = true;
        }
        // With stagingView (NVSharpen last), the reduced NR reads the SR staging image
        // in place and its result goes to NVSharpen as it is: `sharpenSource`.
        ID3D11ShaderResourceView* sharpenSource = nullptr;
        // [NR] PackEyes after the upscaler: eye 1's guides made during eye 0's submission (as PackAhead's).
        const std::function<bool(VamFrameGuides&)> rightGuides = [&](VamFrameGuides& out) {
            if (VamViewCount() != 2 || eye != 0 || !haveGuides || bounds[0] != 0.f || bounds[2] != .5f) return false;
            const float right[4] = {.5f, bounds[1], 1.f, bounds[3]};
            return dlss->PrepareGuides(source, right, 1, inputWidth, inputHeight, out, true);
        };
        // upscaled: the image is DLSS's output of this frame as it came (not sharpened yet): [NR] PackEyes
        // after the upscaler (DenoiseReduced) can make the other eye's the same way.
        const auto denoiseOutput = [&](ID3D11RenderTargetView* stagingView = nullptr, bool upscaled = false) {
            if (!nrAfterTarget) return;
            // With sharpening, keep NR on the SR staging image so the last pass
            // sharpens the denoised result, without another full-resolution copy.
            auto target = stagingView ? p.output.Get() : nrAfterTarget.Get();
            auto view = stagingView ? p.outputView : nrAfterView;
            if (p.nrAfterScale < 100 && p.DenoiseReduced(eye, target, view, outputWidth, outputHeight, *nr, dlss,
                                                         haveGuides ? &guides : nullptr, stagingView ? p.OutputSrv() : nullptr,
                                                         stagingView ? &sharpenSource : nullptr, upscaled, rightGuides))
                return;
            VamFrameGuides outputGuides;
            const bool guided = haveGuides && dlss->PrepareOutputGuides(eye, guides, outputWidth, outputHeight, outputGuides);
            nr->Process(target, stagingView ? stagingView : destination, view, eye, guided ? &outputGuides : nullptr);
            if (!p.nrAfterLogged) {
                OOVR_LOGF("VAM NR after the upscaler eye=%u: NR on the %ux%u output, guides %s; its GPU time counts as the output stage",
                    eye, outputWidth, outputHeight, guided ? "resampled to the output" : "off");
                p.nrAfterLogged = true;
            }
        };
        // NR at render resolution, BEFORE the upscaler, unless NRAfterUpscale
        // (output-size NR costs (output / render)^2 as much).
        // PackEyes: eye 0 runs NR for both eyes, eye 1 takes its image.
        auto& ahead = Ahead();
        const bool aheadReady = eye == 1 && ahead.ready;
        if (eye == 1) ahead.ready = false;
        if (nr && !nrAfterTarget) {
            if (aheadReady && ahead.source == source && ahead.slice == slice && std::equal(bounds, bounds + 4, ahead.bounds) &&
                ahead.width == inputWidth && ahead.height == inputHeight && ahead.format == format && haveGuides && guides.serial == ahead.serial) {
                p.context->CopyResource(p.work.Get(), ahead.texture.Get());
                if (!p.aheadLogged) {
                    OOVR_LOGF("VAM NR eye=%u: image from eye 0's packed NR evaluation (PackEyes)", eye);
                    p.aheadLogged = true;
                }
            } else if (!(eye == 0 && haveGuides && p.PackAhead(source, slice, bounds, format, inputWidth, inputHeight, *nr, *dlss, guides))) {
                nr->Process(p.work.Get(), p.workRtv.Get(), format, eye, haveGuides ? &guides : nullptr);
            }
        }
        p.timer.Mark(StageNr);
        // The game's next frames: textures for the output while DLSS SR
        // upscales (log2(render / output) - 1), their own sampling otherwise.
        // FSR1 stand-in frames (no capture) leave it as it was. Unity renders
        // both eyes in one pass: the left eye's compositor decides (each eye's
        // reads the INI on its own 250 ms schedule, so after a live change the
        // two would disagree for a moment and the bias flip every frame).
        const bool biasEye = eye == 0;
        if (biasEye && (nativeFrame || !p.useDlss || !dlss)) VamMipBias::Set(0.f);
        if (nativeFrame && p.upscale && Impl::NearNative(inputWidth, inputHeight, outputWidth, outputHeight)) {
            const float full[] = {0, 0, 1, 1};
            p.timer.Mark(StageUpscale);
            CheckNis(p.blitter.Copy(p.work.Get(), destination, format, outputWidth, outputHeight, 0, full));
            denoiseOutput();
            p.timer.End();
            p.ReportSubmission(eye, inputWidth, inputHeight, outputWidth, outputHeight, true);
            publishGuides();
            return true;
        }
        if (p.upscale && p.useDlss && dlss && !nativeFrame) {
            const bool sharpen = p.dlssSharpness > 0 && IsSrgb(format);
            const auto direct = haveGuides && !sharpen ? p.DirectTarget(destination, outputWidth, outputHeight) : nullptr;
            if (haveGuides && dlss->Upscale(eye, p.work.Get(), guides, direct ? direct.Get() : p.output.Get(), outputWidth, outputHeight)) {
                // Upscale ends with the copy of its result into the target it was given.
                p.timer.Mark(StageUpscale);
                if (biasEye) VamMipBias::Set((std::log2(static_cast<float>(inputWidth) / outputWidth) - 1.f) * p.mipBias / 100.f);
                if (sharpen) {
                    if (p.dlssSharpenLast) denoiseOutput(p.outputRtv.Get(), true);
                    p.timer.Mark(StageDenoise);
                    p.Sharpen(outputWidth, outputHeight, p.dlssSharpness, sharpenSource);
                    p.timer.Mark(StageSharpen);
                    if (!p.sharpenLogged) {
                        OOVR_LOGF("VAM DLSS SR eye=%u: NVSharpen %d%% position=%s (DLSSSharpness, DLSSSharpenLast)",
                            eye, p.dlssSharpness, p.dlssSharpenLast ? "last" : "after-sr");
                        p.sharpenLogged = true;
                    }
                    const float full[] = {0, 0, 1, 1};
                    CheckNis(p.blitter.Copy(p.sharpened.Get(), destination, p.outputView, outputWidth, outputHeight, 0, full));
                    if (!p.dlssSharpenLast) denoiseOutput();
                } else {
                    if (!direct) {
                        const float full[] = {0, 0, 1, 1};
                        CheckNis(p.blitter.Copy(p.output.Get(), destination, p.outputView, outputWidth, outputHeight, 0, full));
                    }
                    denoiseOutput(nullptr, true);
                }
                p.timer.End();
                if (!p.directLogged) {
                    OOVR_LOGF("VAM DLSS SR eye=%u output %s", eye, direct ? "copied straight into the swapchain image" :
                        "copied, then blitted into the swapchain image (format or size differs)");
                    p.directLogged = true;
                }
                if (!p.dlssLogged) {
                    OOVR_LOGF("VAM DLSS SR eye=%u submitted after NR, before OFXR; input=%ux%u output=%ux%u", eye, inputWidth, inputHeight, outputWidth, outputHeight);
                    p.dlssLogged = true;
                }
                p.ReportSubmission(eye, inputWidth, inputHeight, outputWidth, outputHeight, true);
                publishGuides();
                return true;
            }
            // Missing capture for this frame (or DLSS failure): ordinary linear scaling.
            if (++p.dlssFallbacks == 1 || p.dlssFallbacks % 600 == 0)
                OOVR_LOGF("VAM DLSS SR fallback to linear blit eye=%u (guides=%d, count=%llu)", eye, haveGuides ? 1 : 0,
                    static_cast<unsigned long long>(p.dlssFallbacks));
        }
        if (!p.upscale || p.useFsr) {
            const float full[] = {0, 0, 1, 1};
            p.timer.Mark(StageUpscale);
            CheckNis(p.blitter.Copy(p.work.Get(), destination, format, outputWidth, outputHeight, 0, full));
            denoiseOutput();
            p.timer.End();
            if (!p.logged) {
                OOVR_LOGF("VAM linear blit eye=%u after optional NR; input=%ux%u output=%ux%u; FSR/NIS disabled", eye, inputWidth, inputHeight, outputWidth, outputHeight);
                p.logged = true;
            }
            p.ReportSubmission(eye, inputWidth, inputHeight, outputWidth, outputHeight, true);
            publishGuides();
            return true;
        }
        if (p.useFsr) {
            FsrConstants config{};
            FsrEasuCon(config.easu[0], config.easu[1], config.easu[2], config.easu[3],
                static_cast<float>(inputWidth), static_cast<float>(inputHeight),
                static_cast<float>(inputWidth), static_cast<float>(inputHeight),
                static_cast<float>(outputWidth), static_cast<float>(outputHeight));
            // vrperfkit-style UI mapping: 100 = strongest, 0 disables RCAS.
            FsrRcasCon(config.rcas, 2.f * (1.f - p.fsrSharpness));
            config.outputSize[0] = outputWidth; config.outputSize[1] = outputHeight;
            config.linearInput = IsSrgb(format) ? 0u : 1u;
            config.sharpen = p.fsrSharpness > 0.f ? 1u : 0u;
            ContextScope scope(p.context.Get(), p.state.Get());
            VamWriteConstants(p.context, p.fsrConstants.Get(), &config, sizeof(config));
            p.context->CSSetConstantBuffers(0, 1, p.fsrConstants.GetAddressOf());
            p.context->CSSetSamplers(0, 1, p.sampler.GetAddressOf());
            p.context->CSSetShaderResources(0, 1, p.workSrv.GetAddressOf());
            p.context->CSSetUnorderedAccessViews(0, 1, p.fsrUav.GetAddressOf(), nullptr);
            p.context->CSSetShader(p.easu.Get(), nullptr, 0);
            p.context->Dispatch((outputWidth + 15) / 16, (outputHeight + 15) / 16, 1);
            ID3D11UnorderedAccessView* none = nullptr;
            p.context->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
            p.context->CSSetShaderResources(0, 1, p.fsrSrv.GetAddressOf());
            p.context->CSSetUnorderedAccessViews(0, 1, p.outputUav.GetAddressOf(), nullptr);
            p.context->CSSetShader(p.rcas.Get(), nullptr, 0);
            p.context->Dispatch((outputWidth + 15) / 16, (outputHeight + 15) / 16, 1);
        } else {
        NISConfig config{};
        if (!NVScalerUpdateConfig(config, p.sharpness, 0, 0, inputWidth, inputHeight, inputWidth, inputHeight,
            0, 0, outputWidth, outputHeight, outputWidth, outputHeight, IsSrgb(format) ? NISHDRMode::None : NISHDRMode::Linear))
            throw std::runtime_error("NIS input/output ratio outside SDK range");
        {
            ContextScope scope(p.context.Get(), p.state.Get());
            VamWriteConstants(p.context, p.constants.Get(), &config, sizeof(config));
            p.context->CSSetConstantBuffers(0, 1, p.constants.GetAddressOf());
            p.context->CSSetSamplers(0, 1, p.sampler.GetAddressOf());
            ID3D11ShaderResourceView* srvs[] = {p.workSrv.Get(), p.coefficients[0].Get(), p.coefficients[1].Get()};
            p.context->CSSetShaderResources(0, 3, srvs);
            p.context->CSSetUnorderedAccessViews(0, 1, p.outputUav.GetAddressOf(), nullptr);
            p.context->CSSetShader(IsSrgb(format) ? p.sdr.Get() : p.linear.Get(), nullptr, 0);
            p.context->Dispatch((outputWidth + 31) / 32, (outputHeight + 23) / 24, 1);
        }
        }
        p.timer.Mark(StageUpscale);
        const float full[] = {0, 0, 1, 1};
        CheckNis(p.blitter.Copy(p.output.Get(), destination, p.outputView, outputWidth, outputHeight, 0, full));
        denoiseOutput();
        p.timer.End();
        if (!p.logged) {
            OOVR_LOGF("VAM %s eye=%u dispatch submitted after NR, before OFXR; sharpness=%.2f; input=%ux%u output=%ux%u", p.useFsr ? "FSR1 EASU+RCAS" : "NIS", eye, p.useFsr ? p.fsrSharpness : p.sharpness, inputWidth, inputHeight, outputWidth, outputHeight);
            p.logged = true;
        }
        p.ReportSubmission(eye, inputWidth, inputHeight, outputWidth, outputHeight, true);
        publishGuides();
        return true;
    } catch (const std::exception& e) {
        p.timer.Abandon();
        p.failed = true; p.retryAt=GetTickCount64()+1000;
        p.ReportSubmission(eye, inputWidth, inputHeight, outputWidth, outputHeight, false);
        OOVR_LOGF(VamDesktopDisplay()?"VAM desktop upscaler resource failure; retry in 1s: %s":"VAM upscaler disabled: %s; compositor will use its normal full-eye blit", e.what());
        return false;
    }
}
