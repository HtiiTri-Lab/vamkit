#pragma once
#include <stdint.h>
#include "path_hair_bridge.h"

struct ID3D12Resource;
struct ID3D12GraphicsCommandList4;

namespace rtc {
// Full-scene, input-pixel path integrator; mutually exclusive with the atlas
// lighting route. Installation/performance status is recorded in PLAN.md.
struct PathMaterial {
    // Borrowed during SetPathMaterial; RtCore retains them until GPU retirement.
    // All resources must be COMMON/NON_PIXEL_SHADER_RESOURCE at dispatch.
    ID3D12Resource *base = nullptr, *normal = nullptr, *surface = nullptr, *emission = nullptr, *gloss = nullptr;
    ID3D12Resource *detailNormal = nullptr, *detailMask = nullptr, *alpha = nullptr;
    uint32_t formats[8] = {}; // DXGI_FORMAT, including sRGB decoding for colour maps
    float baseColor[4] = {1,1,1,1};
    float specular[3] = {.04f,.04f,.04f};
    float roughness = .5f;
    float emissionColor[3] = {};
    float metallic = 0;
    float uvST[4] = {1,1,0,0};
    float normalScale = 1;
    float ior = 1.5f;
    float transmission = 0;
    uint32_t flags = 0; // 1 metallic map, 2 smoothness in surface alpha, 4 normal AG, 8 skin, 16 VaM gloss,
                        // 1073741824 an eyeball under its lids (path_trace.hlsl PtLidLight)
    float glossParams[4] = {0,8,0,0}; // VaM gloss offset, shininess, specular offset, reserved
    float skin[28]={}; // lobes (8), gloss tier (12), scattering (4), normal filtering (4)
    float mapST[20]={1,1,0,0, 1,1,0,0, 1,1,0,0, 1,1,0,0, 1,1,0,0}; // normal, surface, emission, gloss, detail
    float bump[4]={1,1,1,0}; // specular, diffuse, detail strength, reserved
    float alphaST[4]={1,1,0,0};
    float alphaAdjust=0;
    float materialControls[4]={0,1,0,0}; // diffuse offset, spec intensity, post-colour alpha adjust, reserved
    uint32_t sampling[8]={}; // Unity wrap U/V (2 bits each), point bit4, nearest mip bit5
    // API 16: the source program's second colour layer and coverage use.
    ID3D12Resource *second=nullptr; // decal or Standard detail albedo
    uint32_t secondFormat=0, secondSampling=0;
    float secondST[4]={1,1,0,0};
    // 1 decal, 2 detail albedo, 4 closure x coverage, 8 specular / coverage,
    // 16 additive authoring, 64 smoothness in albedo alpha, 128 reflection mask,
    // 256 a person's surface (skin, clothing, mesh hair: its depth and motion stay in the guides where see-through)
    uint32_t mode=0;
    float directGain[4]={1,1,1,0}; // analytic-light diffuse gain, mode parameter
    // API 20: the alpha map's content, the same while this value is (the host's copy and its writes). 0: it may change
    // any frame (a borrowed original): the PT's opacity micromaps leave this material's alpha map alone.
    uint64_t alphaVersion=0;
};
bool SetPathMaterial(uint32_t material, const PathMaterial &desc);
void ClearPathMaterials();

struct PathEnvironment {
    // Live Skyshop cubes. No texture copies: caller borrows on the shared queue.
    ID3D12Resource *lighting=nullptr, *background=nullptr, *indoor=nullptr, *probe=nullptr;
    uint32_t formats[4]={};
    float worldToSky[12]={1,0,0,0,0,1,0,0,0,0,1,0};
    float lightingScale=0, backgroundScale=0;
    uint32_t rgbm=1; // bit 0: Skyshop polynomial; bit 1: background is already linear (bit 0 still applies to lighting)
    float diffuseScale=0; // API 16: the lighting cube as diffuse radiance (_ExposureIBL.x); lightingScale is specular
    // API 17: what a camera ray that misses shows. VaM's visible sky is a Skyshop mesh sampled in its own object space
    // (rows: world direction -> background cube direction); with the sky hidden, the camera's clear colour (linear,
    // before camera exposure, added to the cube term).
    float backgroundToSky[12]={1,0,0,0,0,1,0,0,0,0,1,0};
    float backgroundColor[3]={};
    // API 22: how many diffuse vertices a path continues through (PathView::maxBounces still bounds the whole
    // path). Past them it continues only through specular lobes; the diffuse light there is the light samples'.
    uint32_t diffuseBounces=8;
    float indoorScale=0;
    // API 26 (docs\pt-author-intent-20261109): what of Unity's lighting rules PT keeps, which the scene's author
    // lit by. 0 rules: the physical tracer. Each rule is one bit of kPathAuthor*.
    uint32_t authorRules=0;
    float authorAmbient=0;       // kPathAuthorAmbient: share of the first surface's diffuse environment seen unoccluded past authorAmbientRange
    float authorContact=.6f;     // kPathAuthorNoShadow / NoReceive: metres of contact shadow left
    float authorAmbientRange=1;  // metres
    float authorEmitter=1;       // kPathAuthorEmitter: diffuse light from emissive surfaces
    float backgroundNear=0,backgroundFar=0; // kPathAuthorBg*: metres from the camera; nearer than near a surface keeps its budget, between the two a share of paths
    // API 27 (docs\pt-unity-probe-20261109): the scene's Unity reflection probe, what Standard's indirect specular reads
    // (unity_SpecCube0): its HDR decode (ReflectionProbe.textureHDRDecodeValues), world box (min, w: box projection; max),
    // and where it was captured. Under kPathAuthorProbe.
    float probeDecode[4]={},probeMin[4]={},probeMax[4]={},probeCenter[4]={};
};
constexpr uint32_t kPathAuthorNear=1,kPathAuthorNoShadow=2,kPathAuthorAmbient=8,
    kPathAuthorNoReceive=16,kPathAuthorCull=32,kPathAuthorSolid=64,kPathAuthorEmitter=128,kPathAuthorCrisp=256,
    kPathAuthorBounce=1024,kPathAuthorSpot=2048,kPathAuthorPanorama=4096,kPathAuthorSkySh=8192,
    kPathAuthorBgBounce=16384,  // a path whose first surface is not a person (PathMaterial::mode 256, fibres) has no diffuse bounce
    kPathAuthorBgRough=32768,   // on such a path a reflection of roughness >= .4 reads the sky's cube, unoccluded (no ray)
    kPathAuthorProbe=65536;     // API 27: PathMaterial::mode 262144 reads the probe as Unity's Standard does
bool SetPathEnvironment(const PathEnvironment &desc);
// API 23: the characters' eye rigs for the eyelid band on eyeballs (path_trace.hlsl PtEyeBand): `count` records of
// kPathEyeRigBytes in a buffer the trace reads (a read state, or COMMON to promote; kept alive until the list ends).
// Kept for the following RecordPathTrace calls; null or 0: none.
constexpr uint32_t kPathEyeRigBytes=2160;
void SetPathEyeRigs(ID3D12Resource *rigs,uint32_t count);
bool RecordPathHair(ID3D12GraphicsCommandList4 *list,ID3D12Resource *source,const PathHairItem *items,uint32_t count,uint64_t serial);
void GetPathHairStatus(uint32_t &segments,uint64_t &bytes);

struct PathView {
    uint32_t width=0, height=0, samples=1, maxBounces=4;
    uint32_t seed=0, eye=0;
    float jitter[2]={};
    float textureLodBias=0; // same DLSS texture-detail setting as the raster route
    float clipToWorld[16]={}; // row-major; GL clip, with top-left image origin
    float worldToClip[16]={};
    float camera[3]={}, exposure=1;
    // Output resources: RGBA16/32F, except depth/hitDistance (R16/32F).
    // Supplied as UAV, left as UAV. They all share the INPUT dimensions.
    ID3D12Resource *color=nullptr, *diffuse=nullptr, *normal=nullptr, *specular=nullptr;
    ID3D12Resource *depth=nullptr, *hitDistance=nullptr;
    ID3D12Resource *motion=nullptr; // optional RG32F UAV: primary mesh/fiber/sky motion
    float previousWorldToClip[16]={}; // previous unjittered GL view projection
    ID3D12Resource *detailReference=nullptr; // optional input-sized RGBA16F; null means no reference work
    // Optional NRD route only: independently demodulated diffuse/specular
    // radiance, alpha = world-space secondary hit distance. RR uses neither.
    ID3D12Resource *diffuseRadiance=nullptr,*specularRadiance=nullptr;
};
// Records on the SAME queue, after Record() and before the following frame's
// Record(). Caller must preserve scene input resources until this list ends.
// Optional RR F hint; kept outside PathView so old compositor/native binaries
// remain ABI-compatible. No image history, extra rays or extra dispatch.
struct PathResponsivity {
    ID3D12Resource *mask=nullptr; // input-sized R16F UAV, left as UAV
    float strength=0; // positive current-input bias, NOT an exact blend weight
    float motionScale[2]={1,1}; // input-pixel motion -> output pixels/second
};
bool RecordPathTrace(ID3D12GraphicsCommandList4 *list, const PathView &view, const PathResponsivity *response=nullptr);
bool PathTracingAvailable();
bool PathUsesSer();
void GetPathLightingStatus(uint32_t &emitters,uint64_t &bytes);
} // namespace rtc
