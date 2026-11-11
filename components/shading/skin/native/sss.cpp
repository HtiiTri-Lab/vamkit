// Screen-space subsurface scattering for VaM's skin (0.10.0; see sss.h and shaders\sss.hlsl).
//
// VaM lights skin per pixel (a wrapped N.L): light never moves sideways under the skin, so shadow edges stay hard and
// grey, creases (the lips' seam, the nostrils, the eyelids' folds) go black, and fine relief stays as sharp in red as in
// blue. Real skin carries red light millimetres sideways. Here the skin draws of the eye camera's opaque pass also write
// their direct diffuse light without the albedo (E), the albedo and their depth (the replacement programs,
// tools\gen_spec_table.py); when the opaque pass ends, E is blurred where the pixel is still skin with the measured skin
// profile (d'Eon & Luebke's six Gaussians) in millimetres on the skin, across and then down (separable, as Jimenez et
// al.'s screen-space subsurface scattering), and the colour gets albedo * (blurred E - E): only the direct diffuse light
// moves. The colour and depth are copied first (the passes read them while the camera's own views stay as they are).
#ifndef CINTERFACE
#define CINTERFACE
#endif
#ifndef COBJMACROS
#define COBJMACROS
#endif
#ifndef D3D11_NO_HELPERS
#define D3D11_NO_HELPERS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <string>
#include "sss.h"
#include "sss_shaders.h"

namespace {

ID3D11Device *gDev;
ID3D11ComputeShader *gPrep, *gBlur;
ID3D11VertexShader *gVS;
ID3D11PixelShader *gComp;
ID3D11RasterizerState *gRaster;
ID3D11DepthStencilState *gNoDepth;
ID3D11Buffer *gCbH, *gCbV, *gCbTH, *gCbTV;
std::map<std::string, ID3D11BlendState *> gBlends;  // the draw's blend state's description -> the derived one

float gStrength = 1.0f, gWidth = 1.0f, gTanHalfY = 1.17f;
unsigned gViewCount = 2;
float gTStrength = 0.0f, gTWidth = 1.0f;  // 0.20.0 translucency: on (its amount is in the skin programs), blur width
bool gCbDirty = true;
bool gRR = false;
float gRrDetail = .75f;
bool gRrMaterialReference = true;
bool gPathRoute=false;
ID3D11Resource *gPathCameraTarget=nullptr;
ID3D11Resource *gPathDepthTarget=nullptr;
bool gPathDepthReady=false;
struct RrTargets {
    ID3D11Texture2D *n = nullptr, *s = nullptr, *opaque = nullptr;
    ID3D11RenderTargetView *nv = nullptr, *sv = nullptr;
    void Release() {
        if(opaque) ID3D11Texture2D_Release(opaque);
        if(nv) ID3D11RenderTargetView_Release(nv);
        if(sv) ID3D11RenderTargetView_Release(sv);
        if(n) ID3D11Texture2D_Release(n);
        if(s) ID3D11Texture2D_Release(s);
        *this = RrTargets();
    }
} gRr;
int gLastRrReference = 0;
typedef int (__cdecl *RrProcessFn)(ID3D11Resource*, ID3D11Resource*, ID3D11Resource*, ID3D11Resource*, ID3D11Resource*, ID3D11Resource*, ID3D11Resource*, float, float);
typedef int (__cdecl *RrRegisterFn)(void (__cdecl*)());
RrProcessFn gRrProcess = nullptr;

// d'Eon & Luebke, GPU Gems 3 ch. 14 (as tools\scatter_profile.py): variances mm^2, weights per channel
const double kVar[6] = {0.0064, 0.0484, 0.187, 0.567, 1.99, 7.41};
const double kW[3][6] = {{0.233, 0.100, 0.118, 0.113, 0.358, 0.078},
                         {0.455, 0.336, 0.198, 0.007, 0.004, 0.000},
                         {0.649, 0.344, 0.000, 0.007, 0.000, 0.000}};
// 0.20.0: the translucency's blur, the same in every channel (its colour is the skin programs'): 10 mm and 22 mm
// Gaussians (x gTWidth), the wider a third - long enough that a glow cut by a sharp shadow fades over centimetres; the
// other four slots unused (weight 0)
const double kVarT[6] = {100.0, 484.0, 1.0, 1.0, 1.0, 1.0};
const double kWT[6] = {0.65, 0.35, 0.0, 0.0, 0.0, 0.0};
const int kTaps = 21;

#pragma pack(push, 4)
struct Cb {  // shaders\sss.hlsl SssCb
    UINT sizeEyes[4];
    float view[4];
    float dir[4];
    float invTwoVar[8];
    float wr[8], wg[8], wb[8];
    float tapO[kTaps * 4];
    float tapI[kTaps * 8];
};
#pragma pack(pop)

struct Targets {
    UINT w = 0, h = 0;
    unsigned workMode = 0;
    DXGI_FORMAT colorRes = DXGI_FORMAT_UNKNOWN, colorView = DXGI_FORMAT_UNKNOWN, depthRes = DXGI_FORMAT_UNKNOWN;
    UINT colorMips = 1, depthMips = 1;
    ID3D11Texture2D *e = 0, *a = 0, *z = 0, *tr = 0, *p = 0, *t = 0, *b = 0, *bw = 0, *colorCopy = 0, *depthCopy = 0;
    ID3D11RenderTargetView *eRtv = 0, *aRtv = 0, *zRtv = 0, *wRtv = 0;
    ID3D11ShaderResourceView *eSrv = 0, *aSrv = 0, *zSrv = 0, *wSrv = 0, *pSrv = 0, *tSrv = 0, *bSrv = 0, *bwSrv = 0, *colorSrv = 0,
                             *depthSrv = 0;
    ID3D11UnorderedAccessView *pUav = 0, *tUav = 0, *bUav = 0, *bwUav = 0;
    template<class T> static void Drop(T *&p) {if(p)IUnknown_Release((IUnknown*)p);p=nullptr;}
    void ReleaseScatter() {
        Drop(e);Drop(tr);Drop(p);Drop(t);Drop(b);Drop(bw);Drop(colorCopy);Drop(depthCopy);
        Drop(eRtv);Drop(wRtv);Drop(eSrv);Drop(wSrv);Drop(pSrv);Drop(tSrv);Drop(bSrv);Drop(bwSrv);
        Drop(colorSrv);Drop(depthSrv);Drop(pUav);Drop(tUav);Drop(bUav);Drop(bwUav);workMode=0;
    }
    void Release() {
        IUnknown *all[] = {(IUnknown *)e, (IUnknown *)a, (IUnknown *)z, (IUnknown *)tr, (IUnknown *)p, (IUnknown *)t, (IUnknown *)b,
                           (IUnknown *)bw, (IUnknown *)colorCopy, (IUnknown *)depthCopy, (IUnknown *)eRtv, (IUnknown *)aRtv,
                           (IUnknown *)zRtv, (IUnknown *)wRtv, (IUnknown *)eSrv, (IUnknown *)aSrv, (IUnknown *)zSrv, (IUnknown *)wSrv,
                           (IUnknown *)pSrv, (IUnknown *)tSrv, (IUnknown *)bSrv, (IUnknown *)bwSrv, (IUnknown *)colorSrv,
                           (IUnknown *)depthSrv, (IUnknown *)pUav, (IUnknown *)tUav, (IUnknown *)bUav, (IUnknown *)bwUav};
        for (IUnknown *u : all)
            if (u) IUnknown_Release(u);
        *this = Targets();
    }
};
Targets gT;

// the frame (between SssBegin and SssEnd)
bool gArmed, gPrepared, gFrameFailed;
ID3D11Resource *gColor, *gDepth;
ID3D11RenderTargetView *gColorRtv;
float gDepthTol;
int gBound, gSkipped;
const char *gSkip = "";

// status, under gStatLock
SRWLOCK gStatLock = SRWLOCK_INIT;
int gLastBound, gLastSkipped, gComposited, gGpuSamples,gLastScatterTargets,gLastGuideTargets;
double gGpuSumMs;
std::string gLastSkip = "", gInitError = "";

struct Timing {
    ID3D11Query *disjoint = 0, *begin = 0, *end = 0;
    bool pending = false;
};
Timing gTiming[4];
int gTimingNext;

void Collect(ID3D11DeviceContext *c) {
    for (Timing &t : gTiming) {
        if (!t.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 a, b;
        if (ID3D11DeviceContext_GetData(c, (ID3D11Asynchronous *)t.disjoint, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ID3D11DeviceContext_GetData(c, (ID3D11Asynchronous *)t.begin, &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ID3D11DeviceContext_GetData(c, (ID3D11Asynchronous *)t.end, &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        t.pending = false;
        if (dj.Disjoint || b < a) continue;
        AcquireSRWLockExclusive(&gStatLock);
        gGpuSumMs += (double)(b - a) * 1000.0 / (double)dj.Frequency;
        gGpuSamples++;
        ReleaseSRWLockExclusive(&gStatLock);
    }
}

double Phi(double x) { return 0.5 * erfc(-x / sqrt(2.0)); }

// translucency: the 0.20.0 blur (kVarT / kWT x gTWidth) instead of the skin profile
void FillCb(Cb &cb, float dx, float dy, bool translucency = false) {
    memset(&cb, 0, sizeof cb);
    cb.sizeEyes[0] = gT.w;
    cb.sizeEyes[1] = gT.h;
    cb.sizeEyes[2] = gViewCount;  // the eye camera renders both eyes side by side
    cb.view[0] = (float)(2.0 * gTanHalfY / std::max(1u, gT.h));
    cb.view[1] = gDepthTol;
    cb.view[2] = gStrength;
    cb.view[3] = gTStrength;
    cb.dir[0] = dx, cb.dir[1] = dy;
    double var[6], sigma[6], maxSigma = 0;
    for (int k = 0; k < 6; k++) {
        var[k] = translucency ? kVarT[k] * gTWidth * gTWidth : kVar[k] * gWidth * gWidth;
        sigma[k] = sqrt(var[k]);
        if (!translucency || kWT[k] > 0) maxSigma = std::max(maxSigma, sigma[k]);
        cb.invTwoVar[k] = (float)(1.0 / (2.0 * var[k]));
        if (translucency) cb.wr[k] = cb.wg[k] = cb.wb[k] = (float)kWT[k];
        else cb.wr[k] = (float)kW[0][k], cb.wg[k] = (float)kW[1][k], cb.wb[k] = (float)kW[2][k];
    }
    // taps: offset R s |s|, s = -1 .. 1 (denser near the centre), each covering half the way to its neighbours (the
    // outer ones out to infinity), with each Gaussian's share of that interval
    double R = 3.0 * maxSigma, o[kTaps];
    for (int i = 0; i < kTaps; i++) {
        double s = (i - kTaps / 2) / (double)(kTaps / 2);
        o[i] = R * s * fabs(s);
        cb.tapO[4 * i] = (float)o[i];
    }
    for (int i = 0; i < kTaps; i++) {
        double lo = i == 0 ? -1e30 : 0.5 * (o[i - 1] + o[i]), hi = i == kTaps - 1 ? 1e30 : 0.5 * (o[i] + o[i + 1]);
        for (int k = 0; k < 6; k++) cb.tapI[8 * i + k] = (float)(Phi(hi / sigma[k]) - Phi(lo / sigma[k]));
    }
}

void UpdateCb(ID3D11DeviceContext *c) {
    Cb cb;
    FillCb(cb, 1, 0);
    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)gCbH, 0, NULL, &cb, 0, 0);
    FillCb(cb, 0, 1);
    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)gCbV, 0, NULL, &cb, 0, 0);
    FillCb(cb, 1, 0, true);
    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)gCbTH, 0, NULL, &cb, 0, 0);
    FillCb(cb, 0, 1, true);
    ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)gCbTV, 0, NULL, &cb, 0, 0);
    gCbDirty = false;
}

// depth view format -> the resource's typeless group, the shader view, the tolerance between SV_Position.z and it
bool DepthFormats(DXGI_FORMAT view, DXGI_FORMAT &res, DXGI_FORMAT &srv, float &tol) {
    switch (view) {
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        res = DXGI_FORMAT_R24G8_TYPELESS, srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS, tol = 4.0f / 16777215.0f;
        return true;
    case DXGI_FORMAT_D32_FLOAT:
        res = DXGI_FORMAT_R32_TYPELESS, srv = DXGI_FORMAT_R32_FLOAT, tol = 1e-7f;
        return true;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        res = DXGI_FORMAT_R32G8X24_TYPELESS, srv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, tol = 1e-7f;
        return true;
    case DXGI_FORMAT_D16_UNORM:
        res = DXGI_FORMAT_R16_TYPELESS, srv = DXGI_FORMAT_R16_UNORM, tol = 4.0f / 65535.0f;
        return true;
    default:
        return false;
    }
}

bool Tex(UINT w, UINT h, DXGI_FORMAT f, UINT bind, UINT mips, ID3D11Texture2D **t) {
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w, d.Height = h, d.MipLevels = mips, d.ArraySize = 1, d.Format = f, d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT, d.BindFlags = bind;
    return SUCCEEDED(ID3D11Device_CreateTexture2D(gDev, &d, NULL, t));
}

bool Srv(ID3D11Texture2D *t, DXGI_FORMAT f, ID3D11ShaderResourceView **v) {
    D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = f, d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, d.Texture2D.MipLevels = 1;
    return SUCCEEDED(ID3D11Device_CreateShaderResourceView(gDev, (ID3D11Resource *)t, &d, v));
}

// Base reconstruction attributes survive independently of scattering work.
bool EnsureTargets(UINT w, UINT h, DXGI_FORMAT colorRes, DXGI_FORMAT colorView, UINT colorMips, DXGI_FORMAT depthRes,
                   DXGI_FORMAT depthSrv, UINT depthMips) {
    const UINT rt=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    const UINT cs=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE;
    if(!gT.a || gT.w!=w || gT.h!=h || gT.colorRes!=colorRes || gT.colorView!=colorView ||
       gT.colorMips!=colorMips || gT.depthRes!=depthRes || gT.depthMips!=depthMips) {
        gT.Release();gRr.Release();
        bool ok=Tex(w,h,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,rt,1,&gT.a) &&
            Tex(w,h,DXGI_FORMAT_R32_FLOAT,rt,1,&gT.z) &&
            SUCCEEDED(ID3D11Device_CreateRenderTargetView(gDev,(ID3D11Resource*)gT.a,nullptr,&gT.aRtv)) &&
            SUCCEEDED(ID3D11Device_CreateRenderTargetView(gDev,(ID3D11Resource*)gT.z,nullptr,&gT.zRtv)) &&
            Srv(gT.a,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,&gT.aSrv) && Srv(gT.z,DXGI_FORMAT_R32_FLOAT,&gT.zSrv);
        if(!ok){gT.Release();return false;}
        gT.w=w;gT.h=h;gT.colorRes=colorRes;gT.colorView=colorView;gT.colorMips=colorMips;
        gT.depthRes=depthRes;gT.depthMips=depthMips;gCbDirty=true;
    }
    unsigned mode=(gStrength>0?1u:0u)|(gTStrength>0?2u:0u);
    if(gT.workMode!=mode){gT.ReleaseScatter();gCbDirty=true;}
    if(!mode || (gT.workMode==mode && gT.p))return true;
    bool ok=Tex(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,rt,1,&gT.e) &&
        SUCCEEDED(ID3D11Device_CreateRenderTargetView(gDev,(ID3D11Resource*)gT.e,nullptr,&gT.eRtv)) &&
        Srv(gT.e,DXGI_FORMAT_R16G16B16A16_FLOAT,&gT.eSrv) &&
        Tex(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,cs,1,&gT.p) &&
        Tex(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,cs,1,&gT.t) &&
        Srv(gT.p,DXGI_FORMAT_R16G16B16A16_FLOAT,&gT.pSrv) && Srv(gT.t,DXGI_FORMAT_R16G16B16A16_FLOAT,&gT.tSrv) &&
        SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(gDev,(ID3D11Resource*)gT.p,nullptr,&gT.pUav)) &&
        SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(gDev,(ID3D11Resource*)gT.t,nullptr,&gT.tUav)) &&
        Tex(w,h,colorRes,D3D11_BIND_SHADER_RESOURCE,colorMips,&gT.colorCopy) &&
        Tex(w,h,depthRes,D3D11_BIND_SHADER_RESOURCE,depthMips,&gT.depthCopy) &&
        Srv(gT.colorCopy,colorView,&gT.colorSrv) && Srv(gT.depthCopy,depthSrv,&gT.depthSrv);
    if(ok && (mode&1))ok=Tex(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,cs,1,&gT.b) &&
        Srv(gT.b,DXGI_FORMAT_R16G16B16A16_FLOAT,&gT.bSrv) &&
        SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(gDev,(ID3D11Resource*)gT.b,nullptr,&gT.bUav));
    if(ok && (mode&2))ok=Tex(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,rt,1,&gT.tr) &&
        Tex(w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,cs,1,&gT.bw) &&
        SUCCEEDED(ID3D11Device_CreateRenderTargetView(gDev,(ID3D11Resource*)gT.tr,nullptr,&gT.wRtv)) &&
        Srv(gT.tr,DXGI_FORMAT_R16G16B16A16_FLOAT,&gT.wSrv) && Srv(gT.bw,DXGI_FORMAT_R16G16B16A16_FLOAT,&gT.bwSrv) &&
        SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(gDev,(ID3D11Resource*)gT.bw,nullptr,&gT.bwUav));
    if(!ok){gT.ReleaseScatter();return false;}
    gT.workMode=mode;return true;
}

// The first skin draw of the pass: the camera's colour and depth, the targets for them, the depth target cleared.
bool Prepare(ID3D11DeviceContext *c, ID3D11RenderTargetView *rtv, ID3D11DepthStencilView *dsv) {
    D3D11_RENDER_TARGET_VIEW_DESC rd;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd;
    ID3D11RenderTargetView_GetDesc(rtv, &rd);
    ID3D11DepthStencilView_GetDesc(dsv, &dd);
    if (rd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || rd.Texture2D.MipSlice != 0) {
        gSkip = "colour view not a 2-D texture's first mip";
        return false;
    }
    DXGI_FORMAT depthRes, depthSrv;
    if (dd.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D || dd.Texture2D.MipSlice != 0 || !DepthFormats(dd.Format, depthRes, depthSrv, gDepthTol)) {
        gSkip = "depth view not a 2-D texture with a known format";
        return false;
    }
    ID3D11Resource *cr = NULL, *dr = NULL;
    ID3D11View_GetResource((ID3D11View *)rtv, &cr);
    ID3D11View_GetResource((ID3D11View *)dsv, &dr);
    D3D11_TEXTURE2D_DESC cd = {}, ddsc = {};
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)cr, &cd);
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)dr, &ddsc);
    bool ok = true;
    if (cd.SampleDesc.Count != 1 || ddsc.SampleDesc.Count != 1) gSkip = "MSAA", ok = false;
    else if (cd.ArraySize != 1 || ddsc.ArraySize != 1) gSkip = "texture arrays", ok = false;
    else if (cd.Width != ddsc.Width || cd.Height != ddsc.Height) gSkip = "colour and depth sizes differ", ok = false;
    else if (!EnsureTargets(cd.Width, cd.Height, cd.Format, rd.Format, cd.MipLevels, depthRes, depthSrv, ddsc.MipLevels))
        gSkip = "cannot make the targets", ok = false;
    if (!ok) {
        ID3D11Resource_Release(cr);
        ID3D11Resource_Release(dr);
        return false;
    }
    if (gRR && !gRr.n) {
        const UINT bind = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (!Tex(cd.Width, cd.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, bind, 1, &gRr.n) ||
            !Tex(cd.Width, cd.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, bind, 1, &gRr.s) ||
            !Tex(cd.Width, cd.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, 1, &gRr.opaque) ||
            FAILED(ID3D11Device_CreateRenderTargetView(gDev, (ID3D11Resource*)gRr.n, nullptr, &gRr.nv)) ||
            FAILED(ID3D11Device_CreateRenderTargetView(gDev, (ID3D11Resource*)gRr.s, nullptr, &gRr.sv))) {
            gRr.Release(); ID3D11Resource_Release(cr); ID3D11Resource_Release(dr); return false;
        }
    }
    static const FLOAT none[4] = {-1, -1, -1, -1};
    ID3D11DeviceContext_ClearRenderTargetView(c, gT.zRtv, none);
    if (gRR) {
        static const FLOAT zero[4] = {};
        ID3D11DeviceContext_ClearRenderTargetView(c, gRr.nv, zero);
    }
    gColor = cr, gDepth = dr;
    gColorRtv = rtv;
    ID3D11RenderTargetView_AddRef(rtv);
    gPrepared = true;
    return true;
}

// The draw's blend state with the subsurface targets: E as the light (base pass written, light passes added; its w,
// the distance, written by the base pass only), the albedo and depth written by the base pass only.
ID3D11BlendState *Derived(ID3D11BlendState *bs,bool guidesOnly) {
    D3D11_BLEND_DESC d = {};
    if (bs) ID3D11BlendState_GetDesc(bs, &d);
    else {
        for (auto &r : d.RenderTarget) {
            r.SrcBlend = r.SrcBlendAlpha = D3D11_BLEND_ONE;
            r.DestBlend = r.DestBlendAlpha = D3D11_BLEND_ZERO;
            r.BlendOp = r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            r.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        }
    }
    std::string key((const char *)&d, sizeof d);
    key.push_back(gRR ? 1 : 0);
    key.push_back(guidesOnly ? 1 : 0);
    auto it = gBlends.find(key);
    if (it != gBlends.end()) return it->second;
    D3D11_BLEND_DESC o = d;
    o.IndependentBlendEnable = TRUE;
    const D3D11_RENDER_TARGET_BLEND_DESC r0 = d.RenderTarget[0];
    bool add = r0.BlendEnable && r0.DestBlend != D3D11_BLEND_ZERO;
    D3D11_RENDER_TARGET_BLEND_DESC write = {};
    write.SrcBlend = write.SrcBlendAlpha = D3D11_BLEND_ONE;
    write.DestBlend = write.DestBlendAlpha = D3D11_BLEND_ZERO;
    write.BlendOp = write.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    write.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    D3D11_RENDER_TARGET_BLEND_DESC light = write;
    if (add) {
        light.BlendEnable = TRUE;
        light.SrcBlend = r0.SrcBlend, light.DestBlend = r0.DestBlend, light.BlendOp = r0.BlendOp;
        light.SrcBlendAlpha = D3D11_BLEND_ZERO, light.DestBlendAlpha = D3D11_BLEND_ONE;
    }
    D3D11_RENDER_TARGET_BLEND_DESC keep = write;
    keep.RenderTargetWriteMask = 0;
    o.RenderTarget[0] = r0;
    o.RenderTarget[1] = o.RenderTarget[4] = light;  // E and the translucency (0.20.0) sum the lights
    if(guidesOnly) {
        D3D11_RENDER_TARGET_BLEND_DESC zero=write;zero.BlendEnable=TRUE;
        zero.SrcBlend=zero.DestBlend=zero.SrcBlendAlpha=zero.DestBlendAlpha=D3D11_BLEND_ZERO;
        o.RenderTarget[1]=o.RenderTarget[4]=zero;
    }
    o.RenderTarget[2] = o.RenderTarget[3] = add ? keep : write;
    for (int i = 5; i < 8; i++) o.RenderTarget[i] = keep;
    if (gRR) o.RenderTarget[5] = o.RenderTarget[6] = add ? keep : write;
    ID3D11BlendState *out = NULL;
    if (FAILED(ID3D11Device_CreateBlendState(gDev, &o, &out))) out = NULL;
    gBlends[key] = out;
    return out;
}

void ReleaseFrame() {
    if(gPathCameraTarget)ID3D11Resource_Release(gPathCameraTarget);gPathCameraTarget=nullptr;
    if(gPathDepthTarget)ID3D11Resource_Release(gPathDepthTarget);gPathDepthTarget=nullptr;gPathDepthReady=false;
    if (gColor) ID3D11Resource_Release(gColor);
    if (gDepth) ID3D11Resource_Release(gDepth);
    if (gColorRtv) ID3D11RenderTargetView_Release(gColorRtv);
    gColor = gDepth = NULL;
    gColorRtv = NULL;
    gPrepared = false;
}

// the graphics pipeline state the composite draw changes
struct GfxSave {
    ID3D11InputLayout *il;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11VertexShader *vs;
    ID3D11GeometryShader *gs;
    ID3D11HullShader *hs;
    ID3D11DomainShader *ds;
    ID3D11PixelShader *ps;
    ID3D11ShaderResourceView *srv[5];
    ID3D11Buffer *cb;
    ID3D11RasterizerState *rs;
    UINT nvp;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    ID3D11RenderTargetView *rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView *dsv;
    ID3D11BlendState *blend;
    FLOAT factor[4];
    UINT mask;
    ID3D11DepthStencilState *depth;
    UINT ref;
};

void SaveGfx(ID3D11DeviceContext *c, GfxSave &s) {
    memset(&s, 0, sizeof s);
    ID3D11DeviceContext_IAGetInputLayout(c, &s.il);
    ID3D11DeviceContext_IAGetPrimitiveTopology(c, &s.topo);
    ID3D11DeviceContext_VSGetShader(c, &s.vs, NULL, NULL);
    ID3D11DeviceContext_GSGetShader(c, &s.gs, NULL, NULL);
    ID3D11DeviceContext_HSGetShader(c, &s.hs, NULL, NULL);
    ID3D11DeviceContext_DSGetShader(c, &s.ds, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(c, &s.ps, NULL, NULL);
    ID3D11DeviceContext_PSGetShaderResources(c, 0, 5, s.srv);
    ID3D11DeviceContext_PSGetConstantBuffers(c, 0, 1, &s.cb);
    ID3D11DeviceContext_RSGetState(c, &s.rs);
    s.nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11DeviceContext_RSGetViewports(c, &s.nvp, s.vp);
    ID3D11DeviceContext_OMGetRenderTargets(c, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, &s.dsv);
    ID3D11DeviceContext_OMGetBlendState(c, &s.blend, s.factor, &s.mask);
    ID3D11DeviceContext_OMGetDepthStencilState(c, &s.depth, &s.ref);
}

template <class T>
void Rel(T *p) {
    if (p) IUnknown_Release((IUnknown *)p);
}

void RestoreGfx(ID3D11DeviceContext *c, GfxSave &s, SssSetPSFn setPS) {
    ID3D11DeviceContext_IASetInputLayout(c, s.il);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, s.topo);
    ID3D11DeviceContext_VSSetShader(c, s.vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(c, s.gs, NULL, 0);
    ID3D11DeviceContext_HSSetShader(c, s.hs, NULL, 0);
    ID3D11DeviceContext_DSSetShader(c, s.ds, NULL, 0);
    setPS(c, s.ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 5, s.srv);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 1, &s.cb);
    ID3D11DeviceContext_RSSetState(c, s.rs);
    ID3D11DeviceContext_RSSetViewports(c, s.nvp, s.vp);
    ID3D11DeviceContext_OMSetRenderTargets(c, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, s.dsv);
    ID3D11DeviceContext_OMSetBlendState(c, s.blend, s.factor, s.mask);
    ID3D11DeviceContext_OMSetDepthStencilState(c, s.depth, s.ref);
    Rel(s.il), Rel(s.vs), Rel(s.gs), Rel(s.hs), Rel(s.ds), Rel(s.ps), Rel(s.cb), Rel(s.rs), Rel(s.dsv), Rel(s.blend), Rel(s.depth);
    for (auto v : s.srv) Rel(v);
    for (auto v : s.rtv) Rel(v);
}

void RunPasses(ID3D11DeviceContext *c, SssSetPSFn setPS) {
    Collect(c);
    Timing &tm = gTiming[gTimingNext];
    bool timed = tm.disjoint && !tm.pending;
    if (timed) {
        ID3D11DeviceContext_Begin(c, (ID3D11Asynchronous *)tm.disjoint);
        ID3D11DeviceContext_End(c, (ID3D11Asynchronous *)tm.begin);
    }
    if (gCbDirty) UpdateCb(c);
    ID3D11DeviceContext_CopyResource(c, (ID3D11Resource *)gT.depthCopy, gDepth);
    ID3D11DeviceContext_CopyResource(c, (ID3D11Resource *)gT.colorCopy, gColor);

    // compute: prep, blur across, blur down (the compute stage's bindings put back after)
    ID3D11ComputeShader *ocs = NULL;
    ID3D11ShaderResourceView *osrv[3] = {};
    ID3D11UnorderedAccessView *ouav = NULL;
    ID3D11Buffer *ocb = NULL;
    ID3D11DeviceContext_CSGetShader(c, &ocs, NULL, NULL);
    ID3D11DeviceContext_CSGetShaderResources(c, 0, 3, osrv);
    ID3D11DeviceContext_CSGetUnorderedAccessViews(c, 0, 1, &ouav);
    ID3D11DeviceContext_CSGetConstantBuffers(c, 0, 1, &ocb);
    UINT gx = (gT.w + 7) / 8, gy = (gT.h + 7) / 8;
    ID3D11ShaderResourceView *none3[3] = {NULL, NULL, NULL};
    ID3D11UnorderedAccessView *noUav = NULL;
    if(gStrength>0) {
    {
        ID3D11ShaderResourceView *srv[3] = {gT.eSrv, gT.zSrv, gT.depthSrv};
        ID3D11DeviceContext_CSSetShader(c, gPrep, NULL, 0);
        ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 1, &gCbH);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &gT.pUav, NULL);
        ID3D11DeviceContext_CSSetShaderResources(c, 0, 3, srv);
        ID3D11DeviceContext_Dispatch(c, gx, gy, 1);
    }
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 3, none3);
    ID3D11DeviceContext_CSSetShader(c, gBlur, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(c, 1, 1, &gT.aSrv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &gT.tUav, NULL);
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 1, &gT.pSrv);
    ID3D11DeviceContext_Dispatch(c, gx, gy, 1);
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 1, none3);
    ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 1, &gCbV);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &gT.bUav, NULL);
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 1, &gT.tSrv);
    ID3D11DeviceContext_Dispatch(c, gx, gy, 1);
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 1, none3);
    }
    bool translucency = gTStrength > 0;
    if (translucency) {  // 0.20.0: the translucency the same way (prep into p again, across into t, down into bw)
        ID3D11DeviceContext_CSSetShaderResources(c, 1, 1, none3);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &gT.pUav, NULL);
        ID3D11ShaderResourceView *srv[3] = {gT.wSrv, gT.zSrv, gT.depthSrv};
        ID3D11DeviceContext_CSSetShader(c, gPrep, NULL, 0);
        ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 1, &gCbTH);
        ID3D11DeviceContext_CSSetShaderResources(c, 0, 3, srv);
        ID3D11DeviceContext_Dispatch(c, gx, gy, 1);
        ID3D11DeviceContext_CSSetShaderResources(c, 0, 3, none3);
        ID3D11DeviceContext_CSSetShader(c, gBlur, NULL, 0);
        ID3D11DeviceContext_CSSetShaderResources(c, 1, 1, &gT.aSrv);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &gT.tUav, NULL);
        ID3D11DeviceContext_CSSetShaderResources(c, 0, 1, &gT.pSrv);
        ID3D11DeviceContext_Dispatch(c, gx, gy, 1);
        ID3D11DeviceContext_CSSetShaderResources(c, 0, 1, none3);
        ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 1, &gCbTV);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &gT.bwUav, NULL);
        ID3D11DeviceContext_CSSetShaderResources(c, 0, 1, &gT.tSrv);
        ID3D11DeviceContext_Dispatch(c, gx, gy, 1);
        ID3D11DeviceContext_CSSetShaderResources(c, 0, 1, none3);
    }
    ID3D11DeviceContext_CSSetShaderResources(c, 1, 1, none3);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &noUav, NULL);
    ID3D11DeviceContext_CSSetShader(c, ocs, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(c, 0, 3, osrv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &ouav, NULL);
    ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 1, &ocb);
    Rel(ocs), Rel(ouav), Rel(ocb);
    for (auto v : osrv) Rel(v);

    // composite: a full-screen triangle into the camera's colour, non-skin pixels discarded
    GfxSave s;
    SaveGfx(c, s);
    ID3D11DeviceContext_IASetInputLayout(c, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(c, gVS, NULL, 0);
    ID3D11DeviceContext_GSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_HSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_DSSetShader(c, NULL, NULL, 0);
    setPS(c, gComp, NULL, 0);
    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &gColorRtv, NULL);
    ID3D11ShaderResourceView *srv[5] = {gT.colorSrv, gT.eSrv, gT.aSrv, gT.bSrv, translucency ? gT.bwSrv : gT.bSrv};  // view.w 0 when off
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 5, srv);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 1, &gCbH);
    ID3D11DeviceContext_RSSetState(c, gRaster);
    D3D11_VIEWPORT vp = {0, 0, (FLOAT)gT.w, (FLOAT)gT.h, 0, 1};
    ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
    static const FLOAT zero[4] = {0, 0, 0, 0};
    ID3D11DeviceContext_OMSetBlendState(c, NULL, zero, 0xffffffff);
    ID3D11DeviceContext_OMSetDepthStencilState(c, gNoDepth, 0);
    ID3D11DeviceContext_Draw(c, 3, 0);
    ID3D11ShaderResourceView *none5[5] = {NULL, NULL, NULL, NULL, NULL};
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 5, none5);
    RestoreGfx(c, s, setPS);

    if (timed) {
        ID3D11DeviceContext_End(c, (ID3D11Asynchronous *)tm.end);
        ID3D11DeviceContext_End(c, (ID3D11Asynchronous *)tm.disjoint);
        tm.pending = true;
        gTimingNext = (gTimingNext + 1) % 4;
    }
    AcquireSRWLockExclusive(&gStatLock);
    gComposited++;
    ReleaseSRWLockExclusive(&gStatLock);
}

}  // namespace

bool SssInit(ID3D11Device *dev, SssCreatePSFn createPS, std::string &error) {
    gDev = dev;
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE, rd.DepthClipEnable = TRUE;
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE, dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO, dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = (sizeof(Cb) + 15) & ~15u, bd.Usage = D3D11_USAGE_DEFAULT, bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bool ok = SUCCEEDED(ID3D11Device_CreateComputeShader(dev, kSssPrep, sizeof kSssPrep, NULL, &gPrep)) &&
              SUCCEEDED(ID3D11Device_CreateComputeShader(dev, kSssBlur, sizeof kSssBlur, NULL, &gBlur)) &&
              SUCCEEDED(ID3D11Device_CreateVertexShader(dev, kSssVS, sizeof kSssVS, NULL, &gVS)) &&
              SUCCEEDED(createPS(dev, kSssComp, sizeof kSssComp, NULL, &gComp)) &&
              SUCCEEDED(ID3D11Device_CreateRasterizerState(dev, &rd, &gRaster)) &&
              SUCCEEDED(ID3D11Device_CreateDepthStencilState(dev, &dd, &gNoDepth)) &&
              SUCCEEDED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &gCbH)) && SUCCEEDED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &gCbV)) &&
              SUCCEEDED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &gCbTH)) && SUCCEEDED(ID3D11Device_CreateBuffer(dev, &bd, NULL, &gCbTV));
    D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, qt = {D3D11_QUERY_TIMESTAMP, 0};
    for (Timing &t : gTiming)
        ok = ok && SUCCEEDED(ID3D11Device_CreateQuery(dev, &qd, &t.disjoint)) && SUCCEEDED(ID3D11Device_CreateQuery(dev, &qt, &t.begin)) &&
             SUCCEEDED(ID3D11Device_CreateQuery(dev, &qt, &t.end));
    if (!ok) error = gInitError = "cannot create the subsurface shaders or states";
    return ok;
}

void SssSetParams(float strength, float width, float tanHalfY) {
    strength = std::min(1.0f, std::max(0.0f, strength));
    width = std::min(3.0f, std::max(0.25f, width));
    tanHalfY = std::min(4.0f, std::max(0.1f, tanHalfY));
    if (strength != gStrength || width != gWidth || tanHalfY != gTanHalfY) gCbDirty = true;
    gStrength = strength, gWidth = width, gTanHalfY = tanHalfY;
}

void SssSetTranslucency(float strength, float width) {
    strength = std::min(4.0f, std::max(0.0f, strength));
    width = std::min(3.0f, std::max(0.25f, width));
    if (strength != gTStrength || width != gTWidth) gCbDirty = true;
    gTStrength = strength, gTWidth = width;
}

void SssBegin(unsigned viewCount) {
    if(gViewCount!=viewCount){gViewCount=viewCount;gCbDirty=true;}
    if (gArmed || gPrepared) ReleaseFrame();  // a pass that never ended
    if(gPathCameraTarget)ID3D11Resource_Release(gPathCameraTarget);gPathCameraTarget=nullptr;
    if(gRR && gPathRoute) {
        ID3D11DeviceContext *context=nullptr;ID3D11Device_GetImmediateContext(gDev,&context);
        ID3D11RenderTargetView *target=nullptr;ID3D11DeviceContext_OMGetRenderTargets(context,1,&target,nullptr);
        if(target){ID3D11View_GetResource((ID3D11View*)target,&gPathCameraTarget);ID3D11RenderTargetView_Release(target);}
        ID3D11DeviceContext_Release(context);
    }
    gArmed = !gPathRoute && (gStrength > 0 || gTStrength > 0 || gRR) && gPrep != NULL;
    if(!gPathRoute && gStrength<=0 && gTStrength<=0) {
        gT.ReleaseScatter();
        if(!gRR)gT.Release();
    }
    gPrepared = gFrameFailed = false;
    gSkip = "";
    gBound = gSkipped = 0;
}

bool SssArmed() { return gArmed && !gFrameFailed; }
bool SssPathDepthReady() {return gPathDepthReady;}
bool SssMarkPathDepth(ID3D11Resource* color,ID3D11Resource* depth) {
    if(!gPathRoute||!color||color!=gPathCameraTarget||!depth)return false;
    if(gPathDepthTarget)ID3D11Resource_Release(gPathDepthTarget);
    gPathDepthTarget=depth;ID3D11Resource_AddRef(depth);gPathDepthReady=true;return true;
}
void SssDepthCleared(ID3D11DepthStencilView* depth,UINT flags) {
    if(!gPathDepthReady||!(flags&D3D11_CLEAR_DEPTH)||!depth)return;
    ID3D11Resource* resource=nullptr;ID3D11View_GetResource((ID3D11View*)depth,&resource);
    if(resource==gPathDepthTarget)gPathDepthReady=false;
    if(resource)ID3D11Resource_Release(resource);
}
bool SssPathTarget(ID3D11DeviceContext *context) {
    if(!gPathCameraTarget)return false;
    ID3D11RenderTargetView *target=nullptr;ID3D11Resource *resource=nullptr;
    ID3D11DeviceContext_OMGetRenderTargets(context,1,&target,nullptr);
    if(target){ID3D11View_GetResource((ID3D11View*)target,&resource);ID3D11RenderTargetView_Release(target);}
    bool same=resource==gPathCameraTarget;if(resource)ID3D11Resource_Release(resource);return same;
}

bool SssBeforeDraw(ID3D11DeviceContext *c, SssDrawSave &s,bool guidesOnly) {
    memset(&s, 0, sizeof s);
    if (!gArmed || gFrameFailed) return false;
    ID3D11DeviceContext_OMGetRenderTargets(c, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, &s.dsv);
    bool ok = s.rtv[0] && s.dsv;
    for (int i = 1; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) ok = ok && !s.rtv[i];
    if (ok) {
        ID3D11UnorderedAccessView *u[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT - 1] = {};
        ID3D11DeviceContext_OMGetRenderTargetsAndUnorderedAccessViews(c, 0, NULL, NULL, 1, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT - 1, u);
        for (auto v : u)
            if (v) {
                ok = false;
                gSkip = "unordered-access views bound with the colour";
                ID3D11UnorderedAccessView_Release(v);
            }
    } else gSkip = "not one colour target with depth";
    if (ok && !gPrepared) {
        ok = Prepare(c, s.rtv[0], s.dsv);
        if (!ok) gFrameFailed = true;
    } else if (ok) {
        ID3D11Resource *r = NULL;
        ID3D11View_GetResource((ID3D11View *)s.rtv[0], &r);
        ok = r == gColor;
        if (r) ID3D11Resource_Release(r);
        if (!ok) gSkip = "another colour target";
    }
    ID3D11BlendState *derived = NULL;
    if (ok) {
        ID3D11DeviceContext_OMGetBlendState(c, &s.blend, s.factor, &s.mask);
        derived = Derived(s.blend,guidesOnly);
        ok = derived != NULL;
        if (!ok) gSkip = "cannot make a blend state";
    }
    if (!ok) {
        for (auto v : s.rtv) Rel(v);
        Rel(s.dsv), Rel(s.blend);
        memset(&s, 0, sizeof s);
        gSkipped++;
        return false;
    }
    ID3D11RenderTargetView *set[8] = {s.rtv[0], gT.eRtv, gT.aRtv, gT.zRtv, gT.wRtv, gRr.nv, gRr.sv, nullptr};
    ID3D11DeviceContext_OMSetRenderTargets(c, gRR ? 7 : 5, set, s.dsv);
    ID3D11DeviceContext_OMSetBlendState(c, derived, s.factor, s.mask);
    gBound++;
    return true;
}

void SssAfterDraw(ID3D11DeviceContext *c, SssDrawSave &s) {
    ID3D11DeviceContext_OMSetRenderTargets(c, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, s.dsv);
    ID3D11DeviceContext_OMSetBlendState(c, s.blend, s.factor, s.mask);
    for (auto v : s.rtv) Rel(v);
    Rel(s.dsv), Rel(s.blend);
    memset(&s, 0, sizeof s);
}

void SssEnd(ID3D11DeviceContext *c, SssSetPSFn setPS) {
    if (!gArmed) return;
    gArmed = false;
    if (gPrepared && gBound > 0 && (gStrength > 0 || gTStrength > 0)) RunPasses(c, setPS);
    if (gRR && gPrepared && gColor && gRr.opaque) ID3D11DeviceContext_CopyResource(c, (ID3D11Resource*)gRr.opaque, gColor);
    if (!gRR) ReleaseFrame();
}

// Called by OC immediately after its current-frame guide capture, after transparents.
static void __cdecl ReconstructSkin() {
    if (gRR && gRrProcess) {
        bool skin = gPrepared && gBound > 0 && gRr.n;
        gRrProcess(gColor, skin ? (ID3D11Resource*)gT.a : nullptr, skin ? (ID3D11Resource*)gRr.n : nullptr,
            skin ? (ID3D11Resource*)gRr.s : nullptr, skin ? (ID3D11Resource*)gT.z : nullptr,
            skin && gRrMaterialReference ? (ID3D11Resource*)gT.a : nullptr, skin ? (ID3D11Resource*)gRr.opaque : nullptr, gDepthTol, gRrDetail);
    }
    if(!gPathRoute)ReleaseFrame();
}

bool SssSetRR(bool enabled, float detail, bool materialReference) {
    gRrDetail = std::clamp(detail, 0.0f, 1.0f);
    gRrMaterialReference=materialReference && gRrDetail>0;
    if (!enabled && !gRR) {gPathRoute=false;return false;}
    HMODULE oc = GetModuleHandleW(L"openvr_api.dll");
    auto reg = oc ? (RrRegisterFn)GetProcAddress(oc, "VaMDLSS_RegisterRr") : nullptr;
    gRrProcess = oc ? (RrProcessFn)GetProcAddress(oc, "VaMDLSS_Reconstruct") : nullptr;
    bool active = enabled && reg && gRrProcess;
    if (reg) active = reg(active ? ReconstructSkin : nullptr) != 0 && active;
    const bool path=active && !materialReference;
    if(path && !gPathRoute) {
        // PT writes its own primary guides and performs any enabled BSSRDF
        // in the integrator. No legacy screen-space SSS/MRT targets are used.
        ReleaseFrame();gArmed=false;gT.Release();gRr.Release();
    }
    gPathRoute=path;
    if (!active && gRR) { ReleaseFrame(); gRr.Release(); }
    gRR = active;
    return active;
}

void SssResetTiming() {
    AcquireSRWLockExclusive(&gStatLock);
    gGpuSamples = 0;
    gGpuSumMs = 0;
    ReleaseSRWLockExclusive(&gStatLock);
}

void SssFrame() {
    if (gArmed) {  // the camera's pass did not end this frame
        ReleaseFrame();
        gArmed = false;
    }
    AcquireSRWLockExclusive(&gStatLock);
    gLastBound = gBound, gLastSkipped = gSkipped;
    gLastRrReference = gRR && gRrMaterialReference && gT.a ? 1 : 0;
    gLastScatterTargets=0;gLastGuideTargets=0;
    for(auto p:{gT.e,gT.tr,gT.p,gT.t,gT.b,gT.bw,gT.colorCopy,gT.depthCopy})gLastScatterTargets+=p?1:0;
    for(auto p:{gT.a,gT.z,gRr.n,gRr.s,gRr.opaque})gLastGuideTargets+=p?1:0;
    gLastSkip = gSkip;
    ReleaseSRWLockExclusive(&gStatLock);
    gBound = gSkipped = 0;
}

#ifdef SSS_TEST
// Test the real draw/resource path without loading an OpenVR compositor.
void SssTestRR(bool enabled) {
    ReleaseFrame();gArmed=false;gPathRoute=false;
    if(!enabled)gRr.Release();gRR=enabled;gRrMaterialReference=enabled;
}
void SssTestGuides(ID3D11Texture2D **out) {
    out[0]=gRr.n;out[1]=gRr.s;out[2]=gRr.opaque;
}
// tests\sss_test: the intermediate targets (e, a, z, p, t, b, colour copy, depth copy), no references added
void SssTestTargets(ID3D11Texture2D **out) {
    ID3D11Texture2D *all[8] = {gT.e, gT.a, gT.z, gT.p, gT.t, gT.b, gT.colorCopy, gT.depthCopy};
    memcpy(out, all, sizeof all);
}
#endif

std::string SssStatus() {
    char t[768];
    AcquireSRWLockShared(&gStatLock);
    snprintf(t, sizeof t,
             "{\"strength\":%.3f,\"width\":%.3f,\"translucency\":{\"strength\":%.3f,\"width\":%.3f},\"tan_half_y\":%.4f,\"size\":[%u,%u],"
             "\"color_format\":%d,\"color_view\":%d,\"depth_format\":%d,\"rr_reference\":%d,\"scatter_targets\":%d,\"guide_targets\":%d,"
             "\"composited\":%d,\"last_frame\":{\"bound\":%d,\"skipped\":%d},\"skip\":\"%s\",\"gpu\":{\"samples\":%d,\"mean_ms\":%.4f},"
             "\"error\":\"%s\"}",
             gStrength, gWidth, gTStrength, gTWidth, gTanHalfY, gT.w, gT.h, (int)gT.colorRes, (int)gT.colorView, (int)gT.depthRes, gLastRrReference,gLastScatterTargets,gLastGuideTargets, gComposited, gLastBound,
             gLastSkipped, gLastSkip.c_str(), gGpuSamples, gGpuSamples ? gGpuSumMs / gGpuSamples : 0.0, gInitError.c_str());
    ReleaseSRWLockShared(&gStatLock);
    return t;
}
