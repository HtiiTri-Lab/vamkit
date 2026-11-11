// VaM cloth shading, native side (D3D11; loaded by VaMClothShading.dll as VaMClothShadingNative.bin).
//
// VaM draws every custom clothing item with one shader, Custom/Subsurface/TransparentGlossNMNoCullSeparateAlpha
// (ComputeBuff): the skin's lighting code (one Blinn-Phong lobe, a wrap diffuse) plus a separate alpha, a forward base
// pass and one additive pass per pixel light (docs\01-现状.md). This module puts the registered garments' draws through
// replacements of those pixel programs (tools\gen_cloth_programs.py -> cloth_programs.h: each lifted from its bytecode
// and compiled again with a mode in b12): 1 = cloth (GGX and Charlie sheen with specular anti-aliasing).
//
// Which programs: known by the SHA-1 of their bytecode; the device's CreatePixelShader is patched so the game's pixel
// shaders are checked as they are created, and the replacement is created next to each clothing one. PSSetShader
// follows which one is bound. Which draws: the plugin registers each garment's (index buffer, first index) - not the
// layers lying on the skin, not small items at the eyes or jewellery - and only those draws, with a clothing program
// bound, are drawn through the replacement with b12; both are put back after the draw. Mode 0 draws nothing differently.
//
// Context function table hooks as in components/shading/skin/native/skin_transmission.cpp
// (template + live table, chained: works with the skin and hair modules whichever patches first).
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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "cloth_programs.h"

#define CS_API_VERSION 1
#define EXPORT extern "C" __declspec(dllexport)

enum { OP_INSTALL = 1, OP_FRAME = 2 };

static ID3D11Device *gDevice;
static ID3D11DeviceContext *gCtx;
static SRWLOCK gLock = SRWLOCK_INIT;  // gRepl, gDraws, gParams, gError
static char gError[512];
static bool gInstalled, gHookedDevice;

static void SetError(const char *fmt, ...) {
    char buf[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof buf, fmt, a);
    va_end(a);
    AcquireSRWLockExclusive(&gLock);
    memcpy(gError, buf, sizeof gError);
    ReleaseSRWLockExclusive(&gLock);
}

// ---- programs --------------------------------------------------------------------------------------------------------

static uint64_t HashOf(const void *code, SIZE_T size) {
    uint8_t h[20];
    if (BCryptHash(BCRYPT_SHA1_ALG_HANDLE, NULL, 0, (PUCHAR)code, (ULONG)size, h, 20) != 0) return 0;
    uint64_t k = 0;
    for (int i = 0; i < 8; i++) k = (k << 8) | h[i];
    return k;
}

static int ProgramOf(uint64_t k) {  // kClothPrograms is sorted by hash (checked in CS_Init)
    int lo = 0, hi = kClothProgramCount;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (kClothPrograms[m].hash < k) lo = m + 1;
        else hi = m;
    }
    return lo < kClothProgramCount && kClothPrograms[lo].hash == k ? lo : -1;
}

// the game's clothing pixel shader -> its replacement (under gLock)
static std::unordered_map<ID3D11PixelShader *, ID3D11PixelShader *> gRepl;
static std::vector<ID3D11PixelShader *> gRetired;  // replacements of shaders whose address came back, released at OP_FRAME
static volatile LONG gCreatedPS, gCreatedCloth, gCreateFailed;

typedef HRESULT(STDMETHODCALLTYPE *CreatePSFn)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11PixelShader **);
static CreatePSFn oCreatePS;

static HRESULT STDMETHODCALLTYPE hCreatePS(ID3D11Device *d, const void *c, SIZE_T n, ID3D11ClassLinkage *l, ID3D11PixelShader **o) {
    HRESULT hr = oCreatePS(d, c, n, l, o);
    if (SUCCEEDED(hr) && o && *o) {
        InterlockedIncrement(&gCreatedPS);
        int p = ProgramOf(HashOf(c, n));
        ID3D11PixelShader *repl = NULL;
        if (p >= 0) {
            if (SUCCEEDED(oCreatePS(d, kClothPrograms[p].code, kClothPrograms[p].size, NULL, &repl)) && repl) InterlockedIncrement(&gCreatedCloth);
            else {
                repl = NULL;
                InterlockedIncrement(&gCreateFailed);
            }
        }
        AcquireSRWLockExclusive(&gLock);
        auto old = gRepl.find(*o);  // a new shader at an address an old clothing shader had must not keep its entry
        if (old != gRepl.end()) {
            gRetired.push_back(old->second);
            gRepl.erase(old);
        }
        if (repl) gRepl[*o] = repl;
        ReleaseSRWLockExclusive(&gLock);
    }
    return hr;
}

static bool PatchPointer(void **at, void *value) {
    DWORD old;
    if (!VirtualProtect(at, sizeof(void *), PAGE_READWRITE, &old)) return false;
    *at = value;
    VirtualProtect(at, sizeof(void *), old, &old);
    return true;
}

#define DSLOT(f) (offsetof(ID3D11DeviceVtbl, f) / sizeof(void *))
static bool HookDevice() {
    void **t = (void **)gDevice->lpVtbl;
    oCreatePS = (CreatePSFn)t[DSLOT(CreatePixelShader)];
    return PatchPointer(&t[DSLOT(CreatePixelShader)], (void *)hCreatePS);
}

// ---- context hooks ---------------------------------------------------------------------------------------------------

#define CSLOT(f) (offsetof(ID3D11DeviceContextVtbl, f) / sizeof(void *))
typedef void(STDMETHODCALLTYPE *PSSetShaderFn)(ID3D11DeviceContext *, ID3D11PixelShader *, ID3D11ClassInstance *const *, UINT);
typedef void(STDMETHODCALLTYPE *DrawIndexedFn)(ID3D11DeviceContext *, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE *DrawIndexedInstancedFn)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
static PSSetShaderFn oPSSetShader;
static DrawIndexedFn oDrawIndexed;
static DrawIndexedInstancedFn oDrawIndexedInstanced;
static void **gTemplate;

// The runtime's template copy of the immediate context's fast-path table (the place in d3d11.dll's image holding the
// same pointers as the live table's first 12 entries, QueryInterface .. VSSetShader, which never change).
static void **FindTemplate(void **live) {
    HMODULE m = GetModuleHandleW(L"d3d11.dll");
    MODULEINFO mi;
    if (!m || !GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi)) return NULL;
    uint8_t *base = (uint8_t *)mi.lpBaseOfDll, *end = base + mi.SizeOfImage;
    void **found = NULL;
    int count = 0;
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

static bool HookSlot(void **live, size_t slot, void *hook, void **orig) {
    *orig = gTemplate[slot];
    if (!PatchPointer(&gTemplate[slot], hook)) return false;
    if (live[slot] == *orig) live[slot] = hook;
    return true;
}

static bool InModule(void *p, const wchar_t *name) {
    HMODULE m = GetModuleHandleW(name);
    MODULEINFO mi;
    if (!m || !GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi)) return false;
    return (uint8_t *)p >= (uint8_t *)mi.lpBaseOfDll && (uint8_t *)p < (uint8_t *)mi.lpBaseOfDll + mi.SizeOfImage;
}

// ---- registered draws, parameters (render thread reads under the shared lock) ----------------------------------------

struct KeyHash {
    size_t operator()(const std::pair<void *, UINT> &k) const { return std::hash<void *>()(k.first) ^ ((size_t)k.second * 0x9e3779b97f4a7c15ull); }
};
static std::unordered_set<std::pair<void *, UINT>, KeyHash> gDraws;
// ClothCtl (gen_cloth_programs.py HEADER): mode, AA variance, AA cap, gain; sheen alpha begin, alpha whole, gain, 0
static float gParams[8] = {0, 0.15915494f, 0.18f, 2.0f, 0.25f, 0.5f, 2.0f, 0};
static LONG gParamsVersion = 1, gCbVersion;
static ID3D11Buffer *gCb;

static ID3D11PixelShader *gCurPS, *gCurRepl;

struct Stats {
    long draws, replaced, unregistered;
};
static Stats gStatsCur, gStatsLast;

static void STDMETHODCALLTYPE hPSSetShader(ID3D11DeviceContext *c, ID3D11PixelShader *ps, ID3D11ClassInstance *const *ci, UINT n) {
    oPSSetShader(c, ps, ci, n);
    if (c != gCtx) return;
    gCurPS = ps;
    gCurRepl = NULL;
    if (!ps) return;
    AcquireSRWLockShared(&gLock);
    auto it = gRepl.find(ps);
    if (it != gRepl.end()) gCurRepl = it->second;
    ReleaseSRWLockShared(&gLock);
}

static void ClothDraw(ID3D11DeviceContext *c, UINT n, UINT inst, UINT s, INT b, UINT si, bool instanced) {
    ID3D11Buffer *ib = NULL;
    DXGI_FORMAT fmt;
    UINT off;
    ID3D11DeviceContext_IAGetIndexBuffer(c, &ib, &fmt, &off);
    if (ib) ID3D11Buffer_Release(ib);
    bool registered = false;
    float mode = 0;
    LONG version = 0;
    AcquireSRWLockShared(&gLock);
    mode = gParams[0];
    version = gParamsVersion;
    registered = gDraws.count(std::make_pair((void *)ib, s)) != 0;
    ReleaseSRWLockShared(&gLock);
    gStatsCur.draws++;
    if (!registered || mode <= 0) {
        if (mode > 0) gStatsCur.unregistered++;
        if (instanced) oDrawIndexedInstanced(c, n, inst, s, b, si);
        else oDrawIndexed(c, n, s, b);
        return;
    }
    if (version != gCbVersion) {
        float p[8];
        AcquireSRWLockShared(&gLock);
        memcpy(p, gParams, sizeof p);
        ReleaseSRWLockShared(&gLock);
        ID3D11DeviceContext_UpdateSubresource(c, (ID3D11Resource *)gCb, 0, NULL, p, 0, 0);
        gCbVersion = version;
    }
    ID3D11Buffer *oldCb = NULL;
    ID3D11DeviceContext_PSGetConstantBuffers(c, 12, 1, &oldCb);
    ID3D11PixelShader *orig = gCurPS, *repl = gCurRepl;
    oPSSetShader(c, repl, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 12, 1, &gCb);
    if (instanced) oDrawIndexedInstanced(c, n, inst, s, b, si);
    else oDrawIndexed(c, n, s, b);
    oPSSetShader(c, orig, NULL, 0);
    gCurPS = orig;
    gCurRepl = repl;
    ID3D11DeviceContext_PSSetConstantBuffers(c, 12, 1, &oldCb);
    if (oldCb) ID3D11Buffer_Release(oldCb);
    gStatsCur.replaced++;
}

static void STDMETHODCALLTYPE hDrawIndexed(ID3D11DeviceContext *c, UINT n, UINT s, INT b) {
    if (gCurRepl && c == gCtx) ClothDraw(c, n, 1, s, b, 0, false);
    else oDrawIndexed(c, n, s, b);
}

static void STDMETHODCALLTYPE hDrawIndexedInstanced(ID3D11DeviceContext *c, UINT n, UINT inst, UINT s, INT b, UINT si) {
    if (gCurRepl && c == gCtx) ClothDraw(c, n, inst, s, b, si, true);
    else oDrawIndexedInstanced(c, n, inst, s, b, si);
}

static bool HookContext() {
    void **live = (void **)gCtx->lpVtbl;
    // d3d11.dll's own context: its table is rewritten from a template in d3d11.dll's image, which is patched too; a
    // context that wraps it has a table of its own that nothing rewrites: patched where it is
    gTemplate = InModule(live[0], L"d3d11.dll") ? FindTemplate(live) : live;
    if (!gTemplate) return false;
    ID3D11PixelShader *ps = NULL;  // the shader bound now (bound before the hook went in)
    ID3D11DeviceContext_PSGetShader(gCtx, &ps, NULL, NULL);
    gCurPS = ps;
    gCurRepl = NULL;
    if (ps) {
        AcquireSRWLockShared(&gLock);
        auto it = gRepl.find(ps);
        if (it != gRepl.end()) gCurRepl = it->second;
        ReleaseSRWLockShared(&gLock);
        ID3D11PixelShader_Release(ps);
    }
    bool ok = HookSlot(live, CSLOT(PSSetShader), (void *)hPSSetShader, (void **)&oPSSetShader) &&
              HookSlot(live, CSLOT(DrawIndexed), (void *)hDrawIndexed, (void **)&oDrawIndexed) &&
              HookSlot(live, CSLOT(DrawIndexedInstanced), (void *)hDrawIndexedInstanced, (void **)&oDrawIndexedInstanced);
    if (!ok) SetError("cannot patch the context table");
    return ok;
}

// ---- render events ---------------------------------------------------------------------------------------------------

static void __stdcall OnRenderEvent(int ev) {
    if (ev == OP_INSTALL) {
        if (gInstalled || !gCtx) return;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof gParams;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA sd = {gParams, 0, 0};
        if (!gCb && FAILED(ID3D11Device_CreateBuffer(gDevice, &bd, &sd, &gCb))) {
            SetError("cannot create the control constant buffer");
            return;
        }
        gCbVersion = 0;
        gInstalled = HookContext();
    } else if (ev == OP_FRAME) {
        gStatsLast = gStatsCur;
        gStatsCur = Stats();
        std::vector<ID3D11PixelShader *> retired;
        AcquireSRWLockExclusive(&gLock);
        retired.swap(gRetired);
        ReleaseSRWLockExclusive(&gLock);
        for (auto ps : retired) ID3D11PixelShader_Release(ps);
    }
}

// ---- exports ---------------------------------------------------------------------------------------------------------

EXPORT int CS_ApiVersion() { return CS_API_VERSION; }

EXPORT void *CS_RenderEventFunc() { return (void *)OnRenderEvent; }

// texture: any D3D11 texture of the game's device (Texture2D.whiteTexture's native pointer). 1 = the device's
// CreatePixelShader is patched (clothing shaders created from now on get their replacements)
EXPORT int CS_Init(void *texture) {
    for (int i = 1; i < kClothProgramCount; i++)
        if (kClothPrograms[i - 1].hash >= kClothPrograms[i].hash) {
            SetError("cloth_programs.h is not sorted");
            return 0;
        }
    if (gHookedDevice) return 1;
    if (!texture) {
        SetError("no texture");
        return 0;
    }
    ID3D11Resource *r = (ID3D11Resource *)texture;
    ID3D11Resource_GetDevice(r, &gDevice);
    if (!gDevice) {
        SetError("no device");
        return 0;
    }
    ID3D11Device_GetImmediateContext(gDevice, &gCtx);
    gHookedDevice = HookDevice();
    if (!gHookedDevice) SetError("cannot patch CreatePixelShader");
    return gHookedDevice ? 1 : 0;
}

// p: ClothCtl as gen_cloth_programs.py HEADER (8 floats; p[0] the mode: 0 VaM's programs, 1 cloth)
EXPORT void CS_SetParams(const float *p) {
    AcquireSRWLockExclusive(&gLock);
    memcpy(gParams, p, sizeof gParams);
    gParamsVersion++;
    ReleaseSRWLockExclusive(&gLock);
}

// the garments' draws: n (index buffer, first index) pairs, replacing the ones before
EXPORT void CS_SetDraws(void *const *ibs, const int *starts, int n) {
    AcquireSRWLockExclusive(&gLock);
    gDraws.clear();
    for (int i = 0; i < n; i++)
        if (ibs[i]) gDraws.insert(std::make_pair(ibs[i], (UINT)starts[i]));
    ReleaseSRWLockExclusive(&gLock);
}

// Shared material role for the path renderer. Reuses the exact garment
// registration and live controls; no duplicate scene scan or second classifier.
EXPORT int CS_GetPathMaterialV1(void *ib,UINT first,float *parameters) {
    if(!parameters)return 0;
    AcquireSRWLockShared(&gLock);
    bool active=gParams[0]>0 && gDraws.find(std::make_pair(ib,first))!=gDraws.end();
    if(active)memcpy(parameters,gParams,sizeof gParams);
    ReleaseSRWLockShared(&gLock);
    return active?1:0;
}

// out: installed, programs created, clothing programs created, replacements failed, live replacements, registered
// draws, last frame's draws with a clothing program, of them replaced, of them not registered (mode > 0)
EXPORT int CS_Stats(long *out, int cap) {
    long v[9];
    AcquireSRWLockShared(&gLock);
    v[0] = gInstalled ? 1 : 0;
    v[1] = gCreatedPS;
    v[2] = gCreatedCloth;
    v[3] = gCreateFailed;
    v[4] = (long)gRepl.size();
    v[5] = (long)gDraws.size();
    ReleaseSRWLockShared(&gLock);
    v[6] = gStatsLast.draws;
    v[7] = gStatsLast.replaced;
    v[8] = gStatsLast.unregistered;
    int n = std::min(cap, 9);
    for (int i = 0; i < n; i++) out[i] = v[i];
    return n;
}

EXPORT int CS_LastError(char *buf, int cap) {
    AcquireSRWLockShared(&gLock);
    int n = (int)strnlen(gError, sizeof gError);
    if (cap > 0) {
        int k = std::min(n, cap - 1);
        memcpy(buf, gError, k);
        buf[k] = 0;
    }
    ReleaseSRWLockShared(&gLock);
    return n;
}
