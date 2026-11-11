// VaMToneMappingNative 0.2.2: bloom and the highlight shoulder (or, style 1, the film curve) of
// shaders\tonemap.hlsl on the eye camera's HDR target, in place, on Unity's render thread.
//
// The plugin's OnRenderImage binds its source (the camera's HDR target) as the render target and issues the render
// event (op 1, eyes in bits 16-23); this reads the bound target, runs the passes in compute into a texture of its own
// and copies that back over the target, which the plugin then blits to the camera's output. The pipeline state it
// touches (compute stage slots, output merger targets) is put back as it was. A target that is not a single-sampled
// float 2-D texture (HDR off or unavailable) is left alone and counted.
// 0.2.2: a target that takes a UAV (OpenComposite binds its RR image) gets the last pass written into it directly, each
// texel read and written by its own thread: the same values without the copy (and its 2 x 8 bytes per pixel).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "tm_shaders.h"

#define EXPORT extern "C" __declspec(dllexport)
#define TM_API_VERSION 2

namespace {

const int kMaxLevels = 7;
const int kOpTonemap = 1;

struct Pass {
    float src[4], add[4], dst[4], param[4], tint[4], film[4], look[4], cool[4], warm[4];
};

template <class T> void Release(T *&p) {
    if (p) p->Release();
    p = nullptr;
}

ID3D11Device *gDev = nullptr;
ID3D11DeviceContext *gCtx = nullptr;
ID3D11ComputeShader *gDownFirst = nullptr, *gDown = nullptr, *gUp = nullptr, *gComposite = nullptr, *gCompositeFilm = nullptr;
ID3D11ComputeShader *gCompositeInPlace = nullptr, *gCompositeFilmInPlace = nullptr;
ID3D11Buffer *gCb = nullptr;
ID3D11SamplerState *gSampler = nullptr;
bool gObjects = false;

// the textures for one target size and format
struct Targets {
    UINT w = 0, h = 0, eyes = 0;
    DXGI_FORMAT res = DXGI_FORMAT_UNKNOWN, view = DXGI_FORMAT_UNKNOWN;
    int levels = 0;
    UINT ew[kMaxLevels + 1] = {}, lh[kMaxLevels + 1] = {};
    ID3D11Texture2D *down[kMaxLevels + 1] = {}, *up[kMaxLevels + 1] = {}, *out = nullptr;
    ID3D11ShaderResourceView *downSrv[kMaxLevels + 1] = {}, *upSrv[kMaxLevels + 1] = {};
    ID3D11UnorderedAccessView *downUav[kMaxLevels + 1] = {}, *upUav[kMaxLevels + 1] = {}, *outUav = nullptr;
    void Free() {
        for (int k = 0; k <= kMaxLevels; k++) {
            Release(down[k]), Release(up[k]), Release(downSrv[k]), Release(upSrv[k]), Release(downUav[k]), Release(upUav[k]);
        }
        Release(out), Release(outUav);
        w = h = eyes = 0, levels = 0;
    }
} gT;

// the camera's target's own view (the render target pool hands out a few textures in turn)
struct SrcView {
    ID3D11Resource *res = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
};
SrcView gSrcViews[4];
int gSrcNext = 0;

// 0.2.2: a target written in place (one that takes a UAV, when its format can be loaded through one)
struct DstView {
    ID3D11Resource *res = nullptr;
    ID3D11UnorderedAccessView *uav = nullptr;
};
DstView gDstViews[4];
int gDstNext = 0;

// parameters (TM_SetParams)
std::atomic<float> gKnee{0.6f}, gThreshold{1.0f}, gSoftKnee{0.25f}, gIntensity{0.15f};
std::atomic<float> gTintR{1.0f}, gTintG{0.6f}, gTintB{0.4f};
// the style (TM_SetStyle): 0 the shoulder, 1 film; the film's parameters (TM_SetFilm)
std::atomic<int> gStyle{0};
// 0.2.1 (the user asked for it stronger): 0.2.0 had contrast 1.3, mid grey out 0.20, saturation 1.1, crosstalk 6, split 1
std::atomic<float> gContrast{1.5f}, gShoulder{1.0f}, gHdrMax{3.0f}, gMidIn{0.18f}, gMidOut{0.21f};
std::atomic<float> gSaturation{1.15f}, gCrosstalk{4.0f}, gSplit{2.0f};

// Lottes' curve y = z / (z^d b + c), z = x^a, through (midIn, midOut) and (hdrMax, 1)
void FilmCurve(float *f) {
    double a = gContrast.load(), d = gShoulder.load(), mi = gMidIn.load(), mo = gMidOut.load(), hm = gHdrMax.load();
    double m = std::pow(mi, a), h = std::pow(hm, a);
    double b = (h - m / mo) / (std::pow(h, d) - std::pow(m, d)), c = h - std::pow(h, d) * b;
    f[0] = (float)a, f[1] = (float)d, f[2] = (float)b, f[3] = (float)c;
}

// a tint scaled to luminance 1
void Tint(float *t, float r, float g, float b) {
    float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    t[0] = r / l, t[1] = g / l, t[2] = b / l, t[3] = 0.0f;
}

// statistics (TM_Read)
std::mutex gLock;
long long gFrames = 0, gSkipped = 0;
std::string gSkip = "", gError = "";
UINT gLastW = 0, gLastH = 0, gLastEyes = 0;
DXGI_FORMAT gLastFormat = DXGI_FORMAT_UNKNOWN;

// GPU time: a ring of timestamp pairs, read a few frames late without waiting
const int kRing = 4;
struct Timing {
    ID3D11Query *disjoint = nullptr, *begin = nullptr, *end = nullptr;
    bool pending = false;
} gRing[kRing];
int gRingNext = 0;
double gGpuSum = 0.0;
long long gGpuSamples = 0;

bool IsFloat(DXGI_FORMAT f, DXGI_FORMAT &res, DXGI_FORMAT &view) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        res = f, view = DXGI_FORMAT_R16G16B16A16_FLOAT;
        return true;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT:
        res = f, view = DXGI_FORMAT_R32G32B32A32_FLOAT;
        return true;
    default:
        return false;
    }
}

bool Tex(UINT w, UINT h, DXGI_FORMAT res, DXGI_FORMAT view, ID3D11Texture2D **t, ID3D11ShaderResourceView **srv,
         ID3D11UnorderedAccessView **uav) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w, d.Height = h, d.MipLevels = 1, d.ArraySize = 1, d.Format = res, d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT, d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | (srv ? D3D11_BIND_SHADER_RESOURCE : 0);
    if (FAILED(gDev->CreateTexture2D(&d, nullptr, t))) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC u = {};
    u.Format = view, u.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    if (FAILED(gDev->CreateUnorderedAccessView(*t, &u, uav))) return false;
    if (!srv) return true;
    D3D11_SHADER_RESOURCE_VIEW_DESC s = {};
    s.Format = view, s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, s.Texture2D.MipLevels = 1;
    return SUCCEEDED(gDev->CreateShaderResourceView(*t, &s, srv));
}

bool EnsureTargets(UINT w, UINT h, UINT eyes, DXGI_FORMAT res, DXGI_FORMAT view) {
    if (gT.out && gT.w == w && gT.h == h && gT.eyes == eyes && gT.res == res && gT.view == view) return true;
    gT.Free();
    gT.ew[0] = w / eyes, gT.lh[0] = h;
    int levels = 0;
    while (levels < kMaxLevels) {
        UINT ew = (gT.ew[levels] + 1) / 2, lh = (gT.lh[levels] + 1) / 2;
        if (ew < 8 || lh < 8) break;
        levels++;
        gT.ew[levels] = ew, gT.lh[levels] = lh;
    }
    if (levels < 1) return false;
    bool ok = Tex(w, h, res, view, &gT.out, nullptr, &gT.outUav);
    for (int k = 1; ok && k <= levels; k++) {
        ok = Tex(gT.ew[k] * eyes, gT.lh[k], DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, &gT.down[k], &gT.downSrv[k],
                 &gT.downUav[k]);
        if (ok && k < levels)
            ok = Tex(gT.ew[k] * eyes, gT.lh[k], DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, &gT.up[k], &gT.upSrv[k],
                     &gT.upUav[k]);
    }
    if (!ok) {
        gT.Free();
        return false;
    }
    gT.w = w, gT.h = h, gT.eyes = eyes, gT.res = res, gT.view = view, gT.levels = levels;
    return true;
}

ID3D11ShaderResourceView *SourceView(ID3D11Resource *res, DXGI_FORMAT view) {
    for (auto &s : gSrcViews)
        if (s.res == res && s.srv) return s.srv;
    SrcView &s = gSrcViews[gSrcNext];
    gSrcNext = (gSrcNext + 1) % 4;
    Release(s.srv), Release(s.res);
    D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = view, d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, d.Texture2D.MipLevels = 1;
    if (FAILED(gDev->CreateShaderResourceView(res, &d, &s.srv))) return nullptr;
    s.res = res;
    res->AddRef();
    return s.srv;
}

ID3D11UnorderedAccessView *TargetView(ID3D11Resource *res, DXGI_FORMAT view) {
    for (auto &d : gDstViews)
        if (d.res == res && d.uav) return d.uav;
    D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support = {view, 0};
    if (FAILED(gDev->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support, sizeof support)) ||
        !(support.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD))
        return nullptr;
    DstView &d = gDstViews[gDstNext];
    gDstNext = (gDstNext + 1) % 4;
    Release(d.uav), Release(d.res);
    D3D11_UNORDERED_ACCESS_VIEW_DESC u = {};
    u.Format = view, u.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    if (FAILED(gDev->CreateUnorderedAccessView(res, &u, &d.uav))) return nullptr;
    d.res = res;
    res->AddRef();
    return d.uav;
}

void Sizes(float *v, UINT ew, UINT h, UINT eyes) {
    v[0] = (float)ew, v[1] = (float)h, v[2] = 1.0f / (float)(ew * eyes), v[3] = 1.0f / (float)h;
}

void Dispatch(ID3D11ComputeShader *cs, ID3D11ShaderResourceView *t0, ID3D11ShaderResourceView *t1, ID3D11UnorderedAccessView *u0,
              const Pass &p, UINT w, UINT h) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(gCtx->Map(gCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, &p, sizeof p);
    gCtx->Unmap(gCb, 0);
    ID3D11ShaderResourceView *none[2] = {};
    gCtx->CSSetShaderResources(0, 2, none);
    gCtx->CSSetUnorderedAccessViews(0, 1, &u0, nullptr);
    ID3D11ShaderResourceView *srv[2] = {t0, t1};
    gCtx->CSSetShaderResources(0, 2, srv);
    gCtx->CSSetShader(cs, nullptr, 0);
    gCtx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    ID3D11UnorderedAccessView *noUav = nullptr;
    gCtx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
}

void Skip(const char *why) {
    std::lock_guard<std::mutex> l(gLock);
    gSkipped++;
    gSkip = why;
}

void ReadTimings() {
    for (auto &t : gRing) {
        if (!t.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 b, e;
        if (gCtx->GetData(t.disjoint, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (gCtx->GetData(t.begin, &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (gCtx->GetData(t.end, &e, sizeof e, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        t.pending = false;
        if (dj.Disjoint || dj.Frequency == 0 || e < b) continue;
        std::lock_guard<std::mutex> l(gLock);
        gGpuSum += (double)(e - b) * 1000.0 / (double)dj.Frequency;
        gGpuSamples++;
    }
}

void Tonemap(UINT eyes) {
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    gCtx->OMGetRenderTargets(1, &rtv, &dsv);
    if (!rtv) {
        if (dsv) dsv->Release();
        Skip("no render target bound");
        return;
    }
    D3D11_RENDER_TARGET_VIEW_DESC rd;
    rtv->GetDesc(&rd);
    ID3D11Resource *res = nullptr;
    rtv->GetResource(&res);
    D3D11_TEXTURE2D_DESC td = {};
    DXGI_FORMAT rf = DXGI_FORMAT_UNKNOWN, vf = DXGI_FORMAT_UNKNOWN;
    const char *why = nullptr;
    if (rd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || rd.Texture2D.MipSlice != 0) why = "target not a 2-D texture's first mip";
    else {
        ((ID3D11Texture2D *)res)->GetDesc(&td);
        if (td.SampleDesc.Count != 1) why = "multisampled target";
        else if (td.ArraySize != 1) why = "texture array target";
        else if (!IsFloat(td.Format, rf, vf) || (rd.Format != vf && rd.Format != td.Format)) why = "target not HDR (float)";
        else if (!(td.BindFlags & D3D11_BIND_SHADER_RESOURCE)) why = "target not readable";
        else if (eyes == 2 && (td.Width & 1)) why = "odd width for two eyes";
    }
    {
        std::lock_guard<std::mutex> l(gLock);
        gLastW = td.Width, gLastH = td.Height, gLastEyes = eyes, gLastFormat = td.Format;
    }
    ID3D11ShaderResourceView *src = nullptr;
    if (!why && !EnsureTargets(td.Width, td.Height, eyes, rf, vf)) why = "cannot make the textures";
    if (!why && !(src = SourceView(res, vf))) why = "cannot view the target";
    if (why) {
        Skip(why);
        rtv->Release(), res->Release();
        if (dsv) dsv->Release();
        return;
    }

    ReadTimings();
    Timing &tm = gRing[gRingNext];
    bool timed = !tm.pending && tm.disjoint;
    if (timed) {
        gCtx->Begin(tm.disjoint);
        gCtx->End(tm.begin);
    }

    // what this changes, to put back: the compute stage's slots and the targets
    ID3D11ComputeShader *oldCs = nullptr;
    ID3D11ClassInstance *oldCi[256];
    UINT oldCiCount = 256;
    gCtx->CSGetShader(&oldCs, oldCi, &oldCiCount);
    ID3D11ShaderResourceView *oldSrv[2] = {};
    gCtx->CSGetShaderResources(0, 2, oldSrv);
    ID3D11UnorderedAccessView *oldUav = nullptr;
    gCtx->CSGetUnorderedAccessViews(0, 1, &oldUav);
    ID3D11Buffer *oldCb = nullptr;
    gCtx->CSGetConstantBuffers(0, 1, &oldCb);
    ID3D11SamplerState *oldSampler = nullptr;
    gCtx->CSGetSamplers(0, 1, &oldSampler);
    gCtx->OMSetRenderTargets(0, nullptr, nullptr);

    gCtx->CSSetConstantBuffers(0, 1, &gCb);
    gCtx->CSSetSamplers(0, 1, &gSampler);
    const int L = gT.levels;
    Pass p = {};
    p.param[0] = gKnee.load(), p.param[1] = gThreshold.load(), p.param[2] = std::fmax(gSoftKnee.load(), 1e-4f);
    p.param[3] = gIntensity.load() / (float)L;
    p.tint[0] = gTintR.load(), p.tint[1] = gTintG.load(), p.tint[2] = gTintB.load();
    const bool film = gStyle.load() == 1;
    if (film) {
        FilmCurve(p.film);
        p.look[0] = gSaturation.load(), p.look[1] = gCrosstalk.load(), p.look[2] = gSplit.load();
        Tint(p.cool, 0.92f, 1.0f, 1.10f);  // the print's shadows: a little cyan
        Tint(p.warm, 1.06f, 1.0f, 0.90f);  // its highlights: a little warm
    }
    // down: level 1 from the target (threshold), then each from the one above
    for (int k = 1; k <= L; k++) {
        Sizes(p.src, gT.ew[k - 1], gT.lh[k - 1], eyes);
        Sizes(p.dst, gT.ew[k], gT.lh[k], eyes);
        p.dst[2] = (float)eyes;
        Dispatch(k == 1 ? gDownFirst : gDown, k == 1 ? src : gT.downSrv[k - 1], nullptr, gT.downUav[k], p, gT.ew[k] * eyes, gT.lh[k]);
    }
    // up: the sum of level k = its own downsample plus the tent of level k + 1's sum (the last level's sum is itself)
    for (int k = L - 1; k >= 1; k--) {
        ID3D11ShaderResourceView *below = k + 1 == L ? gT.downSrv[L] : gT.upSrv[k + 1];
        Sizes(p.src, gT.ew[k + 1], gT.lh[k + 1], eyes);
        Sizes(p.add, gT.ew[k], gT.lh[k], eyes);
        Sizes(p.dst, gT.ew[k], gT.lh[k], eyes);
        p.dst[2] = (float)eyes;
        Dispatch(gUp, below, gT.downSrv[k], gT.upUav[k], p, gT.ew[k] * eyes, gT.lh[k]);
    }
    // the picture
    Sizes(p.src, gT.ew[0], gT.lh[0], eyes);
    Sizes(p.add, gT.ew[1], gT.lh[1], eyes);
    Sizes(p.dst, gT.ew[0], gT.lh[0], eyes);
    p.dst[2] = (float)eyes;
    ID3D11ShaderResourceView *bloom = L == 1 ? gT.downSrv[1] : gT.upSrv[1];
    ID3D11UnorderedAccessView *inPlace = (td.BindFlags & D3D11_BIND_UNORDERED_ACCESS) ? TargetView(res, vf) : nullptr;
    if (inPlace) {
        Dispatch(film ? gCompositeFilmInPlace : gCompositeInPlace, nullptr, bloom, inPlace, p, td.Width, td.Height);
    } else {
        Dispatch(film ? gCompositeFilm : gComposite, src, bloom, gT.outUav, p, td.Width, td.Height);
        gCtx->CopyResource(res, gT.out);
    }

    gCtx->CSSetShader(oldCs, oldCi, oldCiCount);
    gCtx->CSSetShaderResources(0, 2, oldSrv);
    gCtx->CSSetUnorderedAccessViews(0, 1, &oldUav, nullptr);
    gCtx->CSSetConstantBuffers(0, 1, &oldCb);
    gCtx->CSSetSamplers(0, 1, &oldSampler);
    gCtx->OMSetRenderTargets(1, &rtv, dsv);
    Release(oldCs), Release(oldSrv[0]), Release(oldSrv[1]), Release(oldUav), Release(oldCb), Release(oldSampler);
    for (UINT i = 0; i < oldCiCount; i++) Release(oldCi[i]);

    if (timed) {
        gCtx->End(tm.end);
        gCtx->End(tm.disjoint);
        tm.pending = true;
        gRingNext = (gRingNext + 1) % kRing;
    }
    {
        std::lock_guard<std::mutex> l(gLock);
        gFrames++;
        gSkip = "";
    }
    rtv->Release(), res->Release();
    if (dsv) dsv->Release();
}

bool CreateObjects() {
    bool ok = SUCCEEDED(gDev->CreateComputeShader(kTmDownFirst, sizeof kTmDownFirst, nullptr, &gDownFirst)) &&
              SUCCEEDED(gDev->CreateComputeShader(kTmDown, sizeof kTmDown, nullptr, &gDown)) &&
              SUCCEEDED(gDev->CreateComputeShader(kTmUp, sizeof kTmUp, nullptr, &gUp)) &&
              SUCCEEDED(gDev->CreateComputeShader(kTmComposite, sizeof kTmComposite, nullptr, &gComposite)) &&
              SUCCEEDED(gDev->CreateComputeShader(kTmCompositeFilm, sizeof kTmCompositeFilm, nullptr, &gCompositeFilm)) &&
              SUCCEEDED(gDev->CreateComputeShader(kTmCompositeInPlace, sizeof kTmCompositeInPlace, nullptr, &gCompositeInPlace)) &&
              SUCCEEDED(gDev->CreateComputeShader(kTmCompositeFilmInPlace, sizeof kTmCompositeFilmInPlace, nullptr, &gCompositeFilmInPlace));
    D3D11_BUFFER_DESC b = {};
    b.ByteWidth = sizeof(Pass), b.Usage = D3D11_USAGE_DYNAMIC, b.BindFlags = D3D11_BIND_CONSTANT_BUFFER, b.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(gDev->CreateBuffer(&b, nullptr, &gCb));
    D3D11_SAMPLER_DESC s = {};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    ok = ok && SUCCEEDED(gDev->CreateSamplerState(&s, &gSampler));
    for (auto &t : gRing) {
        D3D11_QUERY_DESC q = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        ok = ok && SUCCEEDED(gDev->CreateQuery(&q, &t.disjoint));
        q.Query = D3D11_QUERY_TIMESTAMP;
        ok = ok && SUCCEEDED(gDev->CreateQuery(&q, &t.begin)) && SUCCEEDED(gDev->CreateQuery(&q, &t.end));
    }
    return ok;
}

void __stdcall OnRenderEvent(int id) {
    if (!gObjects || (id >> 24) != kOpTonemap) return;
    UINT eyes = (UINT)((id >> 16) & 0xff);
    Tonemap(eyes == 2 ? 2u : 1u);
}

}  // namespace

EXPORT int TM_ApiVersion() { return TM_API_VERSION; }

// the device from any of its resources (the plugin's own texture), once
EXPORT int TM_Init(void *resource) {
    if (gDev) return gObjects ? 0 : 3;
    if (!resource) return 1;
    ((ID3D11Resource *)resource)->GetDevice(&gDev);
    if (!gDev) return 2;
    gDev->GetImmediateContext(&gCtx);
    gObjects = CreateObjects();
    if (!gObjects) gError = "cannot create the shaders or buffers";
    return gObjects ? 0 : 3;
}

EXPORT void *TM_RenderEventFunc() { return (void *)OnRenderEvent; }

// test API: the curve's knee, the bloom's threshold, soft knee, intensity and tint
EXPORT void TM_SetParams(float knee, float threshold, float softKnee, float intensity, float r, float g, float b) {
    gKnee = std::fmin(std::fmax(knee, 0.0f), 0.95f), gThreshold = std::fmax(threshold, 0.0f), gSoftKnee = std::fmax(softKnee, 0.0f);
    gIntensity = std::fmax(intensity, 0.0f), gTintR = r, gTintG = g, gTintB = b;
}

// the style: 0 the highlight shoulder (0.1.0), 1 film
EXPORT void TM_SetStyle(int style) { gStyle = style == 1 ? 1 : 0; }

// test API: the film curve (contrast, shoulder, the input reaching white, mid grey in and out) and look (the mids'
// saturation, the crosstalk's exponent, the split tone's strength)
EXPORT void TM_SetFilm(float contrast, float shoulder, float hdrMax, float midIn, float midOut, float saturation, float crosstalk,
                       float split) {
    gContrast = std::fmax(contrast, 0.1f), gShoulder = std::fmax(shoulder, 0.1f), gHdrMax = std::fmax(hdrMax, 1.01f);
    gMidIn = std::fmin(std::fmax(midIn, 0.01f), 0.9f), gMidOut = std::fmin(std::fmax(midOut, 0.01f), 0.9f);
    gSaturation = std::fmax(saturation, 0.0f), gCrosstalk = std::fmax(crosstalk, 0.1f), gSplit = std::fmax(split, 0.0f);
}

// statistics as JSON; the GPU time averages over the frames since the last read
EXPORT int TM_Read(char *buf, int cap) {
    std::lock_guard<std::mutex> l(gLock);
    char s[1024];
    int n = snprintf(s, sizeof s,
                     "{\"api\":%d,\"objects\":%d,\"frames\":%lld,\"skipped\":%lld,\"skip\":\"%s\",\"error\":\"%s\",\"width\":%u,\"height\":%u,"
                     "\"eyes\":%u,\"format\":%d,\"levels\":%d,\"gpu_ms\":%.4f,\"gpu_samples\":%lld,\"knee\":%.3f,\"threshold\":%.3f,"
                     "\"soft_knee\":%.3f,\"intensity\":%.3f,\"style\":%d,\"film\":[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f]}",
                     TM_API_VERSION, gObjects ? 1 : 0, gFrames, gSkipped, gSkip.c_str(), gError.c_str(), gLastW, gLastH, gLastEyes,
                     (int)gLastFormat, gT.levels, gGpuSamples ? gGpuSum / (double)gGpuSamples : 0.0, gGpuSamples, gKnee.load(),
                     gThreshold.load(), gSoftKnee.load(), gIntensity.load(), gStyle.load(), gContrast.load(), gShoulder.load(),
                     gHdrMax.load(), gMidIn.load(), gMidOut.load(), gSaturation.load(), gCrosstalk.load(), gSplit.load());
    gGpuSum = 0.0, gGpuSamples = 0;
    if (n < 0) return 0;
    if (buf && cap > 0) {
        int m = n < cap - 1 ? n : cap - 1;
        memcpy(buf, s, (size_t)m);
        buf[m] = 0;
    }
    return n + 1;
}
