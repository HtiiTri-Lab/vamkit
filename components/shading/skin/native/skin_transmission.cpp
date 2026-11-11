// VaM skin transmission, native side (D3D11, loaded by VaMSkinTransmission.dll as VaMSkinTransmissionNative.bin).
//
// VaM's skin shaders (Custom/Subsurface/*ComputeBuff) light skin from the front only: an ear or a finger with a light
// behind it stays dark. This module adds the light that comes through thin skin. Every skin lighting draw for a spot
// or point light with shadows (ForwardAdd) is followed by one more draw of the same geometry with the same vertex
// shader and the same light's constants and shadow map, through a small pixel shader that adds that light's
// transmission by how much skin the light crosses to get there, read from the light's shadow map (0.7.0;
// shaders\transmission.hlsli; blend one + one on colour, depth test equal, no depth write). Directional lights and
// lights without shadows get none.
//
// Which draws are skin: VaM's skin pixel programs are known by the SHA-1 of their bytecode (tools\gen_skin_table.py ->
// skin_programs.h, with the transmission shader that matches each one's inputs); the device's CreatePixelShader is
// patched so the game's pixel shaders are checked as they are created, and PSSetShader follows which one is bound.
// Which character and material: the draw's index buffer and first index, registered per character by the plugin with
// the thickness textures baked for it (bake.cpp, on a worker thread; textures made on the render thread).
//
// Specular (0.4.0): the registered skin draws can also go through a replacement of VaM's own skin pixel program
// (tools\gen_spec_table.py -> spec_programs.h: each program lifted from its bytecode and compiled again with a skin
// GGX specular next to VaM's Blinn-Phong, chosen by the control constants in b12; mode 0 gives the original's output):
// two GGX lobes from the skin's own gloss, F0 2.8% with Fresnel, N.L, white, the specular normal from the normal map
// at a mip bias (single pores do not catch the light), the spec map as cavity. The replacement is created next to
// each original in CreatePixelShader and bound for the draw only, with b12; both put back after it.
//
// Teeth and tongue (0.8.0): VaM draws them with the skin program, lit as if nothing were around them (no lips, no
// cheeks, no mouth: back teeth as bright as front ones) and the teeth at twice the diffuse (their Subsurface Color 0).
// The bake gives them a two-channel texture (back of the mouth, gum end of a tooth); their draws go through the same
// replacement programs with an entry constant buffer of their own at b12 (VaM's specular and diffuse, the teeth's
// diffuse factor with Subsurface Color teethS, the mouth occlusion) and the texture at t16 / s15. No transmission
// draw for them.
//
// Eyes (0.9.0): the sclera, iris and pupil draws (pupils: Custom/Subsurface/Cull, replaced the same way) go through the
// replacement programs with an entry constant buffer of their own at b12 (VaM's specular and diffuse, the eyelid
// occlusion and the cornea catchlight) and the eye rig at t17. The rig is gathered once a frame per character, before
// its first eye draw: a compute shader (kEyeGatherCS) reads the vertex shader's own vertex buffer (t0: VaM's skinned
// vertices this frame) at the vertices the bake picked - four of each sclera's front ring carry the eyeball, the eyelid
// contact points move with the lids - into a small buffer. A character wearing eye overlay items of its own gets
// either effect off (the plugin tells, ST_SetEyeSkin).
// Subsurface scattering (0.10.0, sss.cpp): between the eye camera's BeforeForwardOpaque and AfterForwardOpaque events
// the registered skin draws go through the replacements (whose extra outputs carry the direct diffuse light, the albedo
// and the depth) with three more targets bound; at AfterForwardOpaque that light is spread under the skin with the
// measured skin profile and put back into the colour. The [Diffuse] look's tint is VaM's then (the plugin's choice).
//
// Skin detail (0.11.0 / 0.12.0): in the same replacements, b12 skinCtl / skinNorm (gen_spec_table.py SKIN DETAIL); for
// the spread of a skin's own roughness each draw's gloss map's p50 / p90 at t18 (see the gloss map stats below).
// Skin tiers (0.13.0): each gloss map carries a tier row the plugin sets per texture (a skin's tier, its face or body),
// bound at t19 of its draws; the plugin also reads textures' stats through the same copies (ST_TexStats) and a face
// normal map's pore-scale slope with a compute shader (ST_NormalSlope), for a skin's starting tier. 0.18.0: one row per
// skin gloss map (ST_SetTierRow, the plugin's continuous level), carrying the map's p50 / spread and the spec map's mean
// over the skin texels only (ST_TexStatsMasked: the UV coverage of the map's skin materials, from the plugin).
//
// Context function table hooks as in the hair-strands component's hair_onepass.cpp (template + live table, chained: works with the
// one-pass hair module whichever patches first).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define CINTERFACE
#define COBJMACROS
#define D3D11_NO_HELPERS
#include <windows.h>
#include <bcrypt.h>
#include <d3d11_1.h>
#include <psapi.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "bake.h"
#include "mouth_gpu.h"
#include "skin_programs.h"
#include "spec_programs.h"
#include "sss.h"
#include "rt.h"
#include "../../rt-core/include/rtcore.h"

#define ST_API_VERSION 45  // API 45: scene record 23, Unity's reflection probe. API 44: API 44: ST_SetPathAuthor's background distances (docs\pt-speed-20261109). API 43: PT reads the author's Unity shadow settings (ST_SetPathAuthor, RtLightRec.shadows bits, RT_DRAW_NO_CAST, draw kind 0x400, path mode 65536; docs\pt-author-intent-20261109); 0.23.5: RT_DRAW_CAMERA_ONLY (cast-shadows Off / outline shells seen by PT camera rays only); 0.23.4: ST_GetPathObservedV1 (the real D3D11 state of author shaders, RtScene); 0.23.3: ST_SetPathUiTexturesV1 (canvas elements with their own UI shader composited over PT); 0.22.66: ST_SetPathBounces (PT diffuse bounces); 0.22.62: ST_PathNoticeV1 (PT's one-time object notes for the log)
// 37  // PT scene records v16: actual pixel-program ownership
// 30  // 0.22.20: ST_SetSkinFuzz, ST_SetSpecDisc (tests). 29: RtScene records v7 add what else reads the environment visibility (RtCore API 9); v6 carry
                           // each material's ambient shader and Skyshop's SH (RtCore API 8).
// 27  // 0.22.4: RtScene records v5 carry displayed hair lengths (RtCore API 7).
// 19  // 0.18.0: one tier row per skin gloss map (ST_SetTierRow), masked texture stats
                          // ST_SetSkinDetail's tiers
#define EXPORT extern "C" __declspec(dllexport)

// render events: (op << 24) | arg
enum { OP_INSTALL = 1, OP_BEGIN = 2, OP_END = 3, OP_FRAME = 4, OP_RT_SCENE = 5 };  // OP_RT_SCENE: with data (StRtScene)

static ID3D11Device *gDevice;
static ID3D11DeviceContext *gCtx;
static SRWLOCK gLock = SRWLOCK_INIT;  // gSkinPS, gError, requests, timing sums
static char gError[512];
static bool gInstalled;
static volatile LONG gEnabled = 1;

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

static double gQpcToMs;
static double NowMs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * gQpcToMs;
}

// ---- skin programs ---------------------------------------------------------------------------------------------------

static std::vector<uint64_t> gSortedHashes;
static std::vector<int> gSortedIndex;
static std::unordered_map<ID3D11PixelShader *, int> gSkinPS;  // live skin pixel shaders -> kSkinPrograms index
static volatile LONG gCreatedPS, gCreatedSkinPS;
// diagnostics (ST_SetDiag): every pixel shader's hash, and per frame which ones draw the registered skin meshes
static std::unordered_map<ID3D11PixelShader *, uint64_t> gPsHash;
static volatile LONG gDiag;
static uint64_t gCurHash;
struct DiagKey {
    uint64_t hash;
    int skin, sub;
    bool operator<(const DiagKey &o) const { return hash != o.hash ? hash < o.hash : skin != o.skin ? skin < o.skin : sub < o.sub; }
};
static std::map<DiagKey, int> gDiagTotal;  // draws per (pixel shader, skin, submesh) since ST_SetDiag(1), under gLock
static volatile LONG64 gHookDraws, gHookDrawsCtx, gHookPSSets;  // since ST_SetDiag(1)

static uint64_t HashOf(const void *code, SIZE_T size) {
    uint8_t h[20];
    if (BCryptHash(BCRYPT_SHA1_ALG_HANDLE, NULL, 0, (PUCHAR)code, (ULONG)size, h, 20) != 0) return 0;
    uint64_t k = 0;
    for (int i = 0; i < 8; i++) k = (k << 8) | h[i];
    return k;
}

static int ProgramOf(uint64_t k) {
    auto it = std::lower_bound(gSortedHashes.begin(), gSortedHashes.end(), k);
    if (it == gSortedHashes.end() || *it != k) return -1;
    return gSortedIndex[it - gSortedHashes.begin()];
}

// specular replacements: the game's skin pixel shader -> the replacement made from kSpecPrograms (under gLock)
static std::unordered_map<ID3D11PixelShader *, ID3D11PixelShader *> gSpecPS;
static std::unordered_map<ID3D11PixelShader *, ID3D11PixelShader *> gSpecBase;
static std::unordered_map<ID3D11PixelShader *, int> gSpecIndex;
static bool gRrActive = false;
static std::vector<ID3D11PixelShader *> gSpecRetired;  // replacements of shaders whose address came back, released at OP_FRAME
// replacement -> it writes the subsurface outputs, its gloss map's texture slot (-1: no skin detail) (under gLock)
struct SpecInfo {
    bool sss;
    int gloss;
};
static std::unordered_map<ID3D11PixelShader *, SpecInfo> gSssPS;
static volatile LONG gCreatedSpecPS, gSpecCreateFailed;

static int SpecProgramOf(uint64_t k) {  // kSpecPrograms is sorted by hash (checked in ST_Init)
    int lo = 0, hi = kSpecProgramCount;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (kSpecPrograms[m].hash < k) lo = m + 1;
        else hi = m;
    }
    return lo < kSpecProgramCount && kSpecPrograms[lo].hash == k ? lo : -1;
}

typedef HRESULT(STDMETHODCALLTYPE *CreatePSFn)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11PixelShader **);
static CreatePSFn oCreatePS;
typedef HRESULT(STDMETHODCALLTYPE *CreateSrvFn)(ID3D11Device *,ID3D11Resource *,const D3D11_SHADER_RESOURCE_VIEW_DESC *,ID3D11ShaderResourceView **);
static CreateSrvFn oCreateSrv;
static HRESULT STDMETHODCALLTYPE hCreateSrv(ID3D11Device *device,ID3D11Resource *resource,const D3D11_SHADER_RESOURCE_VIEW_DESC *desc,ID3D11ShaderResourceView **output) {
    HRESULT hr=oCreateSrv(device,resource,desc,output);
    if(SUCCEEDED(hr) && resource && output && *output) {
        D3D11_SHADER_RESOURCE_VIEW_DESC actual;ID3D11ShaderResourceView_GetDesc(*output,&actual);
        if(actual.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D)RtRememberTextureFormat(resource,(uint32_t)actual.Format);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hCreatePS(ID3D11Device *d, const void *c, SIZE_T n, ID3D11ClassLinkage *l, ID3D11PixelShader **o) {
    HRESULT hr = oCreatePS(d, c, n, l, o);
    if (SUCCEEDED(hr) && o && *o) {
        InterlockedIncrement(&gCreatedPS);
        uint64_t k = HashOf(c, n);
        int p = ProgramOf(k), sp = SpecProgramOf(k);
        ID3D11PixelShader *repl = NULL;
        if (sp >= 0) {
            if (SUCCEEDED(oCreatePS(d, kSpecPrograms[sp].code, kSpecPrograms[sp].size, NULL, &repl)) && repl) InterlockedIncrement(&gCreatedSpecPS);
            else {
                repl = NULL;
                InterlockedIncrement(&gSpecCreateFailed);
            }
        }
        AcquireSRWLockExclusive(&gLock);
        gPsHash[*o] = k;
        // a new shader at an address an old skin shader had must not keep its entry
        if (p >= 0) gSkinPS[*o] = p;
        else gSkinPS.erase(*o);
        auto old = gSpecPS.find(*o);
        if (old != gSpecPS.end()) {
            auto b = gSpecBase.find(*o);
            if (b != gSpecBase.end()) { gSpecRetired.push_back(b->second); gSssPS.erase(b->second); gSpecBase.erase(b); }
            gSpecIndex.erase(*o);
            gSssPS.erase(old->second);
            gSpecPS.erase(old);
        }
        if (repl) {
            gSpecBase[*o] = repl; gSpecIndex[*o] = sp;
            gSpecPS[*o] = repl;
            gSssPS[repl] = SpecInfo{kSpecPrograms[sp].sss != 0, kSpecPrograms[sp].gloss};
        }
        ReleaseSRWLockExclusive(&gLock);
        if (p >= 0) InterlockedIncrement(&gCreatedSkinPS);
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
    oCreateSrv=(CreateSrvFn)t[DSLOT(CreateShaderResourceView)];
    return PatchPointer(&t[DSLOT(CreatePixelShader)], (void *)hCreatePS) &&
        PatchPointer(&t[DSLOT(CreateShaderResourceView)],(void*)hCreateSrv);
}

// ---- context hooks ---------------------------------------------------------------------------------------------------

#define CSLOT(f) (offsetof(ID3D11DeviceContextVtbl, f) / sizeof(void *))
typedef void(STDMETHODCALLTYPE *PSSetShaderFn)(ID3D11DeviceContext *, ID3D11PixelShader *, ID3D11ClassInstance *const *, UINT);
typedef void(STDMETHODCALLTYPE *DrawIndexedFn)(ID3D11DeviceContext *, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE *DrawIndexedInstancedFn)(ID3D11DeviceContext *, UINT, UINT, UINT, INT, UINT);
static PSSetShaderFn oPSSetShader;
static DrawIndexedFn oDrawIndexed;
static DrawIndexedInstancedFn oDrawIndexedInstanced;
typedef void(STDMETHODCALLTYPE *DrawFn)(ID3D11DeviceContext*,UINT,UINT);
typedef void(STDMETHODCALLTYPE *DrawInstancedFn)(ID3D11DeviceContext*,UINT,UINT,UINT,UINT);
typedef void(STDMETHODCALLTYPE *ClearDepthFn)(ID3D11DeviceContext*,ID3D11DepthStencilView*,UINT,FLOAT,UINT8);
static DrawFn oDraw;
static DrawInstancedFn oDrawInstanced;
typedef void(STDMETHODCALLTYPE *DrawIndirectFn)(ID3D11DeviceContext*,ID3D11Buffer*,UINT);
typedef void(STDMETHODCALLTYPE *DrawAutoFn)(ID3D11DeviceContext*);
static DrawIndirectFn oDrawIndexedIndirect,oDrawIndirect;
static DrawAutoFn oDrawAuto;
static ClearDepthFn oClearDepth;
static void **gTemplate;

// The runtime's template copy of the immediate context's fast-path table (see onepass: the place in d3d11.dll's image
// holding the same pointers as the live table's first 12 entries, QueryInterface .. VSSetShader, which never change).
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

// ---- thickness entries (render thread only) ----------------------------------------------------------------------

// skin tier rows: 0-7 the steps' own (ST_SetTiers: face 1-4, body 1-4), the rest one per skin gloss map (ST_SetTierRow,
// 0.18.0); three float4 each (gen_spec_table.py skinTier)
static const int kTierRows = 256, kTierFloats = 12;

struct Params {
    // transmission (transmission.hlsli, 0.7.0): no wrap (light from behind only), Gaussian sigma per colour (mm):
    // red 2.5 mm (measured skin: Jimenez 2010's widest red term ~1.9 mm, FaceWorks 2.7 mm), green and blue in the
    // same proportion; 3 mm lets half the red through, 6 mm 6%, 8 mm under 1% (a nose ridge stays dark)
    float strength = 1.0f, wrap = 0.0f, thicknessScale = 1.0f, maxMM = 20.0f;
    float depth[3] = {2.5f, 1.45f, 0.85f};
    // shadow kernel: the shading point moved inward along the normal and the kernel radius (metres); the falloff
    // (metres; 0 = an even inner ring only) gives the ring taps' weights exp(-r / falloff)
    float exitBias = 0.0015f, kernelRadius = 0.008f, kernelFalloff = 0.00367f;
    float bakeSmoothMM = 3.0f;  // BakeInput::smoothMM for the next bakes
    float noseStrength = 0.0f;
    // specular replacement: off = VaM's own program; b12 = specCtl (mode, normal-map mip bias, pore widening, F0),
    // specLobe (roughness scale, sharp / wide lobe roughness factors, sharp-lobe weight)
    bool specOn = false;
    float spec[8] = {1.0f, 2.5f, 1.0f, 0.028f, 1.0f, 0.75f, 1.3f, 0.85f};
    // with it (eyeGeo.y on skin draws), the IBL reflected about the highlight's own normal (the normal map at the mip
    // bias); off = about VaM's W (full resolution, bent by _SpecularBumpiness), as before (ST_SetEnvSpecNormal, tests)
    bool envSpecNormal = true;
    // candidate (eyeGeo.z on skin draws): each light's diffuse times Burley's grazing factor at the skin's own roughness
    // (gen_spec_table.py SkinGrazing); off = as 0.22.14 (ST_SetSkinGrazing, tests)
    bool skinGrazing = true;
    // 0.22.20 (skinLight on skin draws): the vellus sheen's weight (gen_spec_table.py SkinSheen; 0 on lips; 0 = as 0.22.19,
    // ST_SetSkinFuzz, tests) and each light's specular from its disc (SpecDirect; off = the point light of 0.22.19,
    // ST_SetSpecDisc, tests)
    float skinFuzz = 0.15f;
    bool specDisc = true;
    // diffuse replacement in the same programs: off = VaM's diffuse; b12 continues with scatCtl (mode), scatA (per-channel
    // wrap), scatP (per-channel power)
    bool scatOn = false;
    float overallTranslucency = 0.0f;
    int regionalMode = 1;
    float scat[12] = {0, 0, 0, 0, 0.863f, 0.863f, 0.863f, 0, 2.964f, 2.964f, 2.964f, 0};
    // skin detail (0.11.0) in the same programs, b12 skinCtl / skinNorm: the lobes' alpha^2 at the skin's mean gloss
    // (0 = the skin's own), the clamp of a pixel's alpha^2 over the mean's, intensity from the spec map over its own mean
    // (1) or as 0.10.0 (0); the diffuse normal filter width (metres, 0 = VaM's), the ambient on the diffuse normal (1),
    // the cavity filter width (metres); [7] 1: each skin's tier row (t19, its gloss map's stats at t18; 0.13.0)
    bool detailOn = false;
    float skin[8] = {0, 0.05f, 4.0f, 0, 0, 0, 0, 0};
    // teeth and tongue: off = VaM's own program; the teeth's Subsurface Color in VaM's diffuse factor (1 = 1x where it
    // faces the light), a diffuse gain, darkening at the back of the mouth (strength, where it starts: 0 front teeth ..
    // 1 last molars) and at the gum end of a tooth (strength, power)
    bool teethOn = false;
    float teethS = 1.0f, teethGain = 1.0f, teethBack = 0.6f, teethBackStart = 0.15f, teethGum = 0.35f, teethGumPower = 2.0f;
    // subsurface scattering (sss.cpp): strength 0..1 (0 = off), the skin profile's scale, the eye camera's tan(half
    // vertical field of view)
    float sssStrength = 0.0f, sssWidth = 1.0f, sssTanHalfY = 1.17f;
    // 0.20.0 translucency on female skin with the blocker-distance shadows (its amount = overallTranslucency): the
    // subsurface pass's blur of it (strength, width) and its colour (0 the reviewed warm gray .. 1 the response's own)
    float transStrength = 1.0f, transWidth = 1.0f, transTint = 0.4f;
    // eyes: off = VaM's own program; eyelid occlusion (upper lid darkening, its width, lower lid darkening, its width;
    // widths in eyeball radii) and cornea catchlight (strength, GGX alpha, F0, the cornea's radius in eyeball radii);
    // per character (the plugin's id) which of the two it gets (bit 0 occlusion, bit 1 catchlight; 3 when not listed)
    bool eyeOccOn = false, eyeCatchOn = false;
    float eyeOcc[4] = {0.55f, 0.146f, 0.3f, 0.066f}, eyeCatch[4] = {1.0f, 0.05f, 0.025f, 0.6f};
    std::map<int, int> eyeAllow;
    std::map<int, float> noseFaceFilter;
    float mouthStrength=.95f, tongueWet=1.0f;
    bool mouthOn=true,mouthDebug=false;
    int pathBounces = 0;  // 0.22.66: [RayTracing] PathBounces (rt.h RtSetPathBounces)
    uint32_t authorRules = 0; float authorAmbient = 0, authorContact = .6f, authorAmbientRange = 1, authorEmitter = 1, backgroundNear = 0, backgroundFar = 0;  // API 43, 44
    float rtDetail = .75f;
    std::map<int,bool> tongueHeight;
};
static Params gParams;  // written under gLock, copied to the render thread at OP_FRAME
static Params gParamsRT;
static uint32_t gParamsVersion = 1, gParamsVersionRT = 0;

// One character's eye rig on the GPU: the gather's constants (gen_spec_table.py EYE_GATHER: point counts, the front
// rings' vertices, each eye in its ring's frame, the eyelid contact points on their edges) and the buffer it fills, t17
// of the eye draws ([0] / [2] each eye's centre and scale, [1] / [3] its gaze and radius, then the points on the
// eyeball's sphere).
struct EyeRigGpu {
    ID3D11Buffer *cb = nullptr, *out = nullptr;
    ID3D11UnorderedAccessView *uav = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    int vertexCount = 0, points[2] = {};
    float limbus = 0;  // the sclera's front ring in eyeball radii from the gaze axis (both eyes' mean)
    int frame = -1;    // gFrames of the last gather
    bool ok = false;   // and whether it ran
    ID3D11Buffer *pathVerts = nullptr;            // 0.22.71: PT's record of the skin's vertices (RtPathEye)
    ID3D11ShaderResourceView *pathSrv = nullptr;  // and the gather's view of it
    ~EyeRigGpu() {
        if (pathSrv) ID3D11ShaderResourceView_Release(pathSrv);
        if (srv) ID3D11ShaderResourceView_Release(srv);
        if (uav) ID3D11UnorderedAccessView_Release(uav);
        if (out) ID3D11Buffer_Release(out);
        if (cb) ID3D11Buffer_Release(cb);
    }
};

struct Entry {
    ID3D11ShaderResourceView *srv = nullptr;
    ID3D11Buffer *cb = nullptr;  // TEX_THICKNESS: b13 of the transmission draw; teeth, tongue, eyes: b12 of their draw
    ID3D11Buffer *noseCb = nullptr;  // nasal entries' b12: specular/diffuse settings plus the cavity UV and strength
    float uvScale[2], uvOffset[2];
    uint32_t version = 0;
    int skin = 0, sub = 0, kind = TEX_THICKNESS;
    bool nostrilMaterial = false,hasNose=false;
    bool lips = false;  // 0.22.20: the Lips material (no vellus sheen)
    std::shared_ptr<MouthGpu> mouth;
    ID3D11ShaderResourceView *mouthMap=nullptr;
    ID3D11ShaderResourceView *regionalMap=nullptr;
    bool surfaceOnly=false;
    float regionalUV[4]={};
    float mouthUV[4]={};
    std::shared_ptr<EyeRigGpu> rig;  // TEX_EYE: the character's, shared by its eye draws
    bool eyeOn = false;              // TEX_EYE: occlusion or catchlight on for this character (set with the constants)
    // the character's PT geometry metadata (rt.h RtReceiver, shared by its entries), and nails: VaM's own program
    std::shared_ptr<RtReceiver> rtr;
    bool rtOnly = false;
    ~Entry() {
        if (srv) ID3D11ShaderResourceView_Release(srv);
        if (cb) ID3D11Buffer_Release(cb);
        if (noseCb) ID3D11Buffer_Release(noseCb);
        if (mouthMap) ID3D11ShaderResourceView_Release(mouthMap);
        if (regionalMap) ID3D11ShaderResourceView_Release(regionalMap);
    }
};
struct KeyHash {
    size_t operator()(const std::pair<void *, UINT> &k) const { return std::hash<void *>()(k.first) ^ ((size_t)k.second * 0x9E3779B97F4A7C15ull); }
};
static std::unordered_map<std::pair<void *, UINT>, std::unique_ptr<Entry>, KeyHash> gEntries;
static volatile LONG gRegionalCount;
static volatile LONG gSurfaceOnlyCount, gRegionalBytes;
static volatile LONG gEntryCount;  // gEntries.size() for other threads
static volatile LONG gTeethEntryCount;  // of them teeth and tongue
static volatile LONG gEyeEntryCount;    // and eyes

static void CountEntries() {  // render thread, after gEntries changed
    LONG teeth = 0, eyes = 0;
    LONG regional = 0, surfaceOnly = 0, regionalBytes = 0;
    for (auto &kv : gEntries) {
        if(kv.second->regionalMap) {
            regional++;
            ID3D11Resource *res=nullptr;
            ID3D11ShaderResourceView_GetResource(kv.second->regionalMap,&res);
            D3D11_TEXTURE2D_DESC desc={};
            ID3D11Texture2D_GetDesc((ID3D11Texture2D*)res,&desc);
            regionalBytes+=(LONG)(desc.Width*desc.Height*2);
            ID3D11Resource_Release(res);
        }
        if(kv.second->surfaceOnly) surfaceOnly++;
        teeth += kv.second->kind == TEX_TEETH || kv.second->kind == TEX_TONGUE;
        eyes += kv.second->kind == TEX_EYE;
    }
    LONG all = (LONG)gEntries.size();
    InterlockedExchange(&gRegionalCount,regional);
    InterlockedExchange(&gSurfaceOnlyCount,surfaceOnly);
    InterlockedExchange(&gRegionalBytes,regionalBytes);
    InterlockedExchange(&gEntryCount, all);
    InterlockedExchange(&gTeethEntryCount, teeth);
    InterlockedExchange(&gEyeEntryCount, eyes);
}

static ID3D11BlendState *gBlend;
static ID3D11DepthStencilState *gDepth;
static ID3D11SamplerState *gSampler;
static ID3D11Buffer *gSpecCb;  // b12 for the specular / diffuse replacements: spec[8], scat[12], teeth[12], eyes[16] (off)
static std::vector<ID3D11PixelShader *> gVariantPS;
static ID3D11ComputeShader *gEyeCS;  // the eye gather
static bool gReady;  // objects made

struct FrameStats {
    int skinDraws = 0, extraDraws = 0, unregistered = 0, shadowMismatch = 0, specDraws = 0, teethDraws = 0;
    int eyeDraws = 0, eyeGathers = 0, eyeGatherFails = 0;
    int mouthGathers=0,mouthFails=0;
    int rtRecords = 0;  // the plugin's records
    int sssDraws = 0;
    int tierDraws[kTierRows] = {};
};
static FrameStats gStatsCur, gStatsLast;
static int gFrames;  // OP_FRAME events so far

static int gCurProgram = -1;  // kSkinPrograms index of the bound pixel shader, or -1
static ID3D11PixelShader *gCurPS;
static ID3D11PixelShader *gCurSpecPS;  // the bound pixel shader's specular replacement, or NULL
static bool gCurSpecSss;               // that replacement writes the subsurface outputs
static int gCurSpecGloss = -1;         // and samples its gloss map at this slot (-1: no skin detail)

static Entry *FindEntry(ID3D11DeviceContext *c, UINT start) {
    ID3D11Buffer *ib = NULL;
    DXGI_FORMAT fmt;
    UINT off;
    ID3D11DeviceContext_IAGetIndexBuffer(c, &ib, &fmt, &off);
    if (!ib) return NULL;
    auto it = gEntries.find(std::make_pair((void *)ib, start));
    ID3D11Buffer_Release(ib);
    return it != gEntries.end() ? it->second.get() : NULL;
}

// gen_spec_table.py SpecCtl: specCtl, specLobe, scatCtl, scatA, scatP, teethCtl, teethOcc, teethUV, eyeCtl, eyeOcc,
// eyeCatch, eyeGeo, skinCtl, skinNorm, noseUV, noseCtl, mouthCtl, mouthUV, bodyCtl, bodyUV, shadowCtl (0.19.0),
// shadowCtl2 (0.19.1), skinLight (0.22.20)
static const int kSpecFloats = 92;
static_assert(2 * kEyePoints <= kEyeGatherPoints, "the eye gather holds both eyes' points");

static void SpecConstants(const Params &p, float *cb) {
    memset(cb, 0, kSpecFloats * sizeof(float));
    memcpy(cb, p.spec, sizeof p.spec);
    memcpy(cb + 8, p.scat, sizeof p.scat);
    cb[9] = p.overallTranslucency; // scatCtl.y, skin-only; mouth and eye constants stay zero
    cb[45] = p.envSpecNormal ? 1.0f : 0.0f;  // eyeGeo.y: the IBL about the highlight's normal (skin)
    cb[46] = p.skinGrazing ? 1.0f : 0.0f;    // eyeGeo.z: the diffuse's grazing factor (skin, candidate)
    memcpy(cb + 48, p.skin, sizeof p.skin);
    cb[88] = p.skinFuzz;                     // skinLight.x: the vellus sheen (lips: 0, UpdateEntryConstants)
    cb[89] = p.specDisc ? 1.0f : 0.0f;       // skinLight.y: the specular from the light's disc
}

static void UpdateSpecConstants() {
    float cb[kSpecFloats];
    SpecConstants(gParamsRT, cb);
    ID3D11DeviceContext_UpdateSubresource(gCtx, (ID3D11Resource *)gSpecCb, 0, NULL, cb, 0, 0);
}

static void UpdateEntryConstants(Entry &e) {
    const Params &p = gParamsRT;
    if (e.kind == TEX_EYE) {
        // eyes: VaM's own specular and diffuse (modes 0), the eyelid occlusion and the catchlight unless this character
        // steps aside
        float c[kSpecFloats] = {0};
        auto al = p.eyeAllow.find(e.skin);
        int allow = al != p.eyeAllow.end() ? al->second : 3;
        bool occ = p.eyeOccOn && (allow & 1), cat = p.eyeCatchOn && (allow & 2);
        c[32] = occ ? 1.0f : 0.0f, c[33] = cat ? 1.0f : 0.0f;
        c[34] = (float)e.rig->points[0], c[35] = (float)e.rig->points[1];
        memcpy(c + 36, p.eyeOcc, sizeof p.eyeOcc);
        memcpy(c + 40, p.eyeCatch, sizeof p.eyeCatch);
        c[44] = e.rig->limbus;
        ID3D11DeviceContext_UpdateSubresource(gCtx, (ID3D11Resource *)e.cb, 0, NULL, c, 0, 0);
        e.eyeOn = occ || cat;
        e.version = gParamsVersionRT;
        return;
    }
    if (e.kind != TEX_THICKNESS) {
        // teeth and tongue: VaM's own specular and diffuse (modes 0), the mouth occlusion; on teeth also their own
        // Subsurface Color and gain in VaM's diffuse factor. One occlusion per character: the geometric mouth AO where it
        // runs (0.17.2), else the baked back/gum occlusion
        float c[kSpecFloats] = {0};
        bool teeth = e.kind == TEX_TEETH;
        bool geometric = e.mouth && p.mouthOn;
        c[20] = p.teethOn && !geometric && (e.kind!=TEX_MOUTH || p.mouthOn) ? 1.0f : 0.0f, c[21] = p.teethS, c[22] = p.teethGain, c[23] = teeth && p.teethOn ? 1.0f : 0.0f;
        c[24] = p.teethBack, c[25] = p.teethBackStart, c[26] = teeth ? p.teethGum : 0.0f, c[27] = p.teethGumPower;
        c[28] = e.uvScale[0], c[29] = e.uvScale[1], c[30] = e.uvOffset[0], c[31] = e.uvOffset[1];
        if(e.mouth && p.mouthOn) {
            c[64]=p.mouthStrength; c[66]=p.mouthDebug?2.f:1.f; memcpy(c+68,e.mouthUV,sizeof e.mouthUV);
            if(e.kind==TEX_TONGUE && p.tongueWet>0) {
                c[65]=p.tongueWet;
                auto ht=p.tongueHeight.find(e.skin);c[67]=ht!=p.tongueHeight.end()&&ht->second?1.f:0.f;
                c[0]=1; c[1]=.15f; c[2]=.015f; c[3]=.025f;
                c[4]=1; c[5]=.9f; c[6]=1.35f; c[7]=.72f;
            }
        }
        ID3D11DeviceContext_UpdateSubresource(gCtx, (ID3D11Resource *)e.cb, 0, NULL, c, 0, 0);
        e.version = gParamsVersionRT;
        return;
    }
    float w1 = 1.0f, w2 = 0.0f;
    if (p.kernelFalloff > 0) {
        w1 = expf(-0.5f * p.kernelRadius / p.kernelFalloff);
        w2 = expf(-p.kernelRadius / p.kernelFalloff);
    }
    float c[16] = {e.uvScale[0], e.uvScale[1], e.uvOffset[0], e.uvOffset[1],
                   1.0f / p.depth[0], 1.0f / p.depth[1], 1.0f / p.depth[2], p.strength,
                   p.wrap, p.thicknessScale, p.maxMM, 0,
                   p.exitBias, p.kernelRadius, w1, w2};
    ID3D11DeviceContext_UpdateSubresource(gCtx, (ID3D11Resource *)e.cb, 0, NULL, c, 0, 0);
    if (e.noseCb) {
        float sc[kSpecFloats];
        SpecConstants(p, sc);
        if (e.lips) sc[88] = 0.0f;  // no vellus hair on the lips
        if(e.regionalMap) {sc[10]=(float)p.regionalMode;memcpy(sc+28,e.regionalUV,sizeof e.regionalUV);}
        sc[56] = e.uvScale[0], sc[57] = e.uvScale[1], sc[58] = e.uvOffset[0], sc[59] = e.uvOffset[1];
        sc[60] = e.hasNose?1.0f:0.0f, sc[61] = p.noseStrength;
        if(e.mouth && p.mouthOn) {sc[64]=p.mouthStrength;sc[66]=p.mouthDebug?2.f:1.f;memcpy(sc+68,e.mouthUV,sizeof e.mouthUV);}
        auto nf = p.noseFaceFilter.find(e.skin);
        if (e.nostrilMaterial && nf != p.noseFaceFilter.end()) {
            sc[62] = 1.0f;
            sc[63] = nf->second;
        }
        if (e.rtOnly) memset(sc, 0, sizeof sc);  // nails: VaM's own specular and diffuse
        ID3D11DeviceContext_UpdateSubresource(gCtx, (ID3D11Resource *)e.noseCb, 0, NULL, sc, 0, 0);
    }
    e.version = gParamsVersionRT;
}

// The transmission shader samples the light's shadow map where VaM's program does, with a comparison sampler: only
// draw when that slot really holds a depth map of the right shape and a comparison sampler (anything else would be
// undefined on the GPU).
static bool ShadowBound(ID3D11DeviceContext *c, const SkinVariant &v) {
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11SamplerState *smp = NULL;
    ID3D11DeviceContext_PSGetShaderResources(c, v.shadowT, 1, &srv);
    ID3D11DeviceContext_PSGetSamplers(c, v.shadowS, 1, &smp);
    bool ok = srv && smp;
    if (ok) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        ID3D11ShaderResourceView_GetDesc(srv, &sd);
        ok = sd.ViewDimension == (v.shadowCube ? D3D11_SRV_DIMENSION_TEXTURECUBE : D3D11_SRV_DIMENSION_TEXTURE2D);
        switch (sd.Format) {
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
            break;
        default:
            ok = false;
        }
        D3D11_SAMPLER_DESC pd;
        ID3D11SamplerState_GetDesc(smp, &pd);
        if (pd.Filter < D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT || pd.Filter > D3D11_FILTER_COMPARISON_ANISOTROPIC) ok = false;
    }
    if (srv) ID3D11ShaderResourceView_Release(srv);
    if (smp) ID3D11SamplerState_Release(smp);
    return ok;
}

// After a skin draw: the same draw again with the transmission shader, between saving and restoring what it changes.
static void Transmit(ID3D11DeviceContext *c, Entry *entry, UINT count, UINT start, INT base, UINT instances, UINT startInstance,
                     bool instanced) {
    gStatsCur.skinDraws++;
    if (!entry) {
        gStatsCur.unregistered++;
        return;
    }
    if (gParamsRT.strength <= 0) return;
    Entry &e = *entry;
    if (e.version != gParamsVersionRT) UpdateEntryConstants(e);
    const SkinProgram &prog = kSkinPrograms[gCurProgram];
    if (!ShadowBound(c, kSkinVariants[prog.variant])) {
        gStatsCur.shadowMismatch++;
        return;
    }

    ID3D11Buffer *oldCb = NULL;
    ID3D11ShaderResourceView *oldSrv = NULL;
    ID3D11SamplerState *oldSampler = NULL;
    ID3D11BlendState *oldBlend = NULL;
    ID3D11DepthStencilState *oldDepth = NULL;
    FLOAT oldFactor[4];
    UINT oldMask = 0, oldRef = 0;
    ID3D11DeviceContext_PSGetConstantBuffers(c, 13, 1, &oldCb);
    ID3D11DeviceContext_PSGetShaderResources(c, 16, 1, &oldSrv);
    ID3D11DeviceContext_PSGetSamplers(c, 15, 1, &oldSampler);
    ID3D11DeviceContext_OMGetBlendState(c, &oldBlend, oldFactor, &oldMask);
    ID3D11DeviceContext_OMGetDepthStencilState(c, &oldDepth, &oldRef);

    static const FLOAT zero[4] = {0, 0, 0, 0};
    oPSSetShader(c, gVariantPS[prog.variant], NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 13, 1, &e.cb);
    ID3D11DeviceContext_PSSetShaderResources(c, 16, 1, &e.srv);
    ID3D11DeviceContext_PSSetSamplers(c, 15, 1, &gSampler);
    ID3D11DeviceContext_OMSetBlendState(c, gBlend, zero, 0xffffffff);
    ID3D11DeviceContext_OMSetDepthStencilState(c, gDepth, 0);
    if (instanced) oDrawIndexedInstanced(c, count, instances, start, base, startInstance);
    else oDrawIndexed(c, count, start, base);
    gStatsCur.extraDraws++;

    oPSSetShader(c, gCurPS, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 13, 1, &oldCb);
    ID3D11DeviceContext_PSSetShaderResources(c, 16, 1, &oldSrv);
    ID3D11DeviceContext_PSSetSamplers(c, 15, 1, &oldSampler);
    ID3D11DeviceContext_OMSetBlendState(c, oldBlend, oldFactor, oldMask);
    ID3D11DeviceContext_OMSetDepthStencilState(c, oldDepth, oldRef);
    if (oldCb) ID3D11Buffer_Release(oldCb);
    if (oldSrv) ID3D11ShaderResourceView_Release(oldSrv);
    if (oldSampler) ID3D11SamplerState_Release(oldSampler);
    if (oldBlend) ID3D11BlendState_Release(oldBlend);
    if (oldDepth) ID3D11DepthStencilState_Release(oldDepth);
}

static void STDMETHODCALLTYPE hPSSetShader(ID3D11DeviceContext *c, ID3D11PixelShader *ps, ID3D11ClassInstance *const *ci, UINT n) {
    oPSSetShader(c, ps, ci, n);
    if (gDiag) gHookPSSets++;
    if (c != gCtx) return;
    gCurPS = ps;
    int p = -1;
    ID3D11PixelShader *spec = NULL;
    bool sss = false;
    int gloss = -1;
    if (ps) {
        AcquireSRWLockShared(&gLock);
        auto it = gSkinPS.find(ps);
        if (it != gSkinPS.end()) p = it->second;
        auto sp = gSpecPS.find(ps);
        if (sp != gSpecPS.end()) {
            spec = sp->second;
            auto ss = gSssPS.find(spec);
            sss = ss != gSssPS.end() && ss->second.sss;
            gloss = ss != gSssPS.end() ? ss->second.gloss : -1;
        }
        {
            auto h = gPsHash.find(ps);
            gCurHash = h != gPsHash.end() ? h->second : 0;
        }
        ReleaseSRWLockShared(&gLock);
    }
    gCurProgram = p;
    gCurSpecPS = spec;
    gCurSpecSss = sss;
    gCurSpecGloss = gloss;
}

// diagnostics: a draw of a registered skin mesh, whichever pixel shader draws it
static void DiagDraw(ID3D11DeviceContext *c, UINT start) {
    ID3D11Buffer *ib = NULL;
    DXGI_FORMAT fmt;
    UINT off;
    ID3D11DeviceContext_IAGetIndexBuffer(c, &ib, &fmt, &off);
    if (!ib) return;
    auto it = gEntries.find(std::make_pair((void *)ib, start));
    ID3D11Buffer_Release(ib);
    if (it == gEntries.end()) return;
    DiagKey k = {gCurPS ? gCurHash : 0, it->second->skin, it->second->sub};
    AcquireSRWLockExclusive(&gLock);
    gDiagTotal[k]++;
    ReleaseSRWLockExclusive(&gLock);
}

// Teeth and tongue: their draw through the replacement with their own b12 and the occlusion texture at t16 / s15
// (the transmission's slots: the skin programs use neither), all put back after it.
static void TeethDraw(ID3D11DeviceContext *c, Entry &e, UINT n, UINT inst, UINT s, INT b, UINT si, bool instanced) {
    if(e.mouth && gParamsRT.mouthOn) {
        bool fresh=e.mouth->frame!=gFrames;
        if(!e.mouth->Gather(c,gFrames,b)) {
            if(fresh)gStatsCur.mouthFails++;
            if(instanced)oDrawIndexedInstanced(c,n,inst,s,b,si);else oDrawIndexed(c,n,s,b);
            return;
        }
        if(fresh)gStatsCur.mouthGathers++;
    }
    ID3D11ShaderResourceView *oldMouth[2]={};
    ID3D11DeviceContext_PSGetShaderResources(c,20,2,oldMouth);
    ID3D11ShaderResourceView *mouthViews[2]={e.mouthMap,e.mouth?e.mouth->srv:nullptr};
    ID3D11DeviceContext_PSSetShaderResources(c,20,2,mouthViews);
    if (e.version != gParamsVersionRT) UpdateEntryConstants(e);
    ID3D11Buffer *oldCb = NULL;
    ID3D11ShaderResourceView *oldSrv = NULL;
    ID3D11SamplerState *oldSampler = NULL;
    ID3D11DeviceContext_PSGetConstantBuffers(c, 12, 1, &oldCb);
    ID3D11DeviceContext_PSGetShaderResources(c, 16, 1, &oldSrv);
    ID3D11DeviceContext_PSGetSamplers(c, 15, 1, &oldSampler);
    oPSSetShader(c, gCurSpecPS, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 12, 1, &e.cb);
    ID3D11DeviceContext_PSSetShaderResources(c, 16, 1, &e.srv);
    ID3D11DeviceContext_PSSetSamplers(c, 15, 1, &gSampler);
    if (instanced) oDrawIndexedInstanced(c, n, inst, s, b, si);
    else oDrawIndexed(c, n, s, b);
    oPSSetShader(c, gCurPS, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 12, 1, &oldCb);
    ID3D11DeviceContext_PSSetShaderResources(c, 16, 1, &oldSrv);
    ID3D11DeviceContext_PSSetSamplers(c, 15, 1, &oldSampler);
    if (oldCb) ID3D11Buffer_Release(oldCb);
    if (oldSrv) ID3D11ShaderResourceView_Release(oldSrv);
    if (oldSampler) ID3D11SamplerState_Release(oldSampler);
    ID3D11DeviceContext_PSSetShaderResources(c,20,2,oldMouth);
    for(auto v:oldMouth)if(v)ID3D11ShaderResourceView_Release(v);
    gStatsCur.teethDraws++;
}

// The vertex shader's t0 as the eye gather reads it: a structured buffer of float3 from its first element, at least as
// many as the baked mesh has vertices (the bake's vertex numbers are the mesh's, as the vertex shader indexes it).
static bool EyeVertsOk(ID3D11ShaderResourceView *v, int vertexCount) {
    if (!v) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
    ID3D11ShaderResourceView_GetDesc(v, &sd);
    UINT first, num;
    if (sd.ViewDimension == D3D11_SRV_DIMENSION_BUFFER) first = sd.Buffer.FirstElement, num = sd.Buffer.NumElements;
    else if (sd.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX && sd.BufferEx.Flags == 0) first = sd.BufferEx.FirstElement, num = sd.BufferEx.NumElements;
    else return false;
    if (sd.Format != DXGI_FORMAT_UNKNOWN || first != 0 || num < (UINT)vertexCount) return false;
    ID3D11Resource *res = NULL;
    ID3D11ShaderResourceView_GetResource(v, &res);
    bool ok = false;
    if (res) {
        D3D11_RESOURCE_DIMENSION dim;
        ID3D11Resource_GetType(res, &dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC bd;
            ID3D11Buffer_GetDesc((ID3D11Buffer *)res, &bd);
            ok = (bd.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) && bd.StructureByteStride == 12;
        }
        ID3D11Resource_Release(res);
    }
    return ok;
}

// The character's eye rig for this frame, once: the gather compute shader on the vertex shader's vertex buffer, the
// compute stage's shader, b0, t0 and u0 put back after it.
static bool EyeGatherWith(ID3D11DeviceContext *c, EyeRigGpu &r, ID3D11ShaderResourceView *verts) {
    if (r.frame == gFrames) return r.ok;
    r.frame = gFrames;
    r.ok = EyeVertsOk(verts, r.vertexCount);
    if (r.ok) {
        ID3D11ComputeShader *oldCS = NULL;
        ID3D11Buffer *oldCb = NULL;
        ID3D11ShaderResourceView *oldSrv = NULL;
        ID3D11UnorderedAccessView *oldUav = NULL;
        ID3D11DeviceContext_CSGetShader(c, &oldCS, NULL, NULL);
        ID3D11DeviceContext_CSGetConstantBuffers(c, 0, 1, &oldCb);
        ID3D11DeviceContext_CSGetShaderResources(c, 0, 1, &oldSrv);
        ID3D11DeviceContext_CSGetUnorderedAccessViews(c, 0, 1, &oldUav);
        ID3D11DeviceContext_CSSetShader(c, gEyeCS, NULL, 0);
        ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 1, &r.cb);
        ID3D11DeviceContext_CSSetShaderResources(c, 0, 1, &verts);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &r.uav, NULL);
        ID3D11DeviceContext_Dispatch(c, 1, 1, 1);
        UINT keep = (UINT)-1;
        ID3D11DeviceContext_CSSetShader(c, oldCS, NULL, 0);
        ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 1, &oldCb);
        ID3D11DeviceContext_CSSetShaderResources(c, 0, 1, &oldSrv);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 1, &oldUav, &keep);
        if (oldCS) ID3D11ComputeShader_Release(oldCS);
        if (oldCb) ID3D11Buffer_Release(oldCb);
        if (oldSrv) ID3D11ShaderResourceView_Release(oldSrv);
        if (oldUav) ID3D11UnorderedAccessView_Release(oldUav);
        gStatsCur.eyeGathers++;
    } else gStatsCur.eyeGatherFails++;
    return r.ok;
}
static bool EyeGather(ID3D11DeviceContext *c, EyeRigGpu &r) {
    if (r.frame == gFrames) return r.ok;
    ID3D11ShaderResourceView *verts = NULL;
    ID3D11DeviceContext_VSGetShaderResources(c, 0, 1, &verts);
    const bool ok = EyeGatherWith(c, r, verts);
    if (verts) ID3D11ShaderResourceView_Release(verts);
    return ok;
}

// Sclera, iris, pupil: their draw through the replacement with their own b12 and the eye rig at t17, both put back
// after it; VaM's own draw when this character has both effects off or its rig could not be gathered.
static void EyeDraw(ID3D11DeviceContext *c, Entry &e, UINT n, UINT inst, UINT s, INT b, UINT si, bool instanced) {
    if (e.version != gParamsVersionRT) UpdateEntryConstants(e);
    const bool rig = e.eyeOn && EyeGather(c, *e.rig);
    if (!rig) {
        if (instanced) oDrawIndexedInstanced(c, n, inst, s, b, si);
        else oDrawIndexed(c, n, s, b);
        return;
    }
    ID3D11Buffer *oldCb = NULL;
    ID3D11ShaderResourceView *oldSrv = NULL;
    ID3D11DeviceContext_PSGetConstantBuffers(c, 12, 1, &oldCb);
    ID3D11DeviceContext_PSGetShaderResources(c, 17, 1, &oldSrv);
    oPSSetShader(c, gCurSpecPS, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 12, 1, &e.cb);
    ID3D11DeviceContext_PSSetShaderResources(c, 17, 1, &e.rig->srv);
    if (instanced) oDrawIndexedInstanced(c, n, inst, s, b, si);
    else oDrawIndexed(c, n, s, b);
    oPSSetShader(c, gCurPS, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 12, 1, &oldCb);
    ID3D11DeviceContext_PSSetShaderResources(c, 17, 1, &oldSrv);
    if (oldCb) ID3D11Buffer_Release(oldCb);
    if (oldSrv) ID3D11ShaderResourceView_Release(oldSrv);
    gStatsCur.eyeDraws++;
}

// ---- gloss map stats (skin detail, 0.12.0) -------------------------------------------------------------------------
// Each gloss map's own p10 / p50 / p90 (the raw texel the replacement samples), read once per texture: at its first
// skin draw its <= 256 px mip is copied to a staging texture, read at a later OP_FRAME (decoded from RGBA8 / BGRA8 /
// R8 / BC1-5, sRGB as the view says). The result is a float4 buffer (p50, p90 - p50, 1, p10) bound at t18 of the
// draws sampling that texture; it hangs on the texture as private data, so it goes when VaM releases the texture (no
// reference kept on VaM's textures past the copy). A map it cannot read gets (0, 0, 0, 0): the 0.11.0 ratio.
// The plugin's own questions (ST_TexStats, 0.13.0: a skin's face gloss and spec maps for its tier) take the same copy
// and land in gTexResults instead; ST_TexStatsMasked's (0.18.0) count only the texels its UV coverage mask holds (the
// mask's rows run as the texture's, bottom up: Unity's UV v).
static const GUID kGlossStatsGuid = {0x6b1f2a43, 0x9c1e, 0x4d7a, {0x8f, 0x21, 0x5e, 0x33, 0xa0, 0x47, 0xc2, 0x19}};
struct GlossJob {
    ID3D11Resource *tex;
    ID3D11Texture2D *staging;
    DXGI_FORMAT view;
    UINT w, h;
    int frame;
    int query;  // ST_TexStats' id, 0 = a draw's gloss map
    std::vector<uint8_t> mask;  // ST_TexStatsMasked: mw x mh, nonzero = counted
    int mw, mh;
};
static std::vector<GlossJob> gGlossJobs;  // render thread
struct TexQuery {
    int id;
    ID3D11Resource *tex;  // a reference until copied
    int kind;             // 0 the stats (ST_TexStats), 1 a normal map's slope (ST_NormalSlope)
    std::vector<uint8_t> mask;  // the stats over these texels only (ST_TexStatsMasked; empty = all)
    int mw, mh;
};
static std::vector<TexQuery> gTexQueries;  // asked (under gLock), started at OP_FRAME
struct TexResult {
    int id, state;  // 1 read, -1 failed
    float p10, p50, p90, mean;
};
static std::vector<TexResult> gTexResults;  // the last 512 (under gLock; 0.18.0: two per gloss map besides the face reads)
static int gTexQueryNext = 1;               // (under gLock)
struct GlossSeen {
    UINT w, h;
    DXGI_FORMAT view;
    float p10, p50, p90;
    bool ok;
};
static std::vector<GlossSeen> gGlossSeen;  // the last maps read, for the status (under gLock)
static int gGlossRead, gGlossFailed;       // (under gLock)

static void TexAnswer(int id, bool ok, float p10, float p50, float p90, float mean) {
    AcquireSRWLockExclusive(&gLock);
    TexResult t = {id, ok ? 1 : -1, p10, p50, p90, mean};
    gTexResults.push_back(t);
    if (gTexResults.size() > 512) gTexResults.erase(gTexResults.begin());
    ReleaseSRWLockExclusive(&gLock);
}

static void GlossAttach(ID3D11Resource *r, const GlossJob *j, float p10, float p50, float p90, bool ok) {
    if (j && j->query) {
        TexAnswer(j->query, false, 0, 0, 0, 0);
        return;
    }
    float v[4] = {ok ? p50 : 0.0f, ok ? std::max(p90 - p50, 0.0f) : 0.0f, ok ? 1.0f : 0.0f, ok ? p10 : 0.0f};
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof v;
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {v, 0, 0};
    ID3D11Buffer *b = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    if (SUCCEEDED(ID3D11Device_CreateBuffer(gDevice, &bd, &init, &b))) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = 1;
        if (SUCCEEDED(ID3D11Device_CreateShaderResourceView(gDevice, (ID3D11Resource *)b, &sd, &srv)))
            ID3D11Resource_SetPrivateDataInterface(r, kGlossStatsGuid, (const IUnknown *)srv);
    }
    if (srv) ID3D11ShaderResourceView_Release(srv);
    if (b) ID3D11Buffer_Release(b);
    AcquireSRWLockExclusive(&gLock);
    (ok ? gGlossRead : gGlossFailed)++;
    GlossSeen g = {j ? j->w : 0, j ? j->h : 0, j ? j->view : DXGI_FORMAT_UNKNOWN, p10, p50, p90, ok};
    gGlossSeen.push_back(g);
    if (gGlossSeen.size() > 24) gGlossSeen.erase(gGlossSeen.begin());
    ReleaseSRWLockExclusive(&gLock);
}

// bytes per texel (or per 4x4 block for BC) and where the red channel is; 0 = not read
static int GlossLayout(DXGI_FORMAT f, int *redAt, bool *bc, bool *srgb) {
    *redAt = 0, *bc = false, *srgb = false;
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: *srgb = true; return 4;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return 4;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: *srgb = true; *redAt = 2; return 4;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM: *redAt = 2; return 4;
    case DXGI_FORMAT_R8_UNORM: return 1;
    case DXGI_FORMAT_BC1_UNORM_SRGB: *srgb = true; *bc = true; return 8;
    case DXGI_FORMAT_BC1_UNORM: *bc = true; return 8;
    case DXGI_FORMAT_BC2_UNORM_SRGB: case DXGI_FORMAT_BC3_UNORM_SRGB: *srgb = true; *bc = true; *redAt = 8; return 16;
    case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC3_UNORM: *bc = true; *redAt = 8; return 16;
    case DXGI_FORMAT_BC4_UNORM: *bc = true; *redAt = -1; return 8;   // -1: a BC4 block at 0
    case DXGI_FORMAT_BC5_UNORM: *bc = true; *redAt = -1; return 16;
    default: return 0;
    }
}

static float GlossSrgb(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }

// one 4x4 block's red values
static void GlossBlock(const uint8_t *b, int redAt, bool fourColour, float out[16]) {
    if (redAt < 0) {  // BC4 (BC5's red): two endpoints, 3-bit indices
        float a0 = b[0] / 255.0f, a1 = b[1] / 255.0f, pal[8] = {a0, a1};
        if (b[0] > b[1]) for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7.0f;
        else {
            for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5.0f;
            pal[6] = 0.0f, pal[7] = 1.0f;
        }
        uint64_t bits = 0;
        for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
        for (int i = 0; i < 16; i++) out[i] = pal[(bits >> (3 * i)) & 7];
        return;
    }
    const uint8_t *c = b + redAt;
    unsigned c0 = c[0] | (c[1] << 8), c1 = c[2] | (c[3] << 8);
    auto red = [](unsigned v) { unsigned r = (v >> 11) & 31; return ((r << 3) | (r >> 2)) / 255.0f; };
    float r0 = red(c0), r1 = red(c1), pal[4];
    pal[0] = r0, pal[1] = r1;
    if (fourColour || c0 > c1) pal[2] = (2 * r0 + r1) / 3.0f, pal[3] = (r0 + 2 * r1) / 3.0f;
    else pal[2] = (r0 + r1) * 0.5f, pal[3] = 0.0f;
    uint32_t idx = c[4] | (c[5] << 8) | (c[6] << 16) | ((uint32_t)c[7] << 24);
    for (int i = 0; i < 16; i++) out[i] = pal[(idx >> (2 * i)) & 3];
}

// the red values in texel order (row y at y * w)
static bool GlossDecode(const GlossJob &j, const D3D11_MAPPED_SUBRESOURCE &m, std::vector<float> &v) {
    int redAt;
    bool bc, srgb;
    int bytes = GlossLayout(j.view, &redAt, &bc, &srgb);
    if (!bytes) return false;
    const uint8_t *base = (const uint8_t *)m.pData;
    v.assign((size_t)j.w * j.h, 0.0f);
    if (bc) {
        bool four = j.view != DXGI_FORMAT_BC1_UNORM && j.view != DXGI_FORMAT_BC1_UNORM_SRGB;
        float px[16];
        for (UINT by = 0; by < j.h / 4; by++)
            for (UINT bx = 0; bx < j.w / 4; bx++) {
                GlossBlock(base + by * m.RowPitch + bx * bytes, redAt, four, px);
                for (int i = 0; i < 16; i++) v[(size_t)(by * 4 + i / 4) * j.w + bx * 4 + i % 4] = px[i];
            }
    } else {
        for (UINT y = 0; y < j.h; y++)
            for (UINT x = 0; x < j.w; x++) v[(size_t)y * j.w + x] = base[y * m.RowPitch + x * bytes + redAt] / 255.0f;
    }
    if (srgb)
        for (float &x : v) x = GlossSrgb(x);
    return true;
}

// a texture seen at a skin draw's gloss slot for the first time (query 0), or one the plugin asked about: its <= 256 px
// mip (from the view's most detailed one) to a staging texture
static void GlossCopy(ID3D11Resource *r, DXGI_FORMAT view, UINT mip, int query, const TexQuery *q = NULL) {
    for (auto &j : gGlossJobs)
        if (j.tex == r && !query && !j.query) return;
    if (gGlossJobs.size() >= 16 && !query) return;  // the rest at a later draw
    int redAt;
    bool bc, srgb;
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &d);
    UINT L = mip;
    while (L + 1 < d.MipLevels && std::max(d.Width >> L, d.Height >> L) > 256) L++;
    GlossJob j = {NULL, NULL, view, std::max(1u, d.Width >> L), std::max(1u, d.Height >> L), gFrames, query};
    if (q && !q->mask.empty()) j.mask = q->mask, j.mw = q->mw, j.mh = q->mh;
    if (L >= d.MipLevels || !GlossLayout(view, &redAt, &bc, &srgb) || std::max(j.w, j.h) > 1024 || d.ArraySize != 1 || (bc && (j.w % 4 || j.h % 4))) {
        GlossAttach(r, &j, 0, 0, 0, false);
        return;
    }
    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = j.w, sd.Height = j.h, sd.MipLevels = 1, sd.ArraySize = 1, sd.Format = d.Format, sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(gDevice, &sd, NULL, &j.staging))) {
        GlossAttach(r, &j, 0, 0, 0, false);
        return;
    }
    ID3D11DeviceContext_CopySubresourceRegion(gCtx, (ID3D11Resource *)j.staging, 0, 0, 0, 0, r, L, NULL);
    ID3D11Resource_AddRef(r);
    j.tex = r;
    gGlossJobs.push_back(j);
}

// OP_FRAME: the copies the GPU has done, read
static void GlossFrame() {
    for (size_t i = 0; i < gGlossJobs.size();) {
        GlossJob &j = gGlossJobs[i];
        D3D11_MAPPED_SUBRESOURCE m;
        HRESULT hr = ID3D11DeviceContext_Map(gCtx, (ID3D11Resource *)j.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING && gFrames - j.frame < 120) {
            i++;
            continue;
        }
        std::vector<float> v;
        bool ok = SUCCEEDED(hr) && GlossDecode(j, m, v);
        if (SUCCEEDED(hr)) ID3D11DeviceContext_Unmap(gCtx, (ID3D11Resource *)j.staging, 0);
        if (ok && !j.mask.empty()) {  // the texels the mask holds (nearest mask texel)
            std::vector<float> kept;
            for (UINT y = 0; y < j.h; y++) {
                const uint8_t *mr = j.mask.data() + (size_t)((y * 2 + 1) * j.mh / (2 * j.h)) * j.mw;
                for (UINT x = 0; x < j.w; x++)
                    if (mr[(x * 2 + 1) * j.mw / (2 * j.w)]) kept.push_back(v[(size_t)y * j.w + x]);
            }
            v.swap(kept);
        }
        if (ok && v.size() >= 16) {
            double sum = 0;
            for (float x : v) sum += x;
            auto at = [&](double q) {
                size_t k = (size_t)(q * (v.size() - 1));
                std::nth_element(v.begin(), v.begin() + k, v.end());
                return v[k];
            };
            float p10 = at(0.1), p50 = at(0.5), p90 = at(0.9);
            if (j.query) TexAnswer(j.query, true, p10, p50, p90, (float)(sum / v.size()));
            else GlossAttach(j.tex, &j, p10, p50, p90, true);
        } else GlossAttach(j.tex, &j, 0, 0, 0, false);
        ID3D11Texture2D_Release(j.staging);
        ID3D11Resource_Release(j.tex);
        gGlossJobs.erase(gGlossJobs.begin() + i);
    }
}

// a texture's own format as its raw values read (Unity makes typeless textures; sRGB ones read as stored)
static DXGI_FORMAT RawFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM_SRGB: return DXGI_FORMAT_BC1_UNORM;
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM_SRGB: return DXGI_FORMAT_BC2_UNORM;
    case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM_SRGB: return DXGI_FORMAT_BC3_UNORM;
    case DXGI_FORMAT_BC4_TYPELESS: return DXGI_FORMAT_BC4_UNORM;
    case DXGI_FORMAT_BC5_TYPELESS: return DXGI_FORMAT_BC5_UNORM;
    default: return f;
    }
}

// ---- normal map slope (0.13.0) ------------------------------------------------------------------------------------
// A skin's face normal map's pore-scale slope for its starting tier (ST_NormalSlope): the slope compute shader over the
// map's most detailed mip (every texel's tangent-space x, y against its aligned 8 x 8 block's mean; one sum of squares
// per 64 blocks), copied to a staging buffer and summed at a later OP_FRAME: the RMS as gTexResults' mean.
struct SlopeJob {
    ID3D11Buffer *staging;
    UINT n;         // sums
    double texels;  // in the blocks
    int frame, query;
};
static std::vector<SlopeJob> gSlopeJobs;  // render thread
static ID3D11ComputeShader *gSlopeCS;
static DXGI_FORMAT RawFormat(DXGI_FORMAT f);

// the compute stage's shader, b0, t0 and u0 put back after it
static void SlopeStart(ID3D11Resource *r, int id) {
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(r, &dim);
    D3D11_TEXTURE2D_DESC d = {};
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &d);
    UINT bx = d.Width / 8, by = d.Height / 8, gx = (bx + 7) / 8, gy = (by + 7) / 8;
    ID3D11ShaderResourceView *srv = NULL;
    ID3D11UnorderedAccessView *uav = NULL;
    ID3D11Buffer *out = NULL, *staging = NULL, *cb = NULL;
    bool ok = gSlopeCS && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D && bx && by && d.ArraySize == 1 && gx <= 65535 && gy <= 65535;
    if (ok) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MostDetailedMip = 0;
        sd.Texture2D.MipLevels = 1;
        sd.Format = RawFormat(d.Format);
        if (FAILED(ID3D11Device_CreateShaderResourceView(gDevice, r, &sd, &srv))) {
            sd.Format = d.Format;  // a typed sRGB texture: only its own format
            ok = SUCCEEDED(ID3D11Device_CreateShaderResourceView(gDevice, r, &sd, &srv));
        }
    }
    if (ok) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = gx * gy * sizeof(float);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        D3D11_BUFFER_DESC sb = bd;
        sb.Usage = D3D11_USAGE_STAGING, sb.BindFlags = 0, sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        UINT size[4] = {bx, by, gx, 0};
        D3D11_BUFFER_DESC cd = {};
        cd.ByteWidth = sizeof size;
        cd.Usage = D3D11_USAGE_IMMUTABLE;
        cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA ci = {size, 0, 0};
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32_FLOAT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = gx * gy;
        ok = SUCCEEDED(ID3D11Device_CreateBuffer(gDevice, &bd, NULL, &out)) &&
             SUCCEEDED(ID3D11Device_CreateBuffer(gDevice, &sb, NULL, &staging)) &&
             SUCCEEDED(ID3D11Device_CreateBuffer(gDevice, &cd, &ci, &cb)) &&
             SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(gDevice, (ID3D11Resource *)out, &ud, &uav));
    }
    if (ok) {
        ID3D11ComputeShader *oldCS = NULL;
        ID3D11Buffer *oldCb = NULL;
        ID3D11ShaderResourceView *oldSrv = NULL;
        ID3D11UnorderedAccessView *oldUav = NULL;
        ID3D11DeviceContext_CSGetShader(gCtx, &oldCS, NULL, NULL);
        ID3D11DeviceContext_CSGetConstantBuffers(gCtx, 0, 1, &oldCb);
        ID3D11DeviceContext_CSGetShaderResources(gCtx, 0, 1, &oldSrv);
        ID3D11DeviceContext_CSGetUnorderedAccessViews(gCtx, 0, 1, &oldUav);
        ID3D11DeviceContext_CSSetShader(gCtx, gSlopeCS, NULL, 0);
        ID3D11DeviceContext_CSSetConstantBuffers(gCtx, 0, 1, &cb);
        ID3D11DeviceContext_CSSetShaderResources(gCtx, 0, 1, &srv);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(gCtx, 0, 1, &uav, NULL);
        ID3D11DeviceContext_Dispatch(gCtx, gx, gy, 1);
        UINT keep = (UINT)-1;
        ID3D11DeviceContext_CSSetShader(gCtx, oldCS, NULL, 0);
        ID3D11DeviceContext_CSSetConstantBuffers(gCtx, 0, 1, &oldCb);
        ID3D11DeviceContext_CSSetShaderResources(gCtx, 0, 1, &oldSrv);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(gCtx, 0, 1, &oldUav, &keep);
        if (oldCS) ID3D11ComputeShader_Release(oldCS);
        if (oldCb) ID3D11Buffer_Release(oldCb);
        if (oldSrv) ID3D11ShaderResourceView_Release(oldSrv);
        if (oldUav) ID3D11UnorderedAccessView_Release(oldUav);
        ID3D11DeviceContext_CopyResource(gCtx, (ID3D11Resource *)staging, (ID3D11Resource *)out);
        SlopeJob j = {staging, gx * gy, (double)bx * by * 64.0, gFrames, id};
        gSlopeJobs.push_back(j);
        staging = NULL;
    } else TexAnswer(id, false, 0, 0, 0, 0);
    if (srv) ID3D11ShaderResourceView_Release(srv);
    if (uav) ID3D11UnorderedAccessView_Release(uav);
    if (out) ID3D11Buffer_Release(out);
    if (staging) ID3D11Buffer_Release(staging);
    if (cb) ID3D11Buffer_Release(cb);
}

// OP_FRAME: the sums the GPU has made, read
static void SlopeFrame() {
    for (size_t i = 0; i < gSlopeJobs.size();) {
        SlopeJob &j = gSlopeJobs[i];
        D3D11_MAPPED_SUBRESOURCE m;
        HRESULT hr = ID3D11DeviceContext_Map(gCtx, (ID3D11Resource *)j.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING && gFrames - j.frame < 120) {
            i++;
            continue;
        }
        double sum = 0;
        if (SUCCEEDED(hr)) {
            const float *f = (const float *)m.pData;
            for (UINT k = 0; k < j.n; k++) sum += f[k];
            ID3D11DeviceContext_Unmap(gCtx, (ID3D11Resource *)j.staging, 0);
        }
        bool ok = SUCCEEDED(hr) && j.texels > 0 && sum >= 0 && sum < 1e30;
        TexAnswer(j.query, ok, 0, 0, 0, ok ? (float)sqrt(sum / j.texels) : 0.0f);
        ID3D11Buffer_Release(j.staging);
        gSlopeJobs.erase(gSlopeJobs.begin() + i);
    }
}

// the plugin's questions (ST_TexStats, ST_NormalSlope): copies or slopes started, at most 4 a frame
static void TexQueriesStart() {
    std::vector<TexQuery> q;
    AcquireSRWLockExclusive(&gLock);
    size_t n = std::min<size_t>(gTexQueries.size(), 4);
    q.assign(gTexQueries.begin(), gTexQueries.begin() + n);
    gTexQueries.erase(gTexQueries.begin(), gTexQueries.begin() + n);
    ReleaseSRWLockExclusive(&gLock);
    for (auto &t : q) {
        D3D11_RESOURCE_DIMENSION dim;
        ID3D11Resource_GetType(t.tex, &dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC d;
            ID3D11Texture2D_GetDesc((ID3D11Texture2D *)t.tex, &d);
            if (t.kind == 1) SlopeStart(t.tex, t.id);
            else GlossCopy(t.tex, RawFormat(d.Format), 0, t.id, &t);
        } else TexAnswer(t.id, false, 0, 0, 0, 0);
        ID3D11Resource_Release(t.tex);
    }
}

// ---- skin tiers (0.13.0, 0.18.0) -----------------------------------------------------------------------------------
// A gloss map's tier row hangs on it as private data (an int, set at OP_FRAME from the plugin's ST_SetGlossTier); the
// rows themselves are small buffers, each written again (UpdateSubresource) only when the plugin changes it: (alpha^2 at
// the gloss map's p50, -2 log2 R: R times smoother alpha at its p90, alpha^2 floor, ceiling), (gloss map mip bias,
// highlight gain, least spread, 0), (0.18.0: the skin texels' p50, p90 - p50, the cavity's divisor, 1 = there); a gloss
// map without a row of its own gets the default row.
static const GUID kGlossTierGuid = {0x2d8e4c11, 0x7a3b, 0x4f02, {0x9b, 0x6e, 0x13, 0xc5, 0x88, 0x0a, 0x4d, 0x7f}};
struct TierSet {
    ID3D11Resource *tex;  // a reference until set
    int row;
};
static std::vector<TierSet> gTierSets;  // asked (under gLock), set at OP_FRAME
static int gTierSetsDone;               // (under gLock)
static float gTierRows[kTierRows * kTierFloats];  // as the shader takes them (under gLock)
static float gTierRowsRT[kTierRows * kTierFloats]; // render-thread snapshot for the PT BSDF
static uint32_t gTierRowVer[kTierRows];           // 0 = never set (under gLock)
static int gTierDefault = 2;                      // (under gLock)
static int gTierDefaultRT = 2;                    // render thread
static ID3D11Buffer *gTierBuf[kTierRows];         // render thread
static ID3D11ShaderResourceView *gTierSrv[kTierRows];
static uint32_t gTierBuilt[kTierRows];

static void TierFrame() {
    std::vector<TierSet> v;
    std::vector<int> rows;
    std::vector<float> data;
    AcquireSRWLockExclusive(&gLock);
    v.swap(gTierSets);
    gTierSetsDone += (int)v.size();
    gTierDefaultRT = gTierDefault;
    for (int r = 0; r < kTierRows; r++)
        if (gTierRowVer[r] != gTierBuilt[r]) {
            rows.push_back(r);
            data.insert(data.end(), gTierRows + kTierFloats * r, gTierRows + kTierFloats * (r + 1));
            gTierBuilt[r] = gTierRowVer[r];
        }
    ReleaseSRWLockExclusive(&gLock);
    for (auto &t : v) {
        if (t.row >= 0) ID3D11Resource_SetPrivateData(t.tex, kGlossTierGuid, sizeof t.row, &t.row);
        else ID3D11Resource_SetPrivateData(t.tex, kGlossTierGuid, 0, NULL);
        ID3D11Resource_Release(t.tex);
    }
    for (size_t i = 0; i < rows.size(); i++) {
        int r = rows[i];
        const float *d = data.data() + kTierFloats * i;
        memcpy(gTierRowsRT+kTierFloats*r,d,kTierFloats*sizeof(float));
        if (gTierBuf[r]) {
            ID3D11DeviceContext_UpdateSubresource(gCtx, (ID3D11Resource *)gTierBuf[r], 0, NULL, d, 0, 0);
            continue;
        }
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = kTierFloats * sizeof(float);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = {d, 0, 0};
        if (FAILED(ID3D11Device_CreateBuffer(gDevice, &bd, &init, &gTierBuf[r]))) {
            gTierBuf[r] = NULL;
            continue;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = kTierFloats / 4;
        if (FAILED(ID3D11Device_CreateShaderResourceView(gDevice, (ID3D11Resource *)gTierBuf[r], &sd, &gTierSrv[r]))) gTierSrv[r] = NULL;
    }
}

RtSurfaceRole RtPathSurfaceRole(ID3D11Buffer *indices,uint32_t start,float &diffuseFactor) {
    diffuseFactor=-1;
    auto it=gEntries.find(std::make_pair((void*)indices,(UINT)start));
    if(it==gEntries.end())return RtSurfaceRole::Generic;
    const auto& e=*it->second;const auto& p=gParamsRT;
    if(e.kind==TEX_THICKNESS)return e.rtOnly?RtSurfaceRole::Nails:e.lips?RtSurfaceRole::Lips:RtSurfaceRole::Skin;
    if(e.kind==TEX_TEETH) {
        if(p.teethOn)diffuseFactor=(2-p.teethS)*p.teethGain;
        return RtSurfaceRole::Teeth;
    }
    if(e.kind==TEX_TONGUE)return RtSurfaceRole::Tongue;
    if(e.kind==TEX_EYE)return RtSurfaceRole::Eye;
    if(e.kind==TEX_MOUTH)return RtSurfaceRole::Mouth;
    return RtSurfaceRole::Generic;
}
// 0.22.71: an eyeball draw's eye rig for PT's eyelid band (rt.cpp RecordPathEyes): the same gather, on PT's record of
// the skin's vertices (the buffer the vertex shader reads; the raster eye draws do not run under PT), once a frame, and
// the eyelid occlusion as the draw's constants carry it (UpdateEntryConstants; zeros when this character's is off).
// False: no eye entry, or its rig could not be gathered.
bool RtPathEye(ID3D11DeviceContext *c,ID3D11Buffer *indices,uint32_t start,ID3D11Buffer *vertices,RtPathEyeRig &out) {
    auto it=gEntries.find(std::make_pair((void *)indices,(UINT)start));
    if(it==gEntries.end() || it->second->kind!=TEX_EYE || !it->second->rig)return false;
    const Entry &e=*it->second;EyeRigGpu &r=*e.rig;
    if(r.pathVerts!=vertices) {
        if(r.pathSrv){ID3D11ShaderResourceView_Release(r.pathSrv);r.pathSrv=nullptr;}
        r.pathVerts=vertices;
        D3D11_BUFFER_DESC bd;ID3D11Buffer_GetDesc(vertices,&bd);
        if((bd.BindFlags&D3D11_BIND_SHADER_RESOURCE) && (bd.MiscFlags&D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) && bd.StructureByteStride==12) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd={};
            sd.Format=DXGI_FORMAT_UNKNOWN;sd.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;sd.Buffer.NumElements=bd.ByteWidth/12;
            if(FAILED(ID3D11Device_CreateShaderResourceView(gDevice,(ID3D11Resource *)vertices,&sd,&r.pathSrv)))r.pathSrv=nullptr;
        }
    }
    if(!r.pathSrv || !EyeGatherWith(c,r,r.pathSrv))return false;
    const Params &p=gParamsRT;
    auto al=p.eyeAllow.find(e.skin);
    const int allow=al!=p.eyeAllow.end()?al->second:3;
    out.rig=r.out;out.points[0]=(uint32_t)r.points[0];out.points[1]=(uint32_t)r.points[1];
    if(p.eyeOccOn && (allow&1))memcpy(out.occlusion,p.eyeOcc,sizeof out.occlusion);
    else memset(out.occlusion,0,sizeof out.occlusion);
    return true;
}
bool RtPathSkinParameters(ID3D11Buffer *indices,uint32_t start,ID3D11Resource *gloss,float *p) {
    memset(p,0,28*sizeof(float));
    auto it=gEntries.find(std::make_pair((void *)indices,(UINT)start));
    // Optical role comes from the baked surface kind. A mouth-visibility rig
    // is an auxiliary feature also attached to lip SKIN; it is not a material.
    if(it==gEntries.end())return false;
    const auto &params=gParamsRT;
    if(it->second->kind==TEX_TONGUE && params.mouthOn && params.tongueWet>0) {
        // Reuse the selected tongue's wet dual-lobe material; geometric PT
        // supplies its visibility. No baked mouth-darkening multiplier.
        p[0]=.025f;p[1]=1;p[2]=.9f;p[3]=1.35f;p[4]=.72f;p[5]=1;
        p[23]=std::min(params.tongueWet,1.f);p[24]=.15f*p[23];p[25]=.015f*p[23];
        return true;
    }
    if(it->second->kind!=TEX_THICKNESS || it->second->rtOnly)return false;
    p[0]=params.specOn?params.spec[3]:.028f;
    p[1]=params.specOn?params.spec[4]:1;
    p[2]=params.specOn?params.spec[5]:1;
    p[3]=params.specOn?params.spec[6]:1;
    p[4]=params.specOn?params.spec[7]:1;
    p[5]=1;p[6]=params.skinGrazing?1.f:0.f;p[7]=it->second->lips?0:params.skinFuzz;
    p[20]=params.sssWidth*.001f;p[21]=params.sssStrength;
    p[22]=std::max(0.0f,params.strength); // 11-10: [General] Strength, light through thin skin in the PT too (PtThinCandidate)
    // Carry the installed skin's normal-frequency separation into PT too.
    p[24]=params.specOn?params.spec[1]:0;
    p[25]=params.specOn?params.spec[2]:0;
    p[26]=params.detailOn?params.skin[4]:0;
    p[27]=params.detailOn?params.skin[6]:0;
    if(params.detailOn && params.skin[7]!=0) {
        int row=gTierDefaultRT;
        if(gloss) {
            UINT bytes=sizeof(int);int found=-1;
            if(SUCCEEDED(ID3D11Resource_GetPrivateData(gloss,kGlossTierGuid,&bytes,&found)) && bytes==sizeof(int) && found>=0 && found<kTierRows)row=found;
        }
        if(row>=0 && row<kTierRows) {
            memcpy(p+8,gTierRowsRT+kTierFloats*row,kTierFloats*sizeof(float));
            if(p[8]>0)p[5]=p[13];
        }
    }
    return true;
}

// the stats of the texture at this draw's gloss slot (a reference), or NULL until they are read; its tier row
static ID3D11ShaderResourceView *GlossStats(ID3D11DeviceContext *c, int slot, int *row) {
    *row = gTierDefaultRT;
    ID3D11ShaderResourceView *v = NULL;
    ID3D11DeviceContext_PSGetShaderResources(c, (UINT)slot, 1, &v);
    if (!v) return NULL;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;
    ID3D11ShaderResourceView_GetDesc(v, &vd);
    ID3D11Resource *r = NULL;
    ID3D11ShaderResourceView_GetResource(v, &r);
    ID3D11ShaderResourceView_Release(v);
    if (!r) return NULL;
    IUnknown *u = NULL;
    UINT n = sizeof u;
    ID3D11ShaderResourceView *stats = NULL;
    if (SUCCEEDED(ID3D11Resource_GetPrivateData(r, kGlossStatsGuid, &n, &u)) && u) stats = (ID3D11ShaderResourceView *)u;  // as attached
    else if (vd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) GlossCopy(r, vd.Format, vd.Texture2D.MostDetailedMip, 0);
    int tr = -1;
    UINT tn = sizeof tr;
    if (SUCCEEDED(ID3D11Resource_GetPrivateData(r, kGlossTierGuid, &tn, &tr)) && tn == sizeof tr && tr >= 0 && tr < kTierRows) *row = tr;
    ID3D11Resource_Release(r);
    return stats;
}

// A draw with a skin pixel shader bound: a registered skin's draw goes through the specular / diffuse replacement
// (when either is on), with b12 bound for it; then the transmission draw. Teeth and tongue: their own draw (when on),
// eyes: theirs (when on), never a transmission draw. Nails (rtOnly): VaM's own program.
static void SkinDraw(ID3D11DeviceContext *c, UINT n, UINT inst, UINT s, INT b, UINT si, bool instanced) {
    Entry *e = FindEntry(c, s);
    if (e && e->kind != TEX_THICKNESS) {
        // Eye and mouth materials need their actual RR attributes too, but
        // must remain excluded from skin diffusion. The derived MRT blend
        // writes zero into E/T for these draws, including their alpha channels.
        const bool replaced=gCurSpecPS && (e->kind==TEX_EYE || gParamsRT.teethOn || gParamsRT.mouthOn);
        SssDrawSave guideSave;
        bool guides=replaced && gRrActive && gCurSpecSss && SssArmed() && SssBeforeDraw(c,guideSave,true);
        if (e->kind == TEX_EYE && gCurSpecPS) EyeDraw(c, *e, n, inst, s, b, si, instanced);
        else if (e->kind != TEX_EYE && gCurSpecPS && (gParamsRT.teethOn || gParamsRT.mouthOn)) TeethDraw(c, *e, n, inst, s, b, si, instanced);
        else if (instanced) oDrawIndexedInstanced(c, n, inst, s, b, si);
        else oDrawIndexed(c, n, s, b);
        if(guides){SssAfterDraw(c,guideSave);gStatsCur.sssDraws++;}
        return;
    }
    if (e && e->rtOnly) {  // nails: VaM's own program
        if (instanced) oDrawIndexedInstanced(c, n, inst, s, b, si);
        else oDrawIndexed(c, n, s, b);
        return;
    }
    // SssArmed owns the independent scattering / translucency / RR requirements.
    // SSS=0 must not disable the reconstruction guides.
    bool sss = e && gCurSpecPS && gCurSpecSss && SssArmed();
    if (e && gCurSpecPS && (gParamsRT.specOn || gParamsRT.scatOn || gParamsRT.overallTranslucency > 0 || gParamsRT.detailOn || sss || e->noseCb)) {
        ID3D11ShaderResourceView *oldMouth[2]={};
        bool mouthBound=false;
        if(e->mouth && gParamsRT.mouthOn) {
            bool fresh=e->mouth->frame!=gFrames;
            mouthBound=e->mouth->Gather(c,gFrames,b);
            if(fresh){if(mouthBound)gStatsCur.mouthGathers++;else gStatsCur.mouthFails++;}
            if(mouthBound) {
                ID3D11DeviceContext_PSGetShaderResources(c,20,2,oldMouth);
                ID3D11ShaderResourceView *views[2]={e->mouthMap,e->mouth->srv};
                ID3D11DeviceContext_PSSetShaderResources(c,20,2,views);
            }
        }
        if (e->noseCb && e->version != gParamsVersionRT) UpdateEntryConstants(*e);
        ID3D11Buffer *oldCb = NULL;
        ID3D11DeviceContext_PSGetConstantBuffers(c, 12, 1, &oldCb);
        oPSSetShader(c, gCurSpecPS, NULL, 0);
        ID3D11Buffer *drawCb = e->noseCb && (!e->mouth || !gParamsRT.mouthOn || mouthBound) ? e->noseCb : gSpecCb;
        ID3D11DeviceContext_PSSetConstantBuffers(c, 12, 1, &drawCb);
        ID3D11ShaderResourceView *oldRegional = NULL;
        if(e->regionalMap) {
            ID3D11DeviceContext_PSGetShaderResources(c,22,1,&oldRegional);
            ID3D11DeviceContext_PSSetShaderResources(c,22,1,&e->regionalMap);
        }
        ID3D11ShaderResourceView *oldNose = NULL;
        ID3D11SamplerState *oldNoseSmp = NULL;
        if (e->hasNose || e->regionalMap) {
            ID3D11DeviceContext_PSGetShaderResources(c, 16, 1, &oldNose);
            ID3D11DeviceContext_PSGetSamplers(c, 15, 1, &oldNoseSmp);
            ID3D11DeviceContext_PSSetShaderResources(c, 16, 1, &e->srv);
            ID3D11DeviceContext_PSSetSamplers(c, 15, 1, &gSampler);
        }
        // the gloss map's stats at t18 (NULL until read: the shader then takes the 0.11.0 ratio), its tier row at t19
        bool glossOn = gParamsRT.detailOn && gParamsRT.skin[7] != 0.0f && gCurSpecGloss >= 0;
        ID3D11ShaderResourceView *oldGloss[2] = {NULL, NULL}, *gloss[2] = {NULL, NULL};
        if (glossOn) {
            ID3D11DeviceContext_PSGetShaderResources(c, 18, 2, oldGloss);
            int row;
            gloss[0] = GlossStats(c, gCurSpecGloss, &row);
            gloss[1] = gTierSrv[row];
            ID3D11DeviceContext_PSSetShaderResources(c, 18, 2, gloss);
            gStatsCur.tierDraws[row]++;
        }
        SssDrawSave sv;
        bool bound = sss && SssBeforeDraw(c, sv);
        if (instanced) oDrawIndexedInstanced(c, n, inst, s, b, si);
        else oDrawIndexed(c, n, s, b);
        if (bound) {
            SssAfterDraw(c, sv);
            gStatsCur.sssDraws++;
        }
        oPSSetShader(c, gCurPS, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(c, 12, 1, &oldCb);
        if (oldCb) ID3D11Buffer_Release(oldCb);
        if (e->hasNose || e->regionalMap) {  // as bound
            ID3D11DeviceContext_PSSetShaderResources(c, 16, 1, &oldNose);
            ID3D11DeviceContext_PSSetSamplers(c, 15, 1, &oldNoseSmp);
            if (oldNose) ID3D11ShaderResourceView_Release(oldNose);
            if (oldNoseSmp) ID3D11SamplerState_Release(oldNoseSmp);
        }
        if(e->regionalMap) {
            ID3D11DeviceContext_PSSetShaderResources(c,22,1,&oldRegional);
            if(oldRegional) ID3D11ShaderResourceView_Release(oldRegional);
        }
        if (glossOn) {
            ID3D11DeviceContext_PSSetShaderResources(c, 18, 2, oldGloss);
            for (int k = 0; k < 2; k++)
                if (oldGloss[k]) ID3D11ShaderResourceView_Release(oldGloss[k]);
            if (gloss[0]) ID3D11ShaderResourceView_Release(gloss[0]);
        }
        if(mouthBound) {
            ID3D11DeviceContext_PSSetShaderResources(c,20,2,oldMouth);
            for(auto v:oldMouth)if(v)ID3D11ShaderResourceView_Release(v);
        }
        gStatsCur.specDraws++;
    } else if (instanced) oDrawIndexedInstanced(c, n, inst, s, b, si);
    else oDrawIndexed(c, n, s, b);
    if (gCurProgram >= 0 && gEnabled && (!e || !e->surfaceOnly)) Transmit(c, e, n, s, b, inst, si, instanced);
}

#include "path_overlay.inl"
static uint64_t gPathDepthDraws=0,gPathSkippedDraws=0;
static uint64_t gPathOwnedDepthSkips=0;
static uint64_t gPathUnknownDraws=0;
static void PathUnregisteredDraw(ID3D11DeviceContext* c,int kind,UINT count,UINT first=UINT_MAX,INT base=0,UINT instances=1) {
    if(kind || !count || c!=gCtx || !gReady || !RtPathSelected() || !RtPathRendering() || !SssPathTarget(c))return;
    static const GUID ownedId={0xe32d8c57,0xd7c9,0x4a21,{0x91,0x65,0x43,0x2e,0xaa,0x57,0x19,0x31}};
    ID3D11PixelShader* ps=nullptr;ID3D11DeviceContext_PSGetShader(c,&ps,nullptr,nullptr);
    UINT owned=0,bytes=sizeof owned;if(ps){ID3D11PixelShader_GetPrivateData(ps,ownedId,&bytes,&owned);ID3D11PixelShader_Release(ps);}
    if(owned)return;
    ID3D11DepthStencilView* targetDepth=nullptr;ID3D11DeviceContext_OMGetRenderTargets(c,0,nullptr,&targetDepth);
    if(!targetDepth)return;ID3D11DepthStencilView_Release(targetDepth); // excludes tone/output blits
    ID3D11DepthStencilState* state=nullptr;UINT stencil;
    ID3D11DeviceContext_OMGetDepthStencilState(c,&state,&stencil);
    D3D11_DEPTH_STENCIL_DESC desc={};desc.DepthEnable=true;
    if(state){ID3D11DepthStencilState_GetDesc(state,&desc);ID3D11DepthStencilState_Release(state);}
    if(!desc.DepthEnable)return; // screen compositor; declared overlay capture is separate
    uint64_t hash=PathProgramHash(c);
    if(std::binary_search(std::begin(kPathEnvironmentPrograms),std::end(kPathEnvironmentPrograms),hash))return;
    // A draw the scene registry does not know (a procedural mesh, an instance
    // stream, an ambiguous shared index range). 0.22.62: it no longer withholds
    // the frame and stops PT; this one draw is not traced, and the log names its
    // program once so its source can get an adapter.
    char reason[112];snprintf(reason,sizeof reason,"Unregistered PT scene draw not traced: PS=%016llx (count=%u)",(unsigned long long)hash,count);
    RtPathDrawDiagnostic(c,count,first,base,hash,instances);
    gPathUnknownDraws++;
    static uint64_t noted[64];static int notedCount=0;
    if(std::find(noted,noted+notedCount,hash)==noted+notedCount){if(notedCount<64)noted[notedCount++]=hash;RtPathNotice(reason);}
}
// 0.23.4: the real D3D11 state an author's shader draws with (RT_DRAW_WATCH: a shader without a generated contract and
// without Unity's _SrcBlend / _DstBlend properties: its Blend / ZWrite / Cull are compiled into its passes, which the
// plugin cannot read). Per material and frame, in the PT camera's own target: the first draw that writes colour gives
// the blend, depth test and cull; any draw that writes depth (a ZWrite prepass included) sets the depth-write bit. The
// plugin reads the frames that changed (ST_GetPathObservedV1) and translates the material by them (RtScene).
// Bits: 0 seen, 1 a colour draw seen, 2 blend enabled, 3-7 SrcBlend, 8-12 DestBlend, 13-15 BlendOp (D3D11 values),
// 16 depth write (any draw), 17 depth test (colour draw), 18-19 cull (D3D11_CULL_MODE, colour draw), 20 depth write
// (colour draw).
struct PathObserved { int frame=-1;uint32_t cur=0,stored=0;bool changed=false; };
static SRWLOCK gObservedLock=SRWLOCK_INIT;
static std::unordered_map<uint64_t,PathObserved> gObserved;
static uint64_t gObservedDraws=0;
static void PathObserve(ID3D11DeviceContext* c,uint64_t material) {
    if(!material || c!=gCtx || !SssPathTarget(c))return;
    ID3D11BlendState* blend=nullptr;FLOAT factor[4];UINT mask;ID3D11DeviceContext_OMGetBlendState(c,&blend,factor,&mask);
    D3D11_BLEND_DESC bd={};bd.RenderTarget[0].SrcBlend=D3D11_BLEND_ONE;bd.RenderTarget[0].DestBlend=D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD;bd.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    if(blend){ID3D11BlendState_GetDesc(blend,&bd);ID3D11BlendState_Release(blend);}
    ID3D11DepthStencilState* ds=nullptr;UINT stencil;ID3D11DeviceContext_OMGetDepthStencilState(c,&ds,&stencil);
    D3D11_DEPTH_STENCIL_DESC dd={};dd.DepthEnable=true;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
    if(ds){ID3D11DepthStencilState_GetDesc(ds,&dd);ID3D11DepthStencilState_Release(ds);}
    ID3D11RasterizerState* rs=nullptr;ID3D11DeviceContext_RSGetState(c,&rs);
    D3D11_RASTERIZER_DESC rd={};rd.CullMode=D3D11_CULL_BACK;
    if(rs){ID3D11RasterizerState_GetDesc(rs,&rd);ID3D11RasterizerState_Release(rs);}
    const auto& rt=bd.RenderTarget[0];
    const bool depthWrite=dd.DepthEnable && dd.DepthWriteMask!=D3D11_DEPTH_WRITE_MASK_ZERO;
    AcquireSRWLockExclusive(&gObservedLock);
    auto& o=gObserved[material];
    if(o.frame!=gFrames) {
        if(o.frame>=0 && o.cur!=o.stored){o.stored=o.cur;o.changed=true;}
        o.cur=0;o.frame=gFrames;
    }
    o.cur|=1u|(depthWrite?1u<<16:0u);
    if(rt.RenderTargetWriteMask && !(o.cur&2u)) {
        o.cur|=2u|(rt.BlendEnable?4u:0u)|((uint32_t(rt.SrcBlend)&31u)<<3)|((uint32_t(rt.DestBlend)&31u)<<8)|((uint32_t(rt.BlendOp)&7u)<<13)
            |(dd.DepthEnable?1u<<17:0u)|((uint32_t(rd.CullMode)&3u)<<18)|(depthWrite?1u<<20:0u);
    }
    gObservedDraws++;
    ReleaseSRWLockExclusive(&gObservedLock);
}
// pairs (material instance id = key >> 2, bits) of the materials whose finished frames changed since the last call
EXPORT int ST_GetPathObservedV1(uint32_t* out,int cap) {
    if(!out || cap<2)return 0;
    int n=0;
    AcquireSRWLockExclusive(&gObservedLock);
    for(auto& kv:gObserved) {
        auto& o=kv.second;
        if(o.frame>=0 && o.frame!=gFrames && o.cur!=o.stored){o.stored=o.cur;o.changed=true;}
        if(!o.changed)continue;
        if(n+2>cap)break;
        out[n++]=uint32_t(kv.first>>2);out[n++]=o.stored;o.changed=false;
    }
    ReleaseSRWLockExclusive(&gObservedLock);
    return n/2;
}
static int PathDrawKind(ID3D11DeviceContext* c,UINT n,UINT s,INT b) {
    if(c!=gCtx || !gReady || !RtPathSelected())return 0;
    ID3D11Buffer *ib=nullptr;DXGI_FORMAT format;UINT offset;
    ID3D11DeviceContext_IAGetIndexBuffer(c,&ib,&format,&offset);
    const UINT bytes=format==DXGI_FORMAT_R32_UINT?4:2;
    uint64_t watched=0;
    int kind=RtPathRasterKind(ib,s+offset/bytes,n,b,0,&watched);
    if(kind<0)kind=RtPathRasterKind(ib,s+offset/bytes,n,b,PathProgramHash(c),&watched);
    if(ib)ID3D11Buffer_Release(ib);
    if(watched)PathObserve(c,watched);
    return kind<0?0:kind;
}
// PathOverlayDraw's `known` for a registry kind: 1 UI, 2 an author effect, 3 unknown (an unregistered author effect?)
static int PathOverlayKnown(int kind) { return kind==3?1:kind==5?2:kind==0?3:0; }
static bool PathDepthDraw(ID3D11DeviceContext *c,UINT n,UINT inst,UINT s,INT b,UINT si,bool instanced,int kind) {
    if(inst>1 && kind!=2)return false;
    if(!kind || kind==3 || kind==5 || !SssPathTarget(c))return false;
    if(kind==4){gPathSkippedDraws++;return true;} // null material must never write depth
    if(gCurSpecGloss>=0 && gParamsRT.detailOn) {int row;auto* stats=GlossStats(c,gCurSpecGloss,&row);if(stats)ID3D11ShaderResourceView_Release(stats);}
    if(SssPathDepthReady()) {gPathOwnedDepthSkips++;gPathSkippedDraws++;return true;}
    ID3D11DepthStencilState *state=nullptr;UINT stencil;
    ID3D11DeviceContext_OMGetDepthStencilState(c,&state,&stencil);
    D3D11_DEPTH_STENCIL_DESC desc={};desc.DepthEnable=true;desc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
    if(state){ID3D11DepthStencilState_GetDesc(state,&desc);ID3D11DepthStencilState_Release(state);}
    if(kind==2 || !desc.DepthEnable || desc.DepthWriteMask==D3D11_DEPTH_WRITE_MASK_ZERO) {gPathSkippedDraws++;return true;}
    // Keep inexpensive scene depth for Unity/UI. The PT estimator owns all
    // physical lighting and material guides, so the old pixel shader is idle.
    oPSSetShader(c,nullptr,nullptr,0);
    if(instanced)oDrawIndexedInstanced(c,n,inst,s,b,si);else oDrawIndexed(c,n,s,b);
    oPSSetShader(c,gCurPS,nullptr,0);gPathDepthDraws++;return true;
}
static void STDMETHODCALLTYPE hDrawIndexed(ID3D11DeviceContext *c, UINT n, UINT s, INT b) {
    int kind=PathDrawKind(c,n,s,b);
    if(kind!=1 && kind!=2 && PathOverlayDraw(c,PathOverlayKnown(kind),[&](){oDrawIndexed(c,n,s,b);})){}
    else if(PathDepthDraw(c,n,1,s,b,0,false,kind)){}
    else {
        PathUnregisteredDraw(c,kind,n,s,b);
        if ((gCurProgram >= 0 || gCurSpecPS) && c == gCtx && gReady) SkinDraw(c, n, 1, s, b, 0, false);
        else oDrawIndexed(c, n, s, b);
    }
    if (gDiag) {
        gHookDraws++;
        if (c == gCtx) gHookDrawsCtx++;
        if (c == gCtx && gReady) DiagDraw(c, s);
    }
}

static void STDMETHODCALLTYPE hDrawIndexedInstanced(ID3D11DeviceContext *c, UINT n, UINT inst, UINT s, INT b, UINT si) {
    int kind=PathDrawKind(c,n,s,b);
    if(kind!=1 && kind!=2 && PathOverlayDraw(c,PathOverlayKnown(kind),[&](){oDrawIndexedInstanced(c,n,inst,s,b,si);})){}
    else if(PathDepthDraw(c,n,inst,s,b,si,true,kind)){}
    else {
        PathUnregisteredDraw(c,inst>1 && kind!=2?0:kind,n,s,b,inst);
        if ((gCurProgram >= 0 || gCurSpecPS) && c == gCtx && gReady) SkinDraw(c, n, inst, s, b, si, true);
        else oDrawIndexedInstanced(c, n, inst, s, b, si);
    }
    if (gDiag) {
        gHookDraws++;
        if (c == gCtx) gHookDrawsCtx++;
        if (c == gCtx && gReady) DiagDraw(c, s);
    }
}

static void STDMETHODCALLTYPE hDraw(ID3D11DeviceContext* c,UINT count,UINT start) {
    if(!PathOverlayDraw(c,3,[&](){oDraw(c,count,start);})) {PathUnregisteredDraw(c,0,count);oDraw(c,count,start);}
}
static void STDMETHODCALLTYPE hDrawInstanced(ID3D11DeviceContext* c,UINT count,UINT instances,UINT start,UINT instance) {
    if(!PathOverlayDraw(c,3,[&](){oDrawInstanced(c,count,instances,start,instance);})) {PathUnregisteredDraw(c,0,count);oDrawInstanced(c,count,instances,start,instance);}
}
static void STDMETHODCALLTYPE hDrawIndexedIndirect(ID3D11DeviceContext* c,ID3D11Buffer* args,UINT offset) {
    if(!PathOverlayDraw(c,3,[&](){oDrawIndexedIndirect(c,args,offset);})) {PathUnregisteredDraw(c,0,UINT_MAX);oDrawIndexedIndirect(c,args,offset);}
}
static void STDMETHODCALLTYPE hDrawIndirect(ID3D11DeviceContext* c,ID3D11Buffer* args,UINT offset) {
    if(!PathOverlayDraw(c,3,[&](){oDrawIndirect(c,args,offset);})) {PathUnregisteredDraw(c,0,UINT_MAX);oDrawIndirect(c,args,offset);}
}
static void STDMETHODCALLTYPE hDrawAuto(ID3D11DeviceContext* c) {
    if(!PathOverlayDraw(c,3,[&](){oDrawAuto(c);})) {PathUnregisteredDraw(c,0,UINT_MAX);oDrawAuto(c);}
}
static void STDMETHODCALLTYPE hClearDepth(ID3D11DeviceContext* c,ID3D11DepthStencilView* view,UINT flags,FLOAT depth,UINT8 stencil) {
    SssDepthCleared(view,flags);
    oClearDepth(c,view,flags,depth,stencil);
    if(gOverlayReplay || c!=gCtx || !RtPathSelected())return;
    ID3D11Resource* resource=nullptr;if(view)ID3D11View_GetResource((ID3D11View*)view,&resource);
    for(auto& layer:gPathOverlays)if(layer.frame==gFrames && layer.sourceDepth==resource)
        for(int k=0;k<2;k++)if(layer.dsv[k])oClearDepth(c,layer.dsv[k],flags,depth,stencil);
    if(resource)ID3D11Resource_Release(resource);
}

static bool InModule(void *p, const wchar_t *name) {
    HMODULE m = GetModuleHandleW(name);
    MODULEINFO mi;
    if (!m || !GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi)) return false;
    return (uint8_t *)p >= (uint8_t *)mi.lpBaseOfDll && (uint8_t *)p < (uint8_t *)mi.lpBaseOfDll + mi.SizeOfImage;
}

static void **gLiveAtInstall;  // the context's table when the hooks went in (diagnostics)

// diagnostics: which module a function pointer is in (file name only)
static std::string ModuleOf(void *p) {
    HMODULE m = NULL;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p, &m))
        GetModuleFileNameA(m, path, sizeof path);
    const char *slash = strrchr(path, '\\');
    char t[MAX_PATH + 32];
    snprintf(t, sizeof t, "%s+0x%llx", slash ? slash + 1 : path, (unsigned long long)((uint8_t *)p - (uint8_t *)m));
    return t;
}

static bool HookContext() {
    void **live = (void **)gCtx->lpVtbl;
    gLiveAtInstall = live;
    // d3d11.dll's own context (its functions are in d3d11.dll; the table itself lives in the context object): the table
    // is rewritten from a template in d3d11.dll's image, which is patched too. A context that wraps it (the debug layer
    // in tests\native_test) has a table of its own that nothing rewrites: patched where it is.
    gTemplate = InModule(live[0], L"d3d11.dll") ? FindTemplate(live) : live;
    if (!gTemplate) return false;
    // the shader bound now (bound before the hook went in)
    ID3D11PixelShader *ps = NULL;
    ID3D11DeviceContext_PSGetShader(gCtx, &ps, NULL, NULL);
    gCurPS = ps;
    gCurProgram = -1;
    gCurSpecPS = NULL;
    if (ps) {
        AcquireSRWLockShared(&gLock);
        auto it = gSkinPS.find(ps);
        if (it != gSkinPS.end()) gCurProgram = it->second;
        auto sp = gSpecPS.find(ps);
        if (sp != gSpecPS.end()) {
            gCurSpecPS = sp->second;
            auto ss = gSssPS.find(gCurSpecPS);
            gCurSpecSss = ss != gSssPS.end() && ss->second.sss;
            gCurSpecGloss = ss != gSssPS.end() ? ss->second.gloss : -1;
        }
        ReleaseSRWLockShared(&gLock);
        ID3D11PixelShader_Release(ps);
    }
    bool ok = HookSlot(live, CSLOT(PSSetShader), (void *)hPSSetShader, (void **)&oPSSetShader) &&
              HookSlot(live, CSLOT(DrawIndexed), (void *)hDrawIndexed, (void **)&oDrawIndexed) &&
              HookSlot(live, CSLOT(DrawIndexedInstanced), (void *)hDrawIndexedInstanced, (void **)&oDrawIndexedInstanced) &&
              HookSlot(live, CSLOT(Draw), (void *)hDraw, (void **)&oDraw) &&
              HookSlot(live, CSLOT(DrawInstanced), (void *)hDrawInstanced, (void **)&oDrawInstanced) &&
              HookSlot(live, CSLOT(DrawIndexedInstancedIndirect), (void *)hDrawIndexedIndirect, (void **)&oDrawIndexedIndirect) &&
              HookSlot(live, CSLOT(DrawInstancedIndirect), (void *)hDrawIndirect, (void **)&oDrawIndirect) &&
              HookSlot(live, CSLOT(DrawAuto), (void *)hDrawAuto, (void **)&oDrawAuto) &&
              HookSlot(live, CSLOT(ClearDepthStencilView), (void *)hClearDepth, (void **)&oClearDepth);
    if (!ok) SetError("cannot patch the context table");
    return ok;
}

static bool CreateObjects() {
    D3D11_BLEND_DESC bd = {};
    bd.IndependentBlendEnable = TRUE;
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    for (int i = 1; i < 8; i++) {
        bd.RenderTarget[i].SrcBlend = bd.RenderTarget[i].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[i].DestBlend = bd.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[i].BlendOp = bd.RenderTarget[i].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[i].RenderTargetWriteMask = 0;
    }
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_EQUAL;
    dd.StencilEnable = FALSE;
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BUFFER_DESC cd = {};
    float cb0[kSpecFloats];
    SpecConstants(gParams, cb0);
    cd.ByteWidth = sizeof cb0;
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA ci = {cb0, 0, 0};
    if (FAILED(ID3D11Device_CreateBlendState(gDevice, &bd, &gBlend)) || FAILED(ID3D11Device_CreateDepthStencilState(gDevice, &dd, &gDepth)) ||
        FAILED(ID3D11Device_CreateSamplerState(gDevice, &sd, &gSampler)) || FAILED(ID3D11Device_CreateBuffer(gDevice, &cd, &ci, &gSpecCb))) {
        SetError("cannot create render states");
        return false;
    }
    for (int i = 0; i < kSkinVariantCount; i++) {
        ID3D11PixelShader *ps = NULL;
        HRESULT hr = oCreatePS(gDevice, kSkinVariants[i].code, kSkinVariants[i].size, NULL, &ps);
        if (FAILED(hr)) {
            SetError("transmission shader %d: 0x%08x", i, (unsigned)hr);
            return false;
        }
        gVariantPS.push_back(ps);
    }
    HRESULT hr = ID3D11Device_CreateComputeShader(gDevice, kEyeGatherCS, sizeof kEyeGatherCS, NULL, &gEyeCS);
    if (FAILED(hr)) {
        SetError("eye gather shader: 0x%08x", (unsigned)hr);
        return false;
    }
    if (FAILED(ID3D11Device_CreateComputeShader(gDevice, kNormalSlopeCS, sizeof kNormalSlopeCS, NULL, &gSlopeCS)))
        gSlopeCS = NULL;  // no slopes: the plugin's starting tiers without the normal map
    return true;
}

// The GPU side of a baked eye rig: the gather's constants (immutable) and its output buffer with its views. False (and
// why) when the rig does not fit or points outside the mesh.
static bool CreateEyeRig(const EyeRig &rig, int vertexCount, std::shared_ptr<EyeRigGpu> &out, std::string &error) {
    std::shared_ptr<EyeRigGpu> r(new EyeRigGpu());
    r->vertexCount = vertexCount;
    std::vector<uint32_t> c(28 + 4 * kEyeGatherPoints, 0);
    int k = 0;
    for (int e = 0; e < 2; e++) {
        if ((int)rig.points[e].size() > kEyePoints) {
            error = "too many eyelid points";
            return false;
        }
        r->points[e] = (int)rig.points[e].size();
        c[e] = (uint32_t)r->points[e];
        for (int q = 0; q < 4; q++) {
            if (rig.ring[e][q] < 0 || rig.ring[e][q] >= vertexCount) {
                error = "front ring vertex outside the mesh";
                return false;
            }
            c[4 + 4 * e + q] = (uint32_t)rig.ring[e][q];
        }
        float loc[8] = {rig.local[e][0], rig.local[e][1], rig.local[e][2], rig.span[e],
                        rig.local[e][3], rig.local[e][4], rig.local[e][5], rig.radius[e]};
        memcpy(&c[12 + 8 * e], loc, sizeof loc);
        for (const EyePoint &p : rig.points[e]) {
            if (p.a < 0 || p.a >= vertexCount || p.b < 0 || p.b >= vertexCount) {
                error = "eyelid point outside the mesh";
                return false;
            }
            float tw[2] = {p.t, p.upper + (p.start ? 2.0f : 0.0f)};
            c[28 + 4 * k] = (uint32_t)p.a, c[29 + 4 * k] = (uint32_t)p.b;
            memcpy(&c[30 + 4 * k], tw, sizeof tw);
            k++;
        }
        r->limbus += 0.5f * rig.limbus[e] / std::max(rig.radius[e], 1e-6f);
    }
    D3D11_BUFFER_DESC cd = {};
    cd.ByteWidth = (UINT)(c.size() * 4);
    cd.Usage = D3D11_USAGE_IMMUTABLE;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA ci = {c.data(), 0, 0};
    std::vector<float> zero(4 * (4 + kEyeGatherPoints), 0.0f);  // radius 0 until the first gather: no effect
    D3D11_BUFFER_DESC od = {};
    od.ByteWidth = (UINT)(zero.size() * 4);
    od.Usage = D3D11_USAGE_DEFAULT;
    od.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    D3D11_SUBRESOURCE_DATA oi = {zero.data(), 0, 0};
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd.Buffer.FirstElement = 0;
    sd.Buffer.NumElements = 4 + kEyeGatherPoints;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.FirstElement = 0;
    ud.Buffer.NumElements = 4 + kEyeGatherPoints;
    if (FAILED(ID3D11Device_CreateBuffer(gDevice, &cd, &ci, &r->cb)) || FAILED(ID3D11Device_CreateBuffer(gDevice, &od, &oi, &r->out)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(gDevice, (ID3D11Resource *)r->out, &sd, &r->srv)) ||
        FAILED(ID3D11Device_CreateUnorderedAccessView(gDevice, (ID3D11Resource *)r->out, &ud, &r->uav))) {
        error = "cannot create the eye rig buffers";
        return false;
    }
    out = r;
    return true;
}

// ---- bake jobs ---------------------------------------------------------------------------------------------------

#pragma pack(push, 8)
struct StBakeDesc {  // VaMSkinTransmission.cs BakeDesc: all fields 8 bytes
    int64_t version;  // 1, 2 (regional)
    void *indexBuffer;
    int64_t skin;  // the plugin's id for the character (reports)
    int64_t vertexCount;
    const float *pos, *nrm, *uv;
    int64_t indexCount;
    const int *indices;
    int64_t subCount;
    const int *subIndexStart, *subIndexCount, *subFlags, *subGpuStart;
    const float *subST;  // 4 per submesh: _MainTex scale xy, offset xy
    const float *regional; // version 2 only
};
#pragma pack(pop)

struct Job {
    int id = 0;
    void *indexBuffer = nullptr;
    int skin = 0;
    std::vector<float> pos, nrm, uv, st, regional;
    std::vector<int> indices, gpuStart;
    std::vector<BakeSub> subs;
    float smoothMM = 0;
    BakeOutput out;
    MouthData mouth;
    RtBake *rtBake = nullptr;  // the PT geometry metadata (RtCreateReceiver takes it)
    std::string rtError;
    double rtMs = 0;
    std::string error;
    std::atomic<int> state{0};  // 0 running, 1 done, 2 failed
    double startMs = 0, endMs = 0;
    std::thread worker;
    ~Job() { RtFreeBake(rtBake); }
};
static std::vector<std::unique_ptr<Job>> gJobs;  // under gJobLock
static std::mutex gJobLock;
static std::vector<void *> gUnregister;  // index buffers to drop, under gJobLock
static int gNextJob = 1;
struct JobReport {
    int id, skin, state, textures, threads;
    double ms, msRays;
    std::string error;
    TeethReport teeth;
    int eyes = 0, eyePoints[2] = {}, eyeChains[2] = {}, eyeEntries = 0;
    float eyeWidthMM[2] = {}, eyeHeightMM[2] = {};
    std::string eyeError;
    int rtEntries = 0;
    double rtMs = 0;
    std::string rtBake, rtError;
};
static std::vector<JobReport> gJobReports;  // finished, under gJobLock

static void RunJob(Job *j) {
    BakeInput in;
    in.vertexCount = (int)(j->pos.size() / 3);
    in.pos = j->pos.data();
    in.nrm = j->nrm.data();
    in.uv = j->uv.data();
    in.regional = j->regional.empty() ? nullptr : j->regional.data();
    in.indexCount = (int)j->indices.size();
    in.indices = j->indices.data();
    in.subCount = (int)j->subs.size();
    in.subs = j->subs.data();
    in.smoothMM = j->smoothMM;
    bool ok = Bake(in, j->out, j->error);
    // The managed side marks only confirmed female characters. Keep the older
    // teeth/tongue shading for everyone, without building a male mouth AO rig.
    if(ok && std::any_of(j->subs.begin(), j->subs.end(), [](const BakeSub &s) { return (s.flags & BAKE_MOUTH_AO) != 0; }))
        BuildMouth(in,j->mouth);
    // the PT's geometry metadata for the submeshes marked with a part (every character), unless the device is known
    // to have no 11on12 (RtInit failed)
    if (ok && RtMaybe() && std::any_of(j->subs.begin(), j->subs.end(), [](const BakeSub &s) { return (s.flags & BAKE_RT_PART_MASK) != 0; })) {
        const double t0 = NowMs();
        std::vector<int> start, count;
        std::vector<uint8_t> parts;
        for (const BakeSub &s : j->subs) {
            const int p = (s.flags & BAKE_RT_PART_MASK) >> BAKE_RT_PART_SHIFT;
            start.push_back(s.indexStart);
            count.push_back(s.indexCount);
            parts.push_back(p ? (uint8_t)(p - 1) : (uint8_t)255);
        }
        j->rtBake = RtBakeReceiver(in.pos, in.uv, in.vertexCount, in.indices, in.subCount, start.data(), count.data(), j->gpuStart.data(),
                                   parts.data(), j->rtError);
        j->rtMs = NowMs() - t0;
    }
    j->endMs = NowMs();
    j->state.store(ok ? 1 : 2);
}

// Render thread: textures and entries for finished bakes; drops for removed characters.
static void ProcessJobs() {
    std::vector<std::unique_ptr<Job>> done;
    std::vector<void *> drop;
    {
        std::lock_guard<std::mutex> g(gJobLock);
        for (size_t i = 0; i < gJobs.size();) {
            if (gJobs[i]->state.load() != 0) {
                done.push_back(std::move(gJobs[i]));
                gJobs.erase(gJobs.begin() + i);
            } else i++;
        }
        drop.swap(gUnregister);
    }
    for (void *ib : drop)
        for (auto it = gEntries.begin(); it != gEntries.end();) {
            if (it->first.first == ib) it = gEntries.erase(it);
            else ++it;
        }
    CountEntries();
    for (auto &j : done) {
        j->worker.join();
        JobReport r = {j->id, j->skin, j->state.load(), 0, j->out.threads, j->endMs - j->startMs, j->out.msRays, j->error, j->out.teeth};
        if (r.state == 1) {
            for (const BakeTexture &t : j->out.textures) {
                D3D11_TEXTURE2D_DESC td = {};
                td.Width = t.width;
                td.Height = t.height;
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Format = t.channels == 2 ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R16_FLOAT;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_IMMUTABLE;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA init = {t.texels.data(), (UINT)(t.width * 2 * t.channels), 0};
                ID3D11Texture2D *tex = NULL;
                if(t.kind == TEX_REGIONAL) {
                    auto found=gEntries.find(std::make_pair(j->indexBuffer,(UINT)j->gpuStart[t.sub]));
                    if(found==gEntries.end() || found->second->kind!=TEX_THICKNESS) {r.state=2;r.error="regional receiver missing";break;}
                    Entry &entry=*found->second;
                    HRESULT hr=ID3D11Device_CreateTexture2D(gDevice,&td,&init,&tex);
                    if(SUCCEEDED(hr))hr=ID3D11Device_CreateShaderResourceView(gDevice,(ID3D11Resource*)tex,nullptr,&entry.regionalMap);
                    if(tex)ID3D11Texture2D_Release(tex);
                    if(SUCCEEDED(hr)&&!entry.noseCb) {
                        D3D11_BUFFER_DESC cb={};cb.ByteWidth=kSpecFloats*4;cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
                        hr=ID3D11Device_CreateBuffer(gDevice,&cb,nullptr,&entry.noseCb);
                    }
                    if(FAILED(hr)){r.state=2;r.error="regional GPU map creation failed";break;}
                    const float *st=&j->st[4*t.sub];
                    for(int a=0;a<2;a++){float sx=st[a]!=0?st[a]:1.f,span=t.uvMax[a]-t.uvMin[a];entry.regionalUV[a]=1.f/(sx*span);entry.regionalUV[a+2]=(-st[a+2]/sx-t.uvMin[a])/span;}
                    r.textures++;continue;
                }
                std::unique_ptr<Entry> e(new Entry());
                e->kind = t.kind;
                D3D11_BUFFER_DESC bd = {};
                bd.ByteWidth = t.kind == TEX_THICKNESS ? 64 : kSpecFloats * 4;
                bd.Usage = D3D11_USAGE_DEFAULT;
                bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                if (FAILED(ID3D11Device_CreateTexture2D(gDevice, &td, &init, &tex)) ||
                    FAILED(ID3D11Device_CreateShaderResourceView(gDevice, (ID3D11Resource *)tex, NULL, &e->srv)) ||
                    FAILED(ID3D11Device_CreateBuffer(gDevice, &bd, NULL, &e->cb))) {
                    if (tex) ID3D11Texture2D_Release(tex);
                    r.error = "cannot create a baked texture";
                    r.state = 2;
                    break;
                }
                ID3D11Texture2D_Release(tex);  // the view holds it
                if (t.kind == TEX_THICKNESS && t.channels == 2) {
                    e->hasNose=true;
                    bd.ByteWidth = kSpecFloats * 4;
                    if (FAILED(ID3D11Device_CreateBuffer(gDevice, &bd, NULL, &e->noseCb))) {
                        r.error = "cannot create nasal constants";
                        r.state = 2;
                        break;
                    }
                }
                // shader UV (uv * st.xy + st.zw) -> texture UV: ((uvMain - st.zw) / st.xy - uvMin) / (uvMax - uvMin)
                const float *st = &j->st[4 * t.sub];
                for (int a = 0; a < 2; a++) {
                    float sx = st[a] != 0 ? st[a] : 1.0f, size = t.uvMax[a] - t.uvMin[a];
                    e->uvScale[a] = 1.0f / (sx * size);
                    e->uvOffset[a] = (-st[2 + a] / sx - t.uvMin[a]) / size;
                }
                e->skin = j->skin;
                e->sub = t.sub;
                e->surfaceOnly = (j->subs[t.sub].flags & BAKE_SURFACE_ONLY) != 0;
                e->nostrilMaterial = (j->subs[t.sub].flags & BAKE_NOSTRILS) != 0;
                e->lips = (j->subs[t.sub].flags & BAKE_LIPS) != 0;
                gEntries[std::make_pair(j->indexBuffer, (UINT)j->gpuStart[t.sub])] = std::move(e);
                r.textures++;
            }
        }
        if(r.state==1 && !j->mouth.maps.empty()) {
            auto mouth=CreateMouthGpu(gDevice,j->mouth);
            if(!mouth) { r.state=2; r.error="cannot create mouth AO rig"; }
            else for(const MouthMap &map:j->mouth.maps) {
                auto found=gEntries.find(std::make_pair(j->indexBuffer,(UINT)j->gpuStart[map.sub]));
                if(found==gEntries.end())continue;
                Entry &e=*found->second;
                D3D11_TEXTURE2D_DESC td={};td.Width=map.width;td.Height=map.height;td.MipLevels=1;td.ArraySize=1;
                td.Format=DXGI_FORMAT_R32G32B32A32_UINT;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_IMMUTABLE;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA init={map.texels.data(),(UINT)map.width*16,0};ID3D11Texture2D *tex=nullptr;
                HRESULT hr=ID3D11Device_CreateTexture2D(gDevice,&td,&init,&tex);
                if(SUCCEEDED(hr))hr=ID3D11Device_CreateShaderResourceView(gDevice,(ID3D11Resource*)tex,nullptr,&e.mouthMap);
                if(tex)ID3D11Texture2D_Release(tex);
                if(FAILED(hr)){r.state=2;r.error="cannot create mouth lookup";break;}
                e.mouth=mouth;
                if(e.kind==TEX_THICKNESS&&!e.noseCb) {
                    D3D11_BUFFER_DESC bd={};bd.ByteWidth=kSpecFloats*4;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
                    if(FAILED(ID3D11Device_CreateBuffer(gDevice,&bd,nullptr,&e.noseCb))){r.state=2;r.error="cannot create lip cavity constants";break;}
                }
                const float *st=&j->st[4*map.sub];
                for(int a=0;a<2;a++){float sx=st[a]!=0?st[a]:1.f,span=map.hi[a]-map.lo[a];e.mouthUV[a]=1.f/(sx*span);e.mouthUV[a+2]=(-st[a+2]/sx-map.lo[a])/span;}
            }
        }
        // eyes: one rig for the character, one entry per sclera, iris and pupil submesh
        const EyeRig &er = j->out.eyes;
        r.eyes = er.eyes;
        r.eyeError = er.error;
        for (int e = 0; e < 2; e++) {
            r.eyePoints[e] = (int)er.points[e].size(), r.eyeChains[e] = er.chains[e];
            r.eyeWidthMM[e] = er.widthMM[e], r.eyeHeightMM[e] = er.heightMM[e];
        }
        std::shared_ptr<EyeRigGpu> rig;
        if (r.state == 1 && er.eyes == 2 && CreateEyeRig(er, (int)(j->pos.size() / 3), rig, r.eyeError)) {
            for (size_t s = 0; s < j->subs.size(); s++) {
                if (!(j->subs[s].flags & (BAKE_SCLERA | BAKE_IRIS | BAKE_PUPIL))) continue;
                std::unique_ptr<Entry> e(new Entry());
                e->kind = TEX_EYE;
                e->rig = rig;
                e->skin = j->skin;
                e->sub = (int)s;
                D3D11_BUFFER_DESC bd = {};
                bd.ByteWidth = kSpecFloats * 4;
                bd.Usage = D3D11_USAGE_DEFAULT;
                bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                if (FAILED(ID3D11Device_CreateBuffer(gDevice, &bd, NULL, &e->cb))) {
                    r.eyeError = "cannot create an eye constant buffer";
                    break;
                }
                gEntries[std::make_pair(j->indexBuffer, (UINT)j->gpuStart[s])] = std::move(e);
                r.eyeEntries++;
            }
        }
        // the character's PT geometry metadata (rt.h RtReceiver), shared by its entries (skin, nails, teeth, tongue, eyes); skin entries
        // get their own b12 (UpdateEntryConstants)
        r.rtMs = j->rtMs;
        r.rtError = j->rtError;
        if (j->rtBake) r.rtBake = RtBakeSummary(j->rtBake);
        if (r.state == 1 && j->rtBake && RtAvailable()) {
            RtReceiver *raw = RtCreateReceiver(j->rtBake, j->indexBuffer, j->skin);
            j->rtBake = nullptr;  // taken
            if (!raw) r.rtError = "no receiver (rt.reason)";
            else {
                std::shared_ptr<RtReceiver> rtr(raw, RtDestroyReceiver);
                for (size_t s = 0; s < j->subs.size(); s++) {
                    if (!(j->subs[s].flags & BAKE_RT_PART_MASK)) continue;
                    auto key = std::make_pair(j->indexBuffer, (UINT)j->gpuStart[s]);
                    auto found = gEntries.find(key);
                    if (found == gEntries.end()) continue;
                    Entry &e = *found->second;
                    if (e.kind == TEX_THICKNESS && !e.noseCb) {
                        D3D11_BUFFER_DESC bd = {};
                        bd.ByteWidth = kSpecFloats * 4; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                        if (FAILED(ID3D11Device_CreateBuffer(gDevice, &bd, nullptr, &e.noseCb))) { r.rtError = "cannot create the skin constants"; break; }
                    }
                    e.rtr = rtr;
                    e.rtOnly = (j->subs[s].flags & BAKE_RT_ONLY) != 0;
                    e.version = 0;
                    r.rtEntries++;
                }
            }
        }
        CountEntries();
        std::lock_guard<std::mutex> g(gJobLock);
        gJobReports.push_back(r);
        if (gJobReports.size() > 32) gJobReports.erase(gJobReports.begin());
    }
}

// ---- GPU time of the eye camera's opaque pass --------------------------------------------------------------------

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

// ---- render events -------------------------------------------------------------------------------------------------

static void STDMETHODCALLTYPE OnRenderEvent(int id) {
    int op = (id >> 24) & 0xff;
    switch (op) {
    case OP_INSTALL:
        if (!gInstalled && gCtx) gInstalled = HookContext();
        if (gInstalled && gReady) RtInit(gDevice);  // 0.21.0: DXR when the device is D3D11On12 (else the reason, status)
        break;
    case OP_BEGIN:
        if (gInstalled) {
            TimingBegin();
            // RtFrame has committed this camera's route at PreCull. Frame-end
            // synchronization alone is one frame late on renderer transitions.
            if(gReady)SssSetRR(true,gParamsRT.rtDetail,false);
            if (gReady) SssBegin((id & 0xffff) == 1 ? 1 : 2);
        }
        break;
    case OP_END:
        if (gInstalled) {
            if (gReady) SssEnd(gCtx, oPSSetShader);
            TimingEnd();
        }
        break;
    case OP_FRAME: {
        bool changed = false;
        std::vector<ID3D11PixelShader *> retired;
        AcquireSRWLockExclusive(&gLock);
        if (gParamsVersionRT != gParamsVersion) {
            gParamsRT = gParams;
            gParamsVersionRT = gParamsVersion;
            changed = true;
        }
        // RR's state for the PT bridge (rt.h RtSetReconstruction); the raster skin programs write no RR guides.
        gRrActive = SssSetRR(gReady, gParamsRT.rtDetail, false);
        retired.swap(gSpecRetired);
        if (!retired.empty()) {  // the bound shader's replacement again, in case it was one of these
            auto sp = gCurPS ? gSpecPS.find(gCurPS) : gSpecPS.end();
            gCurSpecPS = sp != gSpecPS.end() ? sp->second : NULL;
            auto ss = gCurSpecPS ? gSssPS.find(gCurSpecPS) : gSssPS.end();
            gCurSpecSss = ss != gSssPS.end() && ss->second.sss;
            gCurSpecGloss = ss != gSssPS.end() ? ss->second.gloss : -1;
        }
        ReleaseSRWLockExclusive(&gLock);
        for (ID3D11PixelShader *ps : retired) ID3D11PixelShader_Release(ps);
        if (changed && gReady) UpdateSpecConstants();
        if (changed) SssSetParams(gParamsRT.sssStrength, gParamsRT.sssWidth, gParamsRT.sssTanHalfY);
        if (changed) SssSetTranslucency(0.0f, gParamsRT.transWidth);  // no skin writes the translucency target
        if (gReady) SssFrame();
        if (gReady) ProcessJobs();
        if (gReady) TexQueriesStart();
        if (gReady) GlossFrame();
        if (gReady) SlopeFrame();
        if (gReady) TierFrame();
        gStatsLast = gStatsCur;
        gStatsCur = FrameStats();

        gFrames++;
        PathOverlayFrame();
        break;
    }
    }
}

// 0.22.0: the first camera's PreCull of the frame (RtScene.cs, Graphics.ExecuteCommandBuffer): the frame's records (the
// plugin's ring slot, rt.h RtSceneHeader version 4) to rt.cpp, which records RtCore's frame on the 11on12 queue.
static void STDMETHODCALLTYPE OnRenderEventAndData(int id, void *data) {
    int op = (id >> 24) & 0xff;
    if (op != OP_RT_SCENE || !gInstalled || !gReady) return;
    // 0.22.62: no records = 光追 off this frame (RtScene.PreCull): PT's frame ends and the strands go back to raster.
    if (!data) { if (RtAvailable()) RtFrameOff(); return; }
    const RtSceneHeader *h = (const RtSceneHeader *)data;
    if (h->version != kRtSceneVersion || h->draws < 0) return;
    gStatsCur.rtRecords = h->draws;
    if (!RtAvailable()) return;
    RtSetReconstruction(gRrActive);
    RtSetPathBounces(gParamsRT.pathBounces);
    RtSetPathAuthor(gParamsRT.authorRules,gParamsRT.authorAmbient,gParamsRT.authorContact,gParamsRT.authorAmbientRange,gParamsRT.authorEmitter,gParamsRT.backgroundNear,gParamsRT.backgroundFar);
    RtFrame(gCtx, data, gFrames);
}

// ---- exports -------------------------------------------------------------------------------------------------------

EXPORT int ST_ApiVersion() { return ST_API_VERSION; }
EXPORT int ST_SetPathDepthReadyV1(ID3D11Resource* color,ID3D11Resource* depth) {return SssMarkPathDepth(color,depth)?1:0;}
EXPORT uint64_t ST_PathBufferBytesV1(ID3D11Buffer *buffer) {
    if(!buffer)return 0;
    D3D11_BUFFER_DESC d={};ID3D11Buffer_GetDesc(buffer,&d);return d.ByteWidth;
}

// Device-level setup from any thread: the device and immediate context, the shader-creation hook (before the game
// creates skin shaders) and the transmission shaders. The context hooks go in on the render thread (OP_INSTALL).
EXPORT int ST_Init(void *resource) {
    if (gDevice) return 0;
    if (!resource) {
        SetError("no resource");
        return 1;
    }
    for (int i = 1; i < kSpecProgramCount; i++)
        if (kSpecPrograms[i - 1].hash >= kSpecPrograms[i].hash) {
            SetError("spec_programs.h not sorted");
            return 5;
        }
    std::vector<std::pair<uint64_t, int>> v;
    for (int i = 0; i < kSkinProgramCount; i++) v.push_back(std::make_pair(kSkinPrograms[i].hash, i));
    std::sort(v.begin(), v.end());
    for (auto &e : v) {
        gSortedHashes.push_back(e.first);
        gSortedIndex.push_back(e.second);
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    gQpcToMs = 1e3 / (double)f.QuadPart;
    ID3D11Resource_GetDevice((ID3D11Resource *)resource, &gDevice);
    if (!gDevice) {
        SetError("no device");
        return 2;
    }
    ID3D11Device_GetImmediateContext(gDevice, &gCtx);
    if (!HookDevice()) {
        SetError("cannot patch the device table");
        return 3;
    }
    gReady = CreateObjects();
    if (gReady) {
        std::string err;
        if (!SssInit(gDevice, oCreatePS, err)) SetError("%s", err.c_str());  // the rest works without it
    }
    return gReady ? 0 : 4;
}

EXPORT void *ST_RenderEventFunc() { return (void *)OnRenderEvent; }
EXPORT void *ST_RenderEventAndDataFunc() { return (void *)OnRenderEventAndData; }
EXPORT int ST_RtAvailable() { return RtAvailable() ? 1 : 0; }
// tests only (SimXR A/B, never in a menu): RtCore's layers (rtcore.h SetTestLayers; 0xffffffff = all)
EXPORT void ST_SetRtTest(unsigned layers) { RtSetTest(layers); }
// tests only: the skin's IBL about the highlight's normal (1, the default) or about VaM's W (0, before)
EXPORT void ST_SetEnvSpecNormal(int on) {
    AcquireSRWLockExclusive(&gLock);
    if (gParams.envSpecNormal != (on != 0)) {
        gParams.envSpecNormal = on != 0;
        gParamsVersion++;
    }
    ReleaseSRWLockExclusive(&gLock);
}
// tests only: the skin diffuse's grazing factor (1, the default) or none (0, as 0.22.14)
EXPORT void ST_SetSkinGrazing(int on) {
    AcquireSRWLockExclusive(&gLock);
    if (gParams.skinGrazing != (on != 0)) {
        gParams.skinGrazing = on != 0;
        gParamsVersion++;
    }
    ReleaseSRWLockExclusive(&gLock);
}
// tests only (0.22.20): the vellus sheen's weight (0.15, the default; 0 = as 0.22.19)
EXPORT void ST_SetSkinFuzz(float weight) {
    AcquireSRWLockExclusive(&gLock);
    float w = weight > 0.0f ? weight : 0.0f;
    if (gParams.skinFuzz != w) {
        gParams.skinFuzz = w;
        gParamsVersion++;
    }
    ReleaseSRWLockExclusive(&gLock);
}
// tests only (0.22.20): each light's specular from its disc (1, the default) or from a point (0, as 0.22.19)
EXPORT void ST_SetSpecDisc(int on) {
    AcquireSRWLockExclusive(&gLock);
    if (gParams.specDisc != (on != 0)) {
        gParams.specDisc = on != 0;
        gParamsVersion++;
    }
    ReleaseSRWLockExclusive(&gLock);
}
EXPORT void ST_SetRtDetail(float value) {
    AcquireSRWLockExclusive(&gLock); gParams.rtDetail = std::clamp(value, 0.0f, 1.0f); gParamsVersion++; ReleaseSRWLockExclusive(&gLock);
}
EXPORT void ST_SetPathAuthor(uint32_t rules,float ambient,float contact,float ambientRange,float emitter,float backgroundNear,float backgroundFar) {
    AcquireSRWLockExclusive(&gLock);
    gParams.authorRules=rules;gParams.authorAmbient=ambient;gParams.authorContact=contact;
    gParams.authorAmbientRange=ambientRange;gParams.authorEmitter=emitter;gParams.backgroundNear=backgroundNear;gParams.backgroundFar=backgroundFar;gParamsVersion++;
    ReleaseSRWLockExclusive(&gLock);
}
EXPORT void ST_SetPathBounces(int bounces) {
    AcquireSRWLockExclusive(&gLock); gParams.pathBounces = std::min(std::max(bounces, 0), 3); gParamsVersion++; ReleaseSRWLockExclusive(&gLock);
}
EXPORT int ST_Installed() { return gInstalled ? 1 : 0; }
EXPORT void ST_SetEnabled(int on) { InterlockedExchange(&gEnabled, on ? 1 : 0); }
EXPORT void ST_SetTongueHeight(int skin,int height) { AcquireSRWLockExclusive(&gLock);gParams.tongueHeight[skin]=height!=0;gParamsVersion++;ReleaseSRWLockExclusive(&gLock);}
EXPORT void ST_SetMouth(float ao,float wet) {
    AcquireSRWLockExclusive(&gLock); gParams.mouthOn=ao>=0;gParams.mouthDebug=ao>1;gParams.mouthStrength=std::max(0.f,std::min(1.f,ao));gParams.tongueWet=std::max(0.f,std::min(1.f,wet));gParamsVersion++;ReleaseSRWLockExclusive(&gLock);
}
EXPORT void ST_SetDiag(int on) {
    if (on) {
        AcquireSRWLockExclusive(&gLock);
        gDiagTotal.clear();
        gHookDraws = gHookDrawsCtx = gHookPSSets = 0;
        ReleaseSRWLockExclusive(&gLock);
    }
    InterlockedExchange(&gDiag, on ? 1 : 0);
}

EXPORT void ST_SetParams(float strength, float wrap, float thicknessScale, float maxMM, float depthR, float depthG, float depthB) {
    AcquireSRWLockExclusive(&gLock);
    gParams.strength = strength;
    gParams.wrap = wrap;
    gParams.thicknessScale = thicknessScale;
    gParams.maxMM = maxMM;
    gParams.depth[0] = std::max(0.01f, depthR);
    gParams.depth[1] = std::max(0.01f, depthG);
    gParams.depth[2] = std::max(0.01f, depthB);
    gParamsVersion++;
    ReleaseSRWLockExclusive(&gLock);
}

// Specular replacement: on 0 = VaM's own skin programs; 1 = skin GGX with: the normal map's mip bias for the specular
// normal, how much the normal variance it removes widens the lobes, F0, a roughness scale (perceptual), the two lobes'
// roughness factors and the sharp lobe's weight; 2 (test) = the replacement programs in their mode 0 (VaM's result).
EXPORT void ST_SetSpecular(int on, float bias, float pore, float f0, float rough, float sharp, float wide, float weight) {
    AcquireSRWLockExclusive(&gLock);
    gParams.specOn = on != 0;
    float v[8] = {on == 1 ? 1.0f : 0.0f, std::min(8.0f, std::max(0.0f, bias)), std::min(8.0f, std::max(0.0f, pore)), std::min(1.0f, std::max(0.0f, f0)),
                  std::min(4.0f, std::max(0.1f, rough)), std::min(4.0f, std::max(0.1f, sharp)), std::min(4.0f, std::max(0.1f, wide)),
                  std::min(1.0f, std::max(0.0f, weight))};
    memcpy(gParams.spec, v, sizeof v);
    gParamsVersion++;
    ReleaseSRWLockExclusive(&gLock);
}

// Diffuse replacement in the same programs: on 0 = VaM's own diffuse; 1 = VaM's own factor times each channel's wrap
// ((N.L + a) / (1 + a))^p over green's at VaM's saturated N.L: VaM's facing the light, warmer near the terminator and
// running on past it; 2 (test) = VaM's diffuse rebuilt.
EXPORT void ST_SetRegional(int mode) {
    AcquireSRWLockExclusive(&gLock);
    gParams.regionalMode=std::max(0,std::min(2,mode));
    gParamsVersion++;
    ReleaseSRWLockExclusive(&gLock);
}

EXPORT void ST_SetOverallTranslucency(float amount) {
    AcquireSRWLockExclusive(&gLock);
    gParams.overallTranslucency = std::max(0.0f, std::min(2.0f, amount));
    gParamsVersion++;
    ReleaseSRWLockExclusive(&gLock);
}

EXPORT void ST_SetScatter(int on, float aR, float aG, float aB, float pR, float pG, float pB) {
    AcquireSRWLockExclusive(&gLock);
    gParams.scatOn = on != 0;
    float a[3] = {aR, aG, aB}, p[3] = {pR, pG, pB};
    gParams.scat[0] = on == 1 ? 1.0f : on == 2 ? 2.0f : 0.0f;
    for (int c = 0; c < 3; c++) {
        gParams.scat[4 + c] = std::min(4.0f, std::max(0.0f, a[c]));
        gParams.scat[8 + c] = std::min(16.0f, std::max(0.5f, p[c]));
    }
    gParamsVersion++;
    ReleaseSRWLockExclusive(&gLock);
}

// Skin detail in the same programs (0.11.0): refA2 > 0 gives every skin the lobes' alpha^2 refA2 at its mean gloss, its
// gloss map's own variation kept within lo..hi times; intensity 1 = the spec map with _SpecOffset over its own mean,
// times saturate(4 _SpecInt); diffuseMM > 0 filters the diffuse normal to that width; ambient 1 = the ambient (SH) on
// the diffuse normal instead of VaM's specular one; cavityMM > 0 filters the spec map of the highlight's cavity;
// tiers 1 (0.13.0): each skin's roughness from its tier row (ST_SetTiers; the row each gloss map has, ST_SetGlossTier)
// and its gloss map's own p50 / p90 instead (0.12.0's one state = the face's tier 3 row).
EXPORT void ST_SetSkinDetail(float refA2, float lo, float hi, int intensity, float diffuseMM, int ambient, float cavityMM, int tiers) {
    AcquireSRWLockExclusive(&gLock);
    float v[8] = {std::min(1.0f, std::max(0.0f, refA2)), std::min(1.0f, std::max(0.0f, lo)), std::min(64.0f, std::max(1.0f, hi)),
                  intensity ? 1.0f : 0.0f, std::min(20.0f, std::max(0.0f, diffuseMM)) * 0.001f, ambient ? 1.0f : 0.0f,
                  std::min(20.0f, std::max(0.0f, cavityMM)) * 0.001f, tiers ? 1.0f : 0.0f};
    memcpy(gParams.skin, v, sizeof v);
    gParams.detailOn = v[0] > 0.0f || v[3] != 0.0f || v[4] > 0.0f || v[5] != 0.0f || v[6] > 0.0f;
    gParamsVersion++;
    ReleaseSRWLockExclusive(&gLock);
}

// Skin tiers (0.13.0): n rows (at most kTierRows) of 8 floats: alpha^2 at the gloss map's p50 (> 0), R (alpha at p50 over
// alpha at p90, 1 .. 4), alpha^2 floor and ceiling, the gloss map's mip bias, the highlight's gain, the least p90 - p50
// the spread is divided by (0.01 .. 0.5), 0; and the row of gloss maps without one of their own.
// one row as the shader takes it (under gLock): the 8 floats as above; c = 0.18.0's (skin p50, p90 - p50, cavity divisor,
// 1 = there) or NULL (0: the draw's t18 stats, the spec map's last mip, a gain of 0 = 1)
static void TierRowSet(int r, const float *a, const float *c) {
    float *o = gTierRows + kTierFloats * r;
    o[0] = std::min(1.0f, std::max(0.001f, a[0]));
    o[1] = -2.0f * log2f(std::min(4.0f, std::max(1.0f, a[1])));
    o[2] = std::min(1.0f, std::max(0.0f, a[2]));
    o[3] = std::min(1.0f, std::max(o[2], a[3]));
    o[4] = std::min(4.0f, std::max(0.0f, a[4]));
    o[5] = std::min(4.0f, std::max(0.0f, a[5]));
    o[6] = std::min(0.5f, std::max(0.01f, a[6]));
    o[7] = 0.0f;
    bool own = c && c[3] > 0.0f;
    o[8] = own ? std::min(1.0f, std::max(0.0f, c[0])) : 0.0f;
    o[9] = own ? std::min(1.0f, std::max(0.0f, c[1])) : 0.0f;
    o[10] = own ? std::min(3.0f, std::max(0.15f, c[2])) : 0.0f;
    o[11] = own ? 1.0f : 0.0f;
    gTierRowVer[r]++;
}

// Skin tiers (0.13.0): n rows (at most 8) of 8 floats: alpha^2 at the gloss map's p50 (> 0), R (alpha at p50 over
// alpha at p90, 1 .. 4), alpha^2 floor and ceiling, the gloss map's mip bias, the highlight's gain, the least p90 - p50
// the spread is divided by (0.01 .. 0.5), 0; and the row of gloss maps without one of their own.
EXPORT void ST_SetTiers(const float *v, int n, int defaultRow) {
    if (!v) return;
    n = std::min(std::max(n, 0), 8);
    AcquireSRWLockExclusive(&gLock);
    for (int r = 0; r < n; r++) TierRowSet(r, v + 8 * r, NULL);
    gTierDefault = std::min(std::max(defaultRow, 0), kTierRows - 1);
    ReleaseSRWLockExclusive(&gLock);
}

// One gloss map's own row (0.18.0; rows 8 .. 255): 12 floats, the 8 of ST_SetTiers then the skin texels' p50,
// p90 - p50, the cavity's divisor (3 x saturate(spec mean + _SpecOffset)) and 1 (0 = not known: the row as ST_SetTiers',
// with t18's stats). The gain is then taken as it is (0 = no highlight). From the next frame on.
EXPORT void ST_SetTierRow(int row, const float *v) {
    if (!v || row < 8 || row >= kTierRows) return;
    AcquireSRWLockExclusive(&gLock);
    TierRowSet(row, v, v + 8);
    ReleaseSRWLockExclusive(&gLock);
}

// The tier row of a gloss map (a texture's native pointer; -1 = none, the default row), from the next frame on.
EXPORT void ST_SetGlossTier(void *tex, int row) {
    if (!tex) return;
    ID3D11Resource *r = (ID3D11Resource *)tex;
    ID3D11Resource_AddRef(r);
    TierSet t = {r, row >= 0 && row < kTierRows ? row : -1};
    AcquireSRWLockExclusive(&gLock);
    gTierSets.push_back(t);
    ReleaseSRWLockExclusive(&gLock);
}

// A texture's p10 / p50 / p90 / mean (its red channel at its <= 256 px mip), read on the render thread a frame or two
// later: the id to ask ST_TexStatsResult with (0 = no texture).
EXPORT int ST_TexStats(void *tex) {
    if (!tex || !gDevice) return 0;
    ID3D11Resource *r = (ID3D11Resource *)tex;
    ID3D11Resource_AddRef(r);
    AcquireSRWLockExclusive(&gLock);
    int id = gTexQueryNext++;
    TexQuery q = {id, r, 0};
    gTexQueries.push_back(q);
    ReleaseSRWLockExclusive(&gLock);
    return id;
}

// The same over the texels a UV coverage mask holds (0.18.0): mw x mh bytes, rows bottom up as the texture's (Unity's UV
// v), nonzero = counted; failed (-1) when it holds fewer than 16 texels at that mip.
EXPORT int ST_TexStatsMasked(void *tex, const uint8_t *mask, int mw, int mh) {
    if (!tex || !gDevice || !mask || mw < 1 || mh < 1 || mw > 4096 || mh > 4096) return 0;
    ID3D11Resource *r = (ID3D11Resource *)tex;
    ID3D11Resource_AddRef(r);
    TexQuery q = {0, r, 0};
    q.mask.assign(mask, mask + (size_t)mw * mh);
    q.mw = mw, q.mh = mh;
    AcquireSRWLockExclusive(&gLock);
    int id = gTexQueryNext++;
    q.id = id;
    gTexQueries.push_back(q);
    ReleaseSRWLockExclusive(&gLock);
    return id;
}

// A normal map's pore-scale slope (RMS of its texels' tangent-space x, y against their aligned 8 x 8 block's mean, at its
// most detailed mip), from the compute shader a frame or two later: ST_TexStatsResult's mean (0 = no texture).
EXPORT int ST_NormalSlope(void *tex) {
    if (!tex || !gDevice) return 0;
    ID3D11Resource *r = (ID3D11Resource *)tex;
    ID3D11Resource_AddRef(r);
    AcquireSRWLockExclusive(&gLock);
    int id = gTexQueryNext++;
    TexQuery q = {id, r, 1};
    gTexQueries.push_back(q);
    ReleaseSRWLockExclusive(&gLock);
    return id;
}

// 1 = read (out: p10, p50, p90, mean), 0 = not yet, -1 = failed or not known (any more)
EXPORT int ST_TexStatsResult(int id, float *out) {
    int state = -1;
    AcquireSRWLockShared(&gLock);
    for (auto &q : gTexQueries)
        if (q.id == id) state = 0;
    for (auto &t : gTexResults)
        if (t.id == id) {
            state = t.state;
            if (out && state == 1) out[0] = t.p10, out[1] = t.p50, out[2] = t.p90, out[3] = t.mean;
        }
    if (state < 0 && id >= gTexQueryNext - 512 && id < gTexQueryNext) {
        bool answered = false;
        for (auto &t : gTexResults) answered |= t.id == id;
        if (!answered) state = 0;  // being copied
    }
    ReleaseSRWLockShared(&gLock);
    return state;
}

EXPORT void ST_SetShadow(float exitBias, float kernelRadius, float kernelFalloff) {
    AcquireSRWLockExclusive(&gLock);
    gParams.exitBias = exitBias;
    gParams.kernelRadius = std::max(0.0f, kernelRadius);
    gParams.kernelFalloff = std::max(0.0f, kernelFalloff);
    gParamsVersion++;
    ReleaseSRWLockExclusive(&gLock);
}

// Teeth and tongue: on 0 = VaM's own program; 1 = the teeth with their own Subsurface Color (1 = 1x diffuse facing the
// light, VaM's teeth 0 = 2x) times a gain, both with the mouth occlusion: darker toward the back of the mouth (strength,
// where it starts along the teeth: 0 front .. 1 last molars) and toward the gum end of a tooth (strength, power).
EXPORT void ST_SetTeeth(int on, float subsurface, float gain, float backStrength, float backStart, float gumStrength, float gumPower) {
    AcquireSRWLockExclusive(&gLock);
    gParams.teethOn = on != 0;
    gParams.teethS = std::min(2.0f, std::max(0.0f, subsurface));
    gParams.teethGain = std::min(4.0f, std::max(0.0f, gain));
    gParams.teethBack = std::min(1.0f, std::max(0.0f, backStrength));
    gParams.teethBackStart = std::min(0.95f, std::max(0.0f, backStart));
    gParams.teethGum = std::min(1.0f, std::max(0.0f, gumStrength));
    gParams.teethGumPower = std::min(8.0f, std::max(0.25f, gumPower));
    gParamsVersion++;
    ReleaseSRWLockExclusive(&gLock);
}

// Eyes: the eyelid occlusion (on, upper lid darkening and width, lower lid darkening and width; widths in eyeball
// radii) and the cornea catchlight (on, strength, GGX alpha, F0, the cornea's radius in eyeball radii); both off =
// VaM's own eye draws.
EXPORT void ST_SetEyes(int occOn, float upper, float upperWidth, float lower, float lowerWidth, int catchOn, float strength, float alpha,
                       float f0, float cornea) {
    AcquireSRWLockExclusive(&gLock);
    Params &p = gParams;
    p.eyeOccOn = occOn != 0;
    p.eyeOcc[0] = std::min(1.0f, std::max(0.0f, upper));
    p.eyeOcc[1] = std::min(1.0f, std::max(0.005f, upperWidth));
    p.eyeOcc[2] = std::min(1.0f, std::max(0.0f, lower));
    p.eyeOcc[3] = std::min(1.0f, std::max(0.005f, lowerWidth));
    p.eyeCatchOn = catchOn != 0;
    p.eyeCatch[0] = std::min(8.0f, std::max(0.0f, strength));
    p.eyeCatch[1] = std::min(1.0f, std::max(0.01f, alpha));
    p.eyeCatch[2] = std::min(1.0f, std::max(0.0f, f0));
    p.eyeCatch[3] = std::min(1.0f, std::max(0.3f, cornea));
    gParamsVersion++;
    ReleaseSRWLockExclusive(&gLock);
}

// Per character (the plugin's id): which eye effects it gets, bit 0 the occlusion, bit 1 the catchlight (3, as for a
// character never named here, = both). For characters wearing eye overlay items that already do one of them.
EXPORT void ST_SetEyeSkin(int skin, int allow) {
    AcquireSRWLockExclusive(&gLock);
    allow &= 3;
    auto it = gParams.eyeAllow.find(skin);
    int old = it != gParams.eyeAllow.end() ? it->second : 3;
    if (allow != old) {
        if (allow == 3) gParams.eyeAllow.erase(skin);
        else gParams.eyeAllow[skin] = allow;
        gParamsVersion++;
    }
    ReleaseSRWLockExclusive(&gLock);
}

// Subsurface scattering: strength 0..1 (0 = off), the skin profile's scale (1 = measured skin).
EXPORT void ST_SetSubsurface(float strength, float width) {
    AcquireSRWLockExclusive(&gLock);
    gParams.sssStrength = std::min(1.0f, std::max(0.0f, strength));
    gParams.sssWidth = std::min(3.0f, std::max(0.25f, width));
    gParamsVersion++;
    ReleaseSRWLockExclusive(&gLock);
}

// 0.20.0 translucency (female skin with the blocker-distance shadows; its amount is [Diffuse] OverallScattering):
// strength of its blurred glow (0 = off, 1 = the candidate), its blur's scale (1 = 10 + 22 mm), its colour (0 warm gray
// .. 1 the angular response's own).
EXPORT void ST_SetTranslucency(float strength, float width, float tint) {
    AcquireSRWLockExclusive(&gLock);
    gParams.transStrength = std::min(4.0f, std::max(0.0f, strength));
    gParams.transWidth = std::min(3.0f, std::max(0.25f, width));
    gParams.transTint = std::min(1.0f, std::max(0.0f, tint));
    gParamsVersion++;
    ReleaseSRWLockExclusive(&gLock);
}

// The eye camera's tan(half vertical field of view), for the millimetres per pixel (the plugin, every frame).
EXPORT void ST_SetSubsurfaceView(float tanHalfY) {
    AcquireSRWLockExclusive(&gLock);
    if (fabsf(tanHalfY - gParams.sssTanHalfY) > 1e-4f) {
        gParams.sssTanHalfY = std::min(4.0f, std::max(0.1f, tanHalfY));
        gParamsVersion++;
    }
    ReleaseSRWLockExclusive(&gLock);
}

// Sideways spread of the thickness bake (BakeInput::smoothMM) for the bakes started from now on.
EXPORT void ST_SetNose(float amount) {
    AcquireSRWLockExclusive(&gLock);
    gParams.noseStrength = std::min(1.0f, std::max(0.0f, amount));
    gParamsVersion++;
    ReleaseSRWLockExclusive(&gLock);
}

EXPORT void ST_SetNoseFilter(int skin, float faceFilter) {
    AcquireSRWLockExclusive(&gLock);
    if (faceFilter < 0.0f) gParams.noseFaceFilter.erase(skin);
    else gParams.noseFaceFilter[skin] = std::min(1.0f, std::max(0.0f, faceFilter));
    gParamsVersion++;
    ReleaseSRWLockExclusive(&gLock);
}

EXPORT void ST_SetBake(float smoothMM) {
    AcquireSRWLockExclusive(&gLock);
    gParams.bakeSmoothMM = std::min(20.0f, std::max(0.0f, smoothMM));
    ReleaseSRWLockExclusive(&gLock);
}

// Copies the mesh and starts the bake on a worker thread; the entries replace this index buffer's old ones when it
// is done (render thread). The id, or a negative error.
EXPORT int ST_Bake(const StBakeDesc *d) {
    if (!d || d->version < 1 || d->version > 2) return -1;
    if (!gReady) return -2;
    std::unique_ptr<Job> j(new Job());
    int nv = (int)d->vertexCount, ni = (int)d->indexCount, ns = (int)d->subCount;
    if (nv <= 0 || ni <= 0 || ns <= 0 || !d->pos || !d->nrm || !d->uv || !d->indices) return -3;
    j->indexBuffer = d->indexBuffer;
    j->skin = (int)d->skin;
    j->pos.assign(d->pos, d->pos + 3 * nv);
    j->nrm.assign(d->nrm, d->nrm + 3 * nv);
    j->uv.assign(d->uv, d->uv + 2 * nv);
    if(d->version == 2 && d->regional) {
        for(int i=0;i<nv;i++) if(!std::isfinite(d->regional[i]) || d->regional[i]<.5f || d->regional[i]>1.08f) return -5;
        j->regional.assign(d->regional,d->regional+nv);
    }
    j->indices.assign(d->indices, d->indices + ni);
    j->st.assign(d->subST, d->subST + 4 * ns);
    j->gpuStart.assign(d->subGpuStart, d->subGpuStart + ns);
    AcquireSRWLockShared(&gLock);
    j->smoothMM = gParams.bakeSmoothMM;
    ReleaseSRWLockShared(&gLock);
    for (int s = 0; s < ns; s++) {
        BakeSub b;
        b.indexStart = d->subIndexStart[s];
        b.indexCount = d->subIndexCount[s];
        b.flags = d->subFlags[s];
        if (b.indexStart < 0 || b.indexCount < 0 || b.indexStart + b.indexCount > ni) return -4;
        j->subs.push_back(b);
    }
    Job *raw = j.get();
    std::lock_guard<std::mutex> g(gJobLock);
    j->id = gNextJob++;
    j->startMs = NowMs();
    j->worker = std::thread(RunJob, raw);
    int id = j->id;
    gJobs.push_back(std::move(j));
    return id;
}

EXPORT void ST_Unregister(void *indexBuffer) {
    std::lock_guard<std::mutex> g(gJobLock);
    gUnregister.push_back(indexBuffer);
}

EXPORT void ST_ResetTiming() {
    AcquireSRWLockExclusive(&gLock);
    gGpuSamples = 0;
    gGpuSumMs = gGpuMinMs = gGpuMaxMs = 0;
    ReleaseSRWLockExclusive(&gLock);
    SssResetTiming();
    RtResetTiming();
}

EXPORT int ST_LastError(char *buf, int cap) {
    AcquireSRWLockShared(&gLock);
    int n = (int)strlen(gError);
    if (buf && cap > 0) {
        int k = std::min(n, cap - 1);
        memcpy(buf, gError, k);
        buf[k] = 0;
    }
    ReleaseSRWLockShared(&gLock);
    return n;
}

// JSON status; returns the length, or minus the length needed.
// MSVC prints non-finite doubles as nan / -nan(ind) / inf, which are not JSON (a 0.22.0 SimXR run stopped on one):
// such a number (right after ':', ',' or '[') becomes null
static void JsonFinite(std::string &s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (!o.empty() && (o.back() == ':' || o.back() == ',' || o.back() == '[') && (c == '-' || c == 'n' || c == 'i')) {
            size_t j = i + (c == '-' ? 1 : 0);
            if (s.compare(j, 3, "nan") == 0 || s.compare(j, 3, "inf") == 0) {
                j += 3;
                while (j < s.size() && (isalpha((unsigned char)s[j]) || s[j] == '(' || s[j] == ')')) j++;
                o += "null";
                i = j;
                continue;
            }
        }
        o += c;
        i++;
    }
    s.swap(o);
}

EXPORT int ST_Read(char *buf, int cap) {
    std::string s;
    char t[4096];
    AcquireSRWLockShared(&gLock);
    snprintf(t, sizeof t, "{\"reconstruction\":{\"requested\":%d,\"active\":%d,\"detail\":%.3f},",
             gRrActive ? 1 : 0, gRrActive ? 1 : 0, gParams.rtDetail);
    s += t;
    snprintf(t,sizeof t,"\"path_raster\":{\"depth_only\":%llu,\"skipped\":%llu,\"unregistered\":%llu,\"pt_depth_reused\":%llu},",
        (unsigned long long)gPathDepthDraws,(unsigned long long)gPathSkippedDraws,(unsigned long long)gPathUnknownDraws,(unsigned long long)gPathOwnedDepthSkips);
    s += t;
    uint64_t overlayBytes=0;unsigned overlayLayers=0;
    for(const auto& layer:gPathOverlays)if(layer.source){overlayBytes+=layer.bytes;overlayLayers++;}
    size_t uiTextures;AcquireSRWLockShared(&gUiTextureLock);uiTextures=gUiTextures.size();ReleaseSRWLockShared(&gUiTextureLock);
    snprintf(t,sizeof t,"\"path_overlay\":{\"layers\":%u,\"bytes\":%llu,\"draws\":%llu,\"ui_draws\":%llu,\"canvas_draws\":%llu,\"canvas_elsewhere\":%llu,\"grab_draws\":%llu,\"ui_textures\":%zu,\"canvas_off\":%d,\"late_draws\":%llu,\"late_off\":%d,\"effect_draws\":%llu,\"failures\":%llu},",
        overlayLayers,(unsigned long long)overlayBytes,(unsigned long long)gOverlayDraws,(unsigned long long)gOverlayUiDraws,(unsigned long long)gOverlayCanvasDraws,(unsigned long long)gOverlayCanvasElsewhere,(unsigned long long)gOverlayGrabDraws,uiTextures,gOverlayCanvasOff?1:0,(unsigned long long)gOverlayLateDraws,gOverlayLateOff?1:0,(unsigned long long)gOverlayEffectDraws,(unsigned long long)gOverlayFailures);
    s+=t;
    { size_t observed;AcquireSRWLockShared(&gObservedLock);observed=gObserved.size();ReleaseSRWLockShared(&gObservedLock);
      snprintf(t,sizeof t,"\"path_author\":{\"registered_effect_draws\":%llu,\"unregistered_effect_draws\":%llu,\"observed_materials\":%zu,\"observed_draws\":%llu,\"test_off\":%d},",
        (unsigned long long)gOverlayRegisteredEffectDraws,(unsigned long long)gOverlayAuthorDraws,observed,(unsigned long long)gObservedDraws,gOverlayAuthorOff?1:0);
      s+=t; }
    snprintf(t, sizeof t,
             "\"installed\":%d,\"ready\":%d,\"enabled\":%d,\"programs\":%d,\"variants\":%d,\"created_ps\":%ld,\"created_skin_ps\":%ld,"
             "\"live_skin_ps\":%zu,\"frames\":%d,\"entries\":%ld,\"last_frame\":{\"skin_draws\":%d,\"extra_draws\":%d,\"unregistered\":%d,\"shadow_mismatch\":%d},"
             "\"gpu_opaque\":{\"samples\":%d,\"mean_ms\":%.4f,\"min_ms\":%.4f,\"max_ms\":%.4f},"
             "\"params\":{\"strength\":%.3f,\"wrap\":%.3f,\"thickness_scale\":%.3f,\"max_mm\":%.2f,\"sigma_mm\":[%.3f,%.3f,%.3f],\"inset_mm\":%.2f,"
             "\"kernel_mm\":%.2f,\"falloff_mm\":%.2f,\"bake_smooth_mm\":%.2f},",
             gInstalled ? 1 : 0, gReady ? 1 : 0, (int)gEnabled, kSkinProgramCount, kSkinVariantCount, gCreatedPS, gCreatedSkinPS, gSkinPS.size(),
             gFrames, gEntryCount, gStatsLast.skinDraws, gStatsLast.extraDraws, gStatsLast.unregistered, gStatsLast.shadowMismatch, gGpuSamples,
             gGpuSamples ? gGpuSumMs / gGpuSamples : 0.0, gGpuMinMs, gGpuMaxMs, gParams.strength, gParams.wrap, gParams.thicknessScale,
             gParams.maxMM, gParams.depth[0], gParams.depth[1], gParams.depth[2], gParams.exitBias * 1000, gParams.kernelRadius * 1000,
             gParams.kernelFalloff * 1000, gParams.bakeSmoothMM);
    s += t;
    snprintf(t, sizeof t,
             "\"specular\":{\"on\":%d,\"mip_bias\":%.3f,\"pore\":%.3f,\"f0\":%.4f,\"roughness\":%.3f,\"sharp\":%.3f,\"wide\":%.3f,"
             "\"sharp_weight\":%.3f,\"programs\":%d,\"created\":%ld,\"create_failed\":%ld,\"live\":%zu,\"last_frame_draws\":%d},",
             gParams.specOn ? 1 : 0, gParams.spec[1], gParams.spec[2], gParams.spec[3], gParams.spec[4], gParams.spec[5], gParams.spec[6],
             gParams.spec[7], kSpecProgramCount, gCreatedSpecPS, gSpecCreateFailed, gSpecPS.size(), gStatsLast.specDraws);
    s += t;
    snprintf(t, sizeof t, "\"overall_translucency\":%.4f,\"regional_mode\":%d,\"regional_entries\":%ld,\"surface_only_entries\":%ld,\"regional_texture_bytes\":%ld,", gParams.overallTranslucency,gParams.regionalMode,gRegionalCount,gSurfaceOnlyCount,gRegionalBytes);
    s += t;
    snprintf(t, sizeof t, "\"scatter\":{\"on\":%d,\"mode\":%d,\"a\":[%.4f,%.4f,%.4f],\"p\":[%.4f,%.4f,%.4f]},", gParams.scatOn ? 1 : 0,
             (int)gParams.scat[0], gParams.scat[4], gParams.scat[5], gParams.scat[6], gParams.scat[8], gParams.scat[9], gParams.scat[10]);
    s += t;
    snprintf(t, sizeof t, "\"detail\":{\"on\":%d,\"ref_a2\":%.4f,\"clamp\":[%.3f,%.3f],\"intensity\":%d,\"diffuse_mm\":%.3f,\"ambient\":%d,\"cavity_mm\":%.3f,\"tiers\":%d,\"gloss_read\":%d,\"gloss_failed\":%d,",
             gParams.detailOn ? 1 : 0, gParams.skin[0], gParams.skin[1], gParams.skin[2], (int)gParams.skin[3], gParams.skin[4] * 1000.0f,
             (int)gParams.skin[5], gParams.skin[6] * 1000.0f, gParams.skin[7] != 0.0f ? 1 : 0, gGlossRead, gGlossFailed);
    s += t;
    // the tier rows as the shader takes them (alpha^2, -2 log2 R, floor, ceiling, mip bias, gain, least spread), the draws
    // per row last frame
    snprintf(t, sizeof t, "\"tier_default\":%d,\"tier_sets\":%d,\"tex_queries\":%d,\"tier_rows\":{", gTierDefault, gTierSetsDone, gTexQueryNext - 1);
    s += t;
    // the rows set (key = row): as the shader takes them, then the draws last frame
    bool firstRow = true;
    for (int r = 0; r < kTierRows; r++) {
        if (!gTierRowVer[r]) continue;
        const float *o = gTierRows + kTierFloats * r;
        snprintf(t, sizeof t, "%s\"%d\":[%.4f,%.4f,%.4f,%.4f,%.3f,%.4f,%.3f,%.4f,%.4f,%.4f,%.0f,%d]", firstRow ? "" : ",", r, o[0], o[1], o[2],
                 o[3], o[4], o[5], o[6], o[8], o[9], o[10], o[11], gStatsLast.tierDraws[r]);
        s += t;
        firstRow = false;
    }
    s += "},\"gloss_maps\":[";
    for (size_t i = 0; i < gGlossSeen.size(); i++) {
        const GlossSeen &g = gGlossSeen[i];
        snprintf(t, sizeof t, "%s{\"size\":[%u,%u],\"format\":%d,\"ok\":%d,\"p\":[%.4f,%.4f,%.4f]}", i ? "," : "", g.w, g.h, (int)g.view,
                 g.ok ? 1 : 0, g.p10, g.p50, g.p90);
        s += t;
    }
    s += "]},";
    s += "\"subsurface\":" + SssStatus() + ",";
    snprintf(t,sizeof t,"\"mouth\":{\"on\":%d,\"ao\":%.3f,\"wet\":%.3f,\"gathers\":%d,\"fails\":%d},",gParams.mouthOn?1:0,gParams.mouthStrength,gParams.tongueWet,gStatsLast.mouthGathers,gStatsLast.mouthFails);
    s += t;
    {
        // 0.22.0: the host (rt.cpp) and RtCore's own numbers
        RtHostStatus rs = RtGetStatus();
        rtc::Status cs = rtc::GetStatus();
        auto clean = [](char *p) { for (; *p; p++) if (*p == '"' || *p == '\\' || (unsigned char)*p < 32) *p = '\''; };
        clean(rs.reason), clean(rs.lastError), clean(rs.bakes), clean(rs.alphaUv), clean(cs.reason), clean(cs.lastError);
        char tr[4096];
        snprintf(tr, sizeof tr,
                 "\"rt\":{\"available\":%d,\"reason\":\"%s\",\"last_error\":\"%s\",\"frames\":%d,\"failures\":%d,\"records\":%d,"
                 "\"owners\":%d,\"geometries\":%d,\"receivers\":%d,\"lights\":%d,\"materials\":%d,"
                 "\"uv_owners\":%d,\"no_occlude_owners\":%d,\"alpha_uv\":\"%s\","
                 "\"path_tracing\":{\"active\":%d,\"calls\":%llu,\"materials\":%d,\"textures\":%d,\"hair_segments\":%u,\"hair_bytes\":%llu,\"emitters\":%u,\"lighting_bytes\":%llu,\"ser\":%u,\"diffuse_bounces\":%d,\"eye_rigs\":%u,\"jitter\":[%.7g,%.7g]},"
                 "\"index_copies\":%d,\"copied_bytes\":%llu,\"cpu_ms\":%.4f,\"wait_ms\":%.4f,"
                 "\"list_gpu_ms\":%.4f,\"samples\":%d,\"bake\":\"%s\","
                 "\"core\":{\"ok\":%d,\"reason\":\"%s\",\"last_error\":\"%s\",\"frames\":%llu,\"failures\":%llu,\"geometries\":%u,"
                 "\"triangles\":%llu,\"tessellated\":%llu,"
                 "\"omm_pending\":%u,\"omm_pt\":{\"uniform\":%u,\"baked\":%u,\"triangles\":%llu},\"descriptors\":{\"static\":%u,\"frame\":%u},"
                 "\"gpu\":{\"samples\":%d,\"mean_ms\":%.4f,\"max_ms\":%.4f,\"build_ms\":%.4f},\"record_cpu_ms\":%.4f}},",
                 rs.available, rs.reason, rs.lastError, rs.frames, rs.failures, rs.records, rs.owners, rs.geometries,
                 rs.receivers, rs.lights, rs.materials, rs.uvOwners,
                 rs.noOccludeOwners, rs.alphaUv, rs.pathTracing, (unsigned long long)rs.pathCalls, rs.pathMaterials, rs.pathTextures, rs.pathHairSegments,(unsigned long long)rs.pathHairBytes,rs.pathEmitters,(unsigned long long)rs.pathLightingBytes,rs.pathSer,gParams.pathBounces,rs.pathEyeRigs,rs.pathJitter[0],rs.pathJitter[1],rs.indexCopies,
                 (unsigned long long)rs.copiedBytes, rs.cpuMs, rs.waitMs, rs.gpuMs, rs.samples, rs.bakes,
                 cs.ok, cs.reason, cs.lastError, (unsigned long long)cs.frames,
                 (unsigned long long)cs.failures, cs.geometries,
                 (unsigned long long)cs.triangles, (unsigned long long)cs.tessellatedTriangles, cs.ommPending,
                 cs.pathOmmUniform, cs.pathOmmBaked, (unsigned long long)cs.pathOmmTriangles, cs.staticDescriptors, cs.frameDescriptors,
                 cs.samples, cs.gpuMs, cs.gpuMaxMs, cs.buildMs, cs.recordCpuMs);
        s += tr;
    }
    snprintf(t, sizeof t, "\"subsurface_draws\":%d,\"subsurface_params\":{\"strength\":%.3f,\"width\":%.3f,\"tan_half_y\":%.4f},",
             gStatsLast.sssDraws, gParams.sssStrength, gParams.sssWidth, gParams.sssTanHalfY);
    s += t;
    snprintf(t, sizeof t,
             "\"teeth\":{\"on\":%d,\"subsurface\":%.3f,\"gain\":%.3f,\"back\":[%.3f,%.3f],\"gum\":[%.3f,%.3f],\"teeth_entries\":%ld,"
             "\"teeth_draws\":%d},",
             gParams.teethOn ? 1 : 0, gParams.teethS, gParams.teethGain, gParams.teethBack, gParams.teethBackStart, gParams.teethGum,
             gParams.teethGumPower, gTeethEntryCount, gStatsLast.teethDraws);
    s += t;
    snprintf(t, sizeof t,
             "\"eyes\":{\"occlusion\":%d,\"upper\":[%.3f,%.3f],\"lower\":[%.3f,%.3f],\"catchlight\":%d,\"catch\":[%.3f,%.3f,%.3f,%.3f],"
             "\"eye_entries\":%ld,\"eye_draws\":%d,\"gathers\":%d,\"gather_fails\":%d,\"allow\":[",
             gParams.eyeOccOn ? 1 : 0, gParams.eyeOcc[0], gParams.eyeOcc[1], gParams.eyeOcc[2], gParams.eyeOcc[3], gParams.eyeCatchOn ? 1 : 0,
             gParams.eyeCatch[0], gParams.eyeCatch[1], gParams.eyeCatch[2], gParams.eyeCatch[3], gEyeEntryCount, gStatsLast.eyeDraws,
             gStatsLast.eyeGathers, gStatsLast.eyeGatherFails);
    s += t;
    for (auto it = gParams.eyeAllow.begin(); it != gParams.eyeAllow.end(); ++it) {
        snprintf(t, sizeof t, "%s[%d,%d]", it == gParams.eyeAllow.begin() ? "" : ",", it->first, it->second);
        s += t;
    }
    s += "]},";
    // diagnostics since ST_SetDiag(1): indexed draws seen by the hooks, and the pixel shaders (first 8 bytes of SHA-1)
    // that drew registered skin meshes
    snprintf(t, sizeof t, "\"diag_hooks\":{\"draws\":%lld,\"draws_ctx\":%lld,\"ps_sets\":%lld", (long long)gHookDraws,
             (long long)gHookDrawsCtx, (long long)gHookPSSets);
    s += t;
    if (gCtx && gInstalled) {
        void **live = (void **)gCtx->lpVtbl;
        snprintf(t, sizeof t, ",\"table_same\":%d,\"template_in_d3d11\":%d,\"live_ps\":\"%s\",\"live_draw\":\"%s\",\"template_draw\":\"%s\"",
                 live == gLiveAtInstall ? 1 : 0, gTemplate != gLiveAtInstall ? 1 : 0, ModuleOf(live[CSLOT(PSSetShader)]).c_str(),
                 ModuleOf(live[CSLOT(DrawIndexed)]).c_str(), ModuleOf(gTemplate[CSLOT(DrawIndexed)]).c_str());
        s += t;
    }
    s += "},";
    s += "\"diag_skin_ps\":[";
    bool first = true;
    for (auto &d : gDiagTotal) {
        snprintf(t, sizeof t, "%s[\"%016llx\",%d,%d,%d]", first ? "" : ",", (unsigned long long)d.first.hash, d.first.skin, d.first.sub,
                 d.second);
        s += t;
        first = false;
    }
    s += "],";
    ReleaseSRWLockShared(&gLock);
    s += "\"bakes\":[";
    {
        std::lock_guard<std::mutex> g(gJobLock);
        for (size_t i = 0; i < gJobReports.size(); i++) {
            const JobReport &r = gJobReports[i];
            snprintf(t, sizeof t,
                     "%s{\"id\":%d,\"skin\":%d,\"state\":%d,\"textures\":%d,\"threads\":%d,\"ms\":%.1f,\"rays_ms\":%.1f,\"error\":\"%s\","
                     "\"teeth\":{\"parts\":%d,\"upper\":%d,\"lower\":%d,\"front_mm\":%.1f,\"depth_mm\":%.1f,\"forward\":[%.3f,%.3f,%.3f]},",
                     i ? "," : "", r.id, r.skin, r.state, r.textures, r.threads, r.ms, r.msRays, r.error.c_str(), r.teeth.teeth, r.teeth.upper,
                     r.teeth.lower, r.teeth.frontMM, r.teeth.depthMM, r.teeth.forward[0], r.teeth.forward[1], r.teeth.forward[2]);
            s += t;
            snprintf(t, sizeof t,
                     "\"eyes\":{\"found\":%d,\"points\":[%d,%d],\"chains\":[%d,%d],\"width_mm\":[%.1f,%.1f],\"height_mm\":[%.1f,%.1f],"
                     "\"entries\":%d,\"error\":\"%s\"},"
                     "\"rt\":{\"entries\":%d,\"bake_ms\":%.1f,\"bake\":\"%s\",\"error\":\"%s\"}}",
                     r.eyes, r.eyePoints[0], r.eyePoints[1], r.eyeChains[0], r.eyeChains[1], r.eyeWidthMM[0], r.eyeWidthMM[1], r.eyeHeightMM[0],
                     r.eyeHeightMM[1], r.eyeEntries, r.eyeError.c_str(), r.rtEntries,
                     r.rtMs, r.rtBake.c_str(), r.rtError.c_str());
            s += t;
        }
        snprintf(t, sizeof t, "],\"bakes_running\":%zu}", gJobs.size());
        s += t;
    }
    JsonFinite(s);
    int n = (int)s.size();
    if (!buf || cap <= n) return -(n + 1);
    memcpy(buf, s.c_str(), n + 1);
    return n;
}
