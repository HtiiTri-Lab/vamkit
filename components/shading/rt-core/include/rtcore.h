#pragma once
// RtCore: the path tracer's scene (characters, clothing, scene meshes) and its tracing (path_tracing.h).
//
// The 11on12 host (components\shading\skin) runs it on the D3D11On12 device's queue after a Flush of Unity's D3D11
// context. It hands RtCore ID3D12Resources and a command list; RtCore records, never submits, never waits on a fence,
// and keeps every resource it creates alive until the host reports the frame that used it complete
// (Frame::completedFrame).
//
// Per frame Record builds or updates the scene's acceleration structure; RecordPathTrace traces it.
//
// Light units are the forward shaders' own: _LightColor0 x attenuation x N.L and ShadeSH9, i.e. irradiance / pi;
// a shader multiplies by its albedo to get outgoing radiance.
//
// Threads: every call on the host's render thread unless noted. Main work arrays retain capacity;
// the per-frame host SRV map still allocates nodes. Nothing here talks to Unity.
#include <stdint.h>

struct ID3D12Device5;
struct ID3D12Resource;
struct ID3D12GraphicsCommandList4;

namespace rtc {

constexpr uint32_t kApiVersion = 27; // API 27: PathEnvironment::probe, Unity's reflection probe for Standard (kPathAuthorProbe, PathMaterial::mode 262144). API 26: API 26: PT reads the author's Unity shadow settings (Light::shadowStrength / shadowNear / unityShadows, kNoCast, MaterialDesc::solidShadow, PathEnvironment::authorRules; docs\pt-author-intent-20261109); API 25: optional local interior cubemap; API 24: PT camera-only geometry (kCameraOnly); API 23: PT eye rigs for the eyelid band (SetPathEyeRigs); API 22: PT diffuse bounce limit (PathEnvironment::diffuseBounces); API 21: PT geometry / fibers hidden from camera rays only (kCameraHidden, kPathHairCameraHidden); API 20: the PT's opacity micromaps (PathMaterial::alphaVersion, Status::pathOmm*); API 19: the installed route keeps the light links too (Hair::lightLink); API 18: per-light layer exclusion (Geometry::lightLink, Light::excludedLinks, PathHairItem::pad); API 17: PT shadow-only proxies, visible-sky transform and camera clear colour; API 16: path material second layer / mode / direct gain, environment diffuse scale; API 15: authored fiber reflection; API 14: tangent streams; API 13: route-specific work buffers; API 10: Frame::ambient; the
                                     // transmission thickness gone (no shader read it: Geometry::thicknessGroup,
                                     // ReceiverDesc::transmission / thicknessGroup, Status::thicknessRays);
                                     // API 9: Frame::environmentSpecular, exposure, shLights (the environment work left out
                                     // when nothing would show it); API 8: bounce sources carry the ambient their shaders
                                     // draw (Frame::skySh, ambientLM,
                                     // MaterialDesc::environment); 7 (0.22.4): Hair::length matches GPUTools' displayed
                                     // strand lengths; API 6: shadowWidthM volume; API 5: Status adds counterFrame/counterPeriod
// Retire already completed GPU objects even if an empty/inactive scene skips Record.
void RetireCompleted(uint64_t completedFrame);

// ---------------------------------------------------------------------------------------------------------------
// Geometry inputs

enum class VertexFormat : uint32_t { None = 0, Float3 = 1, Half4 = 2, Float2 = 3, Half2 = 4, Float4 = 5 };

struct Stream {
    ID3D12Resource *buffer = nullptr;
    uint64_t offset = 0;    // bytes from the start of the buffer to the first vertex's element
    uint32_t stride = 0;    // bytes per vertex
    VertexFormat format = VertexFormat::None;
};

enum class Tess : uint32_t {
    None = 0,
    Phong = 1,  // VaM's GlossNMTessMapped* domain shader: lerp(flat, phong, _TessPhong), no displacement
};

// One submesh (draw) of a geometry: a range of the geometry's index buffer.
struct Submesh {
    uint32_t start = 0, count = 0;  // in indices from Geometry::indexOffset (count a multiple of 3)
    uint32_t material = 0;          // CreateMaterial's id
    Tess tess = Tess::None;
    float tessAlpha = 0;            // _TessPhong
    uint32_t tessLevel = 1;         // the occluder copy's uniform subdivision (host: mean edge / 5 mm, 1..8)
};

enum GeometryFlags : uint32_t {
    kSkinned = 1,          // VaM's skinning compute buffers, rewritten every frame (BLAS updated); the matrix still
                           // applies: identity, or the translation DAZSkinV2 draws with (drawOffset)
    kRigid = 2,            // object space: matrix applies; built once, refitted only when the host marks it dirty
    kDirty = 4,            // kRigid: the vertices changed this frame
    kNoOcclude = 16,       // receives but never blocks: skin decals (wrap offset < 1 mm), cornea / EyeReflection / Tear
    kPrimaryCullBack = 32, // authored back-face culling for camera/primary guides; transport still sees both sides
    kPrimaryCullFront = 64, // authored front-face culling, including reflected instance transforms
    // API 17, path tracing only: Unity's ShadowCastingMode.ShadowsOnly (a shadow proxy): blocks analytic lights, invisible
    // to camera/BSDF/NEE rays. Cast-shadows Off is not translated: PT occludes physically.
    kShadowOnly = 128,
    // API 21, path tracing only: the host's eye camera draws it with nothing covered (a first-person plugin hides the
    // possessed head there; mirrors still draw it): unseen by camera rays, seen by every other ray.
    kCameraHidden = 256,
    // API 24, path tracing only: the author's cast-shadows Off, or an outline shell (an inverted hull): seen by camera
    // rays only (with its authored culling), never by shadow, environment, bounce or reflection rays - as in Unity,
    // where it is in no shadow map and occludes no ambient.
    kCameraOnly = 512,
    // API 26, path tracing only: cast-shadows Off: in no shadow map, so it blocks no analytic light (authorRules
    // kPathAuthorCull); environment, bounce and camera rays still see it.
    kNoCast = 1024,
};

struct Geometry {
    uint64_t id = 0;        // stable across frames: one bottom level per id
    uint32_t flags = 0;     // GeometryFlags
    Stream position;        // required
    Stream normal;          // receivers and tessellated submeshes
    Stream uv;              // Cutout / Translucent submeshes (the alpha texture's UV before _MainTex_ST)
    ID3D12Resource *indexBuffer = nullptr;
    uint64_t indexOffset = 0;       // bytes
    uint32_t index32 = 0;           // 1: 32-bit indices, 0: 16-bit
    int32_t baseVertex = 0;
    float matrix[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};  // object -> world, rows of a 3x4
    uint32_t vertexCount = 0;
    const Submesh *submeshes = nullptr;
    uint32_t submeshCount = 0;
    // Optional Unity CUA skinning. Rest vertices/normals stay on the GPU;
    // one float4 weight + uint4 index per vertex, and world bone rows on CPU.
    ID3D12Resource *skinWeights = nullptr;
    const float *skinMatrices = nullptr; // boneCount * 12, row-major affine
    uint32_t boneCount = 0;
    uint32_t blendFrameCount = 0; // weights follow skinMatrices; sparse deltas follow skinWeights' vertex influences
    Stream tangent;         // authored float4 / half4 tangent and handedness, same deformation phase as normal
    // API 18: the light-link class of the object's layer (one bit of 0x1f; 0: lit by every light). A light whose
    // excludedLinks has the bit neither lights it nor is shadowed by it (Unity's cullingMask). API 19: both routes.
    uint32_t lightLink = 0;
};

// ---------------------------------------------------------------------------------------------------------------
// Materials (load time; the host copies what the GPU needs once, nothing of Unity's is read per frame)

enum class MaterialKind : uint32_t {
    Opaque = 0,       // blocks; bounce radiance = average colour x the direct light at the hit + ambient
    Cutout = 1,       // alpha test as drawn (lace holes, hair caps): blocks where alpha >= cutoff
    Emissive = 2,     // unlit / emissive / lightmapped: blocks; bounce radiance = the average colour itself
    Translucent = 3,  // alpha blended (stockings, sheer fabric, glass): rays go on, transmittance *= 1 - alpha
};

// API 8: which ambient a material's shader adds (VaM's shaders, from their disassembly; Frame::ambientSh, skySh):
enum class EnvironmentShader : uint32_t {
    Unity = 0,     // Standard and the rest: max(0, ShadeSH9(ambientSh))
    VamSkin = 1,   // Custom/Subsurface/*, Custom/Hair/* (skin, clothing, mesh hair): |L0 + f1 L1 + f2 L2| x (1 - _IBLFilter)
                   // of skySh + ambientSh x ambientLM, f1 = a + S (a^2 - a), a = 1 - S/3, f2 the same with 1 - 3S/4 (S =
                   // _SubdermisColor, per channel)
    Marmoset = 2,  // Marmoset/* (Skyshop's own): |skySh| + max(0, ShadeSH9(ambientSh))
};

struct MaterialDesc {
    MaterialKind kind = MaterialKind::Opaque;
    float averageColor[3] = {0.5f, 0.5f, 0.5f};  // linear; main texture's 1x1 mip x _Color (also receivers' albedo)
    float alphaCutoff = 0.5f;                    // Cutout
    float opacity = 1.0f;                        // Translucent: _Color.a (alpha = texture alpha x opacity)
    float uvScaleOffset[4] = {1, 1, 0, 0};       // Cutout / Translucent: _MainTex_ST
    // API 26: Translucent - Unity's shadow map takes it as a cutout on the same alpha at alphaCutoff (a scene object on
    // VaM's Custom/Subsurface programs): PT's analytic shadow rays stop there (kPathAuthorSolid), not at 1 - alpha.
    uint32_t solidShadow = 0;
    // API 8: the diffuse environment the material's shader draws, which its bounce radiance carries (before: every
    // surface Unity's ambient SH, as Standard draws it). A receiver's outgoing radiance takes its skin material's.
    EnvironmentShader environment = EnvironmentShader::Unity;
    float subsurface[3] = {0, 0, 0};             // VamSkin: _SubdermisColor, linear (weights the SH bands)
    float iblFilter = 0;                         // VamSkin: _IBLFilter (the environment x 1 - this)
};

// ---------------------------------------------------------------------------------------------------------------
// Lights

enum class LightType : uint32_t { Directional = 0, Spot = 1, Point = 2 };

struct Light {
    uint64_t id = 0;            // stable across frames
    LightType type = LightType::Spot;
    float position[3] = {};     // world (spot, point)
    float direction[3] = {};    // world, the direction the light travels (directional, spot)
    float color[3] = {};        // as _LightColor0 (colour x intensity, what the forward pass multiplies)
    float range = 0;            // spot, point: VaM's 1/(1+25(d/range)^2) attenuation, nothing past range
    float spotCosOuter = 0;     // spot: cos of half the cone angle
    float radius = 0.25f;       // spot, point: disc radius in metres
    float angularRadius = 0.0873f;  // directional: radians (5 degrees); spot, point (PT only): the disc grows by
                                    // tan(angularRadius) per metre from the light, a constant apparent size
    uint32_t excludedLinks = 0; // API 18: the Geometry::lightLink / Hair::lightLink classes this light leaves out
    // API 26, PT (authorRules): what Unity does with its shadow. shadowStrength: VaM reads it as the shadow's blur, on every
    // surface (a shadow is always fully dark); shadowNear: nothing nearer the light than this casts; unityShadows 0: no
    // shadow at all (shadows None, or a vertex light).
    float shadowStrength = 1, shadowNear = 0;
    uint32_t unityShadows = 1;
};

// ---------------------------------------------------------------------------------------------------------------
// Frame

struct Frame {
    uint64_t frame = 0;           // increasing by one per Record
    uint64_t completedFrame = 0;  // the last frame the GPU has finished (host's fence); RtCore frees and reads back by it
    const Geometry *geometries = nullptr;
    uint32_t geometryCount = 0;
    const Light *lights = nullptr;
    uint32_t lightCount = 0;
    float ambientSh[27] = {};     // unity_SHAr, SHAg, SHAb, SHBr, SHBg, SHBb (xyzw each), SHC (rgb): ShadeSH9's inputs
    // API 8: Skyshop's diffuse SH as VaM's shaders read it (_SH0.._SH8 on the sky-rotated normal) x _ExposureIBL.x, in
    // world directions: per channel (L1 x, y, z, L0), per channel (xy, yz, 3zz - 1, zx), then (xx - yy) rgb - laid out as
    // ambientSh, its second band zero-mean (Skyshop's) where Unity's folds the zz term's mean into L0. 0: Skyshop's
    // diffuse off (most scenes, and DisplayLighting).
    float skySh[27] = {};
    float ambientLM = 1;          // API 8: _ExposureLM.x (unityAmbientIntensity), VamSkin's factor on ambientSh
    float exposure = 1;           // API 9: _ExposureIBL.w (camExposure); the PT scales its camera exposure by it
    uint64_t timestampFrequency = 0;  // the executing queue's GetTimestampFrequency (0: no GPU timing)
};

// ---------------------------------------------------------------------------------------------------------------
// Calls

// Device must support DXR 1.1 (inline ray queries). error gets the reason on false.
bool Init(ID3D12Device5 *device, char *error, int errorBytes);
void Shutdown();  // the host has waited for the GPU to go idle

uint32_t CreateMaterial(const MaterialDesc &m);   // id > 0; 0 on failure
void SetMaterial(uint32_t material, const MaterialDesc &m);
void DestroyMaterial(uint32_t material);

// Records the frame's work into the open list, a DIRECT or (API 4) a COMPUTE one: a host may execute it on a compute
// queue next to its own rendering, fenced after the inputs are written and before the outputs are read. Every
// Record must go to the same queue. SetDescriptorHeaps / SetComputeRootSignature / pipelines are RtCore's
// own; the host restores its own bindings afterwards. Returns false (reason in the status) when nothing could be
// recorded.
bool Record(ID3D12GraphicsCommandList4 *list, const Frame &f);

struct Status {
    uint32_t apiVersion = kApiVersion;
    int ok = 0;
    char reason[160] = "";
    char lastError[160] = "";
    uint64_t frames = 0, failures = 0;
    uint32_t geometries = 0;
    uint64_t triangles = 0, tessellatedTriangles = 0;
    // 0.22.21: bottom levels in the scene built without the opacity micromaps they wait for (the frame's descriptors ran
    // out: baked in a later frame); descriptors in use in the static part, used of the last frame's 4096
    uint32_t ommPending = 0, staticDescriptors = 0, frameDescriptors = 0;
    // API 20: the PT's opacity micromaps in the last frame's bottom levels: submeshes with one state for every triangle,
    // submeshes baked from their alpha map, and their triangles
    uint32_t pathOmmUniform = 0, pathOmmBaked = 0;
    uint64_t pathOmmTriangles = 0;
    // GPU ms of RtCore's part of the list (Record), means since ResetTiming (timestamps read back by completedFrame):
    double gpuMs = 0, gpuMaxMs = 0, buildMs = 0;
    double recordCpuMs = 0;
    int samples = 0;
};
Status GetStatus();
void ResetTiming();

// Test interface (SimXR / offline A/B; never in a menu). Layers: bit 6 tessellation (cleared: the occluder copies are
// not Phong-tessellated).
// Bit 19 (API 20): cleared, the PT's bottom levels carry no opacity micromaps (every cutout / translucent hit asks the
// shaders, as before API 20; the same picture).
// PT bits (0.22.68, docs\pt-eye-audit-20261103), cleared: 0.22.66's. 20: a point / spot light is sampled over its disc
// alone (no draw from a specular lobe narrower than the disc). 21: environment / emitter samples are stopped by clear
// sheets (a continuation through one weighed 1). 22: the cornea / tear film keeps its authored gloss.
// PT bits (0.22.69, docs\pt-eye-light-20261103), cleared: 0.22.68's. 23: VaM's Subsurface family lit directly as
// (2-S) N.L, without its falloff. 24: the eye film at its authored reflection strength.
// PT bit (0.22.70, docs\pt-eyelid-20261103), cleared: 0.22.69's. 25: an eyeball's (PathMaterial flag 1073741824)
// analytic lights' diffuse is not scaled by the share of its hemisphere its lids leave open.
// PT bit (0.22.71, docs\pt-eyelid-band-20261103), cleared: 0.22.70's. 26: an eyeball's albedo without the eyelid band
// of its eye rig (SetPathEyeRigs).
// PT bit (0.23.1, docs\pt-cover-guides-20261104), cleared: 0.23.0's. 27: where a camera ray's coverage varies with
// the seed, the RR guides are the layer above coverage .5 alone (set: every layer weighed by the share of frames
// the path takes it).
// Default: every bit set.
void SetTestLayers(uint32_t mask);
}  // namespace rtc
