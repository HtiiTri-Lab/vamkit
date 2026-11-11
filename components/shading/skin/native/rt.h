#pragma once
// RtCore (components\shading\rt-core) on VaM's D3D11On12 device (rt.cpp). With the D3DInterop d3d11.dll and
// D3DINTEROP_MODE=on12, Unity's D3D11 device is D3D11On12; D3D12 work executed on that device's queue after a Flush of
// the D3D11 context runs in order with VaM's frame.
//
// Once a frame while the path tracer is selected, at the first camera's PreCull (RtScene.cs: Graphics.ExecuteCommandBuffer
// with OP_RT_SCENE), the plugin hands every drawn skin, clothing item and scene mesh (RtDraw), the lights (RtLightRec),
// the GPUTools hair (RtHairRec) and the ambient SH. rt.cpp copies VaM's D3D11 buffers into D3D12 buffers wrapped for
// D3D11 (skins and clothing every frame, meshes, index buffers and UV vertex buffers once), turns the draws into RtCore
// geometries (one per owner) and records RtCore's scene on its own list on the 11on12 queue; the compositor then records
// the path trace (ST_RecordPathTraceV1). Without the 11on12 device, or with the path tracer off, nothing here runs: VaM's
// own lighting and shadow maps with the skin shading of skin_transmission.cpp.
#include <stdint.h>
#include <string>
#include "../../rt-core/include/path_hair_bridge.h"
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Buffer;
struct ID3D11ShaderResourceView;
struct ID3D11SamplerState;
struct ID3D11Resource;
// Persist the engine's actual SRV interpretation on its resource. No retained
// object references and no recurring per-draw texture inspection.
void RtRememberTextureFormat(ID3D11Resource *texture,uint32_t format);

// Must match RtScene.RecordVersion; both native entry points validate this.
constexpr int kRtSceneVersion = 23;
// The plugin's records: a header, then the draws, lights and hairs.
#pragma pack(push, 4)
struct RtDraw {  // one submesh drawn this frame, 592 bytes (record v18)
    uint64_t owner;      // instance id of the skin / clothing / renderer: one geometry per owner
    uint64_t vertices;   // ID3D11Buffer*: the skinned compute buffer (float3), or the mesh's vertex buffer (stream 0)
    uint64_t normals;    // ID3D11Buffer*: skins and clothing: the normals compute buffer (float3); 0: none
    uint64_t indices;    // ID3D11Buffer*: the mesh's index buffer
    uint32_t indexStart, indexCount;
    int32_t baseVertex;
    uint32_t flags;      // RT_DRAW_*
    // vertexCount: the Unity mesh's (skins and clothing too: VaM pads their draw buffers to multiples of 256, so the UV
    // stream's stride is its size / this); vertexStride 0: the vertex buffer's size / vertexCount (mesh vertex buffers)
    uint32_t vertexStride, vertexCount;
    float matrix[12];    // object -> world, rows of a 3x4 (skins: identity + drawOffset)
    uint64_t material;   // the plugin's key for the material (its instance id; 0: an opaque default)
    uint64_t texture;    // ID3D11Resource* of the main texture (the average colour; the alpha too without alphaTexture)
    // rtc::MaterialKind; 0x100 a fine (2048) alpha slot, 0x200 hair cards and scalps (0.22.1); version 6: bits 12-13
    // rtc::EnvironmentShader (the ambient the material's shader draws)
    uint32_t kind;
    float cutoff, opacity;
    float color[3];      // _Color, linear
    float st[4];         // the alpha's _ST: _AlphaTex_ST with alphaTexture, else _MainTex_ST of `texture`
    float tessAlpha;     // > 0: VaM's Phong-tessellated skin shader (_TessPhong)
    // UV0 for the alpha test (Cutout / Translucent): RtCore reads alpha only through it (none: alpha 1, opaque)
    uint32_t uvOffset;   // bytes into a vertex of `uvs` (float2)
    uint64_t uvs;        // ID3D11Buffer*: the Unity mesh's vertex buffer holding UV0 (meshes: the same as `vertices`)
    uint32_t uvStride;   // its bytes per vertex (0: its size / the vertex count); the top bit: half floats
    float alphaAdjust;   // VaM's _AlphaAdjust: the slot holds saturate(alpha + this) (0: the map as it is)
    uint64_t alphaTexture;  // ID3D11Resource*: VaM's separate alpha map (_AlphaTex, alpha in .a), 0: the main texture's alpha
    float subsurface[3]; // version 6: _SubdermisColor, linear (VaM's skin family)
    float iblFilter;     // version 6: _IBLFilter
    uint64_t pathNormal, pathSurface, pathEmission, pathGloss;
    float pathST[4];
    float pathSpecular[3], pathRoughness;
    float pathEmissionColor[3], pathMetallic;
    float pathNormalScale, pathIor, pathTransmission;
    uint32_t pathFlags;
    float pathGlossParams[3];
    uint32_t pathNormalOffset;
    float pathMapST[16],pathBump[4];
    uint64_t pathDetailNormal,pathDetailMask;
    float pathDetailST[4];
    uint64_t skinWeights,skinMatrices;
    uint32_t boneCount,blendFrameCount;
    float pathMaterialControls[4]; // diffuse offset, spec intensity, post-colour alpha adjust, reserved
    uint64_t pathTangents;
    uint32_t pathTangentOffset,pathTangentStride,pathTangentFormat,pathLayoutFlags;
    uint32_t pathSampling[8];
    uint64_t materialRevision;
    float pathLegacyFresnel;
    uint32_t pathProgramSet; // exact shader family's stock pixel-program set
    uint32_t textureVersion,alphaVersion; // source-content changes, separate from scalar material edits
    // version 18: the program's second colour layer and coverage use (rtc::PathMaterial::second / mode / directGain[3])
    uint64_t pathSecond;
    float pathSecondST[4];
    uint32_t pathSecondSampling,pathMode;
    float pathModeParam;
    // version 20: the draw's light-link class (one bit of 0x1f, 0 = every light; RtScene.LinkLights)
    uint32_t lightLink;
};
struct RtLightRec {  // 88 bytes
    uint64_t id;
    uint32_t type;       // rtc::LightType
    uint32_t shadows;    // bit 0: the light casts shadows (traced); API 43, PT: 8-15 Unity's strength, 16-27 near plane (1/1024 m), 28 Unity draws no shadow
    float position[3], direction[3];  // world; direction: where the light travels
    float color[3];      // as _LightColor0
    float range, spotCos, radius, angular;
    float key[4];        // what Unity puts in _WorldSpaceLightPos0 for it
    uint32_t excludedLinks;  // version 20: the link classes its cullingMask leaves out (unlit, cast no shadow)
};
struct RtHairRec {  // 64 bytes
    uint64_t id;
    uint64_t particles;  // ID3D11Buffer*: TessRenderParticles (56-byte structured)
    uint32_t count, perStrand;  // valid points (the guides), points per guide strand (0: split by RootIndex)
    float width;         // 0.22.1: the rendered strands' projected width per guide strand (m; RtScene.AddHairs)
    float lo[3], hi[3];  // world bounds this frame
    float length[3];     // GPUTools _Length.xyz; zero means no visible strand
    PathHairBuildItem path;
    uint32_t lightLink;  // version 20: as RtDraw::lightLink
    uint32_t flags;      // version 21: RT_HAIR_CAMERA_HIDDEN
};
struct RtSceneHeader {  // 760 bytes
    int32_t version;     // kRtSceneVersion
    int32_t draws, lights, hairs;
    float sh[28];        // unity_SHAr .. SHBb (xyzw), SHC (rgb), 0
    // version 4: last frame's cameras that drew people (a stereo camera once per eye); -1: unknown (an orthographic
    // camera, more than 8, none yet): every texel traced
    int32_t viewers;
    // version 7: _ExposureIBL.y and .w, and 1 when Unity folds lights into the receivers' SH (rtc::Frame::environmentSpecular,
    // exposure, shLights)
    float iblSpecular, exposure;
    int32_t shLights;
    float viewer[8][8];  // position xyz, cos of the cone's half angle (<= -1: no cone), forward xyz, 0
    // version 6: Skyshop's diffuse SH x _ExposureIBL.x in world directions (rtc::Frame::skySh), _ExposureLM.x
    float skySh[27];
    float ambientLM;
    uint64_t pathEnvironment[2]; // lighting cube, visible sky cube
    float pathWorldToSky[12];
    float pathEnvironmentScale[4]; // lighting, background before camera exposure, RGBM, reserved
    uint64_t sceneRevision; // history breaks (scene loading); content edits do not reset reconstruction
    float pathEnvironmentDiffuse; // version 18: _ExposureIBL.x, the lighting cube as diffuse radiance before camera exposure
    uint32_t headerPad18;
    // version 19: what a camera ray that misses shows (rtc::PathEnvironment::backgroundToSky / backgroundColor): world
    // direction -> visible-sky cube direction (rows of a 3x4), and the camera's clear colour (linear, before exposure)
    float pathBackgroundToSky[12];
    float pathBackgroundColor[4];
    uint64_t pathIndoorEnvironment; // optional scene-local baked interior cubemap
    float pathIndoorStrength;
    uint32_t headerPad22;
    // version 23: the scene's Unity reflection probe (rtc::PathEnvironment::probe); null: none
    uint64_t pathProbe;
    float pathProbeDecode[4];
    float pathProbeMin[4]; // world box, w: box projection
    float pathProbeMax[4];
    float pathProbeCenter[4];
};
#pragma pack(pop)
static_assert(sizeof(RtDraw) == 592, "RtDraw is the plugin's record");
static_assert(sizeof(RtLightRec) == 88, "RtLightRec is the plugin's record");
static_assert(sizeof(RtHairRec) == 232, "RtHairRec is the plugin's record");
static_assert(sizeof(RtSceneHeader) == 760, "RtSceneHeader is the plugin's record");
enum { RT_DRAW_INDEX32 = 1, RT_DRAW_RIGID = 2, RT_DRAW_SKIN = 4, RT_DRAW_WRAP = 8, RT_DRAW_NO_OCCLUDE = 16, RT_DRAW_CUA_SKIN = 32, RT_DRAW_DYNAMIC_MESH = 64, RT_DRAW_UI = 128, RT_DRAW_DISCARD = 256, RT_DRAW_SHADOW_ONLY = 512,
       // version 21, PT: the eye camera draws it with nothing covered (a first-person plugin's hidden head): its own
       // geometry (rtc::kCameraHidden), unseen by camera rays only (RtScene.EyeRendered)
       RT_DRAW_CAMERA_HIDDEN = 1024,
       // 0.23.4, PT, CPU only (never RtCore's): an author's shader the plugin has no render state for. EFFECT: its raster
       // draws are replayed over PT as an effect (stage 2), with RT_DRAW_UI / pathFlags 32 it is not traced at all,
       // without them it is traced hidden from camera rays (RT_DRAW_CAMERA_HIDDEN). WATCH: the renderer reads its
       // draws' real D3D11 state for the plugin (ST_GetPathObservedV1).
       RT_DRAW_EFFECT = 2048, RT_DRAW_WATCH = 4096,
       // 0.23.5, PT: per renderer, a raster-only outline shell (RtScene.CameraOnly): rtc::kCameraOnly (camera rays only)
       RT_DRAW_CAMERA_ONLY = 8192,
       // API 43, PT: cast-shadows Off: blocks no analytic light (rtc::kNoCast)
       RT_DRAW_NO_CAST = 16384 };
enum { RT_HAIR_CAMERA_HIDDEN = 1 };  // RtHairRec::flags, version 21
bool RtPathSkinParameters(ID3D11Buffer *indices,uint32_t start,struct ID3D11Resource *gloss,float *parameters);
// 0.22.71 (skin_transmission.cpp RtPathEye): a character's eye rig this frame for PT's eyelid band: the gather's output
// (4 + kEyeGatherPoints float4: each eye's centre and scale, its gaze and radius, then the lid points), how many points
// each eye has, and the eyelid occlusion (upper darkening, its width, lower darkening, its width; zeros: off)
struct RtPathEyeRig { ID3D11Buffer *rig=nullptr; uint32_t points[2]={}; float occlusion[4]={}; };
bool RtPathEye(ID3D11DeviceContext *c,ID3D11Buffer *indices,uint32_t start,ID3D11Buffer *vertices,RtPathEyeRig &out);
// Explicit authored surface role: generic, skin, lip skin, teeth, tongue,
// eye, inner mouth, nails. Auxiliary AO rigs never determine this role.
enum class RtSurfaceRole : uint32_t { Generic, Skin, Lips, Teeth, Tongue, Eye, Mouth, Nails, Fabric };
RtSurfaceRole RtPathSurfaceRole(ID3D11Buffer *indices,uint32_t start,float &diffuseFactor);
void RtInvalidatePathFrame(const char *reason);
void RtPathNotice(const char *text); // 0.22.62: a one-time line for the plugin's log

// One character's skin for the PT: its geometry metadata (worker thread, the bake job: each tessellated submesh's
// subdivision), then the receiver that holds it (render thread, ProcessJobs). parts: one per submesh (255: none);
// gpuStart: each submesh's first index in the mesh's index buffer.
struct RtBake;
struct RtReceiver;
RtBake *RtBakeReceiver(const float *pos, const float *uv, int vertexCount, const int *indices, int subCount, const int *subStart,
                       const int *subCount3, const int *gpuStart, const uint8_t *parts, std::string &error);
void RtFreeBake(RtBake *b);
const char *RtBakeSummary(const RtBake *b);  // triangles (status)
// render thread; takes the bake
RtReceiver *RtCreateReceiver(RtBake *bake, void *indexBuffer, int character);
void RtDestroyReceiver(RtReceiver *r);  // any thread; gone at the next frame, freed behind the fence

// The D3D11On12 device under `device` and DXR 1.1 (RtCore's Init), or false (reason in the status). Render thread; once.
bool RtInit(ID3D11Device *device);
bool RtAvailable();
void RtFrameOff();  // render thread, a frame without RtFrame (光追 off): ends PT's frame and gives the strands back to raster
bool RtMaybe();  // any thread: RtInit not tried yet, or it succeeded (the bake jobs make the PT metadata then)
// The frame's records (render thread, OP_RT_SCENE): RtCore's scene recorded and executed while the PT renders.
// frame: the plugin's frame counter (once per frame; a second call ignored).
void RtFrame(ID3D11DeviceContext *c, const void *records, int frame);

void RtRelease();  // render thread, device going away

struct RtHostStatus {
    int available = 0;
    char reason[160] = "";
    char lastError[160] = "";
    int frames = 0, failures = 0, records = 0, owners = 0, geometries = 0, receivers = 0, lights = 0;
    int materials = 0, indexCopies = 0;
    int pathTracing = 0, pathMaterials = 0, pathTextures = 0;
    uint64_t pathCalls = 0;
    float pathJitter[2]={};
    uint32_t pathHairSegments=0;
    uint64_t pathHairBytes=0;
    uint32_t pathEmitters=0;uint64_t pathLightingBytes=0;
    uint32_t pathSer=0;
    uint32_t pathEyeRigs=0;  // 0.22.71: the characters' eye rigs PT's eyelid band read this frame
    int uvOwners = 0, noOccludeOwners = 0;  // last frame: owners with a UV0 stream (alpha test possible), not occluding
    // last frame, owners drawn with a Cutout / Translucent material: "vertices / UV buffer bytes / offset / stride" each,
    // "-" after it where no UV stream was taken (RtCore then reads alpha 1)
    char alphaUv[512] = "";
    uint64_t copiedBytes = 0;   // last frame: D3D11 copies into the wrapped buffers
    double cpuMs = 0, waitMs = 0, gpuMs = 0;  // means since the reset: RtFrame on the render thread (without its slot wait),
    int samples = 0;                          //   the host's D3D12 list (copies + RtCore)
    char bakes[256] = "";       // the last receiver bake's summary
};
RtHostStatus RtGetStatus();
void RtResetTiming();
// render thread before RtFrame: whether the compositor reconstructs with RR (the PT needs its consumer)
void RtSetReconstruction(bool enabled);
bool RtPathSelected(); // render thread: true while the PT route owns lighting
bool RtPathRendering(); // actual prepared scene and ready reconstruction consumer
int RtPathRasterKind(ID3D11Buffer *indices,uint32_t first,uint32_t count,int32_t base,uint64_t program=0,uint64_t *watched=nullptr); // -1 needs actual PS, 0 unknown, 1 physical, 2 fibers, 3 UI, 4 null, 5 effect (0.23.4); watched: the material key of a watched range (RT_DRAW_WATCH), else 0
// 0.22.66: [RayTracing] PathBounces, the diffuse bounces of the PT (0-3)
void RtSetPathBounces(int bounces);
// API 43: rtc::PathEnvironment::author* (docs\pt-author-intent-20261109)
void RtSetPathAuthor(uint32_t rules,float ambient,float contact,float ambientRange,float emitter,float backgroundNear,float backgroundFar);
// tests (SimXR A/B; never in a menu): RtCore's layers (rtcore.h SetTestLayers)
void RtSetTest(unsigned layers);
void RtPathDrawDiagnostic(ID3D11DeviceContext*,uint32_t count,uint32_t first,int32_t base,uint64_t program,uint32_t instances);
