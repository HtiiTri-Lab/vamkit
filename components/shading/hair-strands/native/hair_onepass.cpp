// VaM hair one-pass lighting, native side (D3D11, loaded by VaMHairOnePass.dll as VaMHairOnePassNative.bin).
//
// GPUTools hair (GPUTools/MeshedVR/HairOpt) is drawn by Unity's forward renderer once in ForwardBase and once more,
// whole, in ForwardAdd for every further pixel light that reaches it. This module recognises those draws inside the
// stereo eye camera's opaque pass and (mode 2, merge) replaces them by one draw per hair item and eye that lights all
// of them at once (shaders/hair_onepass.hlsl), or (mode 3, strands, 2.0.1) draws the taken items through a visibility
// buffer: points in compute, the segments' quads rasterised in software, lighting in one full-target pass each for
// ForwardBase and ForwardAdd (shaders/hair_strands.hlsl; see "strands" below). Mode 1 only observes: which hair draws
// come in which order, and how their constant buffers were filled.
//
// How draws are seen. Unity's draws go through the immediate context's function table, which the D3D11 runtime keeps
// inside the context object and rewrites on Flush (slow-path entries) and on the next GPU call (fast-path entries,
// copied from a read-only template table in d3d11.dll). Patching the template and the live table catches every call
// (see tests\hook_test.c). Hair programs are known by the SHA-1 of their bytecode: the device's (static) function
// table is patched so every shader the game creates is hashed, and hair programs get a private-data tag.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define CINTERFACE
#define COBJMACROS
#define D3D11_NO_HELPERS
#include <windows.h>
#include <bcrypt.h>
#include <d3d11_1.h>
#include <psapi.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>
#include "hair_programs.h"
#include "hop_item.h"
#include "../../rt-core/include/startup_display.h"
#include "hop_shaders.h"
#include "hs_item.h"
#include "hs_shaders.h"
#include "../../rt-core/include/path_hair_bridge.h"

#define HOP_API_VERSION 5
#define EXPORT extern "C" __declspec(dllexport)

// render events: (op << 24) | arg
enum { OP_INSTALL = 1, OP_BEGIN = 2, OP_END = 3, OP_FRAME = 4 };
enum { MODE_OFF = 0, MODE_OBSERVE = 1, MODE_MERGE = 2, MODE_STRANDS = 3 };

static const GUID kTagGuid = { 0x7a3c51e2, 0x93b1, 0x4c0d, { 0xa4, 0x52, 0x6e, 0x11, 0x0b, 0x2f, 0x9c, 0x71 } };

static ID3D11Device *gDevice;
static bool gPathLighting=false;
static ID3D11DeviceContext *gCtx;
static ID3D11DeviceContext1 *gCtx1;
static SRWLOCK gLock = SRWLOCK_INIT;
static char gError[512];
static volatile LONG gMode = MODE_OFF;
static volatile LONG gTransmission = 1;  // 2.1.0: the strands' transmission lobe (gResolve.z)
static bool gInstalled;

static void SetError(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vsnprintf(gError, sizeof gError, fmt, a);
    va_end(a);
}

// ---- program tags ---------------------------------------------------------------------------------------------------

static std::vector<uint64_t> gSortedHashes;
static std::vector<int> gSortedIndex;
static volatile LONG gCreated[5], gHairCreated[5];

static int ProgramOf(const void *code, SIZE_T size) {
    uint8_t h[20];
    if (BCryptHash(BCRYPT_SHA1_ALG_HANDLE, NULL, 0, (PUCHAR)code, (ULONG)size, h, 20) != 0) return -1;
    uint64_t k = 0;
    for (int i = 0; i < 8; i++) k = (k << 8) | h[i];
    auto it = std::lower_bound(gSortedHashes.begin(), gSortedHashes.end(), k);
    if (it == gSortedHashes.end() || *it != k) return -1;
    return gSortedIndex[it - gSortedHashes.begin()];
}

static void Tag(ID3D11DeviceChild *shader, const void *code, SIZE_T size, int stage) {
    InterlockedIncrement(&gCreated[stage]);
    int p = ProgramOf(code, size);
    if (p < 0) return;
    InterlockedIncrement(&gHairCreated[stage]);
    int v = p + 1;
    ID3D11DeviceChild_SetPrivateData(shader, kTagGuid, sizeof v, &v);
}

static int TagOf(ID3D11DeviceChild *shader) {
    if (!shader) return -1;
    int v = 0;
    UINT n = sizeof v;
    if (FAILED(ID3D11DeviceChild_GetPrivateData(shader, kTagGuid, &n, &v)) || n != sizeof v) return -1;
    return v - 1;
}

// ---- device hooks (shader creation) --------------------------------------------------------------------------------

#define DSLOT(f) (offsetof(ID3D11DeviceVtbl, f) / sizeof(void *))
typedef HRESULT(STDMETHODCALLTYPE *CreateVSFn)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11VertexShader **);
typedef HRESULT(STDMETHODCALLTYPE *CreateHSFn)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11HullShader **);
typedef HRESULT(STDMETHODCALLTYPE *CreateDSFn)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11DomainShader **);
typedef HRESULT(STDMETHODCALLTYPE *CreateGSFn)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11GeometryShader **);
typedef HRESULT(STDMETHODCALLTYPE *CreatePSFn)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11PixelShader **);
static CreateVSFn oCreateVS;
static CreateHSFn oCreateHS;
static CreateDSFn oCreateDS;
static CreateGSFn oCreateGS;
static CreatePSFn oCreatePS;

static HRESULT STDMETHODCALLTYPE hCreateVS(ID3D11Device *d, const void *c, SIZE_T n, ID3D11ClassLinkage *l, ID3D11VertexShader **o) {
    HRESULT hr = oCreateVS(d, c, n, l, o);
    if (SUCCEEDED(hr) && o && *o) Tag((ID3D11DeviceChild *)*o, c, n, HOP_VS);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hCreateHS(ID3D11Device *d, const void *c, SIZE_T n, ID3D11ClassLinkage *l, ID3D11HullShader **o) {
    HRESULT hr = oCreateHS(d, c, n, l, o);
    if (SUCCEEDED(hr) && o && *o) Tag((ID3D11DeviceChild *)*o, c, n, HOP_HS);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hCreateDS(ID3D11Device *d, const void *c, SIZE_T n, ID3D11ClassLinkage *l, ID3D11DomainShader **o) {
    HRESULT hr = oCreateDS(d, c, n, l, o);
    if (SUCCEEDED(hr) && o && *o) Tag((ID3D11DeviceChild *)*o, c, n, HOP_DS);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hCreateGS(ID3D11Device *d, const void *c, SIZE_T n, ID3D11ClassLinkage *l, ID3D11GeometryShader **o) {
    HRESULT hr = oCreateGS(d, c, n, l, o);
    if (SUCCEEDED(hr) && o && *o) Tag((ID3D11DeviceChild *)*o, c, n, HOP_GS);
    return hr;
}
static HRESULT STDMETHODCALLTYPE hCreatePS(ID3D11Device *d, const void *c, SIZE_T n, ID3D11ClassLinkage *l, ID3D11PixelShader **o) {
    HRESULT hr = oCreatePS(d, c, n, l, o);
    if (SUCCEEDED(hr) && o && *o) Tag((ID3D11DeviceChild *)*o, c, n, HOP_PS);
    return hr;
}

static bool PatchPointer(void **at, void *value) {
    DWORD old;
    if (!VirtualProtect(at, sizeof(void *), PAGE_READWRITE, &old)) return false;
    *at = value;
    VirtualProtect(at, sizeof(void *), old, &old);
    return true;
}

static bool HookDevice() {
    void **t = (void **)gDevice->lpVtbl;
    oCreateVS = (CreateVSFn)t[DSLOT(CreateVertexShader)];
    oCreateHS = (CreateHSFn)t[DSLOT(CreateHullShader)];
    oCreateDS = (CreateDSFn)t[DSLOT(CreateDomainShader)];
    oCreateGS = (CreateGSFn)t[DSLOT(CreateGeometryShader)];
    oCreatePS = (CreatePSFn)t[DSLOT(CreatePixelShader)];
    return PatchPointer(&t[DSLOT(CreateVertexShader)], (void *)hCreateVS) && PatchPointer(&t[DSLOT(CreateHullShader)], (void *)hCreateHS) &&
           PatchPointer(&t[DSLOT(CreateDomainShader)], (void *)hCreateDS) && PatchPointer(&t[DSLOT(CreateGeometryShader)], (void *)hCreateGS) &&
           PatchPointer(&t[DSLOT(CreatePixelShader)], (void *)hCreatePS);
}

// ---- context hooks ---------------------------------------------------------------------------------------------------

#define CSLOT(f) (offsetof(ID3D11DeviceContextVtbl, f) / sizeof(void *))
typedef void(STDMETHODCALLTYPE *DrawIndexedFn)(ID3D11DeviceContext *, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE *DrawIndexedInstancedFn)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE *DrawFn)(ID3D11DeviceContext *, UINT, UINT);
typedef void(STDMETHODCALLTYPE *DrawInstancedFn)(ID3D11DeviceContext *, UINT, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE *DrawIndirectFn)(ID3D11DeviceContext *, ID3D11Buffer *, UINT);
typedef HRESULT(STDMETHODCALLTYPE *MapFn)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
typedef void(STDMETHODCALLTYPE *UnmapFn)(ID3D11DeviceContext *, ID3D11Resource *, UINT);
typedef void(STDMETHODCALLTYPE *UpdateSubresourceFn)(ID3D11DeviceContext *, ID3D11Resource *, UINT, const D3D11_BOX *, const void *, UINT, UINT);
static DrawIndexedFn oDrawIndexed;
static DrawIndexedInstancedFn oDrawIndexedInstanced;
static DrawFn oDraw;
static DrawInstancedFn oDrawInstanced;
static DrawIndirectFn oDrawIndexedInstancedIndirect, oDrawInstancedIndirect;
static MapFn oMap;
static UnmapFn oUnmap;
static UpdateSubresourceFn oUpdateSubresource;
static void **gTemplate;

// The runtime's template copy of the immediate context's fast-path table: the place in d3d11.dll's image that holds
// the same pointers as the live table's first 12 entries (QueryInterface .. VSSetShader never change).
static void **FindTemplate(void **live) {
    HMODULE m = GetModuleHandleW(L"d3d11.dll");
    MODULEINFO mi;
    if (!m || !GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi)) return NULL;
    uint8_t *base = (uint8_t *)mi.lpBaseOfDll, *end = base + mi.SizeOfImage;
    void **found = NULL;
    int count = 0;
    // region by region (a region has one protection), comparing only tables that end inside the region
    for (uint8_t *r = base; r < end;) {
        MEMORY_BASIC_INFORMATION q;
        if (!VirtualQuery(r, &q, sizeof q)) break;
        uint8_t *rEnd = (uint8_t *)q.BaseAddress + q.RegionSize;
        if (rEnd > end) rEnd = end;
        bool readable = q.State == MEM_COMMIT && !(q.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                        (q.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
        if (readable) {
            for (void **t = (void **)r; (uint8_t *)(t + 12) <= rEnd; t++) {
                if (t == live || t[0] != live[0]) continue;
                bool same = true;
                for (int k = 1; k < 12 && same; k++) same = t[k] == live[k];
                if (same) {
                    found = t;
                    count++;
                }
            }
        }
        r = rEnd;
    }
    if (count != 1) {
        SetError("d3d11.dll context table template: %d candidates", count);
        return NULL;
    }
    return found;
}

// Hook one slot: the template (so every fast-path restore installs the hook) and the live table if it holds the fast
// entry now (a slow-path entry is left alone: the runtime replaces it from the template at the next GPU call).
static bool HookSlot(void **live, size_t slot, void *hook, void **orig) {
    *orig = gTemplate[slot];
    if (!PatchPointer(&gTemplate[slot], hook)) return false;
    if (live[slot] == *orig) live[slot] = hook;
    return true;
}

// ---- observation -----------------------------------------------------------------------------------------------------

struct CbUpdate {
    uint8_t method;  // 1 Map(discard), 2 Map(no overwrite), 3 Map(other), 4 UpdateSubresource
    uint32_t frame;
    uint32_t bytes;
};
static std::unordered_map<ID3D11Buffer *, CbUpdate> gCbUpdates;
static std::unordered_map<ID3D11Resource *, int> gIds;  // small ids for buffers and textures in the report
static uint32_t gFrame;
static int gWindowCamera = -1;
static bool gInWindow;

static int IdOf(void *p) {
    if (!p) return 0;
    auto it = gIds.find((ID3D11Resource *)p);
    if (it != gIds.end()) return it->second;
    int id = (int)gIds.size() + 1;
    gIds[(ID3D11Resource *)p] = id;
    return id;
}

struct DrawRec {
    int pass, ps, ds, eye, x, w;
    UINT indices, start, instances;
    INT base;
    int particles, others;  // particle buffer id, non-hair draws since the previous hair draw
    int rtv;                // render target 0 (resource id)
    int srv[6];             // pixel shader textures t0..t5 (resource ids)
};

struct FrameObs {
    uint32_t frame = 0;
    int windows = 0, hairDraws = 0, otherDraws = 0, fb = 0, fa = 0, sc = 0, mv = 0, instancedHair = 0;
    int maps[5] = {}, cbUpdates[5] = {};  // all Map calls on constant buffers by method, during windows
    std::vector<DrawRec> seq;
    std::string bindingsFb, bindingsFa, targets;
    // constant buffers bound to a ForwardBase draw that were written again before the next ForwardBase draw
    int fbChanged[5][14] = {}, fbChangedBytes[5][14] = {};
};
static FrameObs gCur, gLast;
static int gFramesObserved;
static int gOthersSinceHair;
static ID3D11Buffer *gFbBound[5][14];  // the last ForwardBase draw's constant buffers (identity only, no reference)
static bool gFbPending;

static const char *StageName(int s) { static const char *n[] = {"vs", "hs", "ds", "gs", "ps"}; return n[s]; }

static void AppendBindings(std::string &out) {
    char b[256];
    typedef void(STDMETHODCALLTYPE * Get1)(ID3D11DeviceContext1 *, UINT, UINT, ID3D11Buffer **, UINT *, UINT *);
    for (int s = 1; s < 5; s++) {
        ID3D11Buffer *bufs[14] = {};
        UINT first[14] = {}, num[14] = {};
        if (gCtx1) {
            switch (s) {
            case 1: ID3D11DeviceContext1_HSGetConstantBuffers1(gCtx1, 0, 14, bufs, first, num); break;
            case 2: ID3D11DeviceContext1_DSGetConstantBuffers1(gCtx1, 0, 14, bufs, first, num); break;
            case 3: ID3D11DeviceContext1_GSGetConstantBuffers1(gCtx1, 0, 14, bufs, first, num); break;
            case 4: ID3D11DeviceContext1_PSGetConstantBuffers1(gCtx1, 0, 14, bufs, first, num); break;
            }
        }
        for (int k = 0; k < 14; k++) {
            if (!bufs[k]) continue;
            D3D11_BUFFER_DESC d;
            ID3D11Buffer_GetDesc(bufs[k], &d);
            auto it = gCbUpdates.find(bufs[k]);
            snprintf(b, sizeof b, "%s{\"stage\":\"%s\",\"slot\":%d,\"id\":%d,\"bytes\":%u,\"usage\":%d,\"first\":%u,\"num\":%u,\"update\":%d,\"updated_frames_ago\":%d}",
                     out.empty() ? "" : ",", StageName(s), k, IdOf(bufs[k]), d.ByteWidth, (int)d.Usage, first[k], num[k],
                     it == gCbUpdates.end() ? 0 : it->second.method, it == gCbUpdates.end() ? -1 : (int)(gFrame - it->second.frame));
            out += b;
            ID3D11Buffer_Release(bufs[k]);
        }
    }
}

static void AppendTargets(std::string &out) {
    ID3D11RenderTargetView *rtv[8] = {};
    ID3D11DepthStencilView *dsv = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(gCtx, 8, rtv, &dsv);
    char b[256];
    for (int i = 0; i < 8; i++) {
        if (!rtv[i]) continue;
        D3D11_RENDER_TARGET_VIEW_DESC d;
        ID3D11RenderTargetView_GetDesc(rtv[i], &d);
        ID3D11Resource *r;
        ID3D11RenderTargetView_GetResource(rtv[i], &r);
        D3D11_TEXTURE2D_DESC td = {};
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &td);
        snprintf(b, sizeof b, "%s{\"rtv\":%d,\"format\":%d,\"dim\":%d,\"width\":%u,\"height\":%u,\"samples\":%u}", out.empty() ? "" : ",", i,
                 (int)d.Format, (int)d.ViewDimension, td.Width, td.Height, td.SampleDesc.Count);
        out += b;
        ID3D11Resource_Release(r);
        ID3D11RenderTargetView_Release(rtv[i]);
    }
    if (dsv) {
        D3D11_DEPTH_STENCIL_VIEW_DESC d;
        ID3D11DepthStencilView_GetDesc(dsv, &d);
        snprintf(b, sizeof b, "%s{\"dsv\":0,\"format\":%d,\"dim\":%d}", out.empty() ? "" : ",", (int)d.Format, (int)d.ViewDimension);
        out += b;
        ID3D11DepthStencilView_Release(dsv);
    }
}

static void GetConstantBuffers(int stage, ID3D11Buffer **bufs) {
    switch (stage) {
    case 0: ID3D11DeviceContext_VSGetConstantBuffers(gCtx, 0, 14, bufs); break;
    case 1: ID3D11DeviceContext_HSGetConstantBuffers(gCtx, 0, 14, bufs); break;
    case 2: ID3D11DeviceContext_DSGetConstantBuffers(gCtx, 0, 14, bufs); break;
    case 3: ID3D11DeviceContext_GSGetConstantBuffers(gCtx, 0, 14, bufs); break;
    case 4: ID3D11DeviceContext_PSGetConstantBuffers(gCtx, 0, 14, bufs); break;
    }
}

static int ResourceIdOfView(ID3D11View *v) {
    if (!v) return 0;
    ID3D11Resource *r;
    ID3D11View_GetResource(v, &r);
    int id = IdOf(r);
    ID3D11Resource_Release(r);
    return id;
}

static void ObserveDraw(UINT indices, UINT start, INT base, UINT instances) {
    ID3D11PixelShader *ps = NULL;
    ID3D11DeviceContext_PSGetShader(gCtx, &ps, NULL, NULL);
    int p = TagOf((ID3D11DeviceChild *)ps);
    if (ps) ID3D11PixelShader_Release(ps);
    if (p < 0) {
        gCur.otherDraws++;
        gOthersSinceHair++;
        return;
    }
    const HopProgram &prog = kHopPrograms[p];
    gCur.hairDraws++;
    if (instances != 0xFFFFFFFF) gCur.instancedHair++;
    switch (prog.pass) {
    case HOP_PASS_FB: gCur.fb++; break;
    case HOP_PASS_FA: gCur.fa++; break;
    case HOP_PASS_SC: gCur.sc++; break;
    case HOP_PASS_MV: gCur.mv++; break;
    }
    ID3D11DomainShader *ds = NULL;
    ID3D11DeviceContext_DSGetShader(gCtx, &ds, NULL, NULL);
    int dp = TagOf((ID3D11DeviceChild *)ds);
    if (ds) ID3D11DomainShader_Release(ds);
    D3D11_VIEWPORT vp[2] = {};
    UINT nvp = 2;
    ID3D11DeviceContext_RSGetViewports(gCtx, &nvp, vp);
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11DeviceContext_DSGetShaderResources(gCtx, 2, 1, &srv);
    int particles = 0;
    if (srv) {
        ID3D11Resource *r;
        ID3D11ShaderResourceView_GetResource(srv, &r);
        particles = IdOf(r);
        ID3D11Resource_Release(r);
        ID3D11ShaderResourceView_Release(srv);
    }
    if (gCur.seq.size() < 400) {
        DrawRec d = {prog.pass, p, dp, vp[0].TopLeftX > 0 ? 1 : 0, (int)vp[0].TopLeftX, (int)vp[0].Width, indices, start,
                     instances, base, particles, gOthersSinceHair, 0, {}};
        ID3D11RenderTargetView *rtv = NULL;
        ID3D11DeviceContext_OMGetRenderTargets(gCtx, 1, &rtv, NULL);
        d.rtv = ResourceIdOfView((ID3D11View *)rtv);
        if (rtv) ID3D11RenderTargetView_Release(rtv);
        ID3D11ShaderResourceView *srvs[6] = {};
        ID3D11DeviceContext_PSGetShaderResources(gCtx, 0, 6, srvs);
        for (int k = 0; k < 6; k++) {
            d.srv[k] = ResourceIdOfView((ID3D11View *)srvs[k]);
            if (srvs[k]) ID3D11ShaderResourceView_Release(srvs[k]);
        }
        gCur.seq.push_back(d);
    }
    if (prog.pass == HOP_PASS_FB) {
        for (int s = 0; s < 5; s++) {
            ID3D11Buffer *bufs[14] = {};
            GetConstantBuffers(s, bufs);
            for (int k = 0; k < 14; k++) {
                gFbBound[s][k] = bufs[k];
                if (bufs[k]) ID3D11Buffer_Release(bufs[k]);
            }
        }
        gFbPending = true;
    }
    gOthersSinceHair = 0;
    if (prog.pass == HOP_PASS_FB && gCur.bindingsFb.empty()) {
        AppendBindings(gCur.bindingsFb);
        AppendTargets(gCur.targets);
    }
    if (prog.pass == HOP_PASS_FA && gCur.bindingsFa.empty()) AppendBindings(gCur.bindingsFa);
}

static int gWindowMode = MODE_OFF;  // the mode for the current opaque pass (read at its start)
static bool Observing() { return gWindowMode == MODE_OBSERVE && gInWindow; }

// ---- merge: a hair item's ForwardBase draw and its ForwardAdd draws as one draw -------------------------------------
//
// In the eye camera's opaque pass Unity draws every ForwardBase pass first and later, item after item, the ForwardAdd
// passes (per light both eyes). Mode 2 skips a hair item's ForwardBase draw, keeping what it needs (buffers, render
// state, and the constants, from the CPU copies below), skips its ForwardAdd draws, keeping each light's constants,
// textures and samplers, and when they are over (the next draw that is not one of them, or the end of the opaque pass)
// draws the item once per eye with the one-pass shader, between saving and restoring the context state that changes.
// A light the shader cannot draw (cookies, a ninth light, samplers or noise textures differing between lights) ends the
// item early: it is drawn with the lights so far and its remaining ForwardAdd draws go through unchanged (additive on
// top). The hair is drawn after the other opaque objects instead of before them; with the depth test, the same picture.
//
// Constants: Unity writes constant buffers with Map(WRITE_DISCARD) and a full copy. For buffers bound to hair draws
// (watched) Map hands Unity a CPU copy that Unmap then copies into the mapped memory (UpdateSubresource: its data are
// copied too), so their contents are known when a hair draw comes; a buffer seen for the first time, or written some
// other way, is read back (a GPU wait: the statistics count them). A watched buffer is held (one reference) so that
// its address cannot come back as another buffer; SweepShadows drops those the game has released.

struct Shadow {
    ID3D11Buffer *buffer;  // held
    UINT size;
    uint8_t *data;
    void *mapped;  // the runtime's memory while Unity writes into data
    bool valid;
};
static Shadow gShadows[128];
static int gShadowCount;

static Shadow *FindShadow(void *b) {
    for (int i = 0; i < gShadowCount; i++)
        if (gShadows[i].buffer == b) return &gShadows[i];
    return NULL;
}

enum {
    WHY_FB_INSTANCED, WHY_FB_PROGRAM, WHY_FB_VALUES, WHY_FB_STATE, WHY_FB_TWICE, WHY_FA_NO_ITEM, WHY_FA_LIGHT, WHY_FA_VALUES,
    WHY_FA_SAMPLER, WHY_FA_FULL, WHY_FA_STATE, WHY_FB_LATE, WHY_HS_TARGET, WHY_HS_SIZE, WHY_HS_LIGHTS, WHY_HS_RESOURCES, WHY_COUNT
};
static const char *kWhy[WHY_COUNT] = {"fb_instanced", "fb_program", "fb_values", "fb_state", "fb_twice", "fa_no_item", "fa_light_kind",
                                      "fa_values", "fa_sampler", "fa_full", "fa_state", "fb_after_resolve", "hs_target", "hs_size",
                                      "hs_lights", "hs_resources"};

struct MergeStats {
    int fbMerged, fbThrough, faMerged, faThrough, flushes, draws, lights, readbacks, failedReadbacks;
    int why[WHY_COUNT];
    double cpuUs;
    // strands: the pass drew with strands; items, their points and segments; resolves; frame light table; failures
    int hsPass, hsItems, hsFbResolves, hsFaResolves, hsLights, hsFails;
    uint32_t hsPoints, hsSegments;
};
static MergeStats gMergeCur, gMergeLast;
static LONG gReadbacksTotal;

static bool Readback(Shadow &s) {
    D3D11_BUFFER_DESC d = {s.size, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    ID3D11Buffer *staging = NULL;
    if (FAILED(ID3D11Device_CreateBuffer(gDevice, &d, NULL, &staging))) return false;
    ID3D11DeviceContext_CopyResource(gCtx, (ID3D11Resource *)staging, (ID3D11Resource *)s.buffer);
    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = SUCCEEDED(ID3D11DeviceContext_Map(gCtx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
        memcpy(s.data, m.pData, s.size);
        ID3D11DeviceContext_Unmap(gCtx, (ID3D11Resource *)staging, 0);
    }
    ID3D11Buffer_Release(staging);
    s.valid = ok;
    gMergeCur.readbacks++;
    InterlockedIncrement(&gReadbacksTotal);
    return ok;
}

// The watched copy of a constant buffer bound to a hair draw (watching it from now on), or NULL.
static Shadow *Watch(ID3D11Buffer *b) {
    if (!b) return NULL;
    Shadow *s = FindShadow(b);
    if (!s) {
        if (gShadowCount == (int)(sizeof gShadows / sizeof gShadows[0])) return NULL;
        D3D11_BUFFER_DESC d;
        ID3D11Buffer_GetDesc(b, &d);
        if (d.Usage == D3D11_USAGE_STAGING || d.ByteWidth > 65536) return NULL;
        uint8_t *data = (uint8_t *)calloc(1, d.ByteWidth);
        if (!data) return NULL;
        s = &gShadows[gShadowCount++];
        s->buffer = b;
        ID3D11Buffer_AddRef(b);
        s->size = d.ByteWidth;
        s->data = data;
        s->mapped = NULL;
        s->valid = false;
    }
    if (!s->valid && !Readback(*s)) {
        gMergeCur.failedReadbacks++;
        return NULL;
    }
    return s;
}

// Drops the watched buffers that only the copies still hold (the game released them). Render thread, between frames
// (no Shadow pointer is in use and nothing is mapped).
static void SweepShadows() {
    int kept = 0;
    for (int i = 0; i < gShadowCount; i++) {
        Shadow &s = gShadows[i];
        ID3D11Buffer_AddRef(s.buffer);
        ULONG refs = ID3D11Buffer_Release(s.buffer);
        if (refs <= 1 && !s.mapped) {
            ID3D11Buffer_Release(s.buffer);
            free(s.data);
            continue;
        }
        gShadows[kept++] = s;
    }
    gShadowCount = kept;
}

// Which HopItem / HopLight fields a program's constant buffers hold (built once per program).
struct FieldRef {
    uint8_t cb;          // index into the program's constant buffers
    uint8_t light;       // 1: kHopLightFields, 0: kHopItemFields
    uint8_t field;       // index into that table
    uint16_t src;        // member offset in the constant buffer
};
static std::vector<FieldRef> gFieldRefs[256];
static bool gFieldRefsBuilt[256];

static const std::vector<FieldRef> &FieldRefs(int p) {
    if (!gFieldRefsBuilt[p]) {
        const HopProgram &prog = kHopPrograms[p];
        for (int c = 0; c < prog.cbCount; c++)
            for (int m = 0; m < prog.cbs[c].count; m++) {
                const HopMember &mem = prog.cbs[c].members[m];
                for (int f = 0; f < kHopItemFieldCount; f++)
                    if (!strcmp(mem.name, kHopItemFields[f].name)) gFieldRefs[p].push_back(FieldRef{(uint8_t)c, 0, (uint8_t)f, mem.offset});
                for (int f = 0; f < kHopLightFieldCount; f++)
                    if (!strcmp(mem.name, kHopLightFields[f].name)) gFieldRefs[p].push_back(FieldRef{(uint8_t)c, 1, (uint8_t)f, mem.offset});
            }
        gFieldRefsBuilt[p] = true;
    }
    return gFieldRefs[p];
}

static int FieldIndex(const HopField *fields, int count, const char *name) {
    for (int i = 0; i < count; i++)
        if (!strcmp(fields[i].name, name)) return i;
    return -1;
}

// The item fields every ForwardAdd draw may refresh (camera and object, the same for all passes of the item).
static uint64_t gCameraFields;
// The item fields the ForwardBase draw must provide, and those needed for screen positions.
static uint64_t gFbFields, gScreenFields;

static void InitFieldSets() {
    const char *camera[] = {"_ProjectionParams", "unity_StereoScaleOffset", "unity_StereoMatrixVP", "unity_StereoWorldSpaceCameraPos",
                            "unity_ObjectToWorld", "unity_WorldToObject"};
    for (const char *n : camera) gCameraFields |= 1ull << FieldIndex(kHopItemFields, kHopItemFieldCount, n);
    gScreenFields = (1ull << FieldIndex(kHopItemFields, kHopItemFieldCount, "_ProjectionParams")) |
                    (1ull << FieldIndex(kHopItemFields, kHopItemFieldCount, "unity_StereoScaleOffset"));
    uint64_t mono=(1ull << FieldIndex(kHopItemFields,kHopItemFieldCount,"unity_MatrixVP")) |
                  (1ull << FieldIndex(kHopItemFields,kHopItemFieldCount,"_WorldSpaceCameraPos"));
    uint64_t stereo=(1ull << FieldIndex(kHopItemFields,kHopItemFieldCount,"unity_StereoMatrixVP")) |
                    (1ull << FieldIndex(kHopItemFields,kHopItemFieldCount,"unity_StereoWorldSpaceCameraPos"));
    uint64_t scale=1ull << FieldIndex(kHopItemFields,kHopItemFieldCount,"unity_StereoScaleOffset");
    if(VamDesktopDisplay()) { gCameraFields=(gCameraFields&~(stereo|scale))|mono; gScreenFields&=~scale; }
    gFbFields = ((1ull << kHopItemFieldCount) - 1) & ~gScreenFields & ~(VamDesktopDisplay() ? stereo|scale : mono);
}

// Copies the fields program p (bound to stage) has into item / light; false if a constant buffer's contents are unknown.
static bool Extract(int stage, int p, uint8_t *item, uint64_t *itemMask, uint64_t itemAllowed, uint8_t *light, uint32_t *lightMask) {
    if (p < 0) return true;
    const HopProgram &prog = kHopPrograms[p];
    const std::vector<FieldRef> &refs = FieldRefs(p);
    if (refs.empty()) return true;
    ID3D11Buffer *bound[14] = {};
    GetConstantBuffers(stage, bound);
    Shadow *sh[8] = {};
    bool ok = true;
    for (int c = 0; c < prog.cbCount && c < 8; c++) {
        sh[c] = prog.cbs[c].slot < 14 ? Watch(bound[prog.cbs[c].slot]) : NULL;
        if (!sh[c]) ok = false;
    }
    for (const FieldRef &r : refs) {
        Shadow *s = sh[r.cb];
        if (!s || r.src >= s->size) continue;
        const HopField &f = r.light ? kHopLightFields[r.field] : kHopItemFields[r.field];
        size_t bytes = std::min<size_t>(f.bytes, s->size - r.src);
        if (r.light) {
            if (!light) continue;
            memcpy(light + f.offset, s->data + r.src, bytes);
            *lightMask |= 1u << r.field;
        } else {
            if (!item || !(itemAllowed & (1ull << r.field))) continue;
            memcpy(item + f.offset, s->data + r.src, bytes);
            *itemMask |= 1ull << r.field;
        }
    }
    for (int k = 0; k < 14; k++)
        if (bound[k]) ID3D11Buffer_Release(bound[k]);
    return ok;
}

// PS sampler roles of the one-pass shader (slots s0..s6).
enum { ROLE_ROTATION, ROLE_MAIN_SCREEN, ROLE_COOKIE, ROLE_ATTEN, ROLE_COMPARE, ROLE_RAW, ROLE_DIR_SCREEN, ROLE_COUNT };

struct LightCap {
    HopLightData data;
    ID3D11ShaderResourceView *cookie, *atten, *shadow, *cube;
};

// One eye of an item: the ForwardBase draw's buffers and constants (Unity draws it), then the lights of its ForwardAdd
// draws, drawn in one draw with their state where they were.
struct EyeCap {
    int state;  // 0 nothing, 1 ForwardBase seen (taking lights), 2 lights drawn, 3 left to Unity
    D3D11_VIEWPORT vp;
    UINT scissorCount;
    D3D11_RECT scissor[4];  // the lights' scissor rectangles' union
    ID3D11RasterizerState *rs;
    ID3D11BlendState *blend;
    float blendFactor[4];
    UINT sampleMask;
    ID3D11DepthStencilState *dss;
    UINT stencilRef;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    ID3D11Buffer *ib;
    DXGI_FORMAT ibFormat;
    UINT ibOffset, indexCount, startIndex;
    INT baseVertex;
    ID3D11ShaderResourceView *dsSrv[4], *rotation;
    ID3D11SamplerState *dsSmp[2], *role[ROLE_COUNT];
    int lightCount;
    LightCap lights[HOP_MAX_LIGHTS];
    // strands: the ForwardBase draw's output state (the resolve draws with it) and its main light screen-space shadows
    ID3D11RasterizerState *fbRs;
    ID3D11BlendState *fbBlend;
    float fbBlendFactor[4];
    UINT fbSampleMask;
    ID3D11DepthStencilState *fbDss;
    UINT fbStencilRef;
    ID3D11RenderTargetView *fbRtv;
    ID3D11DepthStencilView *fbDsv;
    ID3D11ShaderResourceView *screenSrv;
    ID3D11SamplerState *screenSmp;
    bool screen;
};

struct Item {
    void *particles, *ib;  // key (identity only): particle buffer, index buffer, first index, index count
    UINT startIndex, indexCount;
    HopItemData data;
    uint64_t filled;
    EyeCap eye[2];
};
static Item gItems[64];
static int gItemCount;
static Item *gRun;  // the item whose ForwardAdd draws are coming (its lights drawn when they end)
static bool gEmitting;

static ID3D11VertexShader *gHopVS;
static ID3D11HullShader *gHopHS;
// the lights-only shaders (HOP_BASE 0) by light count (1..8) and screen interpolant
static ID3D11DomainShader *gHopDS[HOP_MAX_LIGHTS + 1];
static ID3D11GeometryShader *gHopGS[HOP_MAX_LIGHTS + 1][2];
static ID3D11PixelShader *gHopPS[HOP_MAX_LIGHTS + 1][2];
static ID3D11Buffer *gItemCb, *gEyeCb[2];
static bool gMergeReady;

#define REL(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)

static void ReleaseEye(EyeCap &e) {
    REL(e.rs);
    REL(e.blend);
    REL(e.dss);
    REL(e.rtv);
    REL(e.dsv);
    REL(e.ib);
    for (auto &v : e.dsSrv) REL(v);
    REL(e.rotation);
    for (auto &v : e.dsSmp) REL(v);
    for (auto &v : e.role) REL(v);
    for (int k = 0; k < e.lightCount; k++) {
        REL(e.lights[k].cookie);
        REL(e.lights[k].atten);
        REL(e.lights[k].shadow);
        REL(e.lights[k].cube);
    }
    REL(e.fbRs);
    REL(e.fbBlend);
    REL(e.fbDss);
    REL(e.fbRtv);
    REL(e.fbDsv);
    REL(e.screenSrv);
    REL(e.screenSmp);
    memset(&e, 0, sizeof e);
}

static bool CreateMergeObjects() {
    bool ok = SUCCEEDED(ID3D11Device_CreateVertexShader(gDevice, kHopVS.code, kHopVS.size, NULL, &gHopVS)) &&
              SUCCEEDED(ID3D11Device_CreateHullShader(gDevice, kHopHS.code, kHopHS.size, NULL, &gHopHS));
    for (int n = 1; ok && n <= HOP_MAX_LIGHTS; n++) {
        ok = SUCCEEDED(ID3D11Device_CreateDomainShader(gDevice, kHopLDS[n].code, kHopLDS[n].size, NULL, &gHopDS[n]));
        for (int s = 0; ok && s < 2; s++)
            ok = SUCCEEDED(ID3D11Device_CreateGeometryShader(gDevice, kHopLGS[n][s].code, kHopLGS[n][s].size, NULL, &gHopGS[n][s])) &&
                 SUCCEEDED(ID3D11Device_CreatePixelShader(gDevice, kHopLPS[n][s].code, kHopLPS[n][s].size, NULL, &gHopPS[n][s]));
    }
    D3D11_BUFFER_DESC d = {sizeof(HopItemData), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ok = ok && SUCCEEDED(ID3D11Device_CreateBuffer(gDevice, &d, NULL, &gItemCb));
    for (int e = 0; ok && e < 2; e++) {
        uint32_t eye[4] = {(uint32_t)e, 0, 0, 0};
        D3D11_BUFFER_DESC ed = {16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA init = {eye, 0, 0};
        ok = SUCCEEDED(ID3D11Device_CreateBuffer(gDevice, &ed, &init, &gEyeCb[e]));
    }
    InitFieldSets();
    if (!ok) SetError("cannot create the one-pass shaders");
    return ok;
}

// Everything Emit and the strands passes change, saved before and put back after.
#define SAVED_PS_SRVS 80
#define SAVED_CS_SRVS 16
#define SAVED_CS_UAVS 6
struct SavedState {
    ID3D11VertexShader *vs;
    ID3D11HullShader *hs;
    ID3D11DomainShader *ds;
    ID3D11GeometryShader *gs;
    ID3D11PixelShader *ps;
    ID3D11ComputeShader *cs;
    ID3D11Buffer *cb[4][2], *csCb[2];
    ID3D11ShaderResourceView *dsSrv[4], *psSrv[SAVED_PS_SRVS], *csSrv[SAVED_CS_SRVS];
    ID3D11UnorderedAccessView *csUav[SAVED_CS_UAVS];
    ID3D11SamplerState *dsSmp[2], *psSmp[ROLE_COUNT], *csSmp[2];
    ID3D11InputLayout *layout;
    ID3D11Buffer *ib;
    DXGI_FORMAT ibFormat;
    UINT ibOffset;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    ID3D11RasterizerState *rs;
    UINT vpCount, scissorCount;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    D3D11_RECT scissor[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    ID3D11BlendState *blend;
    float blendFactor[4];
    UINT sampleMask;
    ID3D11DepthStencilState *dss;
    UINT stencilRef;
    ID3D11RenderTargetView *rtv[8];
    ID3D11DepthStencilView *dsv;
};

static void SaveState(SavedState &s) {
    ID3D11DeviceContext *c = gCtx;
    memset(&s, 0, sizeof s);
    ID3D11DeviceContext_VSGetShader(c, &s.vs, NULL, NULL);
    ID3D11DeviceContext_HSGetShader(c, &s.hs, NULL, NULL);
    ID3D11DeviceContext_DSGetShader(c, &s.ds, NULL, NULL);
    ID3D11DeviceContext_GSGetShader(c, &s.gs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(c, &s.ps, NULL, NULL);
    ID3D11DeviceContext_HSGetConstantBuffers(c, 0, 2, s.cb[0]);
    ID3D11DeviceContext_DSGetConstantBuffers(c, 0, 2, s.cb[1]);
    ID3D11DeviceContext_GSGetConstantBuffers(c, 0, 2, s.cb[2]);
    ID3D11DeviceContext_PSGetConstantBuffers(c, 0, 2, s.cb[3]);
    ID3D11DeviceContext_DSGetShaderResources(c, 0, 4, s.dsSrv);
    ID3D11DeviceContext_PSGetShaderResources(c, 0, SAVED_PS_SRVS, s.psSrv);
    ID3D11DeviceContext_DSGetSamplers(c, 0, 2, s.dsSmp);
    ID3D11DeviceContext_PSGetSamplers(c, 0, ROLE_COUNT, s.psSmp);
    ID3D11DeviceContext_CSGetShader(c, &s.cs, NULL, NULL);
    ID3D11DeviceContext_CSGetConstantBuffers(c, 0, 2, s.csCb);
    ID3D11DeviceContext_CSGetShaderResources(c, 0, SAVED_CS_SRVS, s.csSrv);
    ID3D11DeviceContext_CSGetUnorderedAccessViews(c, 0, SAVED_CS_UAVS, s.csUav);
    ID3D11DeviceContext_CSGetSamplers(c, 0, 2, s.csSmp);
    ID3D11DeviceContext_IAGetInputLayout(c, &s.layout);
    ID3D11DeviceContext_IAGetIndexBuffer(c, &s.ib, &s.ibFormat, &s.ibOffset);
    ID3D11DeviceContext_IAGetPrimitiveTopology(c, &s.topology);
    ID3D11DeviceContext_RSGetState(c, &s.rs);
    s.vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11DeviceContext_RSGetViewports(c, &s.vpCount, s.vp);
    s.scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11DeviceContext_RSGetScissorRects(c, &s.scissorCount, s.scissor);
    ID3D11DeviceContext_OMGetBlendState(c, &s.blend, s.blendFactor, &s.sampleMask);
    ID3D11DeviceContext_OMGetDepthStencilState(c, &s.dss, &s.stencilRef);
    ID3D11DeviceContext_OMGetRenderTargets(c, 8, s.rtv, &s.dsv);
}

static void RestoreState(SavedState &s, bool targets) {
    ID3D11DeviceContext *c = gCtx;
    ID3D11DeviceContext_VSSetShader(c, s.vs, NULL, 0);
    ID3D11DeviceContext_HSSetShader(c, s.hs, NULL, 0);
    ID3D11DeviceContext_DSSetShader(c, s.ds, NULL, 0);
    ID3D11DeviceContext_GSSetShader(c, s.gs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, s.ps, NULL, 0);
    ID3D11DeviceContext_HSSetConstantBuffers(c, 0, 2, s.cb[0]);
    ID3D11DeviceContext_DSSetConstantBuffers(c, 0, 2, s.cb[1]);
    ID3D11DeviceContext_GSSetConstantBuffers(c, 0, 2, s.cb[2]);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 2, s.cb[3]);
    ID3D11DeviceContext_DSSetShaderResources(c, 0, 4, s.dsSrv);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, SAVED_PS_SRVS, s.psSrv);
    ID3D11DeviceContext_DSSetSamplers(c, 0, 2, s.dsSmp);
    ID3D11DeviceContext_PSSetSamplers(c, 0, ROLE_COUNT, s.psSmp);
    ID3D11DeviceContext_CSSetShader(c, s.cs, NULL, 0);
    ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 2, s.csCb);
    ID3D11DeviceContext_CSSetShaderResources(c, 0, SAVED_CS_SRVS, s.csSrv);
    UINT keep[SAVED_CS_UAVS];
    for (UINT &k : keep) k = (UINT)-1;
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, SAVED_CS_UAVS, s.csUav, keep);
    ID3D11DeviceContext_CSSetSamplers(c, 0, 2, s.csSmp);
    ID3D11DeviceContext_IASetInputLayout(c, s.layout);
    ID3D11DeviceContext_IASetIndexBuffer(c, s.ib, s.ibFormat, s.ibOffset);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, s.topology);
    ID3D11DeviceContext_RSSetState(c, s.rs);
    ID3D11DeviceContext_RSSetViewports(c, s.vpCount, s.vp);
    ID3D11DeviceContext_RSSetScissorRects(c, s.scissorCount, s.scissor);
    ID3D11DeviceContext_OMSetBlendState(c, s.blend, s.blendFactor, s.sampleMask);
    ID3D11DeviceContext_OMSetDepthStencilState(c, s.dss, s.stencilRef);
    if (targets) ID3D11DeviceContext_OMSetRenderTargets(c, 8, s.rtv, s.dsv);
    REL(s.vs);
    REL(s.hs);
    REL(s.ds);
    REL(s.gs);
    REL(s.ps);
    REL(s.cs);
    for (auto &st : s.cb)
        for (auto &b : st) REL(b);
    for (auto &b : s.csCb) REL(b);
    for (auto &v : s.dsSrv) REL(v);
    for (auto &v : s.psSrv) REL(v);
    for (auto &v : s.csSrv) REL(v);
    for (auto &v : s.csUav) REL(v);
    for (auto &v : s.dsSmp) REL(v);
    for (auto &v : s.psSmp) REL(v);
    for (auto &v : s.csSmp) REL(v);
    REL(s.layout);
    REL(s.ib);
    REL(s.rs);
    REL(s.blend);
    REL(s.dss);
    for (auto &v : s.rtv) REL(v);
    REL(s.dsv);
}

// ---- test capture: the hair draws of one opaque pass as Unity issued them (tests\onepass_compare --replay) -----------
//
// In merge mode, on request: every hair ForwardBase / ForwardAdd draw (programs, the contents of their constant buffers,
// render state, viewport, scissor rectangles, target, textures and samplers, whether merging took it), every one-pass
// draw (what was uploaded, its state, textures and samplers), and once each the particle, barycentric and index buffers
// and the textures they use (all mip levels and faces). Written at the end of the opaque pass. Records: u32 type, then
// the fields below; all little-endian.

enum { CAP_DRAW = 1, CAP_EMIT = 2, CAP_BUFFER = 3, CAP_TEXTURE = 4, CAP_OTHER = 5 };
static char gCapturePath[MAX_PATH];
static volatile LONG gCaptureRequest, gCapturesTaken;
static bool gCapturing;
static std::string gCap;
static std::vector<void *> gCapBuffers;  // identity only, during the opaque pass

static void CapPut(const void *p, size_t n) { gCap.append((const char *)p, n); }
static void CapU32(uint32_t v) { CapPut(&v, 4); }

// The id of a buffer in the capture; its contents are read back and recorded the first time (a GPU wait).
static int CapBuffer(ID3D11Resource *r) {
    if (!r) return -1;
    for (size_t i = 0; i < gCapBuffers.size(); i++)
        if (gCapBuffers[i] == r) return (int)i;
    D3D11_BUFFER_DESC d;
    ID3D11Buffer_GetDesc((ID3D11Buffer *)r, &d);
    D3D11_BUFFER_DESC sd = {d.ByteWidth, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    ID3D11Buffer *st = NULL;
    if (FAILED(ID3D11Device_CreateBuffer(gDevice, &sd, NULL, &st))) return -1;
    ID3D11DeviceContext_CopyResource(gCtx, (ID3D11Resource *)st, r);
    D3D11_MAPPED_SUBRESOURCE m;
    int id = -1;
    if (SUCCEEDED(ID3D11DeviceContext_Map(gCtx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        id = (int)gCapBuffers.size();
        gCapBuffers.push_back(r);
        CapU32(CAP_BUFFER);
        CapU32((uint32_t)id);
        CapU32(d.ByteWidth);
        CapU32(d.StructureByteStride);
        CapU32(d.BindFlags);
        CapPut(m.pData, d.ByteWidth);
        ID3D11DeviceContext_Unmap(gCtx, (ID3D11Resource *)st, 0);
    }
    ID3D11Buffer_Release(st);
    return id;
}

static bool BlockCompressed(DXGI_FORMAT f) {
    return (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) || (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB);
}

// The id of a 2D / cube texture in the capture (shares ids with buffers); recorded the first time: its description and
// every subresource (row pitch, rows, rows' bytes).
static int CapTexture(ID3D11Resource *r) {
    if (!r) return -1;
    for (size_t i = 0; i < gCapBuffers.size(); i++)
        if (gCapBuffers[i] == r) return (int)i;
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(r, &dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return -1;
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &d);
    if (d.SampleDesc.Count != 1) return -1;
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = d.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE;
    ID3D11Texture2D *st = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(gDevice, &sd, NULL, &st))) return -1;
    ID3D11DeviceContext_CopyResource(gCtx, (ID3D11Resource *)st, r);
    int id = (int)gCapBuffers.size();
    gCapBuffers.push_back(r);
    CapU32(CAP_TEXTURE);
    CapU32((uint32_t)id);
    CapPut(&d, sizeof d);
    for (UINT a = 0; a < d.ArraySize; a++)
        for (UINT m = 0; m < d.MipLevels; m++) {
            UINT sub = a * d.MipLevels + m, h = d.Height >> m;
            if (!h) h = 1;
            UINT rows = BlockCompressed(d.Format) ? (h + 3) / 4 : h;
            D3D11_MAPPED_SUBRESOURCE map;
            if (SUCCEEDED(ID3D11DeviceContext_Map(gCtx, (ID3D11Resource *)st, sub, D3D11_MAP_READ, 0, &map))) {
                CapU32(map.RowPitch);
                CapU32(rows);
                CapPut(map.pData, (size_t)map.RowPitch * rows);
                ID3D11DeviceContext_Unmap(gCtx, (ID3D11Resource *)st, sub);
            } else {
                CapU32(0);
                CapU32(0);
            }
        }
    ID3D11Texture2D_Release(st);
    return id;
}

static int CapViewTexture(ID3D11ShaderResourceView *v) {
    if (!v) return -1;
    ID3D11Resource *r;
    ID3D11ShaderResourceView_GetResource(v, &r);
    int id = CapTexture(r);
    ID3D11Resource_Release(r);
    return id;
}

static void CapScissor() {
    D3D11_RECT rc[4] = {};
    UINT n = 4;
    ID3D11DeviceContext_RSGetScissorRects(gCtx, &n, NULL);
    if (n > 4) n = 4;
    ID3D11DeviceContext_RSGetScissorRects(gCtx, &n, rc);
    CapU32(n);
    CapPut(rc, sizeof rc);
}

static int CapViewBuffer(ID3D11ShaderResourceView *v) {
    if (!v) return -1;
    ID3D11Resource *r;
    ID3D11ShaderResourceView_GetResource(v, &r);
    int id = CapBuffer(r);
    ID3D11Resource_Release(r);
    return id;
}

static void CapState(ID3D11RasterizerState *rs, ID3D11BlendState *bs, const float *factor, UINT mask, ID3D11DepthStencilState *dss, UINT ref) {
    D3D11_RASTERIZER_DESC rd = {};
    if (rs) ID3D11RasterizerState_GetDesc(rs, &rd);
    CapPut(&rd, sizeof rd);
    D3D11_BLEND_DESC bd = {};
    if (bs) ID3D11BlendState_GetDesc(bs, &bd);
    CapPut(&bd, sizeof bd);
    CapPut(factor, 16);
    CapU32(mask);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    if (dss) ID3D11DepthStencilState_GetDesc(dss, &dd);
    CapPut(&dd, sizeof dd);
    CapU32(ref);
}

static void CapSampler(ID3D11SamplerState *s) {
    D3D11_SAMPLER_DESC d = {};
    if (s) ID3D11SamplerState_GetDesc(s, &d);
    CapPut(&d, sizeof d);
}

static void CapTarget(ID3D11RenderTargetView *rtv) {
    D3D11_TEXTURE2D_DESC td = {};
    if (rtv) {
        ID3D11Resource *r;
        ID3D11RenderTargetView_GetResource(rtv, &r);
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &td);
        ID3D11Resource_Release(r);
    }
    CapU32(td.Width);
    CapU32(td.Height);
    CapU32((uint32_t)td.Format);
}

static int StageProgram(int stage) {
    ID3D11DeviceChild *sh = NULL;
    switch (stage) {
    case HOP_VS: ID3D11DeviceContext_VSGetShader(gCtx, (ID3D11VertexShader **)&sh, NULL, NULL); break;
    case HOP_HS: ID3D11DeviceContext_HSGetShader(gCtx, (ID3D11HullShader **)&sh, NULL, NULL); break;
    case HOP_DS: ID3D11DeviceContext_DSGetShader(gCtx, (ID3D11DomainShader **)&sh, NULL, NULL); break;
    case HOP_GS: ID3D11DeviceContext_GSGetShader(gCtx, (ID3D11GeometryShader **)&sh, NULL, NULL); break;
    case HOP_PS: ID3D11DeviceContext_PSGetShader(gCtx, (ID3D11PixelShader **)&sh, NULL, NULL); break;
    }
    int p = TagOf(sh);
    if (sh) ID3D11DeviceChild_Release(sh);
    return p;
}

// A hair ForwardBase / ForwardAdd draw (before merging decides); returns where its "taken" field is, or 0.
static size_t CaptureDraw(UINT n, UINT start, INT base) {
    int prog[5];
    for (int st = 0; st < 5; st++) prog[st] = StageProgram(st);
    if (prog[HOP_PS] < 0 || (kHopPrograms[prog[HOP_PS]].pass != HOP_PASS_FB && kHopPrograms[prog[HOP_PS]].pass != HOP_PASS_FA)) return 0;
    ID3D11DeviceContext *c = gCtx;
    ID3D11ShaderResourceView *dsSrv[4] = {};
    ID3D11DeviceContext_DSGetShaderResources(c, 0, 4, dsSrv);
    int part = CapViewBuffer(dsSrv[2]), bary = CapViewBuffer(dsSrv[3]);
    int dsTex[2] = {CapViewTexture(dsSrv[0]), CapViewTexture(dsSrv[1])};
    for (auto &v : dsSrv) REL(v);
    int psTex[8];
    {
        ID3D11ShaderResourceView *v[8] = {};
        ID3D11DeviceContext_PSGetShaderResources(gCtx, 0, 8, v);
        for (int i = 0; i < 8; i++) {
            psTex[i] = CapViewTexture(v[i]);
            REL(v[i]);
        }
    }
    ID3D11Buffer *ib = NULL;
    DXGI_FORMAT ibFormat;
    UINT ibOffset;
    ID3D11DeviceContext_IAGetIndexBuffer(c, &ib, &ibFormat, &ibOffset);
    int ibId = CapBuffer((ID3D11Resource *)ib);
    REL(ib);
    D3D11_VIEWPORT vp = {};
    UINT nvp = 1;
    ID3D11DeviceContext_RSGetViewports(c, &nvp, &vp);

    CapU32(CAP_DRAW);
    CapU32(kHopPrograms[prog[HOP_PS]].pass);
    CapU32(vp.TopLeftX > 0 ? 1 : 0);
    CapU32(n);
    CapU32(start);
    CapU32((uint32_t)base);
    CapU32((uint32_t)part);
    CapU32((uint32_t)bary);
    CapU32((uint32_t)ibId);
    CapU32((uint32_t)ibFormat);
    CapU32(ibOffset);
    for (int st = 0; st < 5; st++) {
        char hex[12] = {};
        if (prog[st] >= 0) memcpy(hex, kHopPrograms[prog[st]].hex, 10);
        CapPut(hex, 12);
    }
    CapPut(&vp, sizeof vp);
    CapScissor();
    for (int i = 0; i < 2; i++) CapU32((uint32_t)dsTex[i]);
    for (int i = 0; i < 8; i++) CapU32((uint32_t)psTex[i]);
    ID3D11RasterizerState *rs = NULL;
    ID3D11BlendState *bs = NULL;
    ID3D11DepthStencilState *dss = NULL;
    float factor[4];
    UINT mask, ref;
    ID3D11DeviceContext_RSGetState(c, &rs);
    ID3D11DeviceContext_OMGetBlendState(c, &bs, factor, &mask);
    ID3D11DeviceContext_OMGetDepthStencilState(c, &dss, &ref);
    CapState(rs, bs, factor, mask, dss, ref);
    REL(rs);
    REL(bs);
    REL(dss);
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(c, 1, &rtv, NULL);
    CapTarget(rtv);
    REL(rtv);
    // constant buffers of every program: stage, slot, bytes, contents (from the CPU copies)
    std::string cbs;
    uint32_t count = 0;
    for (int st = 0; st < 5; st++) {
        if (prog[st] < 0) continue;
        const HopProgram &pr = kHopPrograms[prog[st]];
        ID3D11Buffer *bound[14] = {};
        GetConstantBuffers(st, bound);
        for (int k = 0; k < pr.cbCount; k++) {
            Shadow *sh = pr.cbs[k].slot < 14 ? Watch(bound[pr.cbs[k].slot]) : NULL;
            uint32_t head[3] = {(uint32_t)st, pr.cbs[k].slot, sh ? sh->size : 0u};
            cbs.append((const char *)head, sizeof head);
            if (sh) cbs.append((const char *)sh->data, sh->size);
            count++;
        }
        for (auto &b : bound) REL(b);
    }
    CapU32(count);
    gCap += cbs;
    // pixel shader textures t0..t7 (dimension, format, size) and samplers s0..s7
    ID3D11ShaderResourceView *srv[8] = {};
    ID3D11SamplerState *smp[8] = {};
    ID3D11DeviceContext_PSGetShaderResources(c, 0, 8, srv);
    ID3D11DeviceContext_PSGetSamplers(c, 0, 8, smp);
    for (int i = 0; i < 8; i++) {
        D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
        D3D11_TEXTURE2D_DESC td = {};
        if (srv[i]) {
            ID3D11ShaderResourceView_GetDesc(srv[i], &d);
            ID3D11Resource *r;
            ID3D11ShaderResourceView_GetResource(srv[i], &r);
            D3D11_RESOURCE_DIMENSION dim;
            ID3D11Resource_GetType(r, &dim);
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &td);
            ID3D11Resource_Release(r);
        }
        uint32_t v[4] = {(uint32_t)d.ViewDimension, (uint32_t)d.Format, td.Width, td.Height};
        CapPut(v, sizeof v);
        CapSampler(smp[i]);
        REL(srv[i]);
        REL(smp[i]);
    }
    size_t taken = gCap.size();
    CapU32(0);
    return taken;
}

// Any other draw in the opaque pass: its blend and depth state, viewport and size (how the hair's place in the order
// matters: blended or not depth-writing draws between a hair item's ForwardBase and its one-pass draw).
static void CaptureOther(UINT n, UINT instances) {
    ID3D11BlendState *bs = NULL;
    ID3D11DepthStencilState *dss = NULL;
    float factor[4];
    UINT mask, ref;
    ID3D11DeviceContext_OMGetBlendState(gCtx, &bs, factor, &mask);
    ID3D11DeviceContext_OMGetDepthStencilState(gCtx, &dss, &ref);
    D3D11_BLEND_DESC bd = {};
    D3D11_DEPTH_STENCIL_DESC dd = {};
    if (bs) ID3D11BlendState_GetDesc(bs, &bd);
    if (dss) ID3D11DepthStencilState_GetDesc(dss, &dd);
    REL(bs);
    REL(dss);
    D3D11_VIEWPORT vp = {};
    UINT nvp = 1;
    ID3D11DeviceContext_RSGetViewports(gCtx, &nvp, &vp);
    ID3D11PixelShader *ps = NULL;
    ID3D11DeviceContext_PSGetShader(gCtx, &ps, NULL, NULL);
    CapU32(CAP_OTHER);
    CapU32(n);
    CapU32(instances);
    CapU32((uint32_t)(uintptr_t)ps);  // identity only
    REL(ps);
    const D3D11_RENDER_TARGET_BLEND_DESC &b = bd.RenderTarget[0];
    uint32_t v[10] = {(uint32_t)b.BlendEnable, (uint32_t)b.SrcBlend, (uint32_t)b.DestBlend, (uint32_t)b.BlendOp, (uint32_t)b.RenderTargetWriteMask,
                      (uint32_t)dd.DepthEnable, (uint32_t)dd.DepthWriteMask, (uint32_t)dd.DepthFunc, (uint32_t)dd.StencilEnable, (uint32_t)bd.AlphaToCoverageEnable};
    CapPut(v, sizeof v);
    CapPut(&vp, sizeof vp);
}

// A one-pass draw: the item's buffers and what Emit set and uploaded.
static void CaptureEmit(EyeCap &e, int eye, int n, int screen, const HopItemData &upload) {
    int part = CapViewBuffer(e.dsSrv[2]), bary = CapViewBuffer(e.dsSrv[3]), ibId = CapBuffer((ID3D11Resource *)e.ib);
    int dsTex[2] = {CapViewTexture(e.dsSrv[0]), CapViewTexture(e.dsSrv[1])};
    int psTex[34];
    for (int i = 0; i < 34; i++) psTex[i] = -1;
    psTex[0] = CapViewTexture(e.rotation);
    for (int k = 0; k < n; k++) {
        psTex[2 + k] = CapViewTexture(e.lights[k].cookie);
        psTex[10 + k] = CapViewTexture(e.lights[k].atten);
        psTex[18 + k] = CapViewTexture(e.lights[k].shadow);
        psTex[26 + k] = CapViewTexture(e.lights[k].cube);
    }
    CapU32(CAP_EMIT);
    CapU32((uint32_t)eye);
    CapU32((uint32_t)n);
    CapU32((uint32_t)screen);
    CapU32(e.indexCount);
    CapU32(e.startIndex);
    CapU32((uint32_t)e.baseVertex);
    CapU32((uint32_t)part);
    CapU32((uint32_t)bary);
    CapU32((uint32_t)ibId);
    CapU32((uint32_t)e.ibFormat);
    CapU32(e.ibOffset);
    CapPut(&e.vp, sizeof e.vp);
    D3D11_RECT rc[4] = {};
    for (UINT i = 0; i < e.scissorCount && i < 4; i++) rc[i] = e.scissor[i];
    CapU32(e.scissorCount < 4 ? e.scissorCount : 4);
    CapPut(rc, sizeof rc);
    for (int i = 0; i < 2; i++) CapU32((uint32_t)dsTex[i]);
    for (int i = 0; i < 34; i++) CapU32((uint32_t)psTex[i]);
    CapState(e.rs, e.blend, e.blendFactor, e.sampleMask, e.dss, e.stencilRef);
    CapTarget(e.rtv);
    for (int r = 0; r < ROLE_COUNT; r++) CapSampler(e.role[r]);
    CapU32(sizeof upload);
    CapPut(&upload, sizeof upload);
}

static void CaptureBegin() {
    gCap.clear();
    gCapBuffers.clear();
    CapPut("HOPC", 4);
    CapU32(4);  // version (4: the one-pass draws are lights-only draws with the ForwardAdd state)
    gCapturing = true;
}

static void CaptureEnd() {
    gCapturing = false;
    FILE *f = NULL;
    if (fopen_s(&f, gCapturePath, "wb") == 0 && f) {
        fwrite(gCap.data(), 1, gCap.size(), f);
        fclose(f);
        InterlockedIncrement(&gCapturesTaken);
    }
    gCap.clear();
    gCap.shrink_to_fit();
    gCapBuffers.clear();
    InterlockedExchange(&gCaptureRequest, 0);
}

static HopItemData gUpload;

// Draws the lights taken so far of one eye of an item (none: nothing to draw).
static void Emit(Item &it, int eye) {
    EyeCap &e = it.eye[eye];
    if (e.state != 1) return;
    e.state = 2;
    int n = e.lightCount;
    if (n == 0) return;
    bool screen = false;
    gUpload = it.data;
    gUpload.flags[0] = (uint32_t)n;
    gUpload.flags[1] = 0;
    for (int k = 0; k < n; k++) {
        gUpload.lights[k] = e.lights[k].data;
        screen = screen || e.lights[k].data.kind[1] == HOP_SHADOW_SCREEN;
    }
    if (gCapturing) CaptureEmit(e, eye, n, screen ? 1 : 0, gUpload);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(gCtx, (ID3D11Resource *)gItemCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, &gUpload, sizeof gUpload);
    ID3D11DeviceContext_Unmap(gCtx, (ID3D11Resource *)gItemCb, 0);

    SavedState saved;
    SaveState(saved);
    ID3D11DeviceContext *c = gCtx;
    int s = screen ? 1 : 0;
    ID3D11DeviceContext_VSSetShader(c, gHopVS, NULL, 0);
    ID3D11DeviceContext_HSSetShader(c, gHopHS, NULL, 0);
    ID3D11DeviceContext_DSSetShader(c, gHopDS[n], NULL, 0);
    ID3D11DeviceContext_GSSetShader(c, gHopGS[n][s], NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, gHopPS[n][s], NULL, 0);
    ID3D11Buffer *cbs[2] = {gItemCb, gEyeCb[eye]};
    ID3D11DeviceContext_HSSetConstantBuffers(c, 0, 2, cbs);
    ID3D11DeviceContext_DSSetConstantBuffers(c, 0, 2, cbs);
    ID3D11DeviceContext_GSSetConstantBuffers(c, 0, 2, cbs);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 2, cbs);
    ID3D11DeviceContext_DSSetShaderResources(c, 0, 4, e.dsSrv);
    ID3D11DeviceContext_DSSetSamplers(c, 0, 2, e.dsSmp);
    ID3D11ShaderResourceView *ps[34] = {};
    ps[0] = e.rotation;
    for (int k = 0; k < n; k++) {
        ps[2 + k] = e.lights[k].cookie;
        ps[10 + k] = e.lights[k].atten;
        ps[18 + k] = e.lights[k].shadow;
        ps[26 + k] = e.lights[k].cube;
    }
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 34, ps);
    ID3D11DeviceContext_PSSetSamplers(c, 0, ROLE_COUNT, e.role);
    ID3D11DeviceContext_IASetIndexBuffer(c, e.ib, e.ibFormat, e.ibOffset);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    ID3D11DeviceContext_RSSetState(c, e.rs);
    ID3D11DeviceContext_RSSetViewports(c, 1, &e.vp);
    ID3D11DeviceContext_RSSetScissorRects(c, e.scissorCount, e.scissor);
    ID3D11DeviceContext_OMSetBlendState(c, e.blend, e.blendFactor, e.sampleMask);
    ID3D11DeviceContext_OMSetDepthStencilState(c, e.dss, e.stencilRef);
    bool targets = saved.rtv[0] != e.rtv || saved.dsv != e.dsv;
    for (int i = 1; i < 8; i++) targets = targets || saved.rtv[i];
    if (targets) ID3D11DeviceContext_OMSetRenderTargets(c, 1, &e.rtv, e.dsv);
    gEmitting = true;
    oDrawIndexed(c, e.indexCount, e.startIndex, e.baseVertex);
    gEmitting = false;
    RestoreState(saved, targets);
    gMergeCur.draws++;
    gMergeCur.lights += n;
}

static void EndRun() {
    if (!gRun) return;
    Emit(*gRun, 0);
    if(VamViewCount()==2)Emit(*gRun, 1);
    gRun = NULL;
}

static int ShaderProgram(int stage) {
    ID3D11DeviceChild *sh = NULL;
    switch (stage) {
    case HOP_HS: ID3D11DeviceContext_HSGetShader(gCtx, (ID3D11HullShader **)&sh, NULL, NULL); break;
    case HOP_DS: ID3D11DeviceContext_DSGetShader(gCtx, (ID3D11DomainShader **)&sh, NULL, NULL); break;
    case HOP_GS: ID3D11DeviceContext_GSGetShader(gCtx, (ID3D11GeometryShader **)&sh, NULL, NULL); break;
    case HOP_PS: ID3D11DeviceContext_PSGetShader(gCtx, (ID3D11PixelShader **)&sh, NULL, NULL); break;
    }
    int p = TagOf(sh);
    if (sh) ID3D11DeviceChild_Release(sh);
    return p;
}

static bool IsProgram(int p, const char *hex) { return p >= 0 && !strcmp(kHopPrograms[p].hex, hex); }

// Notes the ForwardBase draw of one eye (Unity draws it): the item's constants, its buffers and viewport.
static int CaptureFb(Item &it, EyeCap &e, int ps, UINT n, UINT s, INT b, const D3D11_VIEWPORT &vp) {
    int hs = ShaderProgram(HOP_HS), ds = ShaderProgram(HOP_DS), gs = ShaderProgram(HOP_GS);
    bool screen = IsProgram(ps, "6008f0bf73");
    bool domain=VamDesktopDisplay()?IsProgram(ds,"3849722cc3"):IsProgram(ds,"7c48fb0258");
    bool geometry=VamDesktopDisplay()?(IsProgram(gs,"df01d6c426")||IsProgram(gs,"7ffee7c396")):(IsProgram(gs,"ca47be29fb")||IsProgram(gs,"bbc764424a"));
    if (!(screen || IsProgram(ps, "3db8440d12")) || !domain || !geometry)
        return WHY_FB_PROGRAM;
    if(VamDesktopDisplay()) { it.data.stereoScaleOffset[0].v[0]=1;it.data.stereoScaleOffset[0].v[1]=1; }
    uint8_t *item = (uint8_t *)&it.data;
    uint64_t all = ~0ull;
    if (!Extract(HOP_HS, hs, item, &it.filled, all, NULL, NULL) || !Extract(HOP_DS, ds, item, &it.filled, all, NULL, NULL) ||
        !Extract(HOP_GS, gs, item, &it.filled, all, NULL, NULL) || !Extract(HOP_PS, ps, item, &it.filled, all, NULL, NULL))
        return WHY_FB_VALUES;
    if ((it.filled & gFbFields) != gFbFields) return WHY_FB_VALUES;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    ID3D11DeviceContext_IAGetPrimitiveTopology(gCtx, &topology);
    if (topology != D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST) return WHY_FB_STATE;
    ID3D11DeviceContext_IAGetIndexBuffer(gCtx, &e.ib, &e.ibFormat, &e.ibOffset);
    e.vp = vp;
    ID3D11DeviceContext_DSGetShaderResources(gCtx, 0, 4, e.dsSrv);
    ID3D11DeviceContext_DSGetSamplers(gCtx, 0, 2, e.dsSmp);
    if (!e.ib || !e.dsSrv[2] || !e.dsSrv[3]) return WHY_FB_STATE;
    e.indexCount = n;
    e.startIndex = s;
    e.baseVertex = b;
    return -1;
}

// Uses a light sampler for a role: the first light sets it, the others must use the same.
static bool UseRole(EyeCap &e, int role, ID3D11SamplerState *smp) {
    if (!smp) return false;
    if (!e.role[role]) {
        e.role[role] = smp;
        ID3D11SamplerState_AddRef(smp);
        return true;
    }
    return e.role[role] == smp;
}

// Takes one ForwardAdd draw: the light's constants, textures and samplers.
static int CaptureLight(Item &it, EyeCap &e, int ps, ID3D11RenderTargetView *expectedRtv = NULL, ID3D11DepthStencilView *expectedDsv = NULL) {
    if (e.lightCount == HOP_MAX_LIGHTS) return WHY_FA_FULL;
    uint32_t kw = kHopPrograms[ps].keywords;
    if (kw & (HOP_KW_POINT_COOKIE | HOP_KW_DIRECTIONAL_COOKIE)) return WHY_FA_LIGHT;
    int kind = kw & HOP_KW_POINT ? HOP_LIGHT_POINT : kw & HOP_KW_SPOT ? HOP_LIGHT_SPOT : HOP_LIGHT_DIRECTIONAL;
    bool soft = (kw & HOP_KW_SHADOWS_SOFT) != 0;
    int shadow = kw & HOP_KW_SHADOWS_DEPTH ? (soft ? HOP_SHADOW_DEPTH_SOFT : HOP_SHADOW_DEPTH)
               : kw & HOP_KW_SHADOWS_CUBE ? (soft ? HOP_SHADOW_CUBE_SOFT : HOP_SHADOW_CUBE)
               : kw & HOP_KW_SHADOWS_SCREEN ? HOP_SHADOW_SCREEN : HOP_SHADOW_NONE;
    if ((kind == HOP_LIGHT_DIRECTIONAL) != ((shadow == HOP_SHADOW_NONE || shadow == HOP_SHADOW_SCREEN) && !(kw & (HOP_KW_POINT | HOP_KW_SPOT))))
        return WHY_FA_LIGHT;
    LightCap &L = e.lights[e.lightCount];
    memset(&L, 0, sizeof L);
    uint32_t lightMask = 0;
    uint8_t *item = (uint8_t *)&it.data;
    int hs = ShaderProgram(HOP_HS), ds = ShaderProgram(HOP_DS), gs = ShaderProgram(HOP_GS);
    (void)hs;
    if (!Extract(HOP_DS, ds, item, &it.filled, gCameraFields, (uint8_t *)&L.data, &lightMask) ||
        !Extract(HOP_GS, gs, item, &it.filled, gCameraFields, (uint8_t *)&L.data, &lightMask) ||
        !Extract(HOP_PS, ps, item, &it.filled, gCameraFields, (uint8_t *)&L.data, &lightMask))
        return WHY_FA_VALUES;
    auto has = [&](const char *name) { return (lightMask & (1u << FieldIndex(kHopLightFields, kHopLightFieldCount, name))) != 0; };
    bool ok = has("_WorldSpaceLightPos0") && has("_LightColor0");
    if (kind != HOP_LIGHT_DIRECTIONAL) ok = ok && has("unity_WorldToLight");
    if (shadow == HOP_SHADOW_DEPTH || shadow == HOP_SHADOW_DEPTH_SOFT) ok = ok && has("unity_WorldToShadow") && has("_LightShadowData");
    if (shadow == HOP_SHADOW_CUBE || shadow == HOP_SHADOW_CUBE_SOFT)
        ok = ok && has("_LightShadowData") && has("_LightPositionRange") && has("_LightProjectionParams") && (it.filled & gScreenFields) == gScreenFields;
    if (shadow == HOP_SHADOW_SCREEN) ok = ok && (it.filled & gScreenFields) == gScreenFields;
    if (!ok) return WHY_FA_VALUES;
    L.data.kind[0] = (uint32_t)kind;
    L.data.kind[1] = (uint32_t)shadow;

    ID3D11ShaderResourceView *srv[8] = {};
    ID3D11SamplerState *smp[8] = {};
    ID3D11DeviceContext_PSGetShaderResources(gCtx, 0, 8, srv);
    ID3D11DeviceContext_PSGetSamplers(gCtx, 0, 8, smp);
    int why = -1;
    const HopProgram &prog = kHopPrograms[ps];
    for (int t = 0; t < prog.texCount && why < 0; t++) {
        const HopRes &r = prog.tex[t];
        if (!r.name || r.slot >= 8 || r.sampler >= 8) continue;
        ID3D11ShaderResourceView *v = srv[r.slot];
        ID3D11SamplerState *sm = smp[r.sampler];
        ID3D11ShaderResourceView **dst = NULL;
        int role = -1;
        if (!strcmp(r.name, "unity_RandomRotation16")) {
            if (e.rotation && e.rotation != v) why = WHY_FA_SAMPLER;
            else if (!e.rotation && v) dst = &e.rotation;
            role = ROLE_ROTATION;
        } else if (!strcmp(r.name, "_LightTexture0")) {
            dst = kind == HOP_LIGHT_SPOT ? &L.cookie : &L.atten;
            role = kind == HOP_LIGHT_SPOT ? ROLE_COOKIE : ROLE_ATTEN;
        } else if (!strcmp(r.name, "_LightTextureB0")) {
            dst = &L.atten;
            role = ROLE_ATTEN;
        } else if (!strcmp(r.name, "_ShadowMapTexture")) {
            dst = shadow == HOP_SHADOW_CUBE || shadow == HOP_SHADOW_CUBE_SOFT ? &L.cube : &L.shadow;
            role = shadow == HOP_SHADOW_SCREEN ? ROLE_DIR_SCREEN : ROLE_COMPARE;
        }
        if (why < 0 && role >= 0 && !UseRole(e, role, sm)) why = WHY_FA_SAMPLER;
        if (why < 0 && dst && v) {
            *dst = v;
            ID3D11ShaderResourceView_AddRef(v);
        }
    }
    // soft shadows read the shadow map without comparison through s1
    if (why < 0 && soft && !UseRole(e, ROLE_RAW, smp[1])) why = WHY_FA_SAMPLER;
    for (int i = 0; i < 8; i++) {
        REL(srv[i]);
        REL(smp[i]);
    }
    // the draw's state: the first light's; every further light must have the same
    ID3D11RasterizerState *rs = NULL;
    ID3D11BlendState *blend = NULL;
    ID3D11DepthStencilState *dss = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11DepthStencilView *dsv = NULL;
    float factor[4];
    UINT mask, ref;
    ID3D11DeviceContext_RSGetState(gCtx, &rs);
    ID3D11DeviceContext_OMGetBlendState(gCtx, &blend, factor, &mask);
    ID3D11DeviceContext_OMGetDepthStencilState(gCtx, &dss, &ref);
    ID3D11DeviceContext_OMGetRenderTargets(gCtx, 1, &rtv, &dsv);
    bool same = e.lightCount == 0 ? rtv != NULL
                                  : rs == e.rs && blend == e.blend && dss == e.dss && rtv == e.rtv && dsv == e.dsv && mask == e.sampleMask &&
                                        ref == e.stencilRef && !memcmp(factor, e.blendFactor, sizeof factor);
    if (why < 0 && !same) why = WHY_FA_STATE;
    // A strands resolve shares its ForwardBase target. Reject a different target before
    // suppressing this draw, rather than discovering it after the light was captured.
    if (why < 0 && expectedRtv && (rtv != expectedRtv || dsv != expectedDsv)) why = WHY_HS_TARGET;
    // the light's scissor rectangle (the shader applies it per light; the draw's is the union)
    D3D11_RECT rc = {0, 0, 0xffff, 0xffff};
    if (why < 0 && rs) {
        D3D11_RASTERIZER_DESC rd;
        ID3D11RasterizerState_GetDesc(rs, &rd);
        if (rd.ScissorEnable) {
            D3D11_RECT r[1] = {};
            UINT nr = 1;
            ID3D11DeviceContext_RSGetScissorRects(gCtx, &nr, r);
            rc = nr ? r[0] : D3D11_RECT{0, 0, 0, 0};
            rc.left = std::min<LONG>(std::max<LONG>(rc.left, 0), 0xffff), rc.right = std::min<LONG>(std::max<LONG>(rc.right, 0), 0xffff);
            rc.top = std::min<LONG>(std::max<LONG>(rc.top, 0), 0xffff), rc.bottom = std::min<LONG>(std::max<LONG>(rc.bottom, 0), 0xffff);
        }
    }
    if (why >= 0 || e.lightCount > 0) {
        REL(rs);
        REL(blend);
        REL(dss);
        REL(rtv);
        REL(dsv);
    }
    if (why >= 0) {
        REL(L.cookie);
        REL(L.atten);
        REL(L.shadow);
        REL(L.cube);
        return why;
    }
    if (e.lightCount == 0) {
        e.rs = rs, e.blend = blend, e.dss = dss, e.rtv = rtv, e.dsv = dsv;
        memcpy(e.blendFactor, factor, sizeof factor);
        e.sampleMask = mask, e.stencilRef = ref;
        e.scissorCount = 1;
        e.scissor[0] = rc;
    } else {
        D3D11_RECT &u = e.scissor[0];
        u.left = std::min<LONG>(u.left, rc.left), u.top = std::min<LONG>(u.top, rc.top);
        u.right = std::max<LONG>(u.right, rc.right), u.bottom = std::max<LONG>(u.bottom, rc.bottom);
    }
    L.data.kind[2] = (uint32_t)rc.left | (uint32_t)rc.right << 16;
    L.data.kind[3] = (uint32_t)rc.top | (uint32_t)rc.bottom << 16;
    e.lightCount++;
    return -1;
}

static Item *FindItem(void *particles, void *ib, UINT start, UINT count) {
    for (int i = 0; i < gItemCount; i++)
        if (gItems[i].particles == particles && gItems[i].ib == ib && gItems[i].startIndex == start && gItems[i].indexCount == count)
            return &gItems[i];
    return NULL;
}

// ---- strands: hair drawn through a visibility buffer (mode 3) -------------------------------------------------------
//
// shaders/hair_strands.hlsl. A hair item's ForwardBase draws (both eyes) are taken as merging takes ForwardAdd draws:
// their constants, buffers and output state are kept and the draws skipped. When the ForwardBase draws are over (the
// next draw that is not one), every taken item's strand points are computed once (compute, per item), all segments'
// quads of both eyes rasterised in software into a visibility buffer (nearest depth, then which segment), and one
// full-target draw shades the ForwardBase lighting there and writes the depth. The taken items' ForwardAdd draws are
// taken too (each light's constants, textures and samplers, as merging does); compatible full-target batches then
// add their lights (up to 16 per batch, separate masks per item and eye), with the original passes' per-pixel attenuation
// and shadows. ForwardBase draws after that pass's first resolve, and items whose draws cannot be taken, go through
// unchanged (their ForwardAdd draws too: the depth there is the GPU's own). A ForwardAdd light of a taken item that
// cannot be taken goes through as well, but meets a depth the resolve wrote (equal to the GPU's within ~1e-6, not
// always exactly) and misses pixels: that ends strands for a while, the following opaque passes merge (mode 2), 30
// frames the first time and twice as long each time after.

static struct {
    ID3D11ComputeShader *build, *rasterDepth, *rasterId, *clipDepth, *clipId;
    ID3D11VertexShader *resolveVs;
    ID3D11PixelShader *resolveFb, *resolveFa, *clipResolveFb, *clipResolveFa;
    ID3D11RasterizerState *rs;  // resolve: no culling, no scissor
    ID3D11Buffer *dispatchCb, *frameCb, *items, *views, *lights, *domain, *args, *argsInit;
    ID3D11ShaderResourceView *itemsSrv, *viewsSrv, *lightsSrv, *domainSrv, *argsSrv;
    ID3D11UnorderedAccessView *argsUav;
    // grown as needed
    ID3D11Buffer *points, *live, *indices;
    ID3D11ShaderResourceView *pointsSrv, *liveSrv, *indicesSrv;
    ID3D11UnorderedAccessView *pointsUav, *liveUav;
    UINT pointsBytes, liveBytes, indicesBytes;
    ID3D11Buffer *clipped;
    ID3D11ShaderResourceView *clippedSrv;
    ID3D11UnorderedAccessView *clippedUav;
    UINT clippedBytes;
    ID3D11Texture2D *visDepth, *visId;
    ID3D11UnorderedAccessView *visDepthUav, *visIdUav;
    ID3D11ShaderResourceView *visDepthSrv, *visIdSrv;
    UINT visW, visH;
} gHs;
static bool gStrandsObjects;  // shaders and fixed buffers made (HOP_Init)
static bool gStrandsReady;    // and the isoline domain read back (OP_INSTALL)
static bool gStrandsPass;     // this opaque pass draws the items it can take with strands
static bool gFbOpen;          // ForwardBase draws taken since the pass's ForwardBase resolve
static bool gFbDone;          // the pass's ForwardBase resolve is drawn
static bool gFaOpen;          // ForwardAdd lights taken since the last ForwardAdd resolve
static EyeCap *gHsFirst, *gHsFirstEye[2];  // the pass's first taken ForwardBase draw (all must agree), per eye
static EyeCap *gHsFirstScreen;
static HsItem gHsItems[HS_MAX_ITEMS];
static int gHsSlot[64];  // gItems index -> gHsItems index, -1 none
static int gHsCount;
static uint32_t gHsSegments;
static uint64_t gHsAcceptedPoints, gHsAcceptedSegments, gHsAcceptedIndexBytes;
static int gStrandsFails, gStrandsBackoff;

static int TessCount(float f) { return f <= 0.0f ? 0 : (int)ceilf(std::min(std::max(f, 1.0f), 64.0f)); }

static bool HsBuffer(UINT bytes, D3D11_USAGE usage, UINT bind, UINT misc, UINT stride, const void *init, ID3D11Buffer **out) {
    D3D11_BUFFER_DESC d = {bytes, usage, bind, usage == D3D11_USAGE_DYNAMIC ? (UINT)D3D11_CPU_ACCESS_WRITE : 0u, misc, stride};
    D3D11_SUBRESOURCE_DATA s = {init, 0, 0};
    return SUCCEEDED(ID3D11Device_CreateBuffer(gDevice, &d, init ? &s : NULL, out));
}
static bool HsStructured(UINT count, UINT stride, ID3D11Buffer **b, ID3D11ShaderResourceView **srv) {
    if (!HsBuffer(count * stride, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, stride, NULL, b)) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
    v.Format = DXGI_FORMAT_UNKNOWN;
    v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    v.Buffer.NumElements = count;
    return SUCCEEDED(ID3D11Device_CreateShaderResourceView(gDevice, (ID3D11Resource *)*b, &v, srv));
}
static bool HsRawViews(ID3D11Buffer *b, UINT bytes, ID3D11ShaderResourceView **srv, ID3D11UnorderedAccessView **uav) {
    D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
    v.Format = DXGI_FORMAT_R32_TYPELESS;
    v.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    v.BufferEx.NumElements = bytes / 4;
    v.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    if (FAILED(ID3D11Device_CreateShaderResourceView(gDevice, (ID3D11Resource *)b, &v, srv))) return false;
    if (!uav) return true;
    D3D11_UNORDERED_ACCESS_VIEW_DESC u = {};
    u.Format = DXGI_FORMAT_R32_TYPELESS;
    u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements = bytes / 4;
    u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    return SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(gDevice, (ID3D11Resource *)b, &u, uav));
}
// A raw buffer of at least `bytes` (grown by half again when it is too small), with its views.
static bool HsEnsureRaw(ID3D11Buffer **b, ID3D11ShaderResourceView **srv, ID3D11UnorderedAccessView **uav, UINT *have, UINT bytes) {
    if (*b && *have >= bytes) return true;
    REL(*srv);
    if (uav) REL(*uav);
    REL(*b);
    *have = 0;
    UINT n = (UINT)((std::min<uint64_t>(std::max<uint64_t>((uint64_t)bytes + bytes / 2, 4096), 0xfffffff0ull) + 15) & ~15ull);
    if (!HsBuffer(n, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0), D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0,
                  NULL, b) ||
        !HsRawViews(*b, n, srv, uav))
        return false;
    *have = n;
    return true;
}
// The visibility buffer at the eye target's size.
static bool HsEnsureVis(UINT w, UINT h) {
    if (gHs.visDepth && gHs.visW == w && gHs.visH == h) return true;
    REL(gHs.visDepthUav);
    REL(gHs.visIdUav);
    REL(gHs.visDepthSrv);
    REL(gHs.visIdSrv);
    REL(gHs.visDepth);
    REL(gHs.visId);
    gHs.visW = gHs.visH = 0;
    D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, 0};
    if (FAILED(ID3D11Device_CreateTexture2D(gDevice, &d, NULL, &gHs.visDepth)) || FAILED(ID3D11Device_CreateTexture2D(gDevice, &d, NULL, &gHs.visId)) ||
        FAILED(ID3D11Device_CreateUnorderedAccessView(gDevice, (ID3D11Resource *)gHs.visDepth, NULL, &gHs.visDepthUav)) ||
        FAILED(ID3D11Device_CreateUnorderedAccessView(gDevice, (ID3D11Resource *)gHs.visId, NULL, &gHs.visIdUav)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(gDevice, (ID3D11Resource *)gHs.visDepth, NULL, &gHs.visDepthSrv)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(gDevice, (ID3D11Resource *)gHs.visId, NULL, &gHs.visIdSrv)))
        return false;
    gHs.visW = w, gHs.visH = h;
    return true;
}

static bool HsUpload(ID3D11Buffer *b, const void *data, size_t bytes) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(gCtx, (ID3D11Resource *)b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, data, bytes);
    ID3D11DeviceContext_Unmap(gCtx, (ID3D11Resource *)b, 0);
    return true;
}

static bool CreateStrandsObjects() {
    ID3D11Device *d = gDevice;
    bool ok = SUCCEEDED(ID3D11Device_CreateComputeShader(d, kHsBuild.code, kHsBuild.size, NULL, &gHs.build)) &&
              SUCCEEDED(ID3D11Device_CreateComputeShader(d, kHsRasterDepth.code, kHsRasterDepth.size, NULL, &gHs.rasterDepth)) &&
              SUCCEEDED(ID3D11Device_CreateComputeShader(d, kHsRasterId.code, kHsRasterId.size, NULL, &gHs.rasterId)) &&
              SUCCEEDED(ID3D11Device_CreateComputeShader(d, kHsClipDepth.code, kHsClipDepth.size, NULL, &gHs.clipDepth)) &&
              SUCCEEDED(ID3D11Device_CreateComputeShader(d, kHsClipId.code, kHsClipId.size, NULL, &gHs.clipId)) &&
              SUCCEEDED(ID3D11Device_CreateVertexShader(d, kHsResolveVS.code, kHsResolveVS.size, NULL, &gHs.resolveVs)) &&
              SUCCEEDED(ID3D11Device_CreatePixelShader(d, kHsResolveFb.code, kHsResolveFb.size, NULL, &gHs.resolveFb)) &&
              SUCCEEDED(ID3D11Device_CreatePixelShader(d, kHsResolveFa.code, kHsResolveFa.size, NULL, &gHs.resolveFa)) &&
              SUCCEEDED(ID3D11Device_CreatePixelShader(d, kHsClipResolveFb.code, kHsClipResolveFb.size, NULL, &gHs.clipResolveFb)) &&
              SUCCEEDED(ID3D11Device_CreatePixelShader(d, kHsClipResolveFa.code, kHsClipResolveFa.size, NULL, &gHs.clipResolveFa));
    D3D11_RASTERIZER_DESC r = {};
    r.FillMode = D3D11_FILL_SOLID;
    r.CullMode = D3D11_CULL_NONE;
    r.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(ID3D11Device_CreateRasterizerState(d, &r, &gHs.rs));
    ok = ok && HsBuffer(sizeof(HsDispatch), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, 0, 0, NULL, &gHs.dispatchCb) &&
         HsBuffer(sizeof(HsFrameCb), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, 0, 0, NULL, &gHs.frameCb) &&
         HsStructured(HS_MAX_ITEMS, sizeof(HsItem), &gHs.items, &gHs.itemsSrv) && HsStructured(2, sizeof(HsView), &gHs.views, &gHs.viewsSrv) &&
         HsStructured(HS_MAX_LIGHTS, sizeof(HsLight), &gHs.lights, &gHs.lightsSrv);
    // the id pass's thread groups (x y z) and the list's length; the depth pass counts them up from argsInit
    uint32_t init[12] = {0, 1, 1, 0, 0, 1, 1, 0, 3, 0, 0, 0};
    ok = ok && HsBuffer(sizeof init, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                        D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, 0, init, &gHs.args) &&
          HsBuffer(sizeof init, D3D11_USAGE_DEFAULT, 0, 0, 0, init, &gHs.argsInit) && HsRawViews(gHs.args, sizeof init, &gHs.argsSrv, &gHs.argsUav);
    if (!ok) SetError("cannot create the strands shaders and buffers");
    return ok;
}

// The isoline tessellator's domain coordinates (fixed point: up to 1.9e-4 from k / n), read back through stream-out
// once: u of detail 1..64 (k = 0..S), v of density 1..64 (j = 0..L-1). Render thread; the state it changes is put back.
static bool ReadDomain() {
    ID3D11Device *d = gDevice;
    ID3D11DeviceContext *c = gCtx;
    ID3D11VertexShader *vs = NULL;
    ID3D11HullShader *hs = NULL;
    ID3D11DomainShader *ds = NULL;
    ID3D11GeometryShader *so = NULL;
    ID3D11Buffer *out = NULL, *staging = NULL, *cb = NULL, *ib = NULL;
    ID3D11Query *stats = NULL;
    // per table: 2 vertices per line primitive (detail n on one line: n segments; density n with one segment: n lines)
    UINT vertices = 0;
    for (int n = 1; n <= 64; n++) vertices += 4 * n;
    UINT bytes = vertices * 16;
    D3D11_SO_DECLARATION_ENTRY decl = {0, "UV", 0, 0, 4, 0};
    UINT stride = 16;
    uint32_t idx[3] = {0, 1, 2};
    D3D11_QUERY_DESC qd = {D3D11_QUERY_SO_STATISTICS, 0};
    D3D11_BUFFER_DESC sd = {bytes, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
    bool ok = SUCCEEDED(ID3D11Device_CreateVertexShader(d, kHsDomainVS.code, kHsDomainVS.size, NULL, &vs)) &&
              SUCCEEDED(ID3D11Device_CreateHullShader(d, kHsDomainHS.code, kHsDomainHS.size, NULL, &hs)) &&
              SUCCEEDED(ID3D11Device_CreateDomainShader(d, kHsDomainDS.code, kHsDomainDS.size, NULL, &ds)) &&
              SUCCEEDED(ID3D11Device_CreateGeometryShaderWithStreamOutput(d, kHsDomainDS.code, kHsDomainDS.size, &decl, 1, &stride, 1,
                                                                          D3D11_SO_NO_RASTERIZED_STREAM, NULL, &so)) &&
              HsBuffer(bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, NULL, &out) &&
              SUCCEEDED(ID3D11Device_CreateBuffer(d, &sd, NULL, &staging)) &&
              HsBuffer(16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, 0, 0, NULL, &cb) &&
              HsBuffer(12, D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, idx, &ib) && SUCCEEDED(ID3D11Device_CreateQuery(d, &qd, &stats));
    std::vector<float> domain(HS_DOMAIN_VALUES);
    if (ok) {
        SavedState saved;
        SaveState(saved);
        ID3D11Buffer *soWas = NULL;
        ID3D11DeviceContext_SOGetTargets(c, 1, &soWas);
        ID3D11DeviceContext_IASetInputLayout(c, NULL);
        ID3D11DeviceContext_IASetIndexBuffer(c, ib, DXGI_FORMAT_R32_UINT, 0);
        ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
        ID3D11DeviceContext_VSSetShader(c, vs, NULL, 0);
        ID3D11DeviceContext_HSSetShader(c, hs, NULL, 0);
        ID3D11DeviceContext_HSSetConstantBuffers(c, 0, 1, &cb);
        ID3D11DeviceContext_DSSetShader(c, ds, NULL, 0);
        ID3D11DeviceContext_GSSetShader(c, so, NULL, 0);
        ID3D11DeviceContext_PSSetShader(c, NULL, NULL, 0);
        UINT zero = 0;
        ID3D11DeviceContext_SOSetTargets(c, 1, &out, &zero);
        ID3D11DeviceContext_Begin(c, (ID3D11Asynchronous *)stats);
        for (int n = 1; n <= 64; n++)
            for (int t = 0; t < 2; t++) {
                float f[4] = {t ? (float)n : 1.0f, t ? 1.0f : (float)n, 0, 0};  // t 0: detail n; t 1: density n
                HsUpload(cb, f, 16);
                oDrawIndexed(c, 3, 0, 0);  // appends after the previous draw's output
            }
        ID3D11DeviceContext_End(c, (ID3D11Asynchronous *)stats);
        ID3D11Buffer *none = NULL;
        ID3D11DeviceContext_SOSetTargets(c, 1, soWas ? &soWas : &none, &zero);
        REL(soWas);
        RestoreState(saved, false);
        ID3D11DeviceContext_CopyResource(c, (ID3D11Resource *)staging, (ID3D11Resource *)out);
        D3D11_QUERY_DATA_SO_STATISTICS st = {};
        while (ID3D11DeviceContext_GetData(c, (ID3D11Asynchronous *)stats, &st, sizeof st, 0) == S_FALSE) {}
        D3D11_MAPPED_SUBRESOURCE m = {};
        ok = st.NumPrimitivesWritten == vertices / 2 && SUCCEEDED(ID3D11DeviceContext_Map(c, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m));
        if (!ok) SetError("isoline domain: %llu line primitives, expected %u", (unsigned long long)st.NumPrimitivesWritten, vertices / 2);
        if (ok) {
            const float *v = (const float *)m.pData;
            for (int n = 1; n <= 64 && ok; n++) {
                for (int t = 0; t < 2 && ok; t++) {
                    std::vector<float> s;
                    for (int i = 0; i < 2 * n; i++) s.push_back(v[i * 4 + t]);
                    v += 2 * n * 4;
                    std::sort(s.begin(), s.end());
                    s.erase(std::unique(s.begin(), s.end()), s.end());
                    if ((int)s.size() != (t ? n : n + 1)) {
                        SetError("isoline domain: %d values for %s %d", (int)s.size(), t ? "density" : "detail", n);
                        ok = false;
                        break;
                    }
                    float *dst = t ? &domain[2144 + n * (n - 1) / 2] : &domain[(n - 1) * (n + 2) / 2];
                    memcpy(dst, s.data(), s.size() * sizeof(float));
                }
            }
            ID3D11DeviceContext_Unmap(c, (ID3D11Resource *)staging, 0);
        }
    }
    if (ok) {
        D3D11_BUFFER_DESC bd = {HS_DOMAIN_VALUES * 4, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4};
        D3D11_SUBRESOURCE_DATA init = {domain.data(), 0, 0};
        D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
        v.Format = DXGI_FORMAT_UNKNOWN;
        v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        v.Buffer.NumElements = HS_DOMAIN_VALUES;
        ok = SUCCEEDED(ID3D11Device_CreateBuffer(d, &bd, &init, &gHs.domain)) &&
             SUCCEEDED(ID3D11Device_CreateShaderResourceView(d, (ID3D11Resource *)gHs.domain, &v, &gHs.domainSrv));
    }
    REL(vs);
    REL(hs);
    REL(ds);
    REL(so);
    REL(out);
    REL(staging);
    REL(cb);
    REL(ib);
    REL(stats);
    return ok;
}

static void StrandsFail(int why) {
    gMergeCur.why[why]++;
    gMergeCur.hsFails++;
    gStrandsBackoff = 30 << std::min(gStrandsFails, 7);
    gStrandsFails++;
}

static ID3D11Resource *ViewResource(ID3D11View *v) {
    if (!v) return NULL;
    ID3D11Resource *r;
    ID3D11View_GetResource(v, &r);
    ID3D11Resource_Release(r);  // identity only: the view holds it
    return r;
}

// The ForwardBase draw's output state, kept for the resolve; -1 or why the draw cannot be taken. Every taken
// ForwardBase draw of the pass must draw into the same target with the same state, and those of one eye with the same
// viewport (one visibility buffer, one view per eye).
static int CaptureFbStrands(Item &it, int itemIndex, EyeCap &e, int eye, int ps) {
    if (gFbDone) return WHY_FB_LATE;
    if (e.baseVertex != 0) return WHY_FB_STATE;
    if (itemIndex >= HS_MAX_ITEMS) return WHY_HS_SIZE;
    uint64_t segments = (uint64_t)(e.indexCount / 3) * TessCount(it.data.tessFactor.v[0]) * TessCount(it.data.tessFactor.v[1]);
    if (!segments || segments >= (1u << HS_ITEM_SHIFT)) return WHY_HS_SIZE;
    const bool newItem = it.eye[1 - eye].state != 1;
    const uint64_t points = (uint64_t)(e.indexCount / 3) * TessCount(it.data.tessFactor.v[0]) * (TessCount(it.data.tessFactor.v[1]) + 1);
    const uint64_t indexBytes = ((uint64_t)e.indexCount * (e.ibFormat == DXGI_FORMAT_R16_UINT ? 2 : 4) + 3) & ~3ull;
    // Raw-buffer addresses and segment<<2 must fit before taking any original draw.
    if (newItem && (HsPointBufferBytes(gHsAcceptedPoints + points) > 0xfffffff0ull ||
                    gHsAcceptedSegments + segments >= (1ull << 30) || gHsAcceptedIndexBytes + indexBytes > 0xfffffff0ull))
        return WHY_HS_SIZE;
    ID3D11DeviceContext_RSGetState(gCtx, &e.fbRs);
    ID3D11DeviceContext_OMGetBlendState(gCtx, &e.fbBlend, e.fbBlendFactor, &e.fbSampleMask);
    ID3D11DeviceContext_OMGetDepthStencilState(gCtx, &e.fbDss, &e.fbStencilRef);
    ID3D11DeviceContext_OMGetRenderTargets(gCtx, 1, &e.fbRtv, &e.fbDsv);
    if (!e.fbRs || !e.fbDss || !e.fbRtv || !e.fbDsv) return WHY_HS_TARGET;
    e.screen = IsProgram(ps, "6008f0bf73");
    if (e.screen) {
        ID3D11DeviceContext_PSGetShaderResources(gCtx, 0, 1, &e.screenSrv);
        ID3D11DeviceContext_PSGetSamplers(gCtx, 0, 1, &e.screenSmp);
        if (!e.screenSrv || !e.screenSmp) return WHY_FB_STATE;
        if (gHsFirstScreen && (gHsFirstScreen->screenSrv != e.screenSrv || gHsFirstScreen->screenSmp != e.screenSmp)) return WHY_FB_STATE;
    }
    if (!gHsFirst) {
        // a 2D single-sample target, depth test without stencil, solid fill without scissor
        D3D11_RENDER_TARGET_VIEW_DESC rd;
        ID3D11RenderTargetView_GetDesc(e.fbRtv, &rd);
        D3D11_TEXTURE2D_DESC td = {};
        ID3D11Resource *res = ViewResource((ID3D11View *)e.fbRtv);
        D3D11_RESOURCE_DIMENSION dim;
        ID3D11Resource_GetType(res, &dim);
        if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D || rd.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || rd.Texture2D.MipSlice != 0)
            return WHY_HS_TARGET;
        ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &td);
        if (td.SampleDesc.Count != 1 || td.Width > 16384 || td.Height > 16384) return WHY_HS_TARGET;
        D3D11_DEPTH_STENCIL_DESC dd;
        ID3D11DepthStencilState_GetDesc(e.fbDss, &dd);
        D3D11_RASTERIZER_DESC rsd;
        ID3D11RasterizerState_GetDesc(e.fbRs, &rsd);
        if (!dd.DepthEnable || dd.StencilEnable || rsd.FillMode != D3D11_FILL_SOLID || rsd.ScissorEnable) return WHY_HS_TARGET;
    } else if (e.fbRtv != gHsFirst->fbRtv || e.fbDsv != gHsFirst->fbDsv || e.fbRs != gHsFirst->fbRs || e.fbDss != gHsFirst->fbDss ||
               e.fbBlend != gHsFirst->fbBlend || e.fbStencilRef != gHsFirst->fbStencilRef || e.fbSampleMask != gHsFirst->fbSampleMask) {
        return WHY_HS_TARGET;
    }
    EyeCap *v = gHsFirstEye[eye];
    if (v && memcmp(&v->vp, &e.vp, sizeof e.vp)) return WHY_HS_TARGET;
    if (!gHsFirst) gHsFirst = &e;
    if (!v) gHsFirstEye[eye] = &e;
    if (e.screen && !gHsFirstScreen) gHsFirstScreen = &e;
    if (newItem) {
        gHsAcceptedPoints += points;
        gHsAcceptedSegments += segments;
        gHsAcceptedIndexBytes += indexBytes;
    }
    return -1;
}

static void HsDispatchChunks(ID3D11ComputeShader *cs, uint32_t item, uint32_t threads) {
    ID3D11DeviceContext *c = gCtx;
    ID3D11DeviceContext_CSSetShader(c, cs, NULL, 0);
    for (uint32_t first = 0; first < threads; first += 65535u * 64u) {
        HsDispatch d = {};
        d.dispatch[0] = item, d.dispatch[2] = first, d.dispatch[3] = std::min(threads - first, 65535u * 64u);
        HsUpload(gHs.dispatchCb, &d, sizeof d);
        ID3D11DeviceContext_Dispatch(c, (d.dispatch[3] + 63) / 64, 1, 1);
    }
}

static void HsClearCs() {
    ID3D11ShaderResourceView *srv[SAVED_CS_SRVS] = {};
    ID3D11UnorderedAccessView *uav[SAVED_CS_UAVS] = {};
    ID3D11DeviceContext_CSSetShaderResources(gCtx, 0, SAVED_CS_SRVS, srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(gCtx, 0, SAVED_CS_UAVS, uav, NULL);
}

// The full-target draw of a resolve: the shaders, the target, the views' resources (t5..t10).
static void HsResolveDraw(ID3D11PixelShader *ps, ID3D11ShaderResourceView **srv, UINT srvCount, ID3D11SamplerState **smp, ID3D11BlendState *blend,
                          const float *factor, UINT sampleMask, ID3D11DepthStencilState *dss, UINT ref) {
    ID3D11DeviceContext *c = gCtx;
    HsDispatch d = {};
    d.resolve[0] = gHsFirstEye[1] ? (uint32_t)gHsFirstEye[1]->vp.TopLeftX : gHs.visW;
    d.resolve[2] = gTransmission ? 1u : 0u;
    HsUpload(gHs.dispatchCb, &d, sizeof d);
    ID3D11DeviceContext_IASetInputLayout(c, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(c, gHs.resolveVs, NULL, 0);
    ID3D11DeviceContext_HSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_DSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_GSSetShader(c, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 1, &gHs.dispatchCb);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, srvCount, srv);
    ID3D11DeviceContext_PSSetSamplers(c, 0, ROLE_COUNT, smp);
    ID3D11DeviceContext_RSSetState(c, gHs.rs);
    D3D11_VIEWPORT vp = {0, 0, (float)gHs.visW, (float)gHs.visH, 0, 1};
    ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
    ID3D11DeviceContext_OMSetBlendState(c, blend, factor, sampleMask);
    ID3D11DeviceContext_OMSetDepthStencilState(c, dss, ref);
    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &gHsFirst->fbRtv, gHsFirst->fbDsv);
    gEmitting = true;
    oDraw(c, 3, 0);
    ID3D11DeviceContext_PSSetShader(c, ps == gHs.resolveFa ? gHs.clipResolveFa : gHs.clipResolveFb, NULL, 0);
    ID3D11DeviceContext_DrawInstancedIndirect(c, gHs.args, 32);
    gEmitting = false;
}

// The taken items' ForwardBase draws: points, visibility buffer, ForwardBase lighting.
static void StrandsFlushFb() {
    if (!gFbOpen) return;
    gFbOpen = false;
    gFbDone = true;
    ID3D11DeviceContext *c = gCtx;
    HsFrameCb fc = {};
    uint32_t points = 0, indexBytes = 0;
    gHsCount = 0;
    gHsSegments = 0;
    for (int i = 0; i < gItemCount; i++) {
        Item &it = gItems[i];
        gHsSlot[i] = -1;
        uint32_t eyes = (it.eye[0].state == 1 ? 1u : 0u) | (it.eye[1].state == 1 ? 2u : 0u);
        if (!eyes) continue;
        EyeCap &e = it.eye[eyes & 1 ? 0 : 1];
        HsItem &h = gHsItems[gHsCount];
        memcpy(&h, &it.data, 45 * 16);
        uint32_t L = (uint32_t)TessCount(it.data.tessFactor.v[0]), S = (uint32_t)TessCount(it.data.tessFactor.v[1]), patches = e.indexCount / 3;
        UINT isz = e.ibFormat == DXGI_FORMAT_R16_UINT ? 2 : 4;
        h.counts[0] = patches, h.counts[1] = L, h.counts[2] = S, h.counts[3] = points;
        h.flags[0] = 0;
        h.flags[1] = (e.screen ? 1u : 0u) | eyes << 8;
        h.flags[2] = indexBytes / isz;
        h.flags[3] = isz == 2 ? 1u : 0u;
        fc.segStart[gHsCount] = gHsSegments;
        gHsSegments += patches * L * S;
        points += patches * L * (S + 1);
        indexBytes += (e.indexCount * isz + 3) & ~3u;
        gHsSlot[i] = gHsCount++;
    }
    if (!gHsCount) return;
    fc.frame[0] = (uint32_t)gHsCount, fc.frame[1] = gHsSegments;
    fc.segStart[gHsCount] = gHsSegments;
    ID3D11Resource *target = ViewResource((ID3D11View *)gHsFirst->fbRtv);
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)target, &td);
    if (!HsEnsureRaw(&gHs.points, &gHs.pointsSrv, &gHs.pointsUav, &gHs.pointsBytes, (UINT)HsPointBufferBytes(points)) ||
        !HsEnsureRaw(&gHs.live, &gHs.liveSrv, &gHs.liveUav, &gHs.liveBytes, gHsSegments * 4) ||
        !HsEnsureRaw(&gHs.clipped, &gHs.clippedSrv, &gHs.clippedUav, &gHs.clippedBytes, gHsSegments * 4) ||
        !HsEnsureRaw(&gHs.indices, &gHs.indicesSrv, NULL, &gHs.indicesBytes, indexBytes) || !HsEnsureVis(td.Width, td.Height)) {
        // the hair of this pass is lost (resources only fail when the GPU's memory is gone)
        StrandsFail(WHY_HS_RESOURCES);
        return;
    }
    // one view per eye, from its first taken draw
    HsView views[2] = {};
    for (unsigned eye = 0; eye < VamViewCount(); eye++) {
        EyeCap *e = gHsFirstEye[eye];
        if (!e) continue;
        HsView &v = views[eye];
        v.viewport[0] = e->vp.TopLeftX, v.viewport[1] = e->vp.TopLeftY, v.viewport[2] = e->vp.Width, v.viewport[3] = e->vp.Height;
        v.depth[0] = e->vp.MinDepth, v.depth[1] = e->vp.MaxDepth;
        v.clip[0] = std::max(0, (int)e->vp.TopLeftX), v.clip[1] = std::max(0, (int)e->vp.TopLeftY);
        v.clip[2] = std::min((int)td.Width, (int)(e->vp.TopLeftX + e->vp.Width)), v.clip[3] = std::min((int)td.Height, (int)(e->vp.TopLeftY + e->vp.Height));
        D3D11_RASTERIZER_DESC rsd;
        ID3D11RasterizerState_GetDesc(e->fbRs, &rsd);
        D3D11_DEPTH_STENCIL_DESC dd;
        ID3D11DepthStencilState_GetDesc(e->fbDss, &dd);
        v.mode[0] = (uint32_t)rsd.CullMode, v.mode[1] = rsd.FrontCounterClockwise ? 1u : 0u, v.mode[2] = (uint32_t)dd.DepthFunc, v.mode[3] = (uint32_t)eye;
    }
    if (!HsUpload(gHs.items, gHsItems, gHsCount * sizeof(HsItem)) || !HsUpload(gHs.views, views, sizeof views) || !HsUpload(gHs.frameCb, &fc, sizeof fc)) {
        StrandsFail(WHY_HS_RESOURCES);
        return;
    }
    // each item's indices, where the build reads them (an index buffer has no shader view)
    for (int i = 0; i < gItemCount; i++) {
        if (gHsSlot[i] < 0) continue;
        Item &it = gItems[i];
        EyeCap &e = it.eye[it.eye[0].state == 1 ? 0 : 1];
        UINT isz = e.ibFormat == DXGI_FORMAT_R16_UINT ? 2 : 4;
        D3D11_BOX box = {e.ibOffset + e.startIndex * isz, 0, 0, e.ibOffset + (e.startIndex + e.indexCount) * isz, 1, 1};
        ID3D11DeviceContext_CopySubresourceRegion(c, (ID3D11Resource *)gHs.indices, 0, gHsItems[gHsSlot[i]].flags[2] * isz, 0, 0, (ID3D11Resource *)e.ib, 0,
                                                  &box);
    }
    gMergeCur.hsItems += gHsCount;
    gMergeCur.hsPoints += points;
    gMergeCur.hsSegments += gHsSegments;
    gMergeCur.hsFbResolves++;

    SavedState saved;
    SaveState(saved);
    ID3D11Buffer *cbs[2] = {gHs.dispatchCb, gHs.frameCb};
    ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 2, cbs);
    // points: per item (its particles, barycentrics, noise and colour textures)
    HsClearCs();
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &gHs.pointsUav, NULL);
    for (int i = 0; i < gItemCount; i++) {
        if (gHsSlot[i] < 0) continue;
        Item &it = gItems[i];
        EyeCap &e = it.eye[it.eye[0].state == 1 ? 0 : 1];
        ID3D11ShaderResourceView *srv[7] = {e.dsSrv[0], e.dsSrv[1], e.dsSrv[2], e.dsSrv[3], gHs.indicesSrv, gHs.domainSrv, gHs.itemsSrv};
        ID3D11DeviceContext_CSSetShaderResources(c, 0, 7, srv);
        ID3D11DeviceContext_CSSetSamplers(c, 0, 2, e.dsSmp);
        const HsItem &h = gHsItems[gHsSlot[i]];
        HsDispatchChunks(gHs.build, (uint32_t)gHsSlot[i], h.counts[0] * h.counts[1] * (h.counts[2] + 1));
    }
    HsClearCs();
    // depth pass: every segment, both eyes; it lists the segments still nearest somewhere
    uint32_t func = views[gHsFirstEye[0] ? 0 : 1].mode[2];
    uint32_t far_ = func == D3D11_COMPARISON_GREATER || func == D3D11_COMPARISON_GREATER_EQUAL ? 0u : 0xffffffffu;
    UINT farDepth[4] = {far_, far_, far_, far_}, zero[4] = {};
    ID3D11DeviceContext_ClearUnorderedAccessViewUint(c, gHs.visDepthUav, farDepth);
    ID3D11DeviceContext_ClearUnorderedAccessViewUint(c, gHs.visIdUav, zero);
    ID3D11DeviceContext_CopyResource(c, (ID3D11Resource *)gHs.args, (ID3D11Resource *)gHs.argsInit);
    ID3D11ShaderResourceView *common[4] = {gHs.domainSrv, gHs.itemsSrv, gHs.pointsSrv, gHs.viewsSrv};
    ID3D11DeviceContext_CSSetShaderResources(c, 5, 4, common);
    ID3D11UnorderedAccessView *depthUavs[5] = {gHs.visDepthUav, NULL, gHs.liveUav, gHs.argsUav, gHs.clippedUav};
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 5, depthUavs, NULL);
    HsDispatchChunks(gHs.rasterDepth, 0, gHsSegments);
    ID3D11UnorderedAccessView *none[5] = {};
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 5, none, NULL);
    // Complete the group list (clipped at the camera or too large for one thread, a thread group each) before either ID
    // pass reads the final depth; it keeps in each entry the eyes that were nearest somewhere.
    ID3D11UnorderedAccessView *groupUavs[5] = {gHs.visDepthUav, NULL, NULL, NULL, gHs.clippedUav};
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 5, groupUavs, NULL);
    ID3D11ShaderResourceView *groupArgs[2] = {NULL, gHs.argsSrv};
    ID3D11DeviceContext_CSSetShaderResources(c, 14, 2, groupArgs);
    ID3D11DeviceContext_CSSetShader(c, gHs.clipDepth, NULL, 0);
    ID3D11DeviceContext_DispatchIndirect(c, gHs.args, 16);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 5, none, NULL);
    // ID passes: bounded 2D dispatches over the normal and group lists.
    ID3D11ShaderResourceView *clipList[2] = {gHs.clippedSrv, gHs.argsSrv};
    ID3D11UnorderedAccessView *idUavs[2] = {gHs.visDepthUav, gHs.visIdUav};
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 2, idUavs, NULL);
    ID3D11ShaderResourceView *list[2] = {gHs.liveSrv, gHs.argsSrv};
    ID3D11DeviceContext_CSSetShaderResources(c, 14, 2, list);
    ID3D11DeviceContext_CSSetShader(c, gHs.rasterId, NULL, 0);
    ID3D11DeviceContext_DispatchIndirect(c, gHs.args, 0);
    ID3D11DeviceContext_CSSetShaderResources(c, 14, 2, clipList);
    ID3D11DeviceContext_CSSetShader(c, gHs.clipId, NULL, 0);
    ID3D11DeviceContext_DispatchIndirect(c, gHs.args, 16);
    HsClearCs();
    // ForwardBase lighting, with the ForwardBase draws' blend and depth state
    ID3D11ShaderResourceView *srv[13] = {};
    srv[5] = gHs.domainSrv, srv[6] = gHs.itemsSrv, srv[7] = gHs.pointsSrv, srv[8] = gHs.viewsSrv, srv[9] = gHs.visDepthSrv, srv[10] = gHs.visIdSrv;
    srv[12] = gHsFirstScreen ? gHsFirstScreen->screenSrv : NULL;
    ID3D11SamplerState *smp[ROLE_COUNT] = {};
    smp[ROLE_MAIN_SCREEN] = gHsFirstScreen ? gHsFirstScreen->screenSmp : NULL;
    HsResolveDraw(gHs.resolveFb, srv, 13, smp, gHsFirst->fbBlend, gHsFirst->fbBlendFactor, gHsFirst->fbSampleMask, gHsFirst->fbDss, gHsFirst->fbStencilRef);
    RestoreState(saved, true);
}

static bool SameHsLight(const LightCap &a, const HsLight &b, ID3D11ShaderResourceView *const *tex) {
    return !memcmp(&a.data, &b, 13 * 16) && a.data.kind[0] == b.kind[0] && a.data.kind[1] == b.kind[1] && a.cookie == tex[0] && a.atten == tex[1] &&
           a.shadow == tex[2] && a.cube == tex[3];
}

// The lights taken since the last ForwardAdd resolve, given back (the next ForwardAdd draws start again).
static void ReleaseLights(EyeCap &e) {
    for (int k = 0; k < e.lightCount; k++) {
        REL(e.lights[k].cookie);
        REL(e.lights[k].atten);
        REL(e.lights[k].shadow);
        REL(e.lights[k].cube);
    }
    e.lightCount = 0;
    REL(e.rs);
    REL(e.blend);
    REL(e.dss);
    REL(e.rtv);
    REL(e.dsv);
    REL(e.rotation);
    for (auto &r : e.role) REL(r);
}

// The ForwardAdd lights taken since the last resolve, split into compatible full-target batches.
static void StrandsFlushFa() {
    if (!gFaOpen) return;
    gFaOpen = false;
    HsLight lights[HS_MAX_LIGHTS] = {};
    ID3D11ShaderResourceView *tex[HS_MAX_LIGHTS][4] = {};
    uint32_t lightEyes[HS_MAX_LIGHTS] = {};
    int count = 0;
    EyeCap *first = NULL;
    ID3D11SamplerState *roles[ROLE_COUNT] = {};
    ID3D11ShaderResourceView *rotation = NULL;
    auto clearMasks = [&]() {
        for (int i = 0; i < gHsCount; i++) {
            gHsItems[i].flags[0] = 0;
            gHsItems[i].flags[1] &= 0xffffu;
        }
    };
    clearMasks();
    // Resolve a complete compatible batch before starting another. Captured lights remain
    // referenced until every batch is drawn; a table/sampler limit never discards them.
    auto flush = [&]() {
        if (!count) return true;
        for (int i = 0; i < gItemCount; i++)
            if (gHsSlot[i] >= 0) memcpy(&gHsItems[gHsSlot[i]], &gItems[i].data, 45 * 16);
        if (!HsUpload(gHs.items, gHsItems, gHsCount * sizeof(HsItem)) || !HsUpload(gHs.lights, lights, sizeof lights)) return false;
        SavedState saved;
        SaveState(saved);
        ID3D11ShaderResourceView *srv[SAVED_PS_SRVS] = {};
        srv[5] = gHs.domainSrv, srv[6] = gHs.itemsSrv, srv[7] = gHs.pointsSrv, srv[8] = gHs.viewsSrv, srv[9] = gHs.visDepthSrv, srv[10] = gHs.visIdSrv;
        srv[11] = rotation, srv[12] = gHsFirstScreen ? gHsFirstScreen->screenSrv : NULL, srv[13] = gHs.lightsSrv;
        for (int t = 0; t < count; t++) {
            srv[16 + t] = tex[t][0];
            srv[32 + t] = tex[t][1];
            srv[48 + t] = tex[t][2];
            srv[64 + t] = tex[t][3];
        }
        HsResolveDraw(gHs.resolveFa, srv, SAVED_PS_SRVS, roles, first->blend, first->blendFactor, first->sampleMask, first->dss, first->stencilRef);
        RestoreState(saved, true);
        gMergeCur.hsFaResolves++;
        gMergeCur.hsLights = std::max(gMergeCur.hsLights, count);
        count = 0;
        first = NULL;
        rotation = NULL;
        memset(lights, 0, sizeof lights);
        memset(tex, 0, sizeof tex);
        memset(lightEyes, 0, sizeof lightEyes);
        memset(roles, 0, sizeof roles);
        clearMasks();
        return true;
    };
    bool ok = true;
    for (int i = 0; i < gItemCount && ok; i++) {
        Item &it = gItems[i];
        for (unsigned eye = 0; eye < VamViewCount() && ok; eye++) {
            EyeCap &e = it.eye[eye];
            if (e.state != 1 || !e.lightCount) continue;
            if (gHsSlot[i] < 0) { ok = false; break; }
            bool same = !first || (e.blend == first->blend && e.dss == first->dss && e.stencilRef == first->stencilRef &&
                                  e.sampleMask == first->sampleMask && !memcmp(e.blendFactor, first->blendFactor, sizeof e.blendFactor));
            for (int r = 0; r < ROLE_COUNT; r++)
                if (e.role[r] && roles[r] && e.role[r] != roles[r]) same = false;
            if (e.rotation && rotation && e.rotation != rotation) same = false;
            if (!same && !flush()) { ok = false; break; }
            for (int k = 0; k < e.lightCount; k++) {
                const LightCap &L = e.lights[k];
                int t = 0;
                while (t < count) {
                    bool scissor = !(lightEyes[t] & (1u << eye)) ||
                                   (lights[t].scissor[eye * 2] == L.data.kind[2] && lights[t].scissor[eye * 2 + 1] == L.data.kind[3]);
                    if (scissor && SameHsLight(L, lights[t], tex[t])) break;
                    t++;
                }
                if (t == HS_MAX_LIGHTS) {
                    if (!flush()) { ok = false; break; }
                    t = 0;
                }
                if (!first) first = &e;
                for (int r = 0; r < ROLE_COUNT; r++) if (!roles[r]) roles[r] = e.role[r];
                if (!rotation) rotation = e.rotation;
                if (t == count) {
                    memcpy(&lights[t], &L.data, 13 * 16);
                    lights[t].kind[0] = L.data.kind[0], lights[t].kind[1] = L.data.kind[1];
                    tex[t][0] = L.cookie, tex[t][1] = L.atten, tex[t][2] = L.shadow, tex[t][3] = L.cube;
                    count++;
                }
                lights[t].scissor[eye * 2] = L.data.kind[2];
                lights[t].scissor[eye * 2 + 1] = L.data.kind[3];
                lightEyes[t] |= 1u << eye;
                // A split may occur between the eyes: each eye needs its own mask.
                if (eye) gHsItems[gHsSlot[i]].flags[1] |= 1u << (16 + t);
                else gHsItems[gHsSlot[i]].flags[0] |= 1u << t;
            }
        }
    }
    if (!ok || !flush()) StrandsFail(WHY_HS_RESOURCES);
    for (int i = 0; i < gItemCount; i++) {
        ReleaseLights(gItems[i].eye[0]);
        ReleaseLights(gItems[i].eye[1]);
    }
}

static void StrandsBegin() {
    gHsAcceptedPoints = gHsAcceptedSegments = gHsAcceptedIndexBytes = 0;
    gFbOpen = gFbDone = gFaOpen = false;
    gHsFirst = gHsFirstEye[0] = gHsFirstEye[1] = gHsFirstScreen = NULL;
    gHsCount = 0;
    for (int &s : gHsSlot) s = -1;
}

// Before a draw that is not a taken ForwardBase / ForwardAdd draw: what was taken goes in first.
static void StrandsFlush() {
    StrandsFlushFb();
    StrandsFlushFa();
}

// A draw in the eye camera's opaque pass: true when it was taken (not drawn now: a ForwardAdd draw whose light the
// item's lights draw has).
static bool MergeDraw(UINT n, UINT s, INT b, bool instanced) {
    int ps = ShaderProgram(HOP_PS);
    int pass = ps >= 0 ? kHopPrograms[ps].pass : -1;
    if(gPathLighting && (pass==HOP_PASS_FB || pass==HOP_PASS_FA))return true;
    if (pass != HOP_PASS_FB && pass != HOP_PASS_FA) {
        EndRun();
        if (gStrandsPass) StrandsFlush();
        return false;
    }
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11DeviceContext_DSGetShaderResources(gCtx, 2, 1, &srv);
    void *particles = NULL;
    if (srv) {
        ID3D11Resource *r;
        ID3D11ShaderResourceView_GetResource(srv, &r);
        particles = r;
        ID3D11Resource_Release(r);
        ID3D11ShaderResourceView_Release(srv);
    }
    ID3D11Buffer *ibuf = NULL;
    DXGI_FORMAT fmt;
    UINT off;
    ID3D11DeviceContext_IAGetIndexBuffer(gCtx, &ibuf, &fmt, &off);
    if (ibuf) ID3D11Buffer_Release(ibuf);
    D3D11_VIEWPORT vp = {};
    UINT nvp = 1;
    ID3D11DeviceContext_RSGetViewports(gCtx, &nvp, &vp);
    int eye = VamViewCount()==2 && vp.TopLeftX > 0 ? 1 : 0;
    Item *it = FindItem(particles, ibuf, s, n);

    if (pass == HOP_PASS_FB) {
        EndRun();
        // strands: a ForwardBase draw after lights were taken (not the usual order): those lights first
        if (gStrandsPass) StrandsFlushFa();
        int why = -1;
        if (instanced) why = WHY_FB_INSTANCED;
        else if (it && it->eye[eye].state != 0) why = WHY_FB_TWICE;
        else if (!it && gItemCount == (int)(sizeof gItems / sizeof gItems[0])) why = WHY_FB_STATE;
        if (why < 0) {
            if (!it) {
                it = &gItems[gItemCount++];
                memset(it, 0, sizeof *it);
                it->particles = particles;
                it->ib = ibuf;
                it->startIndex = s;
                it->indexCount = n;
            }
            why = CaptureFb(*it, it->eye[eye], ps, n, s, b, vp);
            if (why < 0 && gStrandsPass) why = CaptureFbStrands(*it, (int)(it - gItems), it->eye[eye], eye, ps);
            if (why >= 0) {
                ReleaseEye(it->eye[eye]);
                it->eye[eye].state = 3;
            }
        }
        if (why >= 0) {
            // drawn by Unity (opaque with depth: before or after the strands resolve, the same picture)
            gMergeCur.fbThrough++;
            gMergeCur.why[why]++;
            return false;
        }
        it->eye[eye].state = 1;
        gMergeCur.fbMerged++;
        if (!gStrandsPass) return false;
        gFbOpen = true;
        return true;
    }

    // ForwardAdd
    if (gStrandsPass) StrandsFlushFb();
    if (!it || it->eye[eye].state != 1 || instanced) {
        if (gRun != it) EndRun();
        gMergeCur.faThrough++;
        if (!it || it->eye[eye].state == 0) gMergeCur.why[WHY_FA_NO_ITEM]++;
        // strands: a light of a taken item that goes through meets the resolve's depth
        if (gStrandsPass && it && it->eye[eye].state == 1) StrandsFail(WHY_FA_STATE);
        return false;
    }
    if (gStrandsPass) {
        int why = CaptureLight(*it, it->eye[eye], ps, gHsFirst->fbRtv, gHsFirst->fbDsv);
        if (why == WHY_FA_FULL || why == WHY_FA_SAMPLER || why == WHY_FA_STATE) {
            // Finish captured draws before retrying this draw with a fresh per-item capture.
            StrandsFlushFa();
            why = CaptureLight(*it, it->eye[eye], ps, gHsFirst->fbRtv, gHsFirst->fbDsv);
        }
        if (why >= 0) {
            gMergeCur.faThrough++;
            StrandsFail(why);
            return false;
        }
        gFaOpen = true;
        gMergeCur.faMerged++;
        return true;
    }
    if (gRun != it) {
        EndRun();
        gRun = it;
    }
    int why = CaptureLight(*it, it->eye[eye], ps);
    if (why >= 0) {
        // the lights so far drawn now; this light and the rest of the item's ForwardAdd draws go through
        Emit(*it, eye);
        gMergeCur.flushes++;
        gMergeCur.why[why]++;
        gMergeCur.faThrough++;
        return false;
    }
    gMergeCur.faMerged++;
    return true;
}

static void MergeBegin() {
    gRun = NULL;
    gItemCount = 0;
    if (gStrandsPass) StrandsBegin();
}

static void MergeEnd() {
    EndRun();
    if (gStrandsPass) {
        StrandsFlush();
        gMergeCur.hsPass = 1;
    }
    for (int i = 0; i < gItemCount; i++) {
        ReleaseEye(gItems[i].eye[0]);
        ReleaseEye(gItems[i].eye[1]);
    }
    gItemCount = 0;
    gRun = NULL;
}

static double gQpcToUs;
static double NowUs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * gQpcToUs;
}

static bool Merging() { return gWindowMode == MODE_MERGE && gInWindow && gMergeReady && !gEmitting; }

static void STDMETHODCALLTYPE hDrawIndexed(ID3D11DeviceContext *c, UINT n, UINT s, INT b) {
    if (c == gCtx) {
        if (Observing()) ObserveDraw(n, s, b, 0xFFFFFFFF);
        if (Merging()) {
            size_t capTaken = gCapturing ? CaptureDraw(n, s, b) : 0;
            if (gCapturing && !capTaken) CaptureOther(n, 0);
            double t0 = NowUs();
            bool taken = MergeDraw(n, s, b, false);
            gMergeCur.cpuUs += NowUs() - t0;
            if (capTaken) gCap[capTaken] = taken ? 1 : 0;
            if (taken) return;
        }
    }
    oDrawIndexed(c, n, s, b);
}

static void STDMETHODCALLTYPE hDrawIndexedInstanced(ID3D11DeviceContext *c, UINT n, UINT inst, UINT s, INT b, UINT si) {
    if (c == gCtx) {
        if (Observing()) ObserveDraw(n, s, b, inst);
        if (Merging()) {
            if (gCapturing) CaptureOther(n, inst);
            double t0 = NowUs();
            bool taken = MergeDraw(n, s, b, true);
            gMergeCur.cpuUs += NowUs() - t0;
            if (taken) return;
        }
    }
    oDrawIndexedInstanced(c, n, inst, s, b, si);
}

// Draws that are never hair: the lights of the item whose ForwardAdd draws came just before go in first.
static void OtherDraw(ID3D11DeviceContext *c) {
    if (c != gCtx || !Merging()) return;
    if (gRun) EndRun();
    if (gStrandsPass) StrandsFlush();
}
static void STDMETHODCALLTYPE hDraw(ID3D11DeviceContext *c, UINT n, UINT s) {
    OtherDraw(c);
    oDraw(c, n, s);
}
static void STDMETHODCALLTYPE hDrawInstanced(ID3D11DeviceContext *c, UINT n, UINT inst, UINT s, UINT si) {
    OtherDraw(c);
    oDrawInstanced(c, n, inst, s, si);
}
static void STDMETHODCALLTYPE hDrawIndexedInstancedIndirect(ID3D11DeviceContext *c, ID3D11Buffer *b, UINT off) {
    OtherDraw(c);
    oDrawIndexedInstancedIndirect(c, b, off);
}
static void STDMETHODCALLTYPE hDrawInstancedIndirect(ID3D11DeviceContext *c, ID3D11Buffer *b, UINT off) {
    OtherDraw(c);
    oDrawInstancedIndirect(c, b, off);
}

static bool IsConstantBuffer(ID3D11Resource *r, UINT *bytes) {
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(r, &dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    D3D11_BUFFER_DESC d;
    ID3D11Buffer_GetDesc((ID3D11Buffer *)r, &d);
    *bytes = d.ByteWidth;
    return (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
}

static HRESULT STDMETHODCALLTYPE hMap(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *m) {
    if (c == gCtx && gMode == MODE_OBSERVE) {
        UINT bytes;
        if (IsConstantBuffer(r, &bytes)) {
            uint8_t method = type == D3D11_MAP_WRITE_DISCARD ? 1 : type == D3D11_MAP_WRITE_NO_OVERWRITE ? 2 : 3;
            gCbUpdates[(ID3D11Buffer *)r] = CbUpdate{method, gFrame, bytes};
            if (gInWindow) gCur.maps[method]++;
            if (gInWindow && gFbPending)
                for (int s = 0; s < 5; s++)
                    for (int k = 0; k < 14; k++)
                        if (gFbBound[s][k] == (ID3D11Buffer *)r) {
                            gCur.fbChanged[s][k]++;
                            gCur.fbChangedBytes[s][k] = (int)bytes;
                        }
        }
    }
    if (c == gCtx && gMode >= MODE_MERGE && gShadowCount) {
        Shadow *s = FindShadow(r);
        if (s && type == D3D11_MAP_WRITE_DISCARD && sub == 0 && m) {
            HRESULT hr = oMap(c, r, sub, type, flags, m);
            if (SUCCEEDED(hr)) {
                // Unity writes the CPU copy; Unmap copies it into the buffer
                s->mapped = m->pData;
                m->pData = s->data;
            }
            return hr;
        }
        if (s) s->valid = false;  // written some other way: read back again when needed
    }
    return oMap(c, r, sub, type, flags, m);
}

static void STDMETHODCALLTYPE hUnmap(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub) {
    if (c == gCtx && gShadowCount) {
        Shadow *s = FindShadow(r);
        if (s && s->mapped) {
            memcpy(s->mapped, s->data, s->size);
            s->mapped = NULL;
            s->valid = true;
        }
    }
    oUnmap(c, r, sub);
}

static void STDMETHODCALLTYPE hUpdateSubresource(ID3D11DeviceContext *c, ID3D11Resource *r, UINT sub, const D3D11_BOX *box, const void *data,
                                                 UINT pitch, UINT depthPitch) {
    if (c == gCtx && gMode == MODE_OBSERVE) {
        UINT bytes;
        if (IsConstantBuffer(r, &bytes)) {
            gCbUpdates[(ID3D11Buffer *)r] = CbUpdate{4, gFrame, bytes};
            if (gInWindow) gCur.cbUpdates[4]++;
            if (gInWindow && gFbPending)
                for (int s = 0; s < 5; s++)
                    for (int k = 0; k < 14; k++)
                        if (gFbBound[s][k] == (ID3D11Buffer *)r) {
                            gCur.fbChanged[s][k]++;
                            gCur.fbChangedBytes[s][k] = (int)bytes;
                        }
        }
    }
    if (c == gCtx && gShadowCount) {
        Shadow *s = FindShadow(r);
        if (s && sub == 0 && !box && data) {
            memcpy(s->data, data, s->size);  // a constant buffer is written whole
            s->valid = true;
        } else if (s) {
            s->valid = false;  // read back again when needed
        }
    }
    oUpdateSubresource(c, r, sub, box, data, pitch, depthPitch);
}

static bool HookContext() {
    void **live = (void **)gCtx->lpVtbl;
    gTemplate = FindTemplate(live);
    if (!gTemplate) return false;
    bool ok = HookSlot(live, CSLOT(DrawIndexed), (void *)hDrawIndexed, (void **)&oDrawIndexed) &&
              HookSlot(live, CSLOT(DrawIndexedInstanced), (void *)hDrawIndexedInstanced, (void **)&oDrawIndexedInstanced) &&
              HookSlot(live, CSLOT(Draw), (void *)hDraw, (void **)&oDraw) &&
              HookSlot(live, CSLOT(DrawInstanced), (void *)hDrawInstanced, (void **)&oDrawInstanced) &&
              HookSlot(live, CSLOT(DrawIndexedInstancedIndirect), (void *)hDrawIndexedInstancedIndirect, (void **)&oDrawIndexedInstancedIndirect) &&
              HookSlot(live, CSLOT(DrawInstancedIndirect), (void *)hDrawInstancedIndirect, (void **)&oDrawInstancedIndirect) &&
              HookSlot(live, CSLOT(Map), (void *)hMap, (void **)&oMap) && HookSlot(live, CSLOT(Unmap), (void *)hUnmap, (void **)&oUnmap) &&
              HookSlot(live, CSLOT(UpdateSubresource), (void *)hUpdateSubresource, (void **)&oUpdateSubresource);
    if (!ok) SetError("cannot patch the context table");
    return ok;
}

// ---- render events -----------------------------------------------------------------------------------------------------

// GPU time of the eye camera's opaque pass (all modes): timestamps at its start and end, read a few frames later.
struct GpuTiming {
    ID3D11Query *disjoint, *begin, *end;
    bool pending, open;
};
static GpuTiming gTiming[4];
static int gTimingNext;
static int gGpuSamples;
static double gGpuSumMs, gGpuMinMs, gGpuMaxMs;

static void TimingCollect() {
    for (auto &t : gTiming) {
        if (!t.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 a, b;
        if (ID3D11DeviceContext_GetData(gCtx, (ID3D11Asynchronous *)t.disjoint, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ID3D11DeviceContext_GetData(gCtx, (ID3D11Asynchronous *)t.begin, &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ID3D11DeviceContext_GetData(gCtx, (ID3D11Asynchronous *)t.end, &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        t.pending = false;
        if (dj.Disjoint || b < a) continue;
        double ms = (double)(b - a) * 1000.0 / (double)dj.Frequency;
        AcquireSRWLockExclusive(&gLock);
        if (!gGpuSamples || ms < gGpuMinMs) gGpuMinMs = ms;
        if (!gGpuSamples || ms > gGpuMaxMs) gGpuMaxMs = ms;
        gGpuSumMs += ms;
        gGpuSamples++;
        ReleaseSRWLockExclusive(&gLock);
    }
}

static void TimingBegin() {
    if (!gTiming[0].disjoint) {
        D3D11_QUERY_DESC dd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, td = {D3D11_QUERY_TIMESTAMP, 0};
        for (auto &t : gTiming)
            if (FAILED(ID3D11Device_CreateQuery(gDevice, &dd, &t.disjoint)) || FAILED(ID3D11Device_CreateQuery(gDevice, &td, &t.begin)) ||
                FAILED(ID3D11Device_CreateQuery(gDevice, &td, &t.end)))
                return;
    }
    TimingCollect();
    GpuTiming &t = gTiming[gTimingNext];
    if (t.pending || t.open) return;
    ID3D11DeviceContext_Begin(gCtx, (ID3D11Asynchronous *)t.disjoint);
    ID3D11DeviceContext_End(gCtx, (ID3D11Asynchronous *)t.begin);
    t.open = true;
}

static void TimingEnd() {
    GpuTiming &t = gTiming[gTimingNext];
    if (!t.open) return;
    ID3D11DeviceContext_End(gCtx, (ID3D11Asynchronous *)t.end);
    ID3D11DeviceContext_End(gCtx, (ID3D11Asynchronous *)t.disjoint);
    t.open = false;
    t.pending = true;
    gTimingNext = (gTimingNext + 1) % 4;
}

// Test snapshot: the eye camera's target at the end of the opaque pass, written to a file (header + rows).
static char gSnapshotPath[MAX_PATH];
static volatile LONG gSnapshotRequest, gSnapshotsTaken;

static void TakeSnapshot() {
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(gCtx, 1, &rtv, NULL);
    if (!rtv) return;
    ID3D11Resource *res;
    ID3D11RenderTargetView_GetResource(rtv, &res);
    ID3D11RenderTargetView_Release(rtv);
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &d);
    if (d.SampleDesc.Count == 1 && d.MipLevels >= 1) {
        D3D11_TEXTURE2D_DESC sd = d;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        ID3D11Texture2D *st = NULL;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(gDevice, &sd, NULL, &st))) {
            ID3D11DeviceContext_CopySubresourceRegion(gCtx, (ID3D11Resource *)st, 0, 0, 0, 0, res, 0, NULL);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ID3D11DeviceContext_Map(gCtx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
                FILE *f = NULL;
                if (fopen_s(&f, gSnapshotPath, "wb") == 0 && f) {
                    uint32_t header[4] = {0x50414e53u /* SNAP */, d.Width, d.Height, (uint32_t)d.Format};
                    fwrite(header, sizeof header, 1, f);
                    uint32_t row = d.Width * 4;  // 32-bit formats only
                    for (UINT y = 0; y < d.Height; y++) fwrite((uint8_t *)m.pData + (size_t)y * m.RowPitch, row, 1, f);
                    fclose(f);
                    InterlockedIncrement(&gSnapshotsTaken);
                }
                ID3D11DeviceContext_Unmap(gCtx, (ID3D11Resource *)st, 0);
            }
            ID3D11Texture2D_Release(st);
        }
    }
    ID3D11Resource_Release(res);
    InterlockedExchange(&gSnapshotRequest, 0);
}

static void FinishFrame() {
    AcquireSRWLockExclusive(&gLock);
    if (gCur.windows > 0) {
        gCur.frame = gFrame;
        gLast = gCur;
        gFramesObserved++;
    }
    gMergeLast = gMergeCur;
    ReleaseSRWLockExclusive(&gLock);
    memset(&gMergeCur, 0, sizeof gMergeCur);
    gCur = FrameObs();
    gFrame++;
    if (gCbUpdates.size() > 20000) gCbUpdates.clear();
    if (gIds.size() > 20000) gIds.clear();
}

static void STDMETHODCALLTYPE OnRenderEvent(int id) {
    int op = (id >> 24) & 0xff, arg = id & 0xffffff;
    switch (op) {
    case OP_INSTALL:
        if (!gInstalled && gCtx) gInstalled = HookContext();
        if (gInstalled && gStrandsObjects && !gStrandsReady) gStrandsReady = ReadDomain();
        break;
    case OP_BEGIN: {
        // strands passes take their items as merging does (the merge window), and draw them with strands
        int mode = (int)gMode;
        gStrandsPass = mode == MODE_STRANDS && gStrandsReady && gStrandsBackoff == 0;
        if (mode == MODE_STRANDS) mode = MODE_MERGE;
        if (mode == MODE_MERGE && gWindowMode != MODE_MERGE)
            for (int i = 0; i < gShadowCount; i++) gShadows[i].valid = false;  // written without copies meanwhile
        gWindowMode = mode;
        if (!gInstalled) break;
        gInWindow = true;
        gWindowCamera = arg;
        gOthersSinceHair = 0;
        gCur.windows++;
        TimingBegin();
        if (mode == MODE_MERGE) MergeBegin();
        if (mode == MODE_MERGE && gCaptureRequest && !gCapturing) CaptureBegin();
        break;
    }
    case OP_END:
        if (!gInWindow) break;
        if (gWindowMode == MODE_MERGE) {
            double t0 = NowUs();
            MergeEnd();
            gMergeCur.cpuUs += NowUs() - t0;
            if (gCapturing) CaptureEnd();
        }
        gInWindow = false;
        gFbPending = false;
        TimingEnd();
        if (gSnapshotRequest) TakeSnapshot();
        break;
    case OP_FRAME:
        FinishFrame();
        if (gFrame % 64 == 0) SweepShadows();
        if (gStrandsBackoff > 0) gStrandsBackoff--;
        break;
    }
}

// ---- exports -------------------------------------------------------------------------------------------------------
#include "path_hair_registry.inl"

EXPORT int HOP_ApiVersion() { return HOP_API_VERSION; }

EXPORT int HOP_PathHairSourceV1(PathHairSource *out) {
    if(!out || !out->items)return -1;
    out->points=nullptr;out->count=0;out->serial=(uint64_t)gFrame;
    if(!gFbDone || !gHs.points || gHsCount<=0)return 0;
    if(out->capacity<(uint32_t)gHsCount)return -1;
    for(int i=0;i<gHsCount;i++) {
        const auto &h=gHsItems[i];auto &p=out->items[i];p=PathHairItem{};
        p.firstPoint=h.counts[3];p.strands=h.counts[0]*h.counts[1];p.segments=h.counts[2];
        p.radius=std::max(h.mat0.v[2],0.f);
        p.roughness=std::min(1.f,std::max(.05f,1.f/sqrtf(std::max(h.mat1.v[3],1.f))));
        p.shift=h.mat1.v[2];p.diffuse=h.mat0.v[3];
    }
    out->points=(ID3D11Resource*)gHs.points;out->count=(uint32_t)gHsCount;return 1;
}

// Device-level setup from any thread: the device and immediate context, and the shader-creation hooks (before the game
// creates hair shaders). The context hooks are installed on the render thread (OP_INSTALL).
EXPORT int HOP_Init(void *resource) {
    if (gDevice) return 0;
    if (!resource) {
        SetError("no resource");
        return 1;
    }
    gSortedHashes.clear();
    std::vector<std::pair<uint64_t, int>> v;
    for (int i = 0; i < kHopProgramCount; i++) v.push_back(std::make_pair(kHopPrograms[i].hash, i));
    std::sort(v.begin(), v.end());
    for (auto &e : v) {
        gSortedHashes.push_back(e.first);
        gSortedIndex.push_back(e.second);
    }
    ID3D11Resource_GetDevice((ID3D11Resource *)resource, &gDevice);
    if (!gDevice) {
        SetError("no device");
        return 2;
    }
    ID3D11Device_GetImmediateContext(gDevice, &gCtx);
    ID3D11DeviceContext_QueryInterface(gCtx, IID_ID3D11DeviceContext1, (void **)&gCtx1);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    gQpcToUs = 1e6 / (double)f.QuadPart;
    if (!HookDevice()) {
        SetError("cannot patch the device table");
        return 3;
    }
    gMergeReady = CreateMergeObjects();
    gStrandsObjects = gMergeReady && CreateStrandsObjects();
    return 0;
}

EXPORT void *HOP_RenderEventFunc() { return (void *)OnRenderEvent; }
// Strands without its objects: merge; merge without its own: off. (Strands also needs the isoline domain, read on the
// render thread at OP_INSTALL: until then its passes merge.)
EXPORT void HOP_SetMode(int mode) {
    if (mode == MODE_STRANDS && !gStrandsObjects) mode = MODE_MERGE;
    if (mode == MODE_MERGE && !gMergeReady) mode = MODE_OFF;
    InterlockedExchange(&gMode, mode);
}
EXPORT int HOP_StrandsReady() { return gStrandsReady ? 1 : 0; }
// Test API (2.1.0): the transmission lobe on (default) or off (2.0.1's lighting).
EXPORT void HOP_SetTransmission(int on) { InterlockedExchange(&gTransmission, on ? 1 : 0); }

// Test API: GPU time statistics of the opaque pass start again; a snapshot of the next opaque pass goes to path.
EXPORT void HOP_ResetTiming() {
    AcquireSRWLockExclusive(&gLock);
    gGpuSamples = 0;
    gGpuSumMs = gGpuMinMs = gGpuMaxMs = 0;
    ReleaseSRWLockExclusive(&gLock);
}
EXPORT void HOP_Snapshot(const char *path) {
    snprintf(gSnapshotPath, sizeof gSnapshotPath, "%s", path);
    InterlockedExchange(&gSnapshotRequest, 1);
}
EXPORT int HOP_SnapshotsTaken() { return (int)gSnapshotsTaken; }
// Test API: the hair draws of the next merged opaque pass to path (see CaptureDraw).
EXPORT void HOP_Capture(const char *path) {
    snprintf(gCapturePath, sizeof gCapturePath, "%s", path);
    InterlockedExchange(&gCaptureRequest, 1);
}
EXPORT int HOP_CapturesTaken() { return (int)gCapturesTaken; }
EXPORT int HOP_Installed() { return gInstalled ? 1 : 0; }

EXPORT int HOP_LastError(char *buf, int cap) {
    if (cap <= 0) return 0;
    snprintf(buf, cap, "%s", gError);
    return (int)strlen(buf);
}

EXPORT int HOP_Read(char *buf, int cap) {
    std::string o;
    char b[512];
    AcquireSRWLockShared(&gLock);
    FrameObs f = gLast;
    int frames = gFramesObserved;
    MergeStats ms = gMergeLast;
    int gpuSamples = gGpuSamples;
    double gpuMean = gGpuSamples ? gGpuSumMs / gGpuSamples : 0, gpuMin = gGpuMinMs, gpuMax = gGpuMaxMs;
    ReleaseSRWLockShared(&gLock);
    snprintf(b, sizeof b, "{\"api\":%d,\"installed\":%d,\"mode\":%d,\"frames_observed\":%d,\"created\":[%ld,%ld,%ld,%ld,%ld],\"hair_created\":[%ld,%ld,%ld,%ld,%ld],",
             HOP_API_VERSION, gInstalled ? 1 : 0, (int)gMode, frames, gCreated[0], gCreated[1], gCreated[2], gCreated[3], gCreated[4],
             gHairCreated[0], gHairCreated[1], gHairCreated[2], gHairCreated[3], gHairCreated[4]);
    o += b;
    snprintf(b, sizeof b, "\"merge_ready\":%d,\"gpu\":{\"samples\":%d,\"mean_ms\":%.4f,\"min_ms\":%.4f,\"max_ms\":%.4f},"
             "\"merge\":{\"fb_merged\":%d,\"fb_through\":%d,\"fa_merged\":%d,\"fa_through\":%d,\"flushes\":%d,\"draws\":%d,"
             "\"lights\":%d,\"readbacks\":%d,\"failed_readbacks\":%d,\"readbacks_total\":%ld,\"watched\":%d,\"cpu_us\":%.1f,\"why\":{",
             gMergeReady ? 1 : 0, gpuSamples, gpuMean, gpuMin, gpuMax, ms.fbMerged, ms.fbThrough, ms.faMerged, ms.faThrough, ms.flushes,
             ms.draws, ms.lights, ms.readbacks, ms.failedReadbacks, gReadbacksTotal, gShadowCount, ms.cpuUs);
    o += b;
    for (int i = 0; i < WHY_COUNT; i++) {
        snprintf(b, sizeof b, "%s\"%s\":%d", i ? "," : "", kWhy[i], ms.why[i]);
        o += b;
    }
    o += "}},";
    snprintf(b, sizeof b, "\"strands\":{\"objects\":%d,\"ready\":%d,\"backoff\":%d,\"fails_total\":%d,\"pass\":%d,\"items\":%d,\"points\":%u,"
             "\"segments\":%u,\"fb_resolves\":%d,\"fa_resolves\":%d,\"lights\":%d,\"fails\":%d},",
             gStrandsObjects ? 1 : 0, gStrandsReady ? 1 : 0, gStrandsBackoff, gStrandsFails, ms.hsPass, ms.hsItems, ms.hsPoints, ms.hsSegments,
             ms.hsFbResolves, ms.hsFaResolves, ms.hsLights, ms.hsFails);
    o += b;
    snprintf(b, sizeof b, "\"frame\":{\"frame\":%u,\"windows\":%d,\"hair_draws\":%d,\"other_draws\":%d,\"fb\":%d,\"fa\":%d,\"sc\":%d,\"mv\":%d,"
             "\"instanced_hair\":%d,\"cb_map_discard\":%d,\"cb_map_nooverwrite\":%d,\"cb_map_other\":%d,\"cb_update_subresource\":%d,",
             f.frame, f.windows, f.hairDraws, f.otherDraws, f.fb, f.fa, f.sc, f.mv, f.instancedHair, f.maps[1], f.maps[2], f.maps[3], f.cbUpdates[4]);
    o += b;
    o += "\"targets\":[" + f.targets + "],\"bindings_fb\":[" + f.bindingsFb + "],\"bindings_fa\":[" + f.bindingsFa + "],\"fb_cb_changed\":[";
    bool firstChanged = true;
    for (int s = 0; s < 5; s++)
        for (int k = 0; k < 14; k++)
            if (f.fbChanged[s][k]) {
                snprintf(b, sizeof b, "%s{\"stage\":\"%s\",\"slot\":%d,\"bytes\":%d,\"writes\":%d}", firstChanged ? "" : ",", StageName(s), k,
                         f.fbChangedBytes[s][k], f.fbChanged[s][k]);
                o += b;
                firstChanged = false;
            }
    o += "],\"seq\":[";
    for (size_t i = 0; i < f.seq.size(); i++) {
        const DrawRec &d = f.seq[i];
        snprintf(b, sizeof b, "%s{\"pass\":%d,\"ps\":\"%s\",\"kw\":%u,\"ds\":\"%s\",\"eye\":%d,\"x\":%d,\"w\":%d,\"indices\":%u,\"start\":%u,\"base\":%d,"
                 "\"instances\":%d,\"particles\":%d,\"others_before\":%d,\"rtv\":%d,\"srv\":[%d,%d,%d,%d,%d,%d]}",
                 i ? "," : "", d.pass, kHopPrograms[d.ps].hex, kHopPrograms[d.ps].keywords, d.ds >= 0 ? kHopPrograms[d.ds].hex : "",
                 d.eye, d.x, d.w, d.indices, d.start, d.base, d.instances == 0xFFFFFFFF ? -1 : (int)d.instances, d.particles, d.others,
                 d.rtv, d.srv[0], d.srv[1], d.srv[2], d.srv[3], d.srv[4], d.srv[5]);
        o += b;
    }
    o += "]}}";
    if ((int)o.size() + 1 > cap) return -(int)o.size() - 1;
    memcpy(buf, o.c_str(), o.size() + 1);
    return (int)o.size();
}
