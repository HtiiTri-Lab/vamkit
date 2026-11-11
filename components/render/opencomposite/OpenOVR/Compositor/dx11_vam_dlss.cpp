// VaM DLSS Super Resolution bridge.
//
// The BepInEx plugin VaMDLSSBridge jitters Unity's stereo projection and, at
// CameraEvent.AfterForwardAlpha, calls VaMDLSS_GetRenderEventFunc() through
// CommandBuffer.IssuePluginEventAndData with this frame's camera data. On
// Unity's render thread we copy the depth buffer that is still bound for the
// scene, and at eye submission build upright per-eye depth and motion vectors
// from it. Those feed NR (optional) and NVIDIA DLSS SR through NGX D3D11.
//
// Object motion: VaM skins and clothing are GPU-skinned into world-space
// compute buffers and drawn with Graphics.DrawMesh, so Unity has no motion
// vectors for them. The bridge hands over the vertex/index buffers VaM drew
// this frame; at the capture event they are redrawn here with this and the
// previous frame's vertex positions, keeping only fragments whose depth
// matches the scene depth. Those pixels replace the camera-only motion.
//
// Animated CUAs (bridge 2.8.0, kDrawSkinned): Unity SkinnedMeshRenderers
// (a folding fan held in a hand, dancers, creatures) get no motion vectors
// from Unity either, and their meshes are not rigid. The bridge sends their
// bind-pose positions and bone weights once per mesh and, per frame, every
// bone's skinning matrix (bone localToWorld x bind pose) for this frame and
// the previous one; they are skinned here twice and drawn like the skins.
//
// Hair: VaM's physics hair (GPUTools) expands its strands on the GPU from a
// buffer of tessellated guide-strand points (TessRenderParticle, world space)
// with hull/domain shaders we do not have. Its motion is taken from those
// guide points instead: each guide segment becomes a camera-facing ribbon
// drawn with this and the previous frame's points, kept where its depth is
// near the scene depth and where no skin or clothing already matched (the
// stencil). Neighbouring strands move with their guides, so the ribbons carry
// the motion of the hair around them.
//
// UI: VaM's menus and scene panels are world-space Unity UI, drawn without
// depth, so the guides used to carry whatever stands behind a menu (a wall a
// metre further back): on head turns the menu moved differently from its
// vectors and DLSS smeared its text. The bridge sends every visible element's
// rectangle with this and the previous frame's canvas transform; they are
// drawn here where they are in front of the scene, writing the menu's motion
// and its depth, which the guide pass merges into the guide depth.
//
// UI bypass: with those vectors DLSS SR still smeared menu text on head turns
// (with SR off it did not). Where the UI pass drew a menu in front of the
// scene the guide pass also writes a UI mask, and after the evaluation DLSS's
// output there is replaced by EASU (FSR 1) of this frame's colour alone,
// sampled at this frame's jitter: menus are upscaled without history.
// That alone still left the text shaking: the jitter moves every frame's
// raster by a sub-pixel, and thin glyphs come out differently each time,
// whatever upscales them.
//
// Live render scale and native frames: [Performance] RenderScale is read
// while VaM runs (VaMDLSS_GetRenderScalePercent) and the bridge renders at
// it by scaling Unity's eye texture from the size VaM was given at start
// (VaMDLSS_GetEyeTextureScale), 33% to 100% of the output. v18: the output is
// the runtime's recommendation times [Performance] OutputScale (100-150,
// live; the swapchains are rebuilt for it), 100% with a model is DLAA, and
// DLSSPreset=Off (kStateSrOff) is the only way to frames without DLSS SR:
// the bridge renders them for the output size without jitter and flags them
// (FrameInfo flag 4, VamFrameGuides::nativeFrame): they skip DLSS SR and go
// out 1:1 (dx11_vam_nis). While VaM's own menus are open (the main HUD, or the
// start screen / scene browser world UI; DLSSMenuNative, kStateMenuNative)
// the bridge renders at 100% for that time (flag 8 as well: NR before the
// upscaler is skipped rather than rebuilt for the other size). DLSS SR
// resets its history on the first frame back (the serials it saw jump).
//
// NR after the upscaler (NRAfterUpscale): the guides are resampled to the
// output size (PrepareOutputGuides) so NR can run
// on the upscaled image instead of the render.
//
// Nasolabial folds ([NR] NasolabialReduction): NR deepens them on VaM faces.
// For female characters the bridge sends, with the face submesh's draw, a
// per-vertex buffer of the unmorphed base face: position relative to the
// mouth corners (in mouth widths) and a gate (front of the Face material, not
// lips or nostrils). The skin pass draws a band along the fold from it, per
// pixel, into its own target, so it follows morphs, expressions and the
// head; the guide pass turns that into a keep mask (RGBA16F, G = 1 - cut *
// band, 1 everywhere else), by which NR's composite keeps its result inside
// the band (dx11_nr_prepass.cpp: NR itself gets no ControlMask, as any mask
// turns its own skin mask off).
#include "stdafx.h"
#include "dx11_vam_dlss.h"
#include "vam_render_host.h"
#include "dx11_eye_blit.h"
#include "vam_performance_config.h"
#include "dx11_gpu_stage_timer.h"
#include "vam_fsr_ui_bypass.h"
#include "vam_on12.h"
#include "dx12_rr_stage_timer.h"
#include "../../../../shading/rt-core/include/path_tracing.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_dlssd.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#define A_CPU 1
#include <ffx_a.h>
#include <ffx_fsr1.h>

namespace {
template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;

constexpr uint32_t kFrameMagic = 0x534C4456; // "VDLS"
constexpr uint32_t kFrameVersion = 3;
std::atomic<void (__cdecl*)()> rrCallback{nullptr};
using ToneMapEvent = void (__stdcall*)(int);
ToneMapEvent rrToneMapEvent = nullptr;
std::atomic<uint32_t> rrPostSize[4] = {}; // RR's whole output w, h and whole input w, h (both eyes), for the bridge
constexpr int kCaptureEvent = 0x56440001;
// Bridge 2.9.6, raster route: at CameraEvent.BeforeForwardAlpha, the depth before the transparents (CaptureOpaque).
constexpr int kOpaqueDepthEvent = 5;
constexpr int kApiVersion = 3;
constexpr int kMaxObjectDraws = 4096;
// Unity keeps a released buffer alive while it is referenced here; the bridge
// re-queries native pointers after 300 unseen frames, so this must be longer.
constexpr uint64_t kObjectEvictFrames = 600;

// Mirrors VaMDLSSBridge.cs (explicit byte offsets there). Matrices are
// row-major, Unity/OpenGL clip convention, NON-jittered projections.
#pragma pack(push, 4)
struct FrameInfo {
    uint32_t magic, size, version, flags; // flags: 1 = reset history, 2 = jittered, 4 = native frame (no DLSS SR), 8 = resized for menus
    int64_t serial;
    float jitterX, jitterY;               // image content shift, upright eye pixels, y down
    float prevJitterX, prevJitterY;
    uint32_t eyeWidth, eyeHeight;
    float frameDeltaMs, nearPlane, farPlane, verticalFov;
    float clipToPrevClip[2][16];
    float viewToClip[2][16];
    float clipToView[2][16];
    float viewToWorld[2][16];
};
#pragma pack(pop)
static_assert(sizeof(FrameInfo) == 576, "bridge frame layout must match VaMDLSSBridge.cs");

// Version 2 appends world-space matrices and the object draw list.
#pragma pack(push, 4)
struct FrameExtension {
    float rasterWorldToClip[2][16]; // projection * view Unity rasterized with (jittered), OpenGL clip
    float worldToClip[2][16];       // same without jitter
    float prevWorldToClip[2][16];   // previous frame without jitter (= worldToClip on reset)
    uint64_t draws;                 // ObjectDraw[drawCount], valid during this render event
    int32_t drawCount, drawStride;
    uint32_t objectFlags;           // what the bridge collected: 1 skins, 2 hair, 4 UI, 8 rigid
    uint64_t uiQuads;               // UiQuad[uiQuadCount], valid during this render event
    int32_t uiQuadCount, uiQuadStride;
    uint64_t foldBands;             // ID3D11Buffer*[foldBandCount] (structured float4 per vertex), see kDrawFold
    int32_t foldBandCount;
    // Bridge 2.7.0: per moving rigid renderer, its localToWorld this frame and
    // the previous one (row-major float[16] each), see kDrawRigid. Zero before.
    uint64_t rigidMatrices;
    int32_t rigidMatrixCount, rigidMatrixStride;
};
struct FrameInfoV3 {
    FrameInfo base;
    FrameExtension ext;
    uint32_t viewCount, reserved;
    uint64_t color; // Desktop RenderTexture native pointer; unused for stereo
};
struct ObjectDraw {
    uint64_t key;          // VaM component instance id
    uint64_t vertexBuffer; // ID3D11Buffer*, structured float3/float4 world positions
    uint64_t indexBuffer;  // ID3D11Buffer* of the drawn Mesh
    uint32_t indexStart, indexCount;
    int32_t baseVertex;
    uint32_t flags;        // 1 = 32-bit indices, 2 = hair (see kDrawHair), 4 = fold band (kDrawFold), 8 = rigid (kDrawRigid),
                           // 16 = skinned (kDrawSkinned)
    float offset[3];       // translation of the Graphics.DrawMesh matrix
    uint32_t stride, vertexCount;
    // 1-based: kDrawFold into FrameExtension::foldBands, kDrawRigid into rigidMatrices
    // (kDrawSkinned: its first bone's pair there).
    uint32_t tableIndex;
};
// Hair draws: vertexBuffer is the TessRenderParticle buffer (56-byte
// structured, world position first, RootIndex last), indexBuffer is unused,
// indexCount is the number of valid points and indexStart the points per
// strand (0 when unknown: strands are then split where RootIndex changes).
constexpr uint32_t kDrawHair = 2;
constexpr uint32_t kHairStride = 56;
// A female face submesh: tableIndex names its per-vertex buffer, one float4 per
// vertex of the drawn mesh: (x, y) of the unmorphed base face relative to the
// mouth corners' midpoint in mouth widths (x signed, y up), gate 0..1, 0.
constexpr uint32_t kDrawFold = 4;
constexpr int kMaxFoldBands = 64;
// A moving Unity MeshRenderer (prop, CUA, toy): vertexBuffer is its mesh's
// vertex buffer (stream 0, float3 position first; stride = size / vertexCount,
// stride is 0 here), indexBuffer its index buffer, offset unused.
constexpr uint32_t kDrawRigid = 8;
// A moving Unity SkinnedMeshRenderer (animated CUA). key is its mesh's
// instance id, vertexBuffer the bridge's float3 bind-pose positions in its
// own memory and offset[0..1] (as a uint64) its BoneWeight array (float
// weight[4], int bone[4]), vertexCount of each, valid during the render
// event; both are copied once per mesh. offset[2] (as a uint32) is the bone
// count: that many matrix pairs from tableIndex on are the bones' skinning
// matrices this frame and the previous one. indexBuffer is the mesh's own.
constexpr uint32_t kDrawSkinned = 16;
// Matrix pairs: one per moving rigid renderer, one per bone of a moving skinned one.
constexpr int kMaxRigidMatrices = 4096;
constexpr int32_t kRigidMatrixStride = 128;
// World-space Unity UI (VaM menus, scene panels): one visible element's
// rectangle as three world corners (origin, +x, +y), for this frame and with
// its canvas's previous transform. UI shaders write no depth of their own.
struct UiQuad {
    float current[3][3];
    float previous[3][3];
    uint32_t flags;        // kUiOnTop: drawn by an overlay camera, over the scene
    uint32_t key;          // root canvas instance id
};
constexpr uint32_t kUiOnTop = 1;
#pragma pack(pop)
static_assert(sizeof(FrameExtension) == 448, "bridge frame extension layout");
static_assert(offsetof(FrameExtension, uiQuads) == 404 && offsetof(FrameExtension, uiQuadStride) == 416,
    "bridge UI fields: frame bytes 980 / 988 / 992 in VaMDLSSBridge.cs");
static_assert(offsetof(FrameExtension, foldBands) == 420 && offsetof(FrameExtension, foldBandCount) == 428,
    "bridge fold band fields: frame bytes 996 / 1004 in VaMDLSSBridge.cs");
static_assert(offsetof(FrameExtension, rigidMatrices) == 432 && offsetof(FrameExtension, rigidMatrixStride) == 444,
    "bridge rigid matrix fields: frame bytes 1008 / 1016 / 1020 in VaMDLSSBridge.cs");
static_assert(offsetof(ObjectDraw, tableIndex) == 60, "bridge object draw: table index at byte 60");
static_assert(sizeof(FrameInfoV3) == 1040, "bridge frame layout must match VaMDLSSBridge.cs");
static_assert(sizeof(ObjectDraw) == 64, "bridge object draw layout must match VaMDLSSBridge.cs");
static_assert(sizeof(UiQuad) == 80, "bridge UI rectangle layout must match VaMDLSSBridge.cs");

struct GuideConstants {
    float clipToPrevClip[16];
    int32_t origin[2], size[2];
    float jitterNdc[2], jitterDelta[2];
    int32_t flip, depthMax[2], objectMotion;
    float hairBias;          // DLSS bias-current-colour mask value on hair pixels
    int32_t uiDepthMerge;    // DLSSUIMotion: the UI's depth goes into the guide depth
    float nrFoldCut;         // NasolabialReduction / 100: NR's result taken back by this at the band's core
    float pad;
};
static_assert(sizeof(GuideConstants) == 128, "guide constant layout");

struct ObjectConstants {
    float rasterClip[16];   // GPU convention: y flipped, reversed Z, jittered
    float currentClip[16];  // OpenGL convention, no jitter
    float previousClip[16];
    float offset[4], previousOffset[4];
    float depthParams[4];   // GL P22, P23, absolute and relative depth tolerance
    float person[4];        // x = 1: a person's skin or clothing, also matched against the depth with the transparents
};
static_assert(sizeof(ObjectConstants) == 256, "object constant layout");

struct HairConstants {
    float rasterClip[16];   // as ObjectConstants
    float currentClip[16];
    float previousClip[16];
    float camera[4];        // world position of this eye, w = ribbon half width (metres)
    float depthParams[4];   // GL P22, P23, absolute and relative depth tolerance
    uint32_t count, perStrand, previousValid;
    float maxSegment;       // longer guide segments are not hair (metres)
};
static_assert(sizeof(HairConstants) == 240, "hair constant layout");

struct UiConstants {
    float rasterClip[16];   // as ObjectConstants
    float currentClip[16];
    float previousClip[16];
    float depthParams[4];   // P22, P23, tolerance (metres), relative tolerance
};
static_assert(sizeof(UiConstants) == 208, "UI constant layout");

// vam_fsr1.hlsl with VAM_UI_BYPASS (vam_fsr_ui_bypass.h).
struct UiBypassConstants {
    uint32_t easu[4][4];
    float maskMap[4];        // output pixel centre -> UI mask uv: (p + 0.5) * xy + zw
    uint32_t outputSize[2];
    uint32_t linearInput, pad;
};
static_assert(sizeof(UiBypassConstants) == 96, "UI bypass constant layout");

// Ribbon half width and depth tolerance for hair. Guide strands of VaM hair
// sit millimetres to a couple of centimetres apart, and the strands drawn
// between them are interpolated from them.
constexpr float kHairHalfWidth = 0.010f;
constexpr float kHairDepthAbsolute = 0.025f;
constexpr float kHairDepthRelative = 0.02f;
constexpr float kHairMaxSegment = 0.12f;
constexpr int kHairQueries = 4;
// World-space UI is kept in front of the scene, or on it within this
// tolerance (a panel hung on a wall); behind it, the scene hides the UI.
constexpr float kUiDepthAbsolute = 0.02f;
constexpr float kUiDepthRelative = 0.02f;
constexpr int kMaxUiQuads = 4096;
constexpr int kUiQueries = 4;

constexpr char objectShader[] = R"(
#if !defined(RIGID) && !defined(SKINNED)
StructuredBuffer<VERTEX> currentVertices : register(t0);
StructuredBuffer<VERTEX> previousVertices : register(t1);
#endif
#ifdef SKINNED
// Per bone, rows 0-3 of this frame's skinning matrix, then rows 0-3 of the
// previous frame's (kDrawSkinned); offset.xy = asuint first bone, bone count.
StructuredBuffer<float4> boneRows : register(t1);
#endif
Texture2D<float> sceneDepth : register(t2);
// The depth with the transparents in it (the same as sceneDepth when the bridge gave no depth from before them).
Texture2D<float> fullDepth : register(t5);
// 11-06: the camera color before the transparents and with them (bound when person.y = 1).
Texture2D<float4> opaqueColor : register(t6);
Texture2D<float4> fullColor : register(t7);
cbuffer Draw : register(b0) {
    row_major float4x4 rasterClip;
    row_major float4x4 currentClip;
    row_major float4x4 previousClip;
    float4 offset;
    float4 previousOffset;
    float4 depthParams;
    float4 person;
};
#ifdef FOLD
// Female face submesh: the unmorphed base face per vertex, (x, y) relative
// to the mouth corners in mouth widths, z = gate (see kDrawFold).
StructuredBuffer<float4> foldBase : register(t3);
#endif
struct Fragment {
    float4 position : SV_Position;
    float4 current : TEXCOORD0;
    float4 previous : TEXCOORD1;
#ifdef FOLD
    float3 fold : TEXCOORD2;
#endif
};
#ifdef RIGID
// A rigid mesh from its own vertex buffer: the three matrices already carry
// this frame's localToWorld (raster, current) and the previous one (previous).
Fragment vertexMain(float3 position : POSITION) {
    float4 local = float4(position, 1.0);
    Fragment o;
    o.position = mul(rasterClip, local);
    o.current = mul(currentClip, local);
    o.previous = mul(previousClip, local);
    return o;
}
#elif defined(SKINNED)
// A skinned mesh: the bind pose and bone weights the bridge took from the
// mesh, skinned with this frame's and the previous frame's bone matrices.
// Unweighted vertices (a SkinnedMeshRenderer without bones) follow bone 0.
float3 Skin(float4 local, float4 weight, int4 bone, uint half) {
    uint first = asuint(offset.x), count = asuint(offset.y);
    float3 sum = 0.0;
    [unroll] for (int k = 0; k < 4; ++k) {
        uint row = (first + min((uint)bone[k], count - 1)) * 8 + half;
        sum += weight[k] * float3(dot(boneRows[row], local), dot(boneRows[row + 1], local), dot(boneRows[row + 2], local));
    }
    return sum;
}
Fragment vertexMain(float3 position : POSITION, float4 weight : BLENDWEIGHT, int4 bone : BLENDINDICES) {
    float total = dot(weight, 1.0);
    if (!(total > 1e-6)) { weight = float4(1.0, 0.0, 0.0, 0.0); bone = 0; total = 1.0; }
    weight /= total;
    float4 local = float4(position, 1.0);
    float4 world = float4(Skin(local, weight, bone, 0), 1.0);
    float4 before = float4(Skin(local, weight, bone, 4), 1.0);
    Fragment o;
    o.position = mul(rasterClip, world);
    o.current = mul(currentClip, world);
    o.previous = mul(previousClip, before);
    return o;
}
#else
Fragment vertexMain(uint id : SV_VertexID) {
    float4 world = float4(currentVertices[id].xyz + offset.xyz, 1.0);
    float4 before = float4(previousVertices[id].xyz + previousOffset.xyz, 1.0);
    Fragment o;
    o.position = mul(rasterClip, world);
    o.current = mul(currentClip, world);
    o.previous = mul(previousClip, before);
#ifdef FOLD
    o.fold = foldBase[id].xyz;
#endif
    return o;
}
#endif
#ifdef FOLD
// The nasolabial fold on the Genesis 2 Female base face, from the top of the
// nose wing down to beside the mouth corner (x mirrored), in mouth widths
// from the corners' midpoint (scratchpad band on female1-BaseF.obj: 13.5 to
// 31 mm out, 1632 to 1583 mm up; mouth 42.1 mm wide at 1598.9 mm).
static const float2 kFold[7] = {
    float2(0.3204, 0.7850), float2(0.3797, 0.6189), float2(0.4509, 0.4053), float2(0.5340, 0.1680),
    float2(0.6170, -0.0456), float2(0.6882, -0.2354), float2(0.7357, -0.3778)};
static const float kFoldCore = 0.0475;  // full weight within 2 mm of the line
static const float kFoldCheek = 0.1661; // fades out 7 mm away on the cheek side (its shading)
static const float kFoldLip = 0.0831;   // and 3.5 mm away on the upper-lip side
static const float kFoldEnd = 0.0949;   // 4 mm past either end
float FoldWeight(float2 q) {
    float best = 1e9, side = 0.0;
    bool end = false;
    [unroll] for (int k = 0; k < 6; ++k) {
        float2 a = kFold[k], ab = kFold[k + 1] - a;
        float t = saturate(dot(q - a, ab) / dot(ab, ab));
        float2 off = q - (a + t * ab);
        float d = length(off);
        if (d < best) {
            best = d;
            side = ab.x * off.y - ab.y * off.x; // > 0: out and up, the cheek
            end = (k == 0 && t <= 0.0) || (k == 5 && t >= 1.0);
        }
    }
    float w = 1.0 - saturate((best - kFoldCore) / ((side > 0.0 ? kFoldCheek : kFoldLip) - kFoldCore));
    w = w * w * (3.0 - 2.0 * w);
    if (end) {
        float f = 1.0 - saturate((best - kFoldCore) / kFoldEnd);
        w = min(w, f * f * (3.0 - 2.0 * f));
    }
    return w;
}
#endif
// Distance along the view axis from Unity reversed-Z hardware depth.
float Distance(float d) { return depthParams.y / ((1.0 - 2.0 * d) + depthParams.x); }
struct Output {
    float2 motion : SV_Target0;
    float hair : SV_Target1;
    float fold : SV_Target2;  // nasolabial-fold band weight (0 on all other skin and clothing)
    float depth : SV_Target3; // a person's: the rendered depth it matched, for the guides (0: the scene depth stays)
};
Output pixelMain(Fragment i) {
    float scene = sceneDepth.Load(int3(i.position.xy, 0));
    float own = Distance(i.position.z);
    float tolerance = max(depthParams.z, depthParams.w * own);
    // Only the surface the scene actually shows; tessellation and blend
    // shapes keep the rendered surface within a few millimetres of ours.
    bool shown = abs(Distance(scene) - own) <= tolerance;
    float matched = 0.0;
    if (person.x > 0.5) {
        // 11-05 (user: clothing must always have its depth and motion): sceneDepth is from before the transparents, so a see-through
        // thing in front of a person does not take the person's pixels. VaM's see-through clothing is drawn in the
        // transparent queue with depth writes: a person's skin and clothing also match the depth with the transparents.
        float full = fullDepth.Load(int3(i.position.xy, 0));
        if (abs(Distance(full) - own) <= tolerance) {
            // 11-06 (user: 半透明柔边不止头发有……这部分也是“不该存在的透明”): in the depth with the transparents only,
            // the part takes the pixel where the transparents visibly changed it - by a quarter of what showed before
            // them. A soft see-through edge (hair layers, hems, lace, cards) writes depth where it barely shows.
            bool visible = true;
            if (!shown && person.y > 0.5) {
                float3 before = max(opaqueColor.Load(int3(i.position.xy, 0)).rgb, 0);
                float3 after = max(fullColor.Load(int3(i.position.xy, 0)).rgb, 0);
                visible = length(after - before) >= 0.25 * max(length(before), 0.05);
            }
            if (visible) { matched = full; shown = true; }
        }
        else if (shown) matched = scene;
    }
    if (!shown) discard;
    if (i.current.w <= 1e-6 || i.previous.w <= 1e-6) discard;
    Output o;
    o.depth = matched;
    o.motion = i.previous.xy / i.previous.w - i.current.xy / i.current.w; // OpenGL NDC, current -> previous
    o.hair = 0.0;
#ifdef FOLD
    // Per pixel from the interpolated base position: the face mesh is far
    // coarser than the band. The gate fades it at the Face material's edge.
    o.fold = FoldWeight(float2(abs(i.fold.x), i.fold.y)) * saturate(i.fold.z * 4.0);
#else
    o.fold = 0.0;
#endif
    return o;
}
)";

// VaM physics hair: one camera-facing ribbon per guide segment (six vertices,
// no index buffer), from this and the previous frame's guide points.
constexpr char hairShader[] = R"(
struct Particle {
    float3 position;
    float3 velocity;
    float3 lightCenter;
    float3 color;
    float interpolation;
    int rootIndex;
};
StructuredBuffer<Particle> currentParticles : register(t0);
StructuredBuffer<Particle> previousParticles : register(t1);
Texture2D<float> sceneDepth : register(t2);
Texture2D<float> fullDepth : register(t5); // as in the skin pass
cbuffer Hair : register(b0) {
    row_major float4x4 rasterClip;
    row_major float4x4 currentClip;
    row_major float4x4 previousClip;
    float4 camera;
    float4 depthParams;
    uint count;
    uint perStrand;
    uint previousValid;
    float maxSegment;
};
struct Fragment {
    float4 position : SV_Position;
    float4 current : TEXCOORD0;
    float4 previous : TEXCOORD1;
};
Fragment vertexMain(uint id : SV_VertexID) {
    Fragment o;
    // Outside the clip volume: both triangles of a rejected segment are dropped.
    o.position = float4(2.0, 2.0, 2.0, 1.0);
    o.current = o.previous = float4(0.0, 0.0, 0.0, 1.0);
    uint segment = id / 6, corner = id % 6;
    if (segment + 1 >= count) return o;
    if (perStrand != 0 && segment % perStrand == perStrand - 1) return o; // last point of a strand
    Particle a = currentParticles[segment];
    Particle b = currentParticles[segment + 1];
    if (perStrand == 0 && a.rootIndex != b.rootIndex) return o;
    float3 pa = previousValid != 0 ? previousParticles[segment].position : a.position;
    float3 pb = previousValid != 0 ? previousParticles[segment + 1].position : b.position;
    float3 d = b.position - a.position;
    float len = length(d);
    if (!(len > 1e-5 && len < maxSegment)) return o;
    if (!(length(pa - a.position) < 1.0 && length(pb - b.position) < 1.0)) return o; // NaN or a teleport
    float3 side = cross(d, camera.xyz - 0.5 * (a.position + b.position));
    float sideLen = length(side);
    if (!(sideLen > 1e-9)) return o;
    side *= camera.w / sideLen;
    // Triangles (a-, a+, b-) and (b-, a+, b+).
    bool tip = corner == 2 || corner == 3 || corner == 5;
    float s = (corner == 1 || corner == 4 || corner == 5) ? 1.0 : -1.0;
    float4 world = float4((tip ? b.position : a.position) + s * side, 1.0);
    // The same offset on last frame's points: the ribbon moves as its guide did.
    float4 before = float4((tip ? pb : pa) + s * side, 1.0);
    o.position = mul(rasterClip, world);
    o.current = mul(currentClip, world);
    o.previous = mul(previousClip, before);
    return o;
}
float Distance(float d) { return depthParams.y / ((1.0 - 2.0 * d) + depthParams.x); }
struct Output {
    float2 motion : SV_Target0;
    float hair : SV_Target1;
    float depth : SV_Target2; // the rendered depth it matched (see the skin pass)
};
Output pixelMain(Fragment i) {
    float scene = sceneDepth.Load(int3(i.position.xy, 0));
    float own = Distance(i.position.z);
    float tolerance = max(depthParams.z, depthParams.w * own);
    // The drawn strands lie around their guides, within the hair's volume.
    // 11-06 (user: 头发的外轮廓很不稳定 / 在抽动): hair matches only the depth before the transparents, as before 11-05
    // 22:02. Its soft see-through outer layers (in the transparent queue, writing depth) take no hair motion or depth.
    float matched = scene;
    if (!(abs(Distance(scene) - own) <= tolerance)) discard;
    if (i.current.w <= 1e-6 || i.previous.w <= 1e-6) discard;
    Output o;
    o.depth = matched;
    o.motion = i.previous.xy / i.previous.w - i.current.xy / i.current.w;
    o.hair = 1.0;
    return o;
}
)";

// World-space UI: two triangles per element rectangle (six vertices, no
// index buffer), moved by its canvas between the two frames.
constexpr char uiShader[] = R"(
struct Quad {
    float3 c0, c1, c2;   // this frame: origin, +x corner, +y corner
    float3 p0, p1, p2;   // previous frame
    uint flags;          // 1 = overlay camera; 2 = normalized screen-space corners
    uint key;
};
StructuredBuffer<Quad> quads : register(t0);
Texture2D<float> sceneDepth : register(t2);
cbuffer Ui : register(b0) {
    row_major float4x4 rasterClip;
    row_major float4x4 currentClip;
    row_major float4x4 previousClip;
    float4 depthParams;
};
struct Fragment {
    float4 position : SV_Position;
    float4 current : TEXCOORD0;
    float4 previous : TEXCOORD1;
    nointerpolation uint flags : TEXCOORD2;
};
Fragment vertexMain(uint id : SV_VertexID) {
    Quad q = quads[id / 6];
    uint corner = id % 6;
    // Triangles (origin, +x, +y) and (+y, +x, opposite corner).
    float u = (corner == 1 || corner == 4 || corner == 5) ? 1.0 : 0.0;
    float v = (corner == 2 || corner == 3 || corner == 5) ? 1.0 : 0.0;
    float4 world = float4(q.c0 + u * (q.c1 - q.c0) + v * (q.c2 - q.c0), 1.0);
    float4 before = float4(q.p0 + u * (q.p1 - q.p0) + v * (q.p2 - q.p0), 1.0);
    Fragment o;
    o.position = mul(rasterClip, world);
    o.current = mul(currentClip, world);
    o.previous = mul(previousClip, before);
    if((q.flags&2)!=0) {
        o.position=float4(world.xy*2-1,world.z,1);
        o.current=float4(world.xy*2-1,0,1);o.previous=float4(before.xy*2-1,0,1);
    }
    o.flags = q.flags;
    return o;
}
float Distance(float d) { return depthParams.y / ((1.0 - 2.0 * d) + depthParams.x); }
struct Output {
    float2 motion : SV_Target0;
    float hair : SV_Target1;
    float depth : SV_Target2;
};
Output pixelMain(Fragment i) {
    if ((i.flags & 1) == 0) {
        // In front of the scene or on it; behind it the scene hides the UI.
        float scene = Distance(sceneDepth.Load(int3(i.position.xy, 0)));
        float own = Distance(i.position.z);
        if (!(own <= scene + max(depthParams.z, depthParams.w * own))) discard;
    }
    if (i.current.w <= 1e-6 || i.previous.w <= 1e-6) discard;
    Output o;
    o.motion = i.previous.xy / i.previous.w - i.current.xy / i.current.w; // OpenGL NDC, current -> previous
    o.hair = 0.0;
    o.depth = (i.flags&1)!=0?max(i.position.z,.999f):i.position.z;
    return o;
}
)";

constexpr char guideShader[] = R"(
Texture2D<float> sceneDepth : register(t0);
Texture2D<float2> objectMotionNdc : register(t1);
Texture2D<float> objectHair : register(t2);
Texture2D<float> objectUiDepth : register(t3);
Texture2D<float> objectFold : register(t4);
Texture2D<float> objectDepth : register(t5); // a person's matched depth (0 where none claimed the pixel)
RWTexture2D<float> outDepth : register(u0);
RWTexture2D<float2> outMotion : register(u1);
RWTexture2D<float2> outMotionJittered : register(u2);
RWTexture2D<float2> outMotionHalf : register(u3);
RWTexture2D<float2> outMotionJitteredHalf : register(u4);
RWTexture2D<float> outHairMask : register(u5);
RWTexture2D<float> outUiMask : register(u6);
RWTexture2D<float4> outNrControl : register(u7);
cbuffer Guide : register(b0) {
    row_major float4x4 clipToPrevClip;
    int2 origin; int2 size;
    float2 jitterNdc; float2 jitterDelta;
    int flip; int2 depthMax; int objectMotion;
    float hairBias; int uiDepthMerge; float nrFoldCut; float pad;
};
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)size.x || id.y >= (uint)size.y) return;
    int2 p = int2(id.xy);
    // Upright eye pixel -> submitted texture pixel (Unity D3D eye images are stored flipped).
    int2 source = int2(origin.x + p.x, flip != 0 ? origin.y + size.y - 1 - p.y : origin.y + p.y);
    source = clamp(source, int2(0, 0), depthMax);
    float d = sceneDepth.Load(int3(source, 0));
    // The headset's hidden area (outside the lenses): Unity draws its occlusion mesh black at the
    // near plane (reversed Z 1). It is fixed to the display, so no motion and far depth - with the
    // camera's, a 1 cm near plane gave it hundreds of pixels of parallax on any head translation
    // and frame generation drew it across the view as black bars.
    bool hidden = pad < 2 && d >= 0.9999;
    // World-space UI drawn in front of the scene (0 elsewhere; reversed Z, nearer is larger).
    float ui = objectMotion != 0 ? objectUiDepth.Load(int3(source, 0)) : 0.0;
    if((uint(pad)&1u)!=0) {
        // PT replaces all motion formats later in this same frame. Preserve
        // the raw hidden-area sentinel until its composite/guide merge has
        // consumed it; clearing it here loses the headset mask.
        outDepth[p]=d;outUiMask[p]=ui>0?1:0;outHairMask[p]=0;
        float fold=objectMotion!=0 && ui<=0?objectFold.Load(int3(source,0)):0;
        float keep=1-nrFoldCut*saturate(fold);outNrControl[p]=float4(1,keep,keep,1);
        return;
    }
    // 11-05: d is from before the transparents when the bridge gave both depths; where a person's skin, clothing or
    // hair claimed the pixel, its own depth - see-through clothing included (reversed Z: the nearer is larger).
    if (objectMotion != 0) d = max(d, objectDepth.Load(int3(source, 0)));
    if (uiDepthMerge != 0) d = max(d, ui);
    outDepth[p] = hidden ? 0.0 : d;
    // OpenGL NDC of this pixel's surface point without this frame's jitter.
    float2 ndc = float2((p.x + 0.5) / size.x * 2.0 - 1.0, 1.0 - (p.y + 0.5) / size.y * 2.0) - jitterNdc;
    // Unity reversed Z on D3D: d = 0.5 - 0.5 * zGL.
    float4 previous = mul(clipToPrevClip, float4(ndc, 1.0 - 2.0 * d, 1.0));
    float2 motion = 0;
    if (previous.w > 1e-7) {
        float2 previousNdc = previous.xy / previous.w;
        motion = float2((previousNdc.x - ndc.x) * 0.5 * size.x, (ndc.y - previousNdc.y) * 0.5 * size.y);
        if (any(isnan(motion)) || any(abs(motion) > 16384.0)) motion = 0;
    }
    float hair = 0, fold = 0;
    if (objectMotion != 0) {
        // Skinned VaM geometry and hair drawn this frame: its own motion, not the camera's.
        float2 o = objectMotionNdc.Load(int3(source, 0));
        if (o.x < 1e20) {
            float2 m = float2(o.x * 0.5 * size.x, -o.y * 0.5 * size.y);
            if (!any(isnan(m)) && all(abs(m) < 16384.0)) motion = m;
        }
        if (objectHair.Load(int3(source, 0)) > 0.5) hair = hairBias;
        // Hair over the face never matched the skin's depth; a menu in front does not hide the skin pass.
        if (ui <= 0.0) fold = objectFold.Load(int3(source, 0));
    }
    float2 jitterMotion = jitterDelta;
    if (hidden) motion = jitterMotion = 0;
    outHairMask[p] = hair;                     // DLSS bias-current-colour mask (0 = off)
    outUiMask[p] = ui > 0.0 ? 1.0 : 0.0;         // DLSS SR UI bypass
    // NR's composite keeps its result by G (1 = NR as set; dx11_nr_prepass.cpp).
    float keep = 1.0 - nrFoldCut * saturate(fold);
    outNrControl[p] = float4(1.0, keep, keep, 1.0);
    outMotion[p] = motion;                       // DLSS: current -> previous, jitter removed
    outMotionJittered[p] = motion + jitterMotion; // NR: how the rendered (jittered) pixels moved
    outMotionHalf[p] = motion;                   // frame generation (shareable RG16F)
    outMotionJitteredHalf[p] = motion + jitterMotion; // NR (shareable RG16F)
}
)";

// NR after the upscaler: the render-size guides resampled to the output size.
// An output pixel shows the unjittered point Unity drew into render pixel
// (p + 0.5) * scale + jitter; its motion is in output pixels without jitter,
// as the upscaled image moves.
constexpr char outputGuideShader[] = R"(
Texture2D<float> renderDepth : register(t0);
Texture2D<float2> renderMotion : register(t1);
RWTexture2D<float> outDepth : register(u0);
RWTexture2D<float2> outMotion : register(u1);
cbuffer Map : register(b0) {
    float2 scale; float2 offset;
    float2 motionScale; int2 renderMax;
    uint2 outputSize; uint2 pad;
};
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= outputSize)) return;
    int2 r = clamp(int2(floor((float2(id.xy) + 0.5) * scale + offset)), int2(0, 0), renderMax);
    outDepth[id.xy] = renderDepth.Load(int3(r, 0));
    outMotion[id.xy] = renderMotion.Load(int3(r, 0)) * motionScale;
}
)";
struct OutputGuideConstants {
    float scale[2], offset[2], motionScale[2];
    int32_t renderMax[2];
    uint32_t outputSize[2], pad[2];
};
static_assert(sizeof(OutputGuideConstants) == 48, "output guide constant layout");

void Check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char message[256];
        sprintf_s(message, "%s: HRESULT 0x%08lx", what, static_cast<unsigned long>(hr));
        throw std::runtime_error(message);
    }
}
void CheckNgx(NVSDK_NGX_Result result, const char* what) {
    if (NVSDK_NGX_FAILED(result)) {
        char message[256];
        sprintf_s(message, "%s: NGX 0x%08x", what, static_cast<unsigned>(result));
        throw std::runtime_error(message);
    }
}

std::wstring Environment(const wchar_t* key) {
    wchar_t value[32768]{};
    const DWORD count = GetEnvironmentVariableW(key, value, static_cast<DWORD>(std::size(value)));
    return count && count < std::size(value) ? std::wstring(value, count) : std::wstring();
}

constexpr char kNgxProject[] = "6d1c2f0a-4b8e-4f62-9d3a-7a5e8c1b2f40";
constexpr char kNgxEngineVersion[] = "VaM-1.22-Unity2018.1";

// D3D11On12 (vam_on12.h): DLSS SR is NGX D3D12 there, and NGX D3D12 keeps one setup per process with the first
// init's DLL folders only. OFXR's DLSS-G init lists the DLSS-G folder alone (SR then reports 0xbad00004 and its
// feature fails 0xbad0000b, as a probe that initialised them in that order showed), so this runs before it, once, with DLSS SR's
// and DLSS-G's folders; OFXR's own init afterwards still succeeds and both features are available.
bool PrepareNgx12Folders() {
    static std::mutex onceMutex;
    static bool done = false, ready = false;
    std::lock_guard lock(onceMutex);
    if (done) return ready;
    done = true;
    const auto device = VamOn12ProcessDevice();
    if (!device) return false;
    const auto sr = Environment(L"VAM_DLSS_RUNTIME_PATH"), fg = Environment(L"OFXR_DLSSG_RUNTIME_PATH");
    std::vector<const wchar_t*> paths;
    if (!sr.empty()) paths.push_back(sr.c_str());
    if (!fg.empty()) paths.push_back(fg.c_str());
    if (sr.empty()) {
        OOVR_LOG("VAM DLSS: D3D11On12 without VAM_DLSS_RUNTIME_PATH; NGX D3D12 left to OFXR");
        return false;
    }
    auto logs = Environment(L"VAM_DLSS_LOG");
    if (logs.empty()) logs = sr;
    NVSDK_NGX_FeatureCommonInfo common{};
    common.PathListInfo.Path = paths.data();
    common.PathListInfo.Length = static_cast<unsigned>(paths.size());
    common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    const NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(kNgxProject, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kNgxEngineVersion,
        logs.c_str(), device.Get(), &common);
    ready = !NVSDK_NGX_FAILED(r);
    OOVR_LOGF("VAM DLSS: NGX D3D12 set up first for D3D11On12 with the DLSS SR%s folder%s: 0x%08x", fg.empty() ? "" : " and DLSS-G",
        fg.empty() ? "" : "s", static_cast<unsigned>(r));
    return ready;
}

// Depth-stencil formats -> copyable typeless resource + readable depth SRV.
bool DepthFormats(DXGI_FORMAT format, DXGI_FORMAT& typeless, DXGI_FORMAT& view) {
    switch (format) {
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT:
        typeless = DXGI_FORMAT_R24G8_TYPELESS; view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; return true;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        typeless = DXGI_FORMAT_R32G8X24_TYPELESS; view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; return true;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT:
        typeless = DXGI_FORMAT_R32_TYPELESS; view = DXGI_FORMAT_R32_FLOAT; return true;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM:
        typeless = DXGI_FORMAT_R16_TYPELESS; view = DXGI_FORMAT_R16_UNORM; return true;
    default: return false;
    }
}

// The upscaler's typeless color textures -> a typed format NGX can read/write.
DXGI_FORMAT TypedColor(DXGI_FORMAT typeless) {
    switch (typeless) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

NVSDK_NGX_PerfQuality_Value QualityFor(UINT w, UINT h, UINT ow, UINT oh) {
    // Only selects DLSS's internal tuning; the render size itself comes from
    // RenderScale, which DLSS accepts as an arbitrary input/output ratio.
    // DLAA needs the input and output sizes to be the same (DLSS guide 3.12);
    // a render a pixel or two short of the output (Unity's rounding) is
    // Quality.
    if (w == ow && h == oh) return NVSDK_NGX_PerfQuality_Value_DLAA;
    const float ratio = static_cast<float>(w) / std::max(1u, ow);
    if (ratio >= .62f) return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (ratio >= .54f) return NVSDK_NGX_PerfQuality_Value_Balanced;
    if (ratio >= .42f) return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
}
const char* QualityName(NVSDK_NGX_PerfQuality_Value q) {
    switch (q) {
    case NVSDK_NGX_PerfQuality_Value_DLAA: return "DLAA";
    case NVSDK_NGX_PerfQuality_Value_MaxQuality: return "Quality";
    case NVSDK_NGX_PerfQuality_Value_Balanced: return "Balanced";
    case NVSDK_NGX_PerfQuality_Value_MaxPerf: return "Performance";
    default: return "UltraPerformance";
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

enum StateBits : int {
    kStateSelected = 1,   // DLSS is the selected upscaler and the module is healthy: jitter
    kStateEvaluating = 2, // a DLSS evaluation succeeded within the last second
    kStateFailed = 4,     // permanently disabled for this process
    kStateCaptureProblem = 8,
    kStateObjectMotion = 16, // send the VaM skin/clothing draw list
    kStateHairMotion = 32,   // send the VaM physics hair guide buffers
    kStateUiMotion = 64,     // send the world-space UI element rectangles
    kStateMenuNative = 128,  // DLSSMenuNative: while VaM's menus are open, render native frames
    kStateNrFold = 256,      // NasolabialReduction > 0: send female faces' fold band buffers (removed in v17, never set)
    kStateSrOff = 512,       // DLSSPreset=Off: render native frames (VaMDLSS_GetState, live)
    kStateRigidMotion = 1024, // send moving MeshRenderers of non-Person atoms (bridge 2.7.0)
    kStateSkinnedMotion = 2048, // kDrawSkinned is understood: send moving SkinnedMeshRenderers of CUAs (bridge 2.8.0)
    kStatePathRendering = 32768,
};

// Unity's D3D projection for render textures: y flipped, OpenGL z remapped to
// reversed [1 (near), 0 (far)] - GL.GetGPUProjectionMatrix(p, true).
void GpuClip(const float* gl, float* out) {
    for (int c = 0; c < 4; ++c) {
        out[c] = gl[c];
        out[4 + c] = -gl[4 + c];
        out[8 + c] = 0.5f * gl[12 + c] - 0.5f * gl[8 + c];
        out[12 + c] = gl[12 + c];
    }
}

Ptr<ID3DBlob> CompileShader(const char* source, const char* name, const char* entry, const char* target, const D3D_SHADER_MACRO* macros) {
    Ptr<ID3DBlob> blob, errors;
    const HRESULT hr = D3DCompile(source, strlen(source), name, macros, nullptr, entry, target,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
    if (FAILED(hr)) throw std::runtime_error(errors ? static_cast<const char*>(errors->GetBufferPointer()) : "shader compilation failed");
    return blob;
}

std::atomic<VamDlss::Impl*> instance{nullptr};

// OFXR 0.7443 OFXR_PublishD3D11GuidesV3 (xrfg/provider_api.hpp): matrices in
// D3D clip convention (y up, z 1 at near with depth_inverted), row-major for
// row vectors, without jitter; motion in render pixels.
struct OfxrGuidePublicationV3 {
    uint32_t structSize = sizeof(OfxrGuidePublicationV3);
    uint32_t apiVersion = 3;
    uint64_t swapchain = 0;
    ID3D11Texture2D* depth = nullptr;
    ID3D11Texture2D* motionVectors = nullptr;
    uint32_t width = 0, height = 0;
    uint64_t serial = 0;
    int32_t reset = 0;
    int32_t depthInverted = 1;
    float motionScaleX = 1.f, motionScaleY = 1.f;
    float jitterX = 0.f, jitterY = 0.f;
    float frameTimeDeltaMs = 16.6667f;
    float cameraNear = 0.1f, cameraFar = 1000.f;
    float cameraFov = 0.f, cameraAspect = 1.f;
    float clipToPrevClip[16] = {};
    float prevClipToClip[16] = {};
    float viewToClip[16] = {};
    float clipToView[16] = {};
    float cameraPosition[3] = {};
    float cameraUp[3] = {};
    float cameraRight[3] = {};
    float cameraForward[3] = {};
};
static_assert(sizeof(OfxrGuidePublicationV3) == 400, "OFXR_D3D11GuidePublicationV3 layout");
struct OfxrGuidePublicationV4 { OfxrGuidePublicationV3 base; uint32_t streamKind=1, reserved=0; };
static_assert(sizeof(OfxrGuidePublicationV4)==408,"desktop guide layout");
using OfxrGetProviderApiVersionV3 = uint32_t (*)();
using OfxrPublishD3D11GuidesV3 = int (*)(const OfxrGuidePublicationV3*);
using OfxrPrepareFrameGenerationV3 = int (*)();

// Row-major 4x4 for column vectors.
void Multiply(const float* a, const float* b, float* out) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float sum = 0.f;
            for (int k = 0; k < 4; ++k) sum += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = sum;
        }
}

bool Invert(const float* m, float* out) {
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
        if (std::fabs(a[pivot][c]) < 1e-12) return false;
        if (pivot != c)
            for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[pivot][k]);
        const double scale = 1.0 / a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] *= scale;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = a[r][c];
            for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = static_cast<float>(a[r][c + 4]);
    return true;
}

void Transpose(const float* m, float* out) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[c * 4 + r] = m[r * 4 + c];
}

// Unity camera space (right-handed, -z forward) from D3D view space (+z).
constexpr float kFlipZ[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1};
// OpenGL clip (z -1 near .. 1 far) to reversed D3D clip (z 1 near .. 0 far);
// the guide images are upright, so y is up in both.
constexpr float kGlToD3d[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -0.5f, 0.5f, 0, 0, 0, 1};
constexpr float kD3dToGl[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -2, 1, 0, 0, 0, 1};

void Column(const float* m, int c, float* out, bool normalize, float sign = 1.f) {
    float v[3] = {m[c] * sign, m[4 + c] * sign, m[8 + c] * sign};
    if (normalize) {
        const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (length > 1e-12f)
            for (float& x : v) x /= length;
    }
    std::copy(v, v + 3, out);
}

const char* const kObjectMotionStageNames[1] = {"draw"};
}

struct VamDlss::Impl {
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext> immediate;
    Ptr<ID3D11DeviceContext1> context;
    Ptr<ID3DDeviceContextState> state;
    Ptr<ID3D11ComputeShader> guideCs;
    Ptr<ID3D11Buffer> guideConstants;
    std::mutex mutex;

    // Latest bridge capture (render thread).
    unsigned viewCount = VamViewCount();
    FrameInfo info{};
    FrameExtension ext{};
    bool haveCapture = false;
    ULONGLONG captureTick = 0;
    uint64_t captures = 0;
    Ptr<ID3D11Texture2D> depthCopy;
    Ptr<ID3D11ShaderResourceView> depthSrv;
    D3D11_TEXTURE2D_DESC depthDesc{};
    // 11-05: the depth before the transparents (bridge 2.9.6, kOpaqueDepthEvent) and the one the guides and the
    // non-person motion redraw use: it when it is this frame's, else depthSrv (with the transparents, as before).
    Ptr<ID3D11Texture2D> opaqueCopy;
    Ptr<ID3D11ShaderResourceView> opaqueSrv, sceneSrv;
    D3D11_TEXTURE2D_DESC opaqueDesc{};
    int64_t opaqueSerial = -1;
    int sceneSource = -1;      // logged on change: 1 = before the transparents, 0 = with them
    uint64_t opaqueProblems = 0;
    // 11-06 (user: 半透明柔边不止头发有……这部分也是“不该存在的透明”): the camera color before the transparents (with
    // the opaque depth) and with them (at the capture). A person's part in the depth with the transparents takes the
    // pixel only where the transparents visibly changed it: a soft see-through edge that writes depth but barely shows
    // (hair, hems, lace, cards) takes no depth or motion.
    Ptr<ID3D11Texture2D> opaqueColorCopy, fullColorCopy;
    Ptr<ID3D11ShaderResourceView> opaqueColorSrv, fullColorSrv;
    D3D11_TEXTURE2D_DESC opaqueColorDesc{}, fullColorDesc{};
    int64_t opaqueColorSerial = -1;
    bool colorsThisFrame = false;
    int colorSource = -1;      // logged on change
    // Copies the bound color target into copy (made or remade to match); false when it cannot.
    bool CopyColor(ID3D11RenderTargetView* rtv, Ptr<ID3D11Texture2D>& copy, Ptr<ID3D11ShaderResourceView>& srv,
        D3D11_TEXTURE2D_DESC& have, const char* what) {
        if (!rtv) return false;
        Ptr<ID3D11Resource> resource;
        rtv->GetResource(&resource);
        Ptr<ID3D11Texture2D> texture;
        if (FAILED(resource.As(&texture))) return false;
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.SampleDesc.Count != 1 || desc.ArraySize != 1) return false;
        DXGI_FORMAT view = desc.Format;
        switch (desc.Format) {
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: view = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: view = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: view = DXGI_FORMAT_R8G8B8A8_UNORM; break;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: view = DXGI_FORMAT_B8G8R8A8_UNORM; break;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: view = DXGI_FORMAT_R10G10B10A2_UNORM; break;
        default: break;
        }
        if (!copy || have.Width != desc.Width || have.Height != desc.Height || have.MipLevels != desc.MipLevels ||
            have.Format != desc.Format) {
            copy.Reset(); srv.Reset();
            D3D11_TEXTURE2D_DESC c = desc;
            c.Usage = D3D11_USAGE_DEFAULT;
            c.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            c.CPUAccessFlags = c.MiscFlags = 0;
            if (FAILED(device->CreateTexture2D(&c, nullptr, &copy))) { copy.Reset(); have = {}; return false; }
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = view;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            if (FAILED(device->CreateShaderResourceView(copy.Get(), &sv, &srv))) { copy.Reset(); srv.Reset(); have = {}; return false; }
            have = desc;
            OOVR_LOGF("VAM DLSS %s color copy: %ux%u format=%u", what, desc.Width, desc.Height, desc.Format);
        }
        immediate->CopyResource(copy.Get(), texture.Get());
        return true;
    }
    std::string lastProblem;

    // Object motion (render thread, inside the capture event).
    struct ObjectSource {
        Ptr<ID3D11Buffer> buffer;            // VaM's vertex buffer, referenced so the pointer stays valid
        Ptr<ID3D11ShaderResourceView> srv;
        Ptr<ID3D11Buffer> previous;          // copy taken after last frame's pass
        Ptr<ID3D11ShaderResourceView> previousSrv;
        bool previousValid = false, invalid = false;
        float offset[3]{}, previousOffset[3]{};
        UINT byteWidth = 0, stride = 0;
        uint64_t lastSeen = 0, frameUsed = 0, previousSerial = 0, indexIdentity = 0;
    };
    struct IndexSource {
        Ptr<ID3D11Buffer> buffer;
        UINT byteWidth = 0;
        bool invalid = false;
        uint64_t lastSeen = 0;
    };
    struct FoldSource {
        Ptr<ID3D11Buffer> buffer;            // the bridge's per-vertex base face buffer, referenced
        Ptr<ID3D11ShaderResourceView> srv;
        bool invalid = false;
        uint64_t lastSeen = 0;
    };
    struct RigidSource {
        Ptr<ID3D11Buffer> buffer;            // a Unity mesh's vertex buffer, referenced
        UINT byteWidth = 0, stride = 0, vertexCount = 0;
        bool invalid = false;
        uint64_t lastSeen = 0;
    };
    struct SkinnedSource {
        Ptr<ID3D11Buffer> positions;         // copies of the bridge's bind pose (float3) and
        Ptr<ID3D11Buffer> influences;        // bone weights (float4 weight, int4 bone) for the mesh
        UINT vertexCount = 0;
        uint64_t lastSeen = 0;
    };
    struct HairSource {
        Ptr<ID3D11Buffer> buffer;            // VaM's TessRenderParticle buffer, referenced
        Ptr<ID3D11ShaderResourceView> srv;
        Ptr<ID3D11Buffer> previous;          // its valid points, copied after last frame's pass
        Ptr<ID3D11ShaderResourceView> previousSrv;
        UINT byteWidth = 0;
        uint32_t count = 0, perStrand = 0, previousCount = 0, previousPerStrand = 0;
        bool previousValid = false, invalid = false;
        uint64_t lastSeen = 0, frameUsed = 0, previousSerial = 0;
    };
    struct ObjectMotion {
        bool failed = false, logged = false;
        Ptr<ID3D11VertexShader> vertex3, vertex4;
        Ptr<ID3D11PixelShader> pixel;
        Ptr<ID3D11Buffer> constants;
        Ptr<ID3D11RasterizerState> raster;
        Ptr<ID3D11DepthStencilState> depthState;     // skin/clothing: nearest wins, marks the stencil
        Ptr<ID3D11Texture2D> motion, depth, hairMask;
        Ptr<ID3D11RenderTargetView> motionRtv, hairMaskRtv;
        Ptr<ID3D11ShaderResourceView> motionSrv, hairMaskSrv;
        Ptr<ID3D11DepthStencilView> depthDsv;
        UINT width = 0, height = 0;
        std::unordered_map<uint64_t, ObjectSource> sources;
        std::unordered_map<uint64_t, IndexSource> indices;
        uint64_t frame = 0, validSerial = 0, drawn = 0, rejected = 0;
        bool valid = false;
        // Nasolabial-fold band (female face submeshes, drawn with the skins).
        bool foldFailed = false, foldLogged = false;
        Ptr<ID3D11VertexShader> foldVertex3, foldVertex4;
        Ptr<ID3D11PixelShader> foldPixel;
        Ptr<ID3D11Texture2D> fold;                   // band weight, 0 where no face matched
        Ptr<ID3D11RenderTargetView> foldRtv;
        Ptr<ID3D11ShaderResourceView> foldSrv;
        std::unordered_map<uint64_t, FoldSource> folds;
        uint64_t foldDraws = 0, foldDrawsLast = 0, foldRejected = 0;
        // Moving rigid meshes (drawn with the skins, from their vertex buffers).
        bool rigidFailed = false, rigidLogged = false, rigidRejectLogged = false;
        Ptr<ID3D11VertexShader> rigidVertex;
        Ptr<ID3D11InputLayout> rigidLayout;
        std::unordered_map<uint64_t, RigidSource> rigids;  // by vertex buffer
        uint64_t rigidDraws = 0, rigidRejected = 0;
        // Moving skinned meshes (animated CUAs): skinned here from their bind pose.
        bool skinnedFailed = false, skinnedLogged = false;
        Ptr<ID3D11VertexShader> skinnedVertex;
        Ptr<ID3D11InputLayout> skinnedLayout;
        Ptr<ID3D11Buffer> boneRows;          // this frame's matrix pairs, as float4 rows
        Ptr<ID3D11ShaderResourceView> boneRowsSrv;
        std::unordered_map<uint64_t, SkinnedSource> skinneds; // by mesh instance id
        uint64_t skinnedDraws = 0, skinnedRejected = 0;
        // Hair (second pass, only where the stencil is still clear).
        bool hairFailed = false, hairLogged = false, hairChecked = false;
        Ptr<ID3D11VertexShader> hairVertex;
        Ptr<ID3D11PixelShader> hairPixel;
        Ptr<ID3D11Buffer> hairConstants;
        Ptr<ID3D11DepthStencilState> hairDepthState;
        std::unordered_map<uint64_t, HairSource> hairs;
        // Occlusion queries: how many hair samples matched the scene depth.
        Ptr<ID3D11Query> hairQueries[kHairQueries];
        bool hairQueryPending[kHairQueries]{};
        int nextHairQuery = 0;
        uint64_t hairSegments = 0, hairSamples = 0, hairRejected = 0, hairDrawsTotal = 0, hairChecks = 0, hairEmptyChecks = 0;
        // World-space UI (third pass: in front of the scene, over skin and hair).
        bool uiFailed = false, uiLogged = false, uiChecked = false;
        Ptr<ID3D11VertexShader> uiVertex;
        Ptr<ID3D11PixelShader> uiPixel;
        Ptr<ID3D11Buffer> uiConstants, uiQuads;      // uiQuads: dynamic copy of the bridge's rectangles
        Ptr<ID3D11ShaderResourceView> uiQuadsSrv;
        Ptr<ID3D11DepthStencilState> uiDepthState;
        Ptr<ID3D11Texture2D> uiDepth;                // the UI's own depth, 0 where no UI was drawn
        Ptr<ID3D11RenderTargetView> uiDepthRtv;
        Ptr<ID3D11ShaderResourceView> uiDepthSrv;
        // 11-05: where a person's skin, clothing or hair claimed the pixel, the rendered depth it matched (0 elsewhere).
        Ptr<ID3D11Texture2D> personDepth;
        Ptr<ID3D11RenderTargetView> personDepthRtv;
        Ptr<ID3D11ShaderResourceView> personDepthSrv;
        Ptr<ID3D11Query> uiQueries[kUiQueries];      // UI samples drawn in front of the scene
        bool uiQueryPending[kUiQueries]{};
        int nextUiQuery = 0;
        uint64_t uiRects = 0, uiRectsLast = 0, uiFrames = 0, uiSamples = 0, uiChecks = 0, uiEmptyChecks = 0;
    } objects;
    bool pathOwnsMotion=false;
    // Whole object-motion pass of a frame (both eyes, skin + hair), logged as eye=0.
    GpuStageTimer<1> motionTimer{"object motion, both eyes", kObjectMotionStageNames};

    struct Eye {
        UINT width = 0, height = 0;
        Ptr<ID3D11Texture2D> depth, motion, motionJittered, motionHalf, motionJitteredHalf, hairMask, uiMask, nrControl;
        Ptr<ID3D11UnorderedAccessView> depthUav, motionUav, motionJitteredUav, motionHalfUav, motionJitteredHalfUav, hairMaskUav, uiMaskUav, nrControlUav;
        Ptr<ID3D11ShaderResourceView> uiMaskSrv;
        // NR after the upscaler: the guides at the output size.
        Ptr<ID3D11ShaderResourceView> depthSrv, motionSrv;
        Ptr<ID3D11Texture2D> outDepth, outMotion;
        Ptr<ID3D11UnorderedAccessView> outDepthUav, outMotionUav;
        UINT outWidth = 0, outHeight = 0;
        uint64_t guideSerial = 0;
        uint64_t pathGuideSerial=0;
        uint64_t sceneResetSerial=0;
        VamFrameGuides pathGuides;
        // Why a submission had no guides: capture older than 250 ms, or the
        // capture already used for this eye (no new Unity frame).
        uint64_t staleMisses = 0, reusedMisses = 0;
        // [NR] PackEyes: eye 1's guides made ahead, during eye 0's submission
        // (PrepareGuides ahead = true); eye 1's own submission then gets them
        // for the same capture, image and bounds instead of a reused-capture miss.
        bool ahead = false;
        VamFrameGuides aheadGuides;
        ID3D11Texture2D* aheadSubmitted = nullptr;
        float aheadBounds[4] = {};
        uint64_t aheadPreviousSerial = 0;
        // DLSS SR waits while the render is larger than the output (VaM's
        // own render scale raised), and skips a configuration whose feature
        // could not be created, instead of stopping for the whole run.
        bool oversize = false, excluded = false;
        ULONGLONG badUntil=0;
        UINT badW = 0, badH = 0, badOW = 0, badOH = 0;
        int badPreset = -2;
        // DLSS feature
        NVSDK_NGX_Handle* feature = nullptr;
        UINT fw = 0, fh = 0, fow = 0, foh = 0;
        DXGI_FORMAT typed = DXGI_FORMAT_UNKNOWN, outputTyped = DXGI_FORMAT_UNKNOWN;
        int preset = -1;
        Ptr<ID3D11Texture2D> color, output;
        // UI bypass: output with the menus from EASU, copied out instead.
        Ptr<ID3D11Texture2D> composite;
        Ptr<ID3D11UnorderedAccessView> compositeUav;
        Ptr<ID3D11ShaderResourceView> colorSrv, outputSrv;
        uint64_t evaluatedSerial = 0, evaluations = 0, resets = 0;
    } eyes[2];

    bool ngxReady = false;
    bool failed = false;
    ULONGLONG retryAt=0;
    bool selected = false;
    NVSDK_NGX_Parameter* parameters = nullptr;
    VamPerformanceSettings settings = VamPerformanceStartup();
    ULONGLONG nextSettingsRead = 0, lastEvaluation = 0;
    std::atomic<int> stateBits{0};
    std::wstring logDirectory;
    // UI bypass. A failure turns it off for the run.
    Ptr<ID3D11ComputeShader> uiBypassCs;
    Ptr<ID3D11Buffer> uiBypassConstants;
    Ptr<ID3D11SamplerState> linearClamp;
    bool uiBypassFailed = false, uiBypassLogged = false;
    uint64_t uiBypassFrames = 0;
    // Output-size guides for NR after the upscaler.
    Ptr<ID3D11ComputeShader> outputGuideCs;
    Ptr<ID3D11Buffer> outputGuideConstants;
    bool outputGuidesFailed = false, outputGuidesLogged = false;
    // D3D11On12 (the d3d11-proxy component's d3d11.dll): Unity's device runs on D3D12, NGX D3D11 is unavailable there
    // (0xbad00002), and DLSS SR is NGX D3D12 on the 11on12 queue, in order with the frame (no fence).
    VamOn12 on12;
    VamOn12Commands on12Commands;
#include "vam_rr.inl"

    explicit Impl(ID3D11Device* d) : device(d) {
        device->GetImmediateContext(&immediate);
        Check(immediate.As(&context), "ID3D11DeviceContext1");
        on12 = VamOn12For(d);
        Ptr<ID3D11On12Device> on12Probe;
        if (on12) OOVR_LOG("VAM DLSS: Unity's device is D3D11On12 (D3DInterop d3d11.dll); DLSS SR runs on NGX D3D12 on its queue");
        else if (SUCCEEDED(device.As(&on12Probe)))
            OOVR_LOG("VAM DLSS: Unity's device is D3D11On12 but the D3DInterop proxy's queue was not found; DLSS SR (NGX D3D11) will be unavailable");
        Ptr<ID3D11Device1> d1;
        Check(device.As(&d1), "ID3D11Device1");
        D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
        UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
        Check(d1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &state), "CreateDeviceContextState");
        Ptr<ID3DBlob> blob, errors;
        const HRESULT compiled = D3DCompile(guideShader, strlen(guideShader), "vam_dlss_guides", nullptr, nullptr, "main", "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
        if (FAILED(compiled)) throw std::runtime_error(errors ? static_cast<const char*>(errors->GetBufferPointer()) : "guide shader compilation failed");
        Check(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &guideCs), "CreateComputeShader");
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(GuideConstants);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(device->CreateBuffer(&bd, nullptr, &guideConstants), "guide constants");
        logDirectory = Environment(L"VAM_DLSS_LOG");
        if (!logDirectory.empty()) CreateDirectoryW(logDirectory.c_str(), nullptr);
        try { CreateObjectPipeline(); }
        catch (const std::exception& e) {
            objects.failed = true;
            OOVR_LOGF("VAM DLSS object motion unavailable (camera motion only): %s", e.what());
        }
        if (!objects.failed) {
            try { CreateHairPipeline(); }
            catch (const std::exception& e) {
                objects.hairFailed = true;
                OOVR_LOGF("VAM DLSS hair motion unavailable (hair keeps camera motion): %s", e.what());
            }
            try { CreateUiPipeline(); }
            catch (const std::exception& e) {
                objects.uiFailed = true;
                OOVR_LOGF("VAM DLSS UI motion unavailable (menus keep the depth and motion behind them): %s", e.what());
            }
            try { CreateFoldPipeline(); }
            catch (const std::exception& e) {
                objects.foldFailed = true;
                OOVR_LOGF("VAM NR fold band unavailable (NR stays as set on faces): %s", e.what());
            }
            try { CreateRigidPipeline(); }
            catch (const std::exception& e) {
                objects.rigidFailed = true;
                OOVR_LOGF("VAM DLSS rigid motion unavailable (props keep camera motion): %s", e.what());
            }
            try { CreateSkinnedPipeline(); }
            catch (const std::exception& e) {
                objects.skinnedFailed = true;
                OOVR_LOGF("VAM DLSS skinned motion unavailable (animated CUAs keep camera motion): %s", e.what());
            }
        }
    }

    void CreateObjectPipeline() {
        const D3D_SHADER_MACRO float3Vertices[] = {{"VERTEX", "float3"}, {nullptr, nullptr}};
        const D3D_SHADER_MACRO float4Vertices[] = {{"VERTEX", "float4"}, {nullptr, nullptr}};
        auto vs3 = CompileShader(objectShader, "vam_object_motion", "vertexMain", "vs_5_0", float3Vertices);
        auto vs4 = CompileShader(objectShader, "vam_object_motion", "vertexMain", "vs_5_0", float4Vertices);
        auto ps = CompileShader(objectShader, "vam_object_motion", "pixelMain", "ps_5_0", float3Vertices);
        Check(device->CreateVertexShader(vs3->GetBufferPointer(), vs3->GetBufferSize(), nullptr, &objects.vertex3), "object VS");
        Check(device->CreateVertexShader(vs4->GetBufferPointer(), vs4->GetBufferSize(), nullptr, &objects.vertex4), "object VS4");
        Check(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &objects.pixel), "object PS");
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(ObjectConstants);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(device->CreateBuffer(&bd, nullptr, &objects.constants), "object constants");
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE; // double-sided cloth; own depth test resolves overlap
        rd.DepthClipEnable = TRUE;
        Check(device->CreateRasterizerState(&rd, &objects.raster), "object rasterizer");
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_GREATER; // reversed Z: nearest surface wins
        // Pixels whose skin or clothing matched the scene: hair leaves them alone.
        dd.StencilEnable = TRUE;
        dd.StencilReadMask = dd.StencilWriteMask = 0xff;
        dd.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
        dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
        dd.FrontFace.StencilFailOp = dd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        dd.BackFace = dd.FrontFace;
        Check(device->CreateDepthStencilState(&dd, &objects.depthState), "object depth state");
    }

    // The skin shader with the per-vertex base face (t3) and the band in SV_Target2.
    void CreateFoldPipeline() {
        const D3D_SHADER_MACRO float3Vertices[] = {{"VERTEX", "float3"}, {"FOLD", "1"}, {nullptr, nullptr}};
        const D3D_SHADER_MACRO float4Vertices[] = {{"VERTEX", "float4"}, {"FOLD", "1"}, {nullptr, nullptr}};
        auto vs3 = CompileShader(objectShader, "vam_nr_fold", "vertexMain", "vs_5_0", float3Vertices);
        auto vs4 = CompileShader(objectShader, "vam_nr_fold", "vertexMain", "vs_5_0", float4Vertices);
        auto ps = CompileShader(objectShader, "vam_nr_fold", "pixelMain", "ps_5_0", float3Vertices);
        Check(device->CreateVertexShader(vs3->GetBufferPointer(), vs3->GetBufferSize(), nullptr, &objects.foldVertex3), "fold VS");
        Check(device->CreateVertexShader(vs4->GetBufferPointer(), vs4->GetBufferSize(), nullptr, &objects.foldVertex4), "fold VS4");
        Check(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &objects.foldPixel), "fold PS");
    }

    // The skin pipeline with the position from a Unity mesh's own vertex buffer.
    void CreateRigidPipeline() {
        const D3D_SHADER_MACRO rigid[] = {{"VERTEX", "float3"}, {"RIGID", "1"}, {nullptr, nullptr}};
        auto vs = CompileShader(objectShader, "vam_rigid_motion", "vertexMain", "vs_5_0", rigid);
        Check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &objects.rigidVertex), "rigid VS");
        const D3D11_INPUT_ELEMENT_DESC position{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
        Check(device->CreateInputLayout(&position, 1, vs->GetBufferPointer(), vs->GetBufferSize(), &objects.rigidLayout), "rigid input layout");
    }

    // The skin pipeline skinning a Unity mesh's bind pose (slot 0) by its bone weights (slot 1).
    void CreateSkinnedPipeline() {
        const D3D_SHADER_MACRO skinned[] = {{"VERTEX", "float3"}, {"SKINNED", "1"}, {nullptr, nullptr}};
        auto vs = CompileShader(objectShader, "vam_skinned_motion", "vertexMain", "vs_5_0", skinned);
        Check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &objects.skinnedVertex), "skinned VS");
        const D3D11_INPUT_ELEMENT_DESC elements[3] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_SINT, 1, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        Check(device->CreateInputLayout(elements, 3, vs->GetBufferPointer(), vs->GetBufferSize(), &objects.skinnedLayout), "skinned input layout");
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = kMaxRigidMatrices * kRigidMatrixStride;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 16;
        Check(device->CreateBuffer(&bd, nullptr, &objects.boneRows), "skinned bone rows");
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_UNKNOWN;
        v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        v.Buffer.NumElements = bd.ByteWidth / 16;
        Check(device->CreateShaderResourceView(objects.boneRows.Get(), &v, &objects.boneRowsSrv), "skinned bone rows SRV");
    }

    void CreateHairPipeline() {
        auto vs = CompileShader(hairShader, "vam_hair_motion", "vertexMain", "vs_5_0", nullptr);
        auto ps = CompileShader(hairShader, "vam_hair_motion", "pixelMain", "ps_5_0", nullptr);
        Check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &objects.hairVertex), "hair VS");
        Check(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &objects.hairPixel), "hair PS");
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(HairConstants);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(device->CreateBuffer(&bd, nullptr, &objects.hairConstants), "hair constants");
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_GREATER;
        dd.StencilEnable = TRUE;
        dd.StencilReadMask = 0xff;
        dd.StencilWriteMask = 0;
        dd.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL; // reference 0: not claimed by skin
        dd.FrontFace.StencilPassOp = dd.FrontFace.StencilFailOp = dd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        dd.BackFace = dd.FrontFace;
        Check(device->CreateDepthStencilState(&dd, &objects.hairDepthState), "hair depth state");
        D3D11_QUERY_DESC qd{};
        qd.Query = D3D11_QUERY_OCCLUSION;
        for (auto& q : objects.hairQueries) Check(device->CreateQuery(&qd, &q), "hair occlusion query");
    }

    void CreateUiPipeline() {
        auto vs = CompileShader(uiShader, "vam_ui_motion", "vertexMain", "vs_5_0", nullptr);
        auto ps = CompileShader(uiShader, "vam_ui_motion", "pixelMain", "ps_5_0", nullptr);
        Check(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &objects.uiVertex), "UI VS");
        Check(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &objects.uiPixel), "UI PS");
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(UiConstants);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(device->CreateBuffer(&bd, nullptr, &objects.uiConstants), "UI constants");
        D3D11_BUFFER_DESC qb{};
        qb.ByteWidth = sizeof(UiQuad) * kMaxUiQuads;
        qb.Usage = D3D11_USAGE_DYNAMIC;
        qb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        qb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        qb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        qb.StructureByteStride = sizeof(UiQuad);
        Check(device->CreateBuffer(&qb, nullptr, &objects.uiQuads), "UI rectangles");
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_UNKNOWN;
        v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        v.Buffer.NumElements = kMaxUiQuads;
        Check(device->CreateShaderResourceView(objects.uiQuads.Get(), &v, &objects.uiQuadsSrv), "UI rectangles SRV");
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_GREATER; // reversed Z: UI in front of matched skin or hair takes the pixel
        Check(device->CreateDepthStencilState(&dd, &objects.uiDepthState), "UI depth state");
        D3D11_QUERY_DESC qd{};
        qd.Query = D3D11_QUERY_OCCLUSION;
        for (auto& q : objects.uiQueries) Check(device->CreateQuery(&qd, &q), "UI occlusion query");
    }

    void UpdateState() {
        int bits = 0;
        // The bridge jitters only while both eyes take DLSS: an eye on FSR1
        // (paused, or left out by the DLSSSREyes diagnostic) would show it.
        const bool jitterHold = eyes[0].oversize || eyes[1].oversize || eyes[0].excluded || eyes[1].excluded;
        if (failed) bits |= kStateFailed;
        if(!failed && VamPathRequested())bits|=kStatePathRendering;
        if (!failed && selected && !jitterHold) bits |= kStateSelected;
        if (lastEvaluation && GetTickCount64() - lastEvaluation < 1000) bits |= kStateEvaluating;
        if (!lastProblem.empty()) bits |= kStateCaptureProblem;
        if (!failed && selected && !objects.failed && settings.dlssObjectMotion && (!pathOwnsMotion || settings.nrFold>0)) bits |= kStateObjectMotion;
        if (!pathOwnsMotion && !failed && selected && !objects.failed && !objects.rigidFailed && settings.dlssObjectMotion && settings.dlssRigidMotion) {
            bits |= kStateRigidMotion;
            if (!objects.skinnedFailed) bits |= kStateSkinnedMotion;
        }
        if (!pathOwnsMotion && !failed && selected && !objects.failed && !objects.hairFailed && settings.dlssHairMotion) bits |= kStateHairMotion;
        if (!failed && selected && !objects.failed && !objects.uiFailed && (settings.dlssUiMotion || settings.dlssUiBypass || settings.dlssPreset==6 || VamPathRequested())) bits |= kStateUiMotion;
        // Not tied to the jitter hold: the bridge's menu mode must not depend
        // on what the frames it flags do to these bits.
        if (!failed && selected && settings.dlssMenuNative) bits |= kStateMenuNative;
        if (!failed && selected && !objects.failed && !objects.foldFailed && settings.dlssObjectMotion && settings.nrFold > 0)
            bits |= kStateNrFold;
        stateBits.store(bits);
    }

    void WriteStatus(const char* status, const std::string& detail) noexcept {
        try {
            if (logDirectory.empty()) return;
            std::string text = std::string("{\"status\":\"") + status + "\",\"detail\":\"";
            for (char c : detail) text += (c == '"' || c == '\\') ? '\'' : c;
            text += "\"}\n";
            const auto path = logDirectory + L"\\dlss-status.json";
            HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return;
            DWORD written = 0;
            WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
            CloseHandle(file);
        } catch (...) {}
    }

    void Problem(const std::string& message) {
        if (message == lastProblem) return;
        lastProblem = message;
        OOVR_LOGF("VAM DLSS bridge capture problem: %s", message.c_str());
        WriteStatus("capture-problem", message);
        UpdateState();
    }

    void Fail(const std::string& message) {
        failed = true; retryAt=GetTickCount64()+1000;
        OOVR_LOGF(VamDesktopDisplay()?"VAM desktop DLSS device/resource failure; retry in 1s: %s":"VAM DLSS disabled for this run: %s; FSR1 fallback continues", message.c_str());
        WriteStatus("failed", message);
        UpdateState();
    }

    void EnsureObjectTargets(UINT w, UINT h) {
        if (objects.motion && objects.width == w && objects.height == h) return;
        objects.motion.Reset(); objects.motionRtv.Reset(); objects.motionSrv.Reset();
        objects.hairMask.Reset(); objects.hairMaskRtv.Reset(); objects.hairMaskSrv.Reset();
        objects.uiDepth.Reset(); objects.uiDepthRtv.Reset(); objects.uiDepthSrv.Reset();
        objects.personDepth.Reset(); objects.personDepthRtv.Reset(); objects.personDepthSrv.Reset();
        objects.fold.Reset(); objects.foldRtv.Reset(); objects.foldSrv.Reset();
        objects.depth.Reset(); objects.depthDsv.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h;
        td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.Format = DXGI_FORMAT_R32G32_FLOAT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Check(device->CreateTexture2D(&td, nullptr, &objects.motion), "object motion");
        Check(device->CreateRenderTargetView(objects.motion.Get(), nullptr, &objects.motionRtv), "object motion RTV");
        Check(device->CreateShaderResourceView(objects.motion.Get(), nullptr, &objects.motionSrv), "object motion SRV");
        td.Format = DXGI_FORMAT_R8_UNORM; // 1 where the hair pass wrote the motion
        Check(device->CreateTexture2D(&td, nullptr, &objects.hairMask), "object hair mask");
        Check(device->CreateRenderTargetView(objects.hairMask.Get(), nullptr, &objects.hairMaskRtv), "object hair mask RTV");
        Check(device->CreateShaderResourceView(objects.hairMask.Get(), nullptr, &objects.hairMaskSrv), "object hair mask SRV");
        // The nasolabial-fold band weight (skin pass, female faces).
        Check(device->CreateTexture2D(&td, nullptr, &objects.fold), "object fold band");
        Check(device->CreateRenderTargetView(objects.fold.Get(), nullptr, &objects.foldRtv), "object fold band RTV");
        Check(device->CreateShaderResourceView(objects.fold.Get(), nullptr, &objects.foldSrv), "object fold band SRV");
        td.Format = DXGI_FORMAT_R32_FLOAT; // the UI pass's own depth, reversed Z
        Check(device->CreateTexture2D(&td, nullptr, &objects.uiDepth), "object UI depth");
        Check(device->CreateRenderTargetView(objects.uiDepth.Get(), nullptr, &objects.uiDepthRtv), "object UI depth RTV");
        Check(device->CreateShaderResourceView(objects.uiDepth.Get(), nullptr, &objects.uiDepthSrv), "object UI depth SRV");
        Check(device->CreateTexture2D(&td, nullptr, &objects.personDepth), "object person depth");
        Check(device->CreateRenderTargetView(objects.personDepth.Get(), nullptr, &objects.personDepthRtv), "object person depth RTV");
        Check(device->CreateShaderResourceView(objects.personDepth.Get(), nullptr, &objects.personDepthSrv), "object person depth SRV");
        td.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        Check(device->CreateTexture2D(&td, nullptr, &objects.depth), "object depth");
        Check(device->CreateDepthStencilView(objects.depth.Get(), nullptr, &objects.depthDsv), "object DSV");
        objects.width = w; objects.height = h;
    }

    ObjectSource* ResolveSource(const ObjectDraw& d) {
        if (!d.vertexBuffer || (d.stride != 12 && d.stride != 16)) return nullptr;
        ObjectSource& s = objects.sources[d.key];
        s.lastSeen = objects.frame;
        auto* raw = reinterpret_cast<ID3D11Buffer*>(d.vertexBuffer);
        if (s.buffer.Get() != raw) {
            Ptr<ID3D11Buffer> buffer = raw; // AddRef first, then inspect
            D3D11_BUFFER_DESC desc{};
            buffer->GetDesc(&desc);
            s.buffer = buffer;
            s.srv.Reset();
            s.invalid = !(desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) || !(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
                desc.StructureByteStride != d.stride || desc.Usage != D3D11_USAGE_DEFAULT || desc.ByteWidth < d.stride;
            if (s.invalid) return nullptr;
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = DXGI_FORMAT_UNKNOWN;
            v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            v.Buffer.FirstElement = 0;
            v.Buffer.NumElements = desc.ByteWidth / desc.StructureByteStride;
            Check(device->CreateShaderResourceView(buffer.Get(), &v, &s.srv), "object vertex SRV");
            if (desc.ByteWidth != s.byteWidth || desc.StructureByteStride != s.stride) {
                s.previous.Reset(); s.previousSrv.Reset();
                s.previousValid = false;
            }
            s.byteWidth = desc.ByteWidth;
            s.stride = desc.StructureByteStride;
        }
        if (s.invalid) return nullptr;
        if (s.frameUsed != objects.frame) {
            if(s.previousSerial+1!=static_cast<uint64_t>(info.serial) || (info.flags&1) || s.indexIdentity!=d.indexBuffer)
                s.previousValid=false;
            s.indexIdentity=d.indexBuffer;
            s.frameUsed = objects.frame;
            std::copy(d.offset, d.offset + 3, s.offset);
        }
        return &s;
    }

    ID3D11Buffer* ResolveIndices(const ObjectDraw& d) {
        if (!d.indexBuffer || !d.indexCount) return nullptr;
        IndexSource& s = objects.indices[d.indexBuffer];
        s.lastSeen = objects.frame;
        if (!s.buffer) {
            s.buffer = reinterpret_cast<ID3D11Buffer*>(d.indexBuffer);
            D3D11_BUFFER_DESC desc{};
            s.buffer->GetDesc(&desc);
            s.byteWidth = desc.ByteWidth;
            s.invalid = !(desc.BindFlags & D3D11_BIND_INDEX_BUFFER);
        }
        const uint64_t size = (d.flags & 1) ? 4 : 2;
        if (s.invalid || (static_cast<uint64_t>(d.indexStart) + d.indexCount) * size > s.byteWidth) return nullptr;
        return s.buffer.Get();
    }

    // A rigid mesh's vertex buffer (kDrawRigid). Unity sizes a mesh's vertex
    // stream exactly, so its stride is the size over the vertex count; the
    // position is its first element (float3).
    RigidSource* ResolveRigid(const ObjectDraw& d) {
        if (!d.vertexBuffer || !d.vertexCount) return nullptr;
        RigidSource& s = objects.rigids[d.vertexBuffer];
        s.lastSeen = objects.frame;
        if (!s.buffer || s.vertexCount != d.vertexCount) {
            if (!s.buffer) {
                s.buffer = reinterpret_cast<ID3D11Buffer*>(d.vertexBuffer); // AddRef first, then inspect
                D3D11_BUFFER_DESC desc{};
                s.buffer->GetDesc(&desc);
                s.byteWidth = (desc.BindFlags & D3D11_BIND_VERTEX_BUFFER) ? desc.ByteWidth : 0;
            }
            s.vertexCount = d.vertexCount;
            s.stride = s.byteWidth / d.vertexCount;
            s.invalid = !s.byteWidth || s.byteWidth % d.vertexCount != 0 || s.stride < 12 || s.stride > 256 || s.stride % 4 != 0;
            if (s.invalid && !objects.rigidRejectLogged) {
                objects.rigidRejectLogged = true;
                OOVR_LOGF("VAM DLSS rigid motion: a vertex buffer rejected (%u bytes for %u vertices; first one only)", s.byteWidth, d.vertexCount);
            }
        }
        return s.invalid ? nullptr : &s;
    }

    // A skinned mesh's bind pose and bone weights (kDrawSkinned), copied from
    // the bridge's memory the first time the mesh is drawn.
    SkinnedSource* ResolveSkinned(const ObjectDraw& d) {
        uint64_t weights;
        std::memcpy(&weights, d.offset, sizeof(weights));
        if (!d.vertexBuffer || !weights || !d.vertexCount) return nullptr;
        SkinnedSource& s = objects.skinneds[d.key];
        s.lastSeen = objects.frame;
        if (s.positions && s.vertexCount == d.vertexCount) return &s;
        D3D11_BUFFER_DESC bd{};
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA data{};
        bd.ByteWidth = d.vertexCount * 12;
        data.pSysMem = reinterpret_cast<const void*>(d.vertexBuffer);
        Check(device->CreateBuffer(&bd, &data, &s.positions), "skinned bind pose");
        bd.ByteWidth = d.vertexCount * 32;
        data.pSysMem = reinterpret_cast<const void*>(weights);
        Check(device->CreateBuffer(&bd, &data, &s.influences), "skinned bone weights");
        s.vertexCount = d.vertexCount;
        return &s;
    }

    // A face draw's base face buffer (kDrawFold), from this frame's table.
    ID3D11ShaderResourceView* ResolveFold(const ObjectDraw& d, const uint64_t* table, int tableCount) {
        if (!(d.flags & kDrawFold) || !table || d.tableIndex < 1 || static_cast<int>(d.tableIndex) > tableCount) return nullptr;
        const uint64_t pointer = table[d.tableIndex - 1];
        if (!pointer) return nullptr;
        FoldSource& s = objects.folds[pointer];
        s.lastSeen = objects.frame;
        if (!s.buffer) {
            Ptr<ID3D11Buffer> buffer = reinterpret_cast<ID3D11Buffer*>(pointer); // AddRef first, then inspect
            D3D11_BUFFER_DESC desc{};
            buffer->GetDesc(&desc);
            s.buffer = buffer;
            s.invalid = !(desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) || !(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
                desc.StructureByteStride != 16 || desc.ByteWidth < 16;
            if (s.invalid) {
                OOVR_LOGF("VAM NR fold band: bridge buffer rejected (stride %u, %u bytes, bind 0x%x, misc 0x%x)",
                    desc.StructureByteStride, desc.ByteWidth, desc.BindFlags, desc.MiscFlags);
                return nullptr;
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = DXGI_FORMAT_UNKNOWN;
            v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            v.Buffer.NumElements = desc.ByteWidth / 16; // reads past the end return 0: no band
            Check(device->CreateShaderResourceView(buffer.Get(), &v, &s.srv), "fold band SRV");
        }
        return s.invalid ? nullptr : s.srv.Get();
    }

    // VaM physics hair: the TessRenderParticle buffer its renderer reads.
    HairSource* ResolveHair(const ObjectDraw& d) {
        if (!d.vertexBuffer || d.stride != kHairStride || d.indexCount < 2) return nullptr;
        HairSource& s = objects.hairs[d.key];
        s.lastSeen = objects.frame;
        auto* raw = reinterpret_cast<ID3D11Buffer*>(d.vertexBuffer);
        if (s.buffer.Get() != raw) {
            Ptr<ID3D11Buffer> buffer = raw; // AddRef first, then inspect
            D3D11_BUFFER_DESC desc{};
            buffer->GetDesc(&desc);
            s.buffer = buffer;
            s.srv.Reset();
            s.previousValid = false;        // a rebuilt hair: no history for its points yet
            s.invalid = !(desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) || !(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) ||
                desc.StructureByteStride != kHairStride || desc.Usage != D3D11_USAGE_DEFAULT || desc.ByteWidth < 2 * kHairStride;
            if (s.invalid) return nullptr;
            D3D11_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = DXGI_FORMAT_UNKNOWN;
            v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            v.Buffer.FirstElement = 0;
            v.Buffer.NumElements = desc.ByteWidth / kHairStride;
            Check(device->CreateShaderResourceView(buffer.Get(), &v, &s.srv), "hair particle SRV");
            if (desc.ByteWidth != s.byteWidth) { s.previous.Reset(); s.previousSrv.Reset(); }
            s.byteWidth = desc.ByteWidth;
        }
        if (s.invalid || static_cast<uint64_t>(d.indexCount) * kHairStride > s.byteWidth) return nullptr;
        if (s.frameUsed != objects.frame) {
            if(s.previousSerial+1!=static_cast<uint64_t>(info.serial) || (info.flags&1))s.previousValid=false;
            s.frameUsed = objects.frame;
            s.count = d.indexCount;
            // Points per strand from VaM's segment counts; otherwise RootIndex splits strands.
            s.perStrand = d.indexStart >= 2 && d.indexCount % d.indexStart == 0 ? d.indexStart : 0;
        }
        return &s;
    }

    void HarvestHairQueries() {
        for (int i = 0; i < kHairQueries; ++i) {
            if (!objects.hairQueryPending[i]) continue;
            UINT64 samples = 0;
            const HRESULT hr = immediate->GetData(objects.hairQueries[i].Get(), &samples, sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_FALSE) continue;
            objects.hairQueryPending[i] = false;
            if (FAILED(hr)) continue;
            objects.hairSamples = samples;
            ++objects.hairChecks;
            if (!samples) ++objects.hairEmptyChecks;
            if (!objects.hairChecked) {
                objects.hairChecked = true;
                OOVR_LOGF("VAM DLSS hair motion: first check, %llu hair samples matched the scene depth (%llu guide segments drawn)%s",
                    static_cast<unsigned long long>(samples), static_cast<unsigned long long>(objects.hairSegments),
                    samples ? "" : "; none yet (hair off screen, or it does not write depth)");
            }
        }
    }

    void HarvestUiQueries() {
        for (int i = 0; i < kUiQueries; ++i) {
            if (!objects.uiQueryPending[i]) continue;
            UINT64 samples = 0;
            const HRESULT hr = immediate->GetData(objects.uiQueries[i].Get(), &samples, sizeof(samples), D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_FALSE) continue;
            objects.uiQueryPending[i] = false;
            if (FAILED(hr)) continue;
            objects.uiSamples = samples;
            ++objects.uiChecks;
            if (!samples) ++objects.uiEmptyChecks;
            if (!objects.uiChecked) {
                objects.uiChecked = true;
                OOVR_LOGF("VAM DLSS UI motion: first check, %llu UI samples drawn in front of the scene (%llu UI rectangles)%s",
                    static_cast<unsigned long long>(samples), static_cast<unsigned long long>(objects.uiRectsLast),
                    samples ? "" : "; none yet (menu off screen, or behind the scene)");
            }
        }
    }

    // Moving skinned meshes (kDrawSkinned), inside RenderObjectMotion's skin
    // pass: its targets, depth-stencil state and constant buffer are bound.
    void DrawSkinned(const FrameInfoV3& frame, const ObjectDraw* draws, int count, const float* table, int pairs, float eyeWidth,
                     uint64_t& drawn, uint64_t& rejected) {
        const FrameExtension& x = frame.ext;
        bool bound = false;
        ObjectConstants c{};
        for (int i = 0; i < count; ++i) {
            const ObjectDraw& d = draws[i];
            if (!(d.flags & kDrawSkinned)) continue;
            uint32_t bones;
            std::memcpy(&bones, &d.offset[2], sizeof(bones));
            const bool inTable = d.tableIndex >= 1 && bones >= 1 &&
                static_cast<uint64_t>(d.tableIndex) - 1 + bones <= static_cast<uint64_t>(pairs);
            SkinnedSource* s = inTable ? ResolveSkinned(d) : nullptr;
            ID3D11Buffer* indices = s ? ResolveIndices(d) : nullptr;
            if (!s || !indices) { ++rejected; continue; }
            if (!bound) {
                // All of this frame's pairs, once: the skinned draws index into them.
                D3D11_MAPPED_SUBRESOURCE mapped{};
                Check(context->Map(objects.boneRows.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "skinned bone rows map");
                std::memcpy(mapped.pData, table, static_cast<size_t>(pairs) * kRigidMatrixStride);
                context->Unmap(objects.boneRows.Get(), 0);
                context->IASetInputLayout(objects.skinnedLayout.Get());
                context->VSSetShader(objects.skinnedVertex.Get(), nullptr, 0);
                context->PSSetShader(objects.pixel.Get(), nullptr, 0);
                ID3D11ShaderResourceView* rows[2] = {nullptr, objects.boneRowsSrv.Get()};
                context->VSSetShaderResources(0, 2, rows);
                bound = true;
            }
            ID3D11Buffer* vertices[2] = {s->positions.Get(), s->influences.Get()};
            const UINT strides[2] = {12, 32}, offsets[2] = {0, 0};
            context->IASetVertexBuffers(0, 2, vertices, strides, offsets);
            context->IASetIndexBuffer(indices, (d.flags & 1) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0);
            const uint32_t first = d.tableIndex - 1;
            std::memcpy(&c.offset[0], &first, sizeof(first));
            std::memcpy(&c.offset[1], &bones, sizeof(bones));
            for (unsigned eye = 0; eye < frame.viewCount; ++eye) {
                D3D11_VIEWPORT vp{eye * eyeWidth, 0.f, eyeWidth, static_cast<float>(depthDesc.Height), 0.f, 1.f};
                context->RSSetViewports(1, &vp);
                GpuClip(x.rasterWorldToClip[eye], c.rasterClip);
                std::copy(x.worldToClip[eye], x.worldToClip[eye] + 16, c.currentClip);
                std::copy(x.prevWorldToClip[eye], x.prevWorldToClip[eye] + 16, c.previousClip);
                const float* p = frame.base.viewToClip[eye];
                c.depthParams[0] = p[10];  // P22
                c.depthParams[1] = p[11];  // P23
                c.depthParams[2] = 0.015f; // metres
                c.depthParams[3] = 0.03f;
                VamWriteConstants(context, objects.constants.Get(), &c, sizeof(c));
                context->DrawIndexed(d.indexCount, d.indexStart, d.baseVertex);
            }
            ++drawn;
        }
    }

    // Redraws VaM's GPU-skinned meshes and moving rigid meshes, then its physics hair, then the
    // world-space UI rectangles into a double-wide NDC motion target aligned
    // with the captured depth. Inside Unity's render event: the private
    // context state restores everything Unity had bound.
    void RenderObjectMotion(const FrameInfoV3& frame) {
        // A duplicate capture must not overwrite last frame's positions and
        // then reinterpret the current positions as its own previous frame.
        if(objects.valid && objects.validSerial==static_cast<uint64_t>(frame.base.serial) && !(frame.base.flags&1))return;
        objects.valid = false;
        objects.uiRectsLast=objects.foldDrawsLast=0;
        const FrameExtension& x = frame.ext;
        const bool skins = settings.dlssObjectMotion && (!pathOwnsMotion || settings.nrFold>0);
        const bool hair = !pathOwnsMotion && settings.dlssHairMotion && !objects.hairFailed;
        const bool haveDraws = x.draws && x.drawCount > 0 && x.drawStride == sizeof(ObjectDraw);
        const bool ui = (settings.dlssUiMotion || settings.dlssUiBypass || settings.dlssPreset==6 || VamPathRequested()) && !objects.uiFailed && x.uiQuads && x.uiQuadCount > 0 &&
            x.uiQuadStride == sizeof(UiQuad);
        if (objects.failed || (!((skins || hair) && haveDraws) && !ui)) return;
        const auto* draws = haveDraws ? reinterpret_cast<const ObjectDraw*>(x.draws) : nullptr;
        const int count = haveDraws ? std::min(x.drawCount, kMaxObjectDraws) : 0;
        // Female faces' fold band (NasolabialReduction > 0): their buffers, by draw.
        const bool fold = skins && settings.nrFold > 0 && !objects.foldFailed && x.foldBands && x.foldBandCount > 0;
        const auto* foldTable = fold ? reinterpret_cast<const uint64_t*>(x.foldBands) : nullptr;
        const int foldCount = fold ? std::min(x.foldBandCount, kMaxFoldBands) : 0;
        // Moving rigid meshes: their matrix pairs, by draw.
        const bool rigid = !pathOwnsMotion && skins && settings.dlssRigidMotion && !objects.rigidFailed && x.rigidMatrices && x.rigidMatrixCount > 0 &&
            x.rigidMatrixStride == kRigidMatrixStride;
        const auto* rigidTable = rigid ? reinterpret_cast<const float*>(x.rigidMatrices) : nullptr;
        const int rigidCount = rigid ? std::min(x.rigidMatrixCount, kMaxRigidMatrices) : 0;
        ++objects.frame;
        EnsureObjectTargets(depthDesc.Width, depthDesc.Height);
        if (hair) HarvestHairQueries();
        if (!objects.uiFailed) HarvestUiQueries();
        const float eyeWidth = float(depthDesc.Width) / frame.viewCount;
        uint64_t drawn = 0, rejected = 0, hairDrawn = 0, hairRejected = 0, hairSegments = 0, uiDrawn = 0, foldDrawn = 0, foldRejected = 0;
        uint64_t rigidDrawn = 0, rigidRejected = 0, skinnedDrawn = 0, skinnedRejected = 0;
        {
            ContextScope scope(context.Get(), state.Get());
            const float clear[4] = {1e30f, 1e30f, 0.f, 0.f};
            const float none[4] = {0.f, 0.f, 0.f, 0.f};
            context->ClearRenderTargetView(objects.motionRtv.Get(), clear);
            context->ClearRenderTargetView(objects.hairMaskRtv.Get(), none);
            context->ClearRenderTargetView(objects.uiDepthRtv.Get(), none);
            context->ClearRenderTargetView(objects.foldRtv.Get(), none);
            context->ClearRenderTargetView(objects.personDepthRtv.Get(), none);
            context->ClearDepthStencilView(objects.depthDsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.f, 0);
            // Skins and clothing write the fold band too (0 but on female faces): the nearest surface owns it.
            // And their matched depth (skins and clothing; 0 from props and CUAs: the scene depth stays there).
            ID3D11RenderTargetView* rtvs[4] = {objects.motionRtv.Get(), objects.hairMaskRtv.Get(), objects.foldRtv.Get(),
                objects.personDepthRtv.Get()};
            context->OMSetRenderTargets(4, rtvs, objects.depthDsv.Get());
            context->RSSetState(objects.raster.Get());
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->IASetInputLayout(nullptr);
            // t2: the scene depth (before the transparents when the bridge gave it), t5: with the transparents.
            context->PSSetShaderResources(2, 1, sceneSrv.GetAddressOf());
            context->PSSetShaderResources(5, 1, depthSrv.GetAddressOf());
            // t6, t7: the colors before and with the transparents (11-06, see opaqueColorCopy).
            ID3D11ShaderResourceView* colors[2] = {colorsThisFrame ? opaqueColorSrv.Get() : nullptr,
                colorsThisFrame ? fullColorSrv.Get() : nullptr};
            context->PSSetShaderResources(6, 2, colors);
            if (skins) {
                // Skin and clothing first: they match the scene depth tightly and mark the stencil.
                context->OMSetDepthStencilState(objects.depthState.Get(), 1);
                context->PSSetShader(objects.pixel.Get(), nullptr, 0);
                context->VSSetConstantBuffers(0, 1, objects.constants.GetAddressOf());
                context->PSSetConstantBuffers(0, 1, objects.constants.GetAddressOf());
                ObjectConstants c{};
                for (int i = 0; i < count; ++i) {
                    const ObjectDraw& d = draws[i];
                    if (d.flags & (kDrawHair | kDrawRigid | kDrawSkinned)) continue;
                    ObjectSource* s = ResolveSource(d);
                    ID3D11Buffer* indices = s ? ResolveIndices(d) : nullptr;
                    if (!s || !indices) { ++rejected; continue; }
                    ID3D11ShaderResourceView* foldBase = fold && (d.flags & kDrawFold) ? ResolveFold(d, foldTable, foldCount) : nullptr;
                    if (fold && (d.flags & kDrawFold) && !foldBase) ++foldRejected;
                    if (foldBase) {
                        context->VSSetShader(s->stride == 16 ? objects.foldVertex4.Get() : objects.foldVertex3.Get(), nullptr, 0);
                        context->PSSetShader(objects.foldPixel.Get(), nullptr, 0);
                    } else {
                        context->VSSetShader(s->stride == 16 ? objects.vertex4.Get() : objects.vertex3.Get(), nullptr, 0);
                        context->PSSetShader(objects.pixel.Get(), nullptr, 0);
                    }
                    ID3D11ShaderResourceView* vertices[4] = {s->srv.Get(), s->previousValid ? s->previousSrv.Get() : s->srv.Get(), nullptr, foldBase};
                    context->VSSetShaderResources(0, 4, vertices);
                    context->IASetIndexBuffer(indices, (d.flags & 1) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0);
                    const float* previousOffset = s->previousValid ? s->previousOffset : s->offset;
                    for (int k = 0; k < 3; ++k) { c.offset[k] = s->offset[k]; c.previousOffset[k] = previousOffset[k]; }
                    for (unsigned eye = 0; eye < frame.viewCount; ++eye) {
                        D3D11_VIEWPORT vp{eye * eyeWidth, 0.f, eyeWidth, static_cast<float>(depthDesc.Height), 0.f, 1.f};
                        context->RSSetViewports(1, &vp);
                        GpuClip(x.rasterWorldToClip[eye], c.rasterClip);
                        std::copy(x.worldToClip[eye], x.worldToClip[eye] + 16, c.currentClip);
                        std::copy(x.prevWorldToClip[eye], x.prevWorldToClip[eye] + 16, c.previousClip);
                        const float* p = frame.base.viewToClip[eye];
                        c.depthParams[0] = p[10];  // P22
                        c.depthParams[1] = p[11];  // P23
                        c.depthParams[2] = 0.015f; // metres
                        c.depthParams[3] = 0.03f;
                        c.person[0] = 1.f;         // a person's skin, clothing or mesh hair (see objectShader's pixelMain)
                        c.person[1] = colorsThisFrame ? 1.f : 0.f; // t6, t7 bound: the see-through test
                        VamWriteConstants(context, objects.constants.Get(), &c, sizeof(c));
                        context->DrawIndexed(d.indexCount, d.indexStart, d.baseVertex);
                    }
                    ++drawn;
                    if (foldBase) ++foldDrawn;
                }
                if (rigid) {
                    // Moving props: same targets, depth test and stencil as the skins.
                    ID3D11ShaderResourceView* noVertices[4] = {};
                    context->VSSetShaderResources(0, 4, noVertices);
                    context->IASetInputLayout(objects.rigidLayout.Get());
                    context->VSSetShader(objects.rigidVertex.Get(), nullptr, 0);
                    context->PSSetShader(objects.pixel.Get(), nullptr, 0);
                    ObjectConstants r{};
                    for (int i = 0; i < count; ++i) {
                        const ObjectDraw& d = draws[i];
                        if (!(d.flags & kDrawRigid)) continue;
                        RigidSource* s = d.tableIndex >= 1 && static_cast<int>(d.tableIndex) <= rigidCount ? ResolveRigid(d) : nullptr;
                        ID3D11Buffer* indices = s ? ResolveIndices(d) : nullptr;
                        if (!s || !indices) { ++rigidRejected; continue; }
                        const float* current = rigidTable + (d.tableIndex - 1) * (kRigidMatrixStride / sizeof(float));
                        const float* previous = current + 16;
                        ID3D11Buffer* vertices = s->buffer.Get();
                        const UINT stride = s->stride, offset = 0;
                        context->IASetVertexBuffers(0, 1, &vertices, &stride, &offset);
                        context->IASetIndexBuffer(indices, (d.flags & 1) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0);
                        for (unsigned eye = 0; eye < frame.viewCount; ++eye) {
                            D3D11_VIEWPORT vp{eye * eyeWidth, 0.f, eyeWidth, static_cast<float>(depthDesc.Height), 0.f, 1.f};
                            context->RSSetViewports(1, &vp);
                            float raster[16];
                            Multiply(x.rasterWorldToClip[eye], current, raster);
                            GpuClip(raster, r.rasterClip);
                            Multiply(x.worldToClip[eye], current, r.currentClip);
                            Multiply(x.prevWorldToClip[eye], previous, r.previousClip);
                            const float* p = frame.base.viewToClip[eye];
                            r.depthParams[0] = p[10];  // P22
                            r.depthParams[1] = p[11];  // P23
                            r.depthParams[2] = 0.015f; // metres
                            r.depthParams[3] = 0.03f;
                            VamWriteConstants(context, objects.constants.Get(), &r, sizeof(r));
                            context->DrawIndexed(d.indexCount, d.indexStart, d.baseVertex);
                        }
                        ++rigidDrawn;
                    }
                    ID3D11Buffer* noBuffer = nullptr;
                    const UINT zero = 0;
                    context->IASetVertexBuffers(0, 1, &noBuffer, &zero, &zero);
                    context->IASetInputLayout(nullptr);
                }
                if (rigid && !objects.skinnedFailed) {
                    // Animated CUAs: same targets, depth test and stencil, skinned twice in the shader.
                    try { DrawSkinned(frame, draws, count, rigidTable, rigidCount, eyeWidth, skinnedDrawn, skinnedRejected); }
                    catch (const std::exception& e) {
                        objects.skinnedFailed = true;
                        OOVR_LOGF("VAM DLSS skinned motion disabled for this run (animated CUAs keep camera motion): %s", e.what());
                        UpdateState();
                    }
                    ID3D11Buffer* noBuffers[2] = {};
                    const UINT zeros[2] = {};
                    context->IASetVertexBuffers(0, 2, noBuffers, zeros, zeros);
                    context->IASetInputLayout(nullptr);
                    ID3D11ShaderResourceView* noRows = nullptr;
                    context->VSSetShaderResources(1, 1, &noRows);
                }
            }
            if (hair) {
                // Hair where no skin or clothing claimed the pixel; nearest ribbon wins.
                // Its shader writes no fold band: that target is left out.
                ID3D11RenderTargetView* hairTargets[3] = {objects.motionRtv.Get(), objects.hairMaskRtv.Get(), objects.personDepthRtv.Get()};
                context->OMSetRenderTargets(3, hairTargets, objects.depthDsv.Get());
                ID3D11Query* query = nullptr;
                context->OMSetDepthStencilState(objects.hairDepthState.Get(), 0);
                context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
                context->VSSetShader(objects.hairVertex.Get(), nullptr, 0);
                context->PSSetShader(objects.hairPixel.Get(), nullptr, 0);
                context->VSSetConstantBuffers(0, 1, objects.hairConstants.GetAddressOf());
                context->PSSetConstantBuffers(0, 1, objects.hairConstants.GetAddressOf());
                HairConstants h{};
                for (int i = 0; i < count; ++i) {
                    const ObjectDraw& d = draws[i];
                    if (!(d.flags & kDrawHair)) continue;
                    HairSource* s = ResolveHair(d);
                    if (!s) { ++hairRejected; continue; }
                    if (!query && !objects.hairQueryPending[objects.nextHairQuery]) {
                        query = objects.hairQueries[objects.nextHairQuery].Get();
                        immediate->Begin(query);
                    }
                    const bool history = s->previousValid && s->previous && s->previousCount == s->count && s->previousPerStrand == s->perStrand;
                    ID3D11ShaderResourceView* particles[2] = {s->srv.Get(), history ? s->previousSrv.Get() : s->srv.Get()};
                    context->VSSetShaderResources(0, 2, particles);
                    h.count = s->count;
                    h.perStrand = s->perStrand;
                    h.previousValid = history ? 1u : 0u;
                    h.maxSegment = kHairMaxSegment;
                    for (unsigned eye = 0; eye < frame.viewCount; ++eye) {
                        D3D11_VIEWPORT vp{eye * eyeWidth, 0.f, eyeWidth, static_cast<float>(depthDesc.Height), 0.f, 1.f};
                        context->RSSetViewports(1, &vp);
                        GpuClip(x.rasterWorldToClip[eye], h.rasterClip);
                        std::copy(x.worldToClip[eye], x.worldToClip[eye] + 16, h.currentClip);
                        std::copy(x.prevWorldToClip[eye], x.prevWorldToClip[eye] + 16, h.previousClip);
                        Column(frame.base.viewToWorld[eye], 3, h.camera, false);
                        h.camera[3] = kHairHalfWidth;
                        const float* p = frame.base.viewToClip[eye];
                        h.depthParams[0] = p[10];
                        h.depthParams[1] = p[11];
                        h.depthParams[2] = kHairDepthAbsolute;
                        h.depthParams[3] = kHairDepthRelative;
                        VamWriteConstants(context, objects.hairConstants.Get(), &h, sizeof(h));
                        context->Draw((s->count - 1) * 6, 0);
                    }
                    hairSegments += s->count - 1;
                    ++hairDrawn;
                }
                if (query) {
                    immediate->End(query);
                    objects.hairQueryPending[objects.nextHairQuery] = true;
                    objects.nextHairQuery = (objects.nextHairQuery + 1) % kHairQueries;
                }
            }
            if (ui) {
                // Menus last: where a UI element is in front of the scene (and of
                // matched skin or hair) it takes the pixel's motion and depth.
                const int quads = std::min(x.uiQuadCount, kMaxUiQuads);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                Check(context->Map(objects.uiQuads.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "UI rectangles map");
                std::memcpy(mapped.pData, reinterpret_cast<const void*>(x.uiQuads), sizeof(UiQuad) * quads);
                context->Unmap(objects.uiQuads.Get(), 0);
                // DLSSUIMotion=0 with the bypass on: only the UI depth, for the UI mask.
                const bool uiMotion=settings.dlssUiMotion || VamPathRequested();
                ID3D11RenderTargetView* uiTargets[3] = {uiMotion ? objects.motionRtv.Get() : nullptr,
                    uiMotion ? objects.hairMaskRtv.Get() : nullptr, objects.uiDepthRtv.Get()};
                context->OMSetRenderTargets(3, uiTargets, objects.depthDsv.Get());
                context->OMSetDepthStencilState(objects.uiDepthState.Get(), 0);
                context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
                context->VSSetShader(objects.uiVertex.Get(), nullptr, 0);
                context->PSSetShader(objects.uiPixel.Get(), nullptr, 0);
                ID3D11ShaderResourceView* rectangles[2] = {objects.uiQuadsSrv.Get(), nullptr};
                context->VSSetShaderResources(0, 2, rectangles);
                context->VSSetConstantBuffers(0, 1, objects.uiConstants.GetAddressOf());
                context->PSSetConstantBuffers(0, 1, objects.uiConstants.GetAddressOf());
                ID3D11Query* query = nullptr;
                if (!objects.uiQueryPending[objects.nextUiQuery]) {
                    query = objects.uiQueries[objects.nextUiQuery].Get();
                    immediate->Begin(query);
                }
                UiConstants u{};
                for (unsigned eye = 0; eye < frame.viewCount; ++eye) {
                    D3D11_VIEWPORT vp{eye * eyeWidth, 0.f, eyeWidth, static_cast<float>(depthDesc.Height), 0.f, 1.f};
                    context->RSSetViewports(1, &vp);
                    GpuClip(x.rasterWorldToClip[eye], u.rasterClip);
                    std::copy(x.worldToClip[eye], x.worldToClip[eye] + 16, u.currentClip);
                    std::copy(x.prevWorldToClip[eye], x.prevWorldToClip[eye] + 16, u.previousClip);
                    const float* p = frame.base.viewToClip[eye];
                    u.depthParams[0] = p[10];
                    u.depthParams[1] = p[11];
                    u.depthParams[2] = kUiDepthAbsolute;
                    u.depthParams[3] = kUiDepthRelative;
                    VamWriteConstants(context, objects.uiConstants.Get(), &u, sizeof(u));
                    context->Draw(static_cast<UINT>(quads) * 6, 0);
                }
                if (query) {
                    immediate->End(query);
                    objects.uiQueryPending[objects.nextUiQuery] = true;
                    objects.nextUiQuery = (objects.nextUiQuery + 1) % kUiQueries;
                }
                uiDrawn = static_cast<uint64_t>(quads);
            }
            // This frame's positions become next frame's history.
            for (auto& [key, s] : objects.sources) {
                if (s.frameUsed != objects.frame || s.invalid || !s.buffer) continue;
                if (!s.previous) {
                    D3D11_BUFFER_DESC desc{};
                    s.buffer->GetDesc(&desc);
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    desc.CPUAccessFlags = 0;
                    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
                    Check(device->CreateBuffer(&desc, nullptr, &s.previous), "object history");
                    D3D11_SHADER_RESOURCE_VIEW_DESC v{};
                    v.Format = DXGI_FORMAT_UNKNOWN;
                    v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                    v.Buffer.NumElements = desc.ByteWidth / desc.StructureByteStride;
                    Check(device->CreateShaderResourceView(s.previous.Get(), &v, &s.previousSrv), "object history SRV");
                }
                context->CopyResource(s.previous.Get(), s.buffer.Get());
                s.previousValid = true;
                s.previousSerial=static_cast<uint64_t>(frame.base.serial);
                std::copy(s.offset, s.offset + 3, s.previousOffset);
            }
            for (auto& [key, s] : objects.hairs) {
                if (s.frameUsed != objects.frame || s.invalid || !s.buffer) continue;
                if (!s.previous) {
                    D3D11_BUFFER_DESC desc{};
                    desc.ByteWidth = s.byteWidth;
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
                    desc.StructureByteStride = kHairStride;
                    Check(device->CreateBuffer(&desc, nullptr, &s.previous), "hair history");
                    D3D11_SHADER_RESOURCE_VIEW_DESC v{};
                    v.Format = DXGI_FORMAT_UNKNOWN;
                    v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                    v.Buffer.NumElements = s.byteWidth / kHairStride;
                    Check(device->CreateShaderResourceView(s.previous.Get(), &v, &s.previousSrv), "hair history SRV");
                }
                const D3D11_BOX box{0, 0, 0, s.count * kHairStride, 1, 1}; // the valid points only
                context->CopySubresourceRegion(s.previous.Get(), 0, 0, 0, 0, s.buffer.Get(), 0, &box);
                s.previousValid = true;
                s.previousSerial=static_cast<uint64_t>(frame.base.serial);
                s.previousCount = s.count;
                s.previousPerStrand = s.perStrand;
            }
        }
        for (auto it = objects.sources.begin(); it != objects.sources.end();)
            it = it->second.lastSeen + kObjectEvictFrames < objects.frame ? objects.sources.erase(it) : std::next(it);
        for (auto it = objects.indices.begin(); it != objects.indices.end();)
            it = it->second.lastSeen + kObjectEvictFrames < objects.frame ? objects.indices.erase(it) : std::next(it);
        for (auto it = objects.hairs.begin(); it != objects.hairs.end();)
            it = it->second.lastSeen + kObjectEvictFrames < objects.frame ? objects.hairs.erase(it) : std::next(it);
        for (auto it = objects.folds.begin(); it != objects.folds.end();)
            it = it->second.lastSeen + kObjectEvictFrames < objects.frame ? objects.folds.erase(it) : std::next(it);
        for (auto it = objects.rigids.begin(); it != objects.rigids.end();)
            it = it->second.lastSeen + kObjectEvictFrames < objects.frame ? objects.rigids.erase(it) : std::next(it);
        for (auto it = objects.skinneds.begin(); it != objects.skinneds.end();)
            it = it->second.lastSeen + kObjectEvictFrames < objects.frame ? objects.skinneds.erase(it) : std::next(it);
        objects.rigidDraws += rigidDrawn;
        objects.rigidRejected += rigidRejected;
        objects.skinnedDraws += skinnedDrawn;
        objects.skinnedRejected += skinnedRejected;
        objects.foldDraws += foldDrawn;
        objects.foldDrawsLast = foldDrawn;
        objects.foldRejected += foldRejected;
        objects.drawn += drawn;
        objects.rejected += rejected;
        objects.hairDrawsTotal += hairDrawn;
        objects.hairRejected += hairRejected;
        if (hairDrawn) objects.hairSegments = hairSegments;
        objects.uiRectsLast = uiDrawn;
        if (uiDrawn) {
            objects.uiRects += uiDrawn;
            ++objects.uiFrames;
        }
        objects.valid = drawn > 0 || rigidDrawn > 0 || skinnedDrawn > 0 || hairDrawn > 0 || uiDrawn > 0;
        objects.validSerial = static_cast<uint64_t>(frame.base.serial);
        if (!objects.logged && drawn) {
            objects.logged = true;
            OOVR_LOGF("VAM DLSS object motion active: %llu draws, %zu skinned sources, %llu rejected",
                static_cast<unsigned long long>(drawn), objects.sources.size(), static_cast<unsigned long long>(rejected));
        }
        if (!objects.rigidLogged && rigidDrawn) {
            objects.rigidLogged = true;
            OOVR_LOGF("VAM DLSS rigid motion active: %llu submesh draws of %d moving renderer(s), %zu vertex buffers, %llu rejected",
                static_cast<unsigned long long>(rigidDrawn), rigidCount, objects.rigids.size(), static_cast<unsigned long long>(rigidRejected));
        }
        if (!objects.skinnedLogged && skinnedDrawn) {
            objects.skinnedLogged = true;
            OOVR_LOGF("VAM DLSS skinned motion active: %llu submesh draws of moving SkinnedMeshRenderer(s), %zu meshes, %d matrix pairs, %llu rejected",
                static_cast<unsigned long long>(skinnedDrawn), objects.skinneds.size(), rigidCount, static_cast<unsigned long long>(skinnedRejected));
        }
        if (!objects.hairLogged && hairDrawn) {
            objects.hairLogged = true;
            const auto& first = objects.hairs.begin()->second;
            OOVR_LOGF("VAM DLSS hair motion active: %llu hair items, %llu guide segments (%u points, %u per strand%s), ribbon %.0f mm, depth tolerance %.0f mm",
                static_cast<unsigned long long>(hairDrawn), static_cast<unsigned long long>(hairSegments), first.count, first.perStrand,
                first.perStrand ? "" : " unknown: split by RootIndex", kHairHalfWidth * 2000.f, kHairDepthAbsolute * 1000.f);
        }
        if (!objects.foldLogged && foldDrawn) {
            objects.foldLogged = true;
            OOVR_LOGF("VAM NR fold band active: %llu female face draw(s) carry the nasolabial band (NasolabialReduction=%d%%: NR's result x %.2f at its core)",
                static_cast<unsigned long long>(foldDrawn), settings.nrFold, 1.f - settings.nrFold / 100.f);
        }
        if (!objects.uiLogged && uiDrawn) {
            objects.uiLogged = true;
            OOVR_LOGF("VAM DLSS UI motion active: %llu UI rectangles from the bridge (menus, scene panels), kept in front of the scene or within %.0f mm of it",
                static_cast<unsigned long long>(uiDrawn), kUiDepthAbsolute * 1000.f);
        }
        if (objects.frame % 900 == 0) {
            OOVR_LOGF("VAM DLSS object motion: %llu draws this frame, %zu sources, %llu rejected in total",
                static_cast<unsigned long long>(drawn), objects.sources.size(), static_cast<unsigned long long>(objects.rejected));
            if (rigid || objects.rigidDraws)
                OOVR_LOGF("VAM DLSS rigid motion: %llu submesh draws this frame, %llu so far, %zu vertex buffers, %llu rejected in total",
                    static_cast<unsigned long long>(rigidDrawn), static_cast<unsigned long long>(objects.rigidDraws), objects.rigids.size(),
                    static_cast<unsigned long long>(objects.rigidRejected));
            if (objects.skinnedDraws || objects.skinnedRejected)
                OOVR_LOGF("VAM DLSS skinned motion: %llu submesh draws this frame, %llu so far, %zu meshes, %llu rejected in total",
                    static_cast<unsigned long long>(skinnedDrawn), static_cast<unsigned long long>(objects.skinnedDraws), objects.skinneds.size(),
                    static_cast<unsigned long long>(objects.skinnedRejected));
            if (fold || objects.foldDraws)
                OOVR_LOGF("VAM NR fold band: %llu face draws this frame, %llu so far, %llu rejected in total",
                    static_cast<unsigned long long>(foldDrawn), static_cast<unsigned long long>(objects.foldDraws),
                    static_cast<unsigned long long>(objects.foldRejected));
            if (hair && (hairDrawn || objects.hairDrawsTotal))
                OOVR_LOGF("VAM DLSS hair motion: %llu hair items this frame, %llu guide segments, %llu samples matched (latest check), %llu of %llu checks empty, %llu rejected in total",
                    static_cast<unsigned long long>(hairDrawn), static_cast<unsigned long long>(hairSegments),
                    static_cast<unsigned long long>(objects.hairSamples), static_cast<unsigned long long>(objects.hairEmptyChecks),
                    static_cast<unsigned long long>(objects.hairChecks), static_cast<unsigned long long>(objects.hairRejected));
            if (ui || objects.uiFrames)
                OOVR_LOGF("VAM DLSS UI motion: %llu UI rectangles this frame, %llu samples in front of the scene (latest check), %llu of %llu checks empty, %llu frames with UI so far",
                    static_cast<unsigned long long>(uiDrawn), static_cast<unsigned long long>(objects.uiSamples),
                    static_cast<unsigned long long>(objects.uiEmptyChecks), static_cast<unsigned long long>(objects.uiChecks),
                    static_cast<unsigned long long>(objects.uiFrames));
        }
    }

    // Unity render thread, during the main camera, before the transparents (bridge 2.9.6, raster route): the depth of
    // the opaque scene. A see-through thing that writes depth (haze, beams, glitter shells, glass) is not in it, so it
    // takes no person's depth or motion in the guides; Capture still copies the depth with the transparents, which a
    // person's own see-through clothing is matched against (user 11-05: clothing must always have depth and motion).
    void CaptureOpaque(const FrameInfoV3& frameV2) {
        std::lock_guard lock(mutex);
        if (failed) return;
        Ptr<ID3D11RenderTargetView> rtv;
        Ptr<ID3D11DepthStencilView> dsv;
        immediate->OMGetRenderTargets(1, &rtv, &dsv);
        auto skip = [&](const char* why) {
            if (opaqueProblems++ % 600 == 0)
                OOVR_LOGF("VAM DLSS opaque depth not taken (%s, count=%llu)", why, static_cast<unsigned long long>(opaqueProblems));
        };
        if (!dsv) { skip("no depth buffer bound at CameraEvent.BeforeForwardAlpha"); return; }
        Ptr<ID3D11Resource> resource;
        dsv->GetResource(&resource);
        Ptr<ID3D11Texture2D> texture;
        if (FAILED(resource.As(&texture))) { skip("not a 2D texture"); return; }
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        DXGI_FORMAT typeless, view;
        if (desc.SampleDesc.Count != 1 || desc.ArraySize != 1 || !DepthFormats(desc.Format, typeless, view)) { skip("unsupported depth target"); return; }
        if (!opaqueCopy || opaqueDesc.Width != desc.Width || opaqueDesc.Height != desc.Height ||
            opaqueDesc.MipLevels != desc.MipLevels || opaqueDesc.Format != typeless) {
            opaqueCopy.Reset(); opaqueSrv.Reset();
            D3D11_TEXTURE2D_DESC copy = desc;
            copy.Format = typeless;
            copy.Usage = D3D11_USAGE_DEFAULT;
            copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            copy.CPUAccessFlags = copy.MiscFlags = 0;
            Check(device->CreateTexture2D(&copy, nullptr, &opaqueCopy), "opaque depth copy");
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = view;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            Check(device->CreateShaderResourceView(opaqueCopy.Get(), &sv, &opaqueSrv), "opaque depth SRV");
            opaqueDesc = copy;
            OOVR_LOGF("VAM DLSS bridge opaque depth capture: %ux%u format=%u", desc.Width, desc.Height, desc.Format);
        }
        immediate->CopyResource(opaqueCopy.Get(), texture.Get());
        opaqueSerial = frameV2.base.serial;
        // 11-06: and the color before the transparents (see opaqueColorCopy).
        opaqueColorSerial = CopyColor(rtv.Get(), opaqueColorCopy, opaqueColorSrv, opaqueColorDesc, "opaque")
            ? frameV2.base.serial : -1;
    }

    // Unity render thread, during the main stereo camera, after transparents.
    void Capture(const FrameInfoV3& frameV2) {
        const FrameInfo& frame = frameV2.base;
        std::lock_guard lock(mutex);
        if(failed && VamDesktopDisplay() && GetTickCount64()>=retryAt){
            for(auto& e:eyes){ReleaseFeature(e);e.evaluatedSerial=0;e.badW=0;}
            failed=false;haveCapture=false;UpdateState();
        }
        if (failed) return;
        Ptr<ID3D11RenderTargetView> rtv;
        Ptr<ID3D11DepthStencilView> dsv;
        immediate->OMGetRenderTargets(1, &rtv, &dsv);
        if (!dsv) { Problem("no depth buffer bound at CameraEvent.AfterForwardAlpha"); return; }
        Ptr<ID3D11Resource> resource;
        dsv->GetResource(&resource);
        Ptr<ID3D11Texture2D> texture;
        if (FAILED(resource.As(&texture))) { Problem("depth target is not a 2D texture"); return; }
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.SampleDesc.Count != 1) { Problem("MSAA depth buffer; the bridge must turn MSAA off"); return; }
        if (desc.ArraySize != 1) { Problem("texture-array stereo depth is not supported"); return; }
        DXGI_FORMAT typeless, view;
        if (!DepthFormats(desc.Format, typeless, view)) { Problem("unsupported depth format " + std::to_string(desc.Format)); return; }
        if (!depthCopy || depthDesc.Width != desc.Width || depthDesc.Height != desc.Height ||
            depthDesc.MipLevels != desc.MipLevels || depthDesc.Format != typeless) {
            depthCopy.Reset(); depthSrv.Reset();
            D3D11_TEXTURE2D_DESC copy = desc;
            copy.Format = typeless;
            copy.Usage = D3D11_USAGE_DEFAULT;
            copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            copy.CPUAccessFlags = copy.MiscFlags = 0;
            Check(device->CreateTexture2D(&copy, nullptr, &depthCopy), "depth copy");
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = view;
            sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sv.Texture2D.MipLevels = 1;
            Check(device->CreateShaderResourceView(depthCopy.Get(), &sv, &depthSrv), "depth SRV");
            depthDesc = copy;
            D3D11_TEXTURE2D_DESC colorDesc{};
            if (rtv) {
                Ptr<ID3D11Resource> colorResource;
                rtv->GetResource(&colorResource);
                Ptr<ID3D11Texture2D> colorTexture;
                if (SUCCEEDED(colorResource.As(&colorTexture))) colorTexture->GetDesc(&colorDesc);
            }
            OOVR_LOGF("VAM DLSS bridge depth capture: depth=%ux%u format=%u color target=%ux%u format=%u eye=%ux%u",
                desc.Width, desc.Height, desc.Format, colorDesc.Width, colorDesc.Height, colorDesc.Format, frame.eyeWidth, frame.eyeHeight);
        }
        rrCamera.Reset();
        if ((ReadVamPerformanceSettings().dlssPreset == 6 || VamPathRequested()) && rtv) { Ptr<ID3D11Resource> color; rtv->GetResource(&color); color.As(&rrCamera); }
        immediate->CopyResource(depthCopy.Get(), texture.Get());
        // 11-05: the depth before the transparents, when the bridge gave it for this frame (raster route, 2.9.6).
        const bool opaqueThisFrame = opaqueSrv && opaqueSerial == frame.serial && opaqueDesc.Width == depthDesc.Width &&
            opaqueDesc.Height == depthDesc.Height && opaqueDesc.Format == depthDesc.Format;
        sceneSrv = opaqueThisFrame ? opaqueSrv : depthSrv;
        if ((opaqueThisFrame ? 1 : 0) != sceneSource) {
            sceneSource = opaqueThisFrame ? 1 : 0;
            OOVR_LOGF("VAM DLSS scene depth: %s", opaqueThisFrame
                ? "before the transparents (guides, props); a person's skin and clothing (mesh hair too) also match the depth with them where the transparents visibly changed the pixel"
                : "with the transparents (no depth from before them this frame)");
        }
        // 11-06: the colors before and with the transparents, both this frame's and the depth's size (else a part in the
        // depth with the transparents takes the pixel as on 11-05).
        colorsThisFrame = false;
        if (opaqueThisFrame && opaqueColorSrv && opaqueColorSerial == frame.serial &&
            CopyColor(rtv.Get(), fullColorCopy, fullColorSrv, fullColorDesc, "full")) {
            colorsThisFrame = opaqueColorDesc.Width == fullColorDesc.Width && opaqueColorDesc.Height == fullColorDesc.Height &&
                opaqueColorDesc.Format == fullColorDesc.Format && fullColorDesc.Width == depthDesc.Width &&
                fullColorDesc.Height == depthDesc.Height;
        }
        if ((colorsThisFrame ? 1 : 0) != colorSource) {
            colorSource = colorsThisFrame ? 1 : 0;
            OOVR_LOGF("VAM DLSS see-through test: %s", colorsThisFrame
                ? "a person's part in the depth with the transparents takes the pixel only where they visibly changed it"
                : "off (no matching colors before and with the transparents this frame)");
        }
        if (haveCapture && frame.serial == info.serial && ++doubleCaptures % 600 == 1)
            OOVR_LOGF("VAM DLSS bridge: Unity frame %lld captured again; the later capture is used (count=%llu)",
                static_cast<long long>(frame.serial), static_cast<unsigned long long>(doubleCaptures));
        viewCount = frameV2.viewCount;
        info = frame;
        ext = frameV2.ext;
        haveCapture = true;
        captureTick = GetTickCount64();
        if (++captures == 1) {
            OOVR_LOG("VAM DLSS bridge: first frame capture received from BepInEx");
            WriteStatus("capturing", "bridge frames received");
        }
        if (!lastProblem.empty()) { lastProblem.clear(); UpdateState(); }
        HMODULE skinModule=GetModuleHandleW(L"VaMSkinTransmissionNative.bin");
        auto pathEnabled=skinModule?reinterpret_cast<int(__cdecl*)()>(GetProcAddress(skinModule,"ST_PathTracingEnabledV1")):nullptr;
        const bool pathMotion=pathEnabled && pathEnabled()!=0;
        if(pathMotion!=pathOwnsMotion) {
            pathOwnsMotion=pathMotion;
            // These are exclusively the old raster-motion histories. PT owns
            // independent previous geometry; release stale copies on either
            // transition so returning raster motion starts with fresh history.
            // boneRows stays: it is created once at startup and rewritten whole
            // every frame, and DrawSkinned would Map a null buffer without it.
            objects.sources.clear();objects.indices.clear();objects.hairs.clear();
            objects.folds.clear();objects.rigids.clear();objects.skinneds.clear();
            objects.valid=false;objects.hairSegments=0;
            UpdateState();
            OOVR_LOGF("VAM PT owns motion=%d: legacy mesh/hair motion %s; UI retained; NR fold=%d",pathOwnsMotion?1:0,pathOwnsMotion?"stopped":"restored",settings.nrFold);
        }
        motionTimer.Begin(device.Get(), immediate.Get(), 0);
        try {
            RenderObjectMotion(frameV2);
            motionTimer.End();
        } catch (const std::exception& e) {
            motionTimer.Abandon();
            objects.failed = true;
            objects.valid = false;
            OOVR_LOGF("VAM DLSS object motion disabled for this run (camera motion only): %s", e.what());
            UpdateState();
        }
    }

    void EnsureGuideTextures(Eye& eye, UINT w, UINT h) {
        if (eye.width == w && eye.height == h && eye.depth) return;
        eye.depth.Reset(); eye.motion.Reset(); eye.motionJittered.Reset(); eye.motionHalf.Reset(); eye.motionJitteredHalf.Reset(); eye.hairMask.Reset();
        eye.depthUav.Reset(); eye.motionUav.Reset(); eye.motionJitteredUav.Reset(); eye.motionHalfUav.Reset(); eye.motionJitteredHalfUav.Reset(); eye.hairMaskUav.Reset();
        eye.uiMask.Reset(); eye.uiMaskUav.Reset(); eye.uiMaskSrv.Reset();
        eye.nrControl.Reset(); eye.nrControlUav.Reset();
        eye.depthSrv.Reset(); eye.motionSrv.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h;
        td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        td.Format = DXGI_FORMAT_R32_FLOAT;
        Check(device->CreateTexture2D(&td, nullptr, &eye.depth), "guide depth");
        Check(device->CreateUnorderedAccessView(eye.depth.Get(), nullptr, &eye.depthUav), "guide depth UAV");
        td.Format = DXGI_FORMAT_R32G32_FLOAT;
        Check(device->CreateTexture2D(&td, nullptr, &eye.motion), "guide motion");
        Check(device->CreateUnorderedAccessView(eye.motion.Get(), nullptr, &eye.motionUav), "guide motion UAV");
        Check(device->CreateTexture2D(&td, nullptr, &eye.motionJittered), "guide jittered motion");
        Check(device->CreateUnorderedAccessView(eye.motionJittered.Get(), nullptr, &eye.motionJitteredUav), "guide jittered motion UAV");
        td.Format = DXGI_FORMAT_R16G16_FLOAT;
        Check(device->CreateTexture2D(&td, nullptr, &eye.motionHalf), "guide half motion");
        Check(device->CreateUnorderedAccessView(eye.motionHalf.Get(), nullptr, &eye.motionHalfUav), "guide half motion UAV");
        Check(device->CreateTexture2D(&td, nullptr, &eye.motionJitteredHalf), "guide half jittered motion");
        Check(device->CreateUnorderedAccessView(eye.motionJitteredHalf.Get(), nullptr, &eye.motionJitteredHalfUav), "guide half jittered motion UAV");
        td.Format = DXGI_FORMAT_R8_UNORM;
        Check(device->CreateTexture2D(&td, nullptr, &eye.hairMask), "guide hair mask");
        Check(device->CreateUnorderedAccessView(eye.hairMask.Get(), nullptr, &eye.hairMaskUav), "guide hair mask UAV");
        Check(device->CreateTexture2D(&td, nullptr, &eye.uiMask), "guide UI mask");
        Check(device->CreateUnorderedAccessView(eye.uiMask.Get(), nullptr, &eye.uiMaskUav), "guide UI mask UAV");
        Check(device->CreateShaderResourceView(eye.uiMask.Get(), nullptr, &eye.uiMaskSrv), "guide UI mask SRV");
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; // NR keep mask (G)
        Check(device->CreateTexture2D(&td, nullptr, &eye.nrControl), "guide NR control mask");
        Check(device->CreateUnorderedAccessView(eye.nrControl.Get(), nullptr, &eye.nrControlUav), "guide NR control mask UAV");
        eye.width = w; eye.height = h;
        eye.guideSerial = 0;
    }

    bool PrepareGuides(ID3D11Texture2D* submitted, const float* bounds, unsigned eyeIndex, UINT w, UINT h, VamFrameGuides& out, bool ahead, bool reconstruction = false) {
        std::lock_guard lock(mutex);
        if (failed || !haveCapture || eyeIndex > 1 || !submitted || !bounds) return false;
        auto& eye = eyes[eyeIndex];
        if (eye.ahead && !ahead && !reconstruction) {
            eye.ahead = false;
            if (eye.guideSerial == static_cast<uint64_t>(info.serial) && eye.aheadGuides.serial == eye.guideSerial &&
                eye.aheadSubmitted == submitted && std::equal(bounds, bounds + 4, eye.aheadBounds) && eye.width == w && eye.height == h) {
                out = eye.aheadGuides;
                return true;
            }
            // Not the submission they were made for: made again below.
            eye.guideSerial = eye.aheadPreviousSerial;
        }
        // RR is invoked immediately after this render event's successful capture.
        // First-use shader/NGX creation may exceed 250 ms without changing the
        // frame. Only asynchronous eye submission needs a wall-clock age guard.
        if (!reconstruction && GetTickCount64() - captureTick > 250) {
            if (++eye.staleMisses == 1 || eye.staleMisses % 600 == 0)
                OOVR_LOGF("VAM DLSS guides missing eye=%u: last bridge capture is %llu ms old (count=%llu)", eyeIndex,
                    static_cast<unsigned long long>(GetTickCount64() - captureTick), static_cast<unsigned long long>(eye.staleMisses));
            return false;
        }
        // Each Unity frame produces exactly one capture: reusing an older one
        // for a new color image would give DLSS mismatched depth and motion.
        if (!reconstruction && eye.guideSerial == static_cast<uint64_t>(info.serial) && eye.guideSerial) {
            if (++eye.reusedMisses == 1 || eye.reusedMisses % 600 == 0)
                OOVR_LOGF("VAM DLSS guides missing eye=%u: submitted again without a new Unity frame (serial %lld, count=%llu)", eyeIndex,
                    static_cast<long long>(info.serial), static_cast<unsigned long long>(eye.reusedMisses));
            return false;
        }
        D3D11_TEXTURE2D_DESC sd{};
        submitted->GetDesc(&sd);
        if (sd.Width != depthDesc.Width || sd.Height != depthDesc.Height) {
            Problem("depth " + std::to_string(depthDesc.Width) + "x" + std::to_string(depthDesc.Height) +
                " does not match submitted eye texture " + std::to_string(sd.Width) + "x" + std::to_string(sd.Height));
            return false;
        }
        const float u0 = std::min(bounds[0], bounds[2]), v0 = std::min(bounds[1], bounds[3]);
        const bool flip = bounds[1] > bounds[3];
        EnsureGuideTextures(eye, w, h);
        if(!reconstruction && eye.pathGuideSerial==static_cast<uint64_t>(info.serial) && eye.pathGuideSerial) {
            out=eye.pathGuides;const uint64_t previous=eye.guideSerial;eye.guideSerial=out.serial;
            if(ahead){eye.ahead=true;eye.aheadGuides=out;eye.aheadSubmitted=submitted;std::copy(bounds,bounds+4,eye.aheadBounds);eye.aheadPreviousSerial=previous;}
            return true;
        }
        if(reconstruction)eye.pathGuideSerial=0;
        GuideConstants c{};
        const float* m = info.clipToPrevClip[eyeIndex];
        std::copy(m, m + 16, c.clipToPrevClip);
        c.origin[0] = static_cast<int32_t>(std::lround(u0 * sd.Width));
        c.origin[1] = static_cast<int32_t>(std::lround(v0 * sd.Height));
        c.size[0] = static_cast<int32_t>(w); c.size[1] = static_cast<int32_t>(h);
        const bool jittered = (info.flags & 2) != 0;
        const float jx = jittered ? info.jitterX : 0.f, jy = jittered ? info.jitterY : 0.f;
        c.jitterNdc[0] = 2.f * jx / w;
        c.jitterNdc[1] = -2.f * jy / h;
        c.jitterDelta[0] = info.prevJitterX - jx;
        c.jitterDelta[1] = info.prevJitterY - jy;
        c.flip = flip ? 1 : 0;
        c.depthMax[0] = static_cast<int32_t>(sd.Width) - 1; c.depthMax[1] = static_cast<int32_t>(sd.Height) - 1;
        c.objectMotion = objects.valid && objects.validSerial == static_cast<uint64_t>(info.serial) &&
            objects.width == sd.Width && objects.height == sd.Height ? 1 : 0;
        c.hairBias = settings.dlssHairMotion ? settings.dlssHairBias / 100.f : 0.f;
        c.uiDepthMerge = !reconstruction && settings.dlssUiMotion ? 1 : 0;
        // The band is drawn only while this is above 0 (kStateNrFold); 0 leaves NR's composite as it was.
        const bool nrControl = settings.nrFold > 0 && !objects.failed && !objects.foldFailed;
        c.nrFoldCut = nrControl ? settings.nrFold / 100.f : 0.f;
        c.pad=(reconstruction && pathOwnsMotion?1.f:0.f)+(viewCount==1?2.f:0.f);
        {
            ContextScope scope(context.Get(), state.Get());
            VamWriteConstants(context, guideConstants.Get(), &c, sizeof(c));
            context->CSSetShader(guideCs.Get(), nullptr, 0);
            context->CSSetConstantBuffers(0, 1, guideConstants.GetAddressOf());
            ID3D11ShaderResourceView* sources[6] = {sceneSrv.Get(), c.objectMotion ? objects.motionSrv.Get() : nullptr,
                c.objectMotion ? objects.hairMaskSrv.Get() : nullptr, c.objectMotion ? objects.uiDepthSrv.Get() : nullptr,
                c.objectMotion ? objects.foldSrv.Get() : nullptr, c.objectMotion ? objects.personDepthSrv.Get() : nullptr};
            context->CSSetShaderResources(0, 6, sources);
            ID3D11UnorderedAccessView* uavs[] = {eye.depthUav.Get(), eye.motionUav.Get(), eye.motionJitteredUav.Get(), eye.motionHalfUav.Get(),
                eye.motionJitteredHalfUav.Get(), eye.hairMaskUav.Get(), eye.uiMaskUav.Get(), eye.nrControlUav.Get()};
            context->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
            context->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        }
        const bool gap = eye.guideSerial == 0 || static_cast<uint64_t>(info.serial) != eye.guideSerial + 1;
        const uint64_t previousSerial = eye.guideSerial;
        if (!reconstruction) eye.guideSerial = static_cast<uint64_t>(info.serial);
        out.depth = eye.depth.Get();
        out.motion = eye.motion.Get();
        out.motionJittered = eye.motionJittered.Get();
        out.motionHalf = eye.motionHalf.Get();
        out.motionJitteredHalf = eye.motionJitteredHalf.Get();
        out.hairMask = c.hairBias > 0.f ? eye.hairMask.Get() : nullptr;
        // Only on frames whose UI pass drew menus (elsewhere the mask is all 0).
        out.uiMask = c.objectMotion && objects.uiRectsLast > 0 && (settings.dlssUiBypass || reconstruction) ? eye.uiMask.Get() : nullptr;
        // Every frame while it is on, faces in view or not (all 1 then).
        out.nrControl = nrControl ? eye.nrControl.Get() : nullptr;
        out.jitterX = jx; out.jitterY = jy;
        out.frameDeltaMs = info.frameDeltaMs > 0.f ? info.frameDeltaMs : 16.6667f;
        out.serial = static_cast<uint64_t>(info.serial);
        out.reset = (info.flags & 1) != 0 || gap || eye.sceneResetSerial==static_cast<uint64_t>(info.serial);
        out.nativeFrame = (info.flags & 4) != 0;
        out.menuResized = (info.flags & 8) != 0;
        std::copy(info.viewToClip[eyeIndex], info.viewToClip[eyeIndex] + 16, out.viewToClip);
        std::copy(info.clipToPrevClip[eyeIndex], info.clipToPrevClip[eyeIndex] + 16, out.clipToPrevClip);
        std::copy(info.viewToWorld[eyeIndex], info.viewToWorld[eyeIndex] + 16, out.viewToWorld);
        out.nearPlane = info.nearPlane;
        out.farPlane = info.farPlane;
        if (info.eyeWidth != w || info.eyeHeight != h) {
            static bool logged = false;
            if (!logged) {
                OOVR_LOGF("VAM DLSS bridge: Unity eye size %ux%u differs from submitted crop %ux%u; using the crop", info.eyeWidth, info.eyeHeight, w, h);
                logged = true;
            }
        }
        if (ahead) {
            eye.ahead = true;
            eye.aheadGuides = out;
            eye.aheadSubmitted = submitted;
            std::copy(bounds, bounds + 4, eye.aheadBounds);
            eye.aheadPreviousSerial = previousSerial;
        }
        return true;
    }

    // OFXR DLSS Frame Generation brings up NGX D3D12 in this process too.
    // Left to its first pair it came up after the left eye's SR feature and
    // before the right eye's, so the eyes did not start from the same NGX
    // state; asked for here, it is up before either.
    void InitializeNgx() {
        if (ngxReady) return;
        // Under D3D11On12 this first sets NGX D3D12 up with DLSS SR's folder (PrepareNgx12Folders).
        VamDlss::PrepareFrameGenerationNgx();
        const auto runtime = Environment(L"VAM_DLSS_RUNTIME_PATH");
        if (runtime.empty()) throw std::runtime_error("VAM_DLSS_RUNTIME_PATH is not set");
        if (on12) {
            if (!PrepareNgx12Folders()) throw std::runtime_error("NGX D3D12 was not set up with the DLSS SR folder (D3D11On12)");
            CheckNgx(NVSDK_NGX_D3D12_GetCapabilityParameters(&parameters), "NVSDK_NGX_D3D12_GetCapabilityParameters");
            on12Commands.Init(on12.device.Get(), on12.queue.Get());
        } else {
            const auto logs = logDirectory.empty() ? runtime : logDirectory;
            const wchar_t* paths[] = {runtime.c_str()};
            NVSDK_NGX_FeatureCommonInfo common{};
            common.PathListInfo.Path = paths;
            common.PathListInfo.Length = 1;
            common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
            CheckNgx(NVSDK_NGX_D3D11_Init_with_ProjectID(kNgxProject, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kNgxEngineVersion,
                logs.c_str(), device.Get(), &common), "NVSDK_NGX_D3D11_Init_with_ProjectID");
            CheckNgx(NVSDK_NGX_D3D11_GetCapabilityParameters(&parameters), "NVSDK_NGX_D3D11_GetCapabilityParameters");
        }
        int available = 0, needsDriver = 0;
        parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
        parameters->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
        if (!available) {
            int initResult = 0;
            parameters->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
            throw std::runtime_error("DLSS SR unavailable (needs driver update=" + std::to_string(needsDriver) +
                ", init result=" + std::to_string(initResult) + ")");
        }
        ngxReady = true;
        OOVR_LOG(on12 ? "VAM DLSS: NGX D3D12 on the 11on12 queue; DLSS Super Resolution available"
                      : "VAM DLSS: NGX D3D11 initialized; DLSS Super Resolution available");
    }

    void ReleaseFeature(Eye& eye) {
        if (!eye.feature) return;
        if (on12) {
            // The queue may still run evaluations of it.
            on12Commands.Drain();
            NVSDK_NGX_D3D12_ReleaseFeature(eye.feature);
        } else {
            NVSDK_NGX_D3D11_ReleaseFeature(eye.feature);
        }
        eye.feature = nullptr;
    }

    // NGX D3D12 on the 11on12 queue: the eye's D3D11 textures unwrapped, evaluated after a Flush (in order with
    // the frame), returned. NGX wants its inputs as shader resources and the output as a UAV.
    void EvaluateOn12(Eye& eye, const NVSDK_NGX_D3D11_DLSS_Eval_Params& e11) {
        VamOn12Unwrapped r(on12, {eye.color.Get(), e11.pInDepth, e11.pInMotionVectors, e11.pInBiasCurrentColorMask, eye.output.Get()});
        context->Flush();
        ID3D12GraphicsCommandList* list = on12Commands.Begin();
        constexpr auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        r.Transition(list, {read, read, read, read, D3D12_RESOURCE_STATE_UNORDERED_ACCESS});
        NVSDK_NGX_D3D12_DLSS_Eval_Params e{};
        e.Feature.pInColor = r[0];
        e.Feature.pInOutput = r[4];
        e.pInDepth = r[1];
        e.pInMotionVectors = r[2];
        e.pInBiasCurrentColorMask = r[3];
        e.InJitterOffsetX = e11.InJitterOffsetX;
        e.InJitterOffsetY = e11.InJitterOffsetY;
        e.InRenderSubrectDimensions = e11.InRenderSubrectDimensions;
        e.InReset = e11.InReset;
        e.InMVScaleX = e11.InMVScaleX;
        e.InMVScaleY = e11.InMVScaleY;
        e.InFrameTimeDeltaInMsec = e11.InFrameTimeDeltaInMsec;
        e.InPreExposure = e11.InPreExposure;
        e.InExposureScale = e11.InExposureScale;
        CheckNgx(NGX_D3D12_EVALUATE_DLSS_EXT(list, eye.feature, parameters, &e), "NGX_D3D12_EVALUATE_DLSS_EXT");
        r.Restore(list, {read, read, read, read, D3D12_RESOURCE_STATE_UNORDERED_ACCESS});
        on12Commands.Submit();
    }

    static NVSDK_NGX_DLSS_Create_Params CreateParams(NVSDK_NGX_Parameter* p, UINT w, UINT h, UINT ow, UINT oh, bool hdr, int preset) {
        for (const char* name : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                                 NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                                 NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
            NVSDK_NGX_Parameter_SetUI(p, name, static_cast<unsigned>(preset));
        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature.InWidth = w; create.Feature.InHeight = h;
        create.Feature.InTargetWidth = ow; create.Feature.InTargetHeight = oh;
        create.Feature.InPerfQualityValue = QualityFor(w, h, ow, oh);
        // Render-size, non-dilated vectors without jitter; Unity reversed Z.
        create.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
            (hdr ? (NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) : 0);
        create.InEnableOutputSubrects = false;
        return create;
    }

    // Upscale's per-eye checks: false means FSR1 for this frame.
    bool Eligible(unsigned eyeIndex, UINT w, UINT h, UINT ow, UINT oh) {
        auto& eye = eyes[eyeIndex];
        // DLSS only upscales. VaM's own render scale (its settings menu)
        // multiplies the launcher's, and above it the render outgrows the
        // output: FSR1 takes those frames and DLSS resumes once it fits.
        const bool oversize = w > ow || h > oh;
        if (oversize != eye.oversize) {
            if (oversize)
                OOVR_LOGF("VAM DLSS SR paused eye=%u: render %ux%u is larger than the output %ux%u (VaM render scale above the launcher's); FSR1 until it fits",
                    eyeIndex, w, h, ow, oh);
            else
                OOVR_LOGF("VAM DLSS SR resumed eye=%u: render %ux%u, output %ux%u", eyeIndex, w, h, ow, oh);
            eye.oversize = oversize;
            UpdateState();
        }
        const bool excluded = (settings.dlssSrEyes & (1 << eyeIndex)) == 0;
        if (excluded != eye.excluded) {
            OOVR_LOGF("VAM DLSS SR eye=%u %s by DLSSSREyes=%d (jitter %s)", eyeIndex, excluded ? "switched to FSR1" : "back on DLSS",
                settings.dlssSrEyes, excluded || eyes[eyeIndex ^ 1].excluded ? "off" : "on");
            eye.excluded = excluded;
            eye.evaluatedSerial = 0;
            UpdateState();
        }
        if (oversize || excluded) return false;
        if(VamDesktopDisplay() && GetTickCount64()>=eye.badUntil)eye.badW=0;
        return !(w == eye.badW && h == eye.badH && ow == eye.badOW && oh == eye.badOH && settings.dlssPreset == eye.badPreset);
    }

    // UI bypass: EASU at this frame's jitter. Unity rendered what the
    // unjittered pixel q shows at q + jitter (render pixels, y down), and the
    // UI mask was rasterized with that jitter too.
    static UiBypassConstants UiBypassSetup(UINT w, UINT h, UINT ow, UINT oh, DXGI_FORMAT typed, const VamFrameGuides& guides) {
        UiBypassConstants c{};
        FsrEasuConOffset(c.easu[0], c.easu[1], c.easu[2], c.easu[3], static_cast<float>(w), static_cast<float>(h),
            static_cast<float>(w), static_cast<float>(h), static_cast<float>(ow), static_cast<float>(oh), guides.jitterX, guides.jitterY);
        c.maskMap[0] = 1.f / ow;
        c.maskMap[1] = 1.f / oh;
        c.maskMap[2] = guides.jitterX / w;
        c.maskMap[3] = guides.jitterY / h;
        c.outputSize[0] = ow; c.outputSize[1] = oh;
        // 8-bit images are gamma-encoded already, as DLSS takes them.
        c.linearInput = typed == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1u : 0u;
        return c;
    }

    void LogUiBypass(unsigned eyeIndex, const VamFrameGuides& guides) {
        if (!uiBypassLogged) {
            uiBypassLogged = true;
            OOVR_LOGF("VAM DLSS SR UI bypass active: eye=%u menus in front of the scene take EASU of this frame at jitter %.3f,%.3f instead of DLSS's output",
                eyeIndex, guides.jitterX, guides.jitterY);
        }
        if (++uiBypassFrames % 1800 == 0)
            OOVR_LOGF("VAM DLSS SR UI bypass: %llu eye frames with menus so far", static_cast<unsigned long long>(uiBypassFrames));
    }

    // After the evaluation: eye.composite = eye.output with the
    // menus' pixels from EASU of eye.color (this frame, after NR).
    bool BypassUi(Eye& eye, const VamFrameGuides& guides) {
        if (uiBypassFailed || !eye.uiMaskSrv) return false;
        try {
            if (!uiBypassCs) {
                Check(device->CreateComputeShader(vam_fsr_ui_bypass, sizeof(vam_fsr_ui_bypass), nullptr, &uiBypassCs), "UI bypass shader");
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = sizeof(UiBypassConstants);
                bd.Usage = D3D11_USAGE_DYNAMIC;
                bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                Check(device->CreateBuffer(&bd, nullptr, &uiBypassConstants), "UI bypass constants");
                D3D11_SAMPLER_DESC sd{};
                sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
                sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
                sd.MaxLOD = D3D11_FLOAT32_MAX;
                Check(device->CreateSamplerState(&sd, &linearClamp), "UI bypass sampler");
            }
            if (!eye.composite) {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = eye.fow; td.Height = eye.foh;
                td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.Format = eye.outputTyped;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                Check(device->CreateTexture2D(&td, nullptr, &eye.composite), "UI bypass composite");
                Check(device->CreateUnorderedAccessView(eye.composite.Get(), nullptr, &eye.compositeUav), "UI bypass composite UAV");
                Check(device->CreateShaderResourceView(eye.color.Get(), nullptr, &eye.colorSrv), "DLSS color input SRV");
                Check(device->CreateShaderResourceView(eye.output.Get(), nullptr, &eye.outputSrv), "DLSS output SRV");
            }
            const UiBypassConstants c = UiBypassSetup(eye.fw, eye.fh, eye.fow, eye.foh, eye.typed, guides);
            ContextScope scope(context.Get(), state.Get());
            VamWriteConstants(context, uiBypassConstants.Get(), &c, sizeof(c));
            context->CSSetShader(uiBypassCs.Get(), nullptr, 0);
            context->CSSetConstantBuffers(0, 1, uiBypassConstants.GetAddressOf());
            context->CSSetSamplers(0, 1, linearClamp.GetAddressOf());
            ID3D11ShaderResourceView* sources[3] = {eye.colorSrv.Get(), eye.outputSrv.Get(), eye.uiMaskSrv.Get()};
            context->CSSetShaderResources(0, 3, sources);
            context->CSSetUnorderedAccessViews(0, 1, eye.compositeUav.GetAddressOf(), nullptr);
            context->Dispatch((eye.fow + 15) / 16, (eye.foh + 15) / 16, 1);
        } catch (const std::exception& e) {
            uiBypassFailed = true;
            OOVR_LOGF("VAM DLSS SR UI bypass off for this run (menus keep DLSS's output): %s", e.what());
            return false;
        }
        LogUiBypass(static_cast<unsigned>(&eye - eyes), guides);
        return true;
    }

    static OutputGuideConstants OutputGuideSetup(UINT w, UINT h, UINT ow, UINT oh, const VamFrameGuides& guides) {
        OutputGuideConstants c{};
        c.scale[0] = static_cast<float>(w) / ow;
        c.scale[1] = static_cast<float>(h) / oh;
        c.offset[0] = guides.jitterX;
        c.offset[1] = guides.jitterY;
        c.motionScale[0] = static_cast<float>(ow) / w;
        c.motionScale[1] = static_cast<float>(oh) / h;
        c.renderMax[0] = static_cast<int32_t>(w) - 1;
        c.renderMax[1] = static_cast<int32_t>(h) - 1;
        c.outputSize[0] = ow; c.outputSize[1] = oh;
        return c;
    }

    void LogOutputGuides(unsigned eyeIndex, UINT w, UINT h, UINT ow, UINT oh) {
        if (outputGuidesLogged) return;
        outputGuidesLogged = true;
        OOVR_LOGF("VAM NR after the upscaler: eye=%u guides resampled %ux%u -> %ux%u (depth nearest, motion in output pixels without jitter)",
            eyeIndex, w, h, ow, oh);
    }

    // This eye's guides (from PrepareGuides, same frame) at the
    // output size, in `out` (depth and motionJitteredHalf, as NR reads them).
    bool PrepareOutputGuides(unsigned eyeIndex, const VamFrameGuides& guides, UINT ow, UINT oh, VamFrameGuides& out) {
        std::lock_guard lock(mutex);
        if (failed || outputGuidesFailed || eyeIndex > 1 || !ow || !oh) return false;
        auto& eye = eyes[eyeIndex];
        if (!guides.depth || guides.depth != eye.depth.Get() || guides.motion != eye.motion.Get()) return false;
        try {
            if (!outputGuideCs) {
                auto cs = CompileShader(outputGuideShader, "vam_output_guides", "main", "cs_5_0", nullptr);
                Check(device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &outputGuideCs), "output guide shader");
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = sizeof(OutputGuideConstants);
                bd.Usage = D3D11_USAGE_DYNAMIC;
                bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                Check(device->CreateBuffer(&bd, nullptr, &outputGuideConstants), "output guide constants");
            }
            if (!eye.depthSrv) {
                Check(device->CreateShaderResourceView(eye.depth.Get(), nullptr, &eye.depthSrv), "guide depth SRV");
                Check(device->CreateShaderResourceView(eye.motion.Get(), nullptr, &eye.motionSrv), "guide motion SRV");
            }
            if (!eye.outDepth || eye.outWidth != ow || eye.outHeight != oh) {
                eye.outDepth.Reset(); eye.outMotion.Reset(); eye.outDepthUav.Reset(); eye.outMotionUav.Reset();
                D3D11_TEXTURE2D_DESC td{};
                td.Width = ow; td.Height = oh;
                td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                td.Format = DXGI_FORMAT_R32_FLOAT;
                Check(device->CreateTexture2D(&td, nullptr, &eye.outDepth), "output-size depth");
                Check(device->CreateUnorderedAccessView(eye.outDepth.Get(), nullptr, &eye.outDepthUav), "output-size depth UAV");
                td.Format = DXGI_FORMAT_R16G16_FLOAT;
                Check(device->CreateTexture2D(&td, nullptr, &eye.outMotion), "output-size motion");
                Check(device->CreateUnorderedAccessView(eye.outMotion.Get(), nullptr, &eye.outMotionUav), "output-size motion UAV");
                eye.outWidth = ow; eye.outHeight = oh;
            }
            const OutputGuideConstants c = OutputGuideSetup(eye.width, eye.height, ow, oh, guides);
            ContextScope scope(context.Get(), state.Get());
            VamWriteConstants(context, outputGuideConstants.Get(), &c, sizeof(c));
            context->CSSetShader(outputGuideCs.Get(), nullptr, 0);
            context->CSSetConstantBuffers(0, 1, outputGuideConstants.GetAddressOf());
            ID3D11ShaderResourceView* sources[2] = {eye.depthSrv.Get(), eye.motionSrv.Get()};
            context->CSSetShaderResources(0, 2, sources);
            ID3D11UnorderedAccessView* targets[2] = {eye.outDepthUav.Get(), eye.outMotionUav.Get()};
            context->CSSetUnorderedAccessViews(0, 2, targets, nullptr);
            context->Dispatch((ow + 7) / 8, (oh + 7) / 8, 1);
        } catch (const std::exception& e) {
            outputGuidesFailed = true;
            OOVR_LOGF("VAM NR after the upscaler: output-size guides off for this run (NR runs without depth and motion): %s", e.what());
            return false;
        }
        out = VamFrameGuides{};
        out.depth = eye.outDepth.Get();
        out.motionHalf = out.motionJitteredHalf = eye.outMotion.Get();
        out.nrControl = guides.nrControl; // render size: the composite reads it at the scaled pixel
        out.frameDeltaMs = guides.frameDeltaMs;
        out.serial = guides.serial;
        out.reset = guides.reset;
        LogOutputGuides(eyeIndex, eye.width, eye.height, ow, oh);
        return true;
    }

    void EnsureFeature(Eye& eye, UINT w, UINT h, UINT ow, UINT oh, DXGI_FORMAT typed, DXGI_FORMAT outputTyped) {
        const int preset = settings.dlssPreset;
        if (eye.feature && eye.fw == w && eye.fh == h && eye.fow == ow && eye.foh == oh && eye.typed == typed &&
            eye.outputTyped == outputTyped && eye.preset == preset) return;
        ReleaseFeature(eye);
        eye.color.Reset(); eye.output.Reset();
        eye.composite.Reset(); eye.compositeUav.Reset(); eye.colorSrv.Reset(); eye.outputSrv.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h;
        td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.Format = typed;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        Check(device->CreateTexture2D(&td, nullptr, &eye.color), "DLSS color input");
        td.Width = ow; td.Height = oh;
        // Typed twin of the upscaler's typeless output: CopyResource-compatible.
        td.Format = outputTyped;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        Check(device->CreateTexture2D(&td, nullptr, &eye.output), "DLSS output");
        const bool hdr = typed == DXGI_FORMAT_R16G16B16A16_FLOAT;
        auto create = CreateParams(parameters, w, h, ow, oh, hdr, preset);
        const auto quality = create.Feature.InPerfQualityValue;
        if (on12) {
            ID3D12GraphicsCommandList* list = on12Commands.Begin();
            const NVSDK_NGX_Result created = NGX_D3D12_CREATE_DLSS_EXT(list, 1, 1, &eye.feature, parameters, &create);
            on12Commands.Submit();
            CheckNgx(created, "NGX_D3D12_CREATE_DLSS_EXT");
        } else {
            ContextScope scope(context.Get(), state.Get());
            CheckNgx(NGX_D3D11_CREATE_DLSS_EXT(immediate.Get(), &eye.feature, parameters, &create), "NGX_D3D11_CREATE_DLSS_EXT");
        }
        eye.fw = w; eye.fh = h; eye.fow = ow; eye.foh = oh; eye.typed = typed; eye.outputTyped = outputTyped; eye.preset = preset;
        eye.evaluatedSerial = 0;
        OOVR_LOGF("VAM DLSS SR feature created: eye=%u input=%ux%u output=%ux%u mode=%s preset=%d %s",
            static_cast<unsigned>(&eye - eyes), w, h, ow, oh, QualityName(quality), preset, hdr ? "HDR" : "LDR (gamma-encoded)");
    }

    void RefreshSettings(ULONGLONG now) {
        if (now >= nextSettingsRead) {
            settings = ReadVamPerformanceSettings();
            nextSettingsRead = now + 250;
        }
    }

    bool Upscale(unsigned eyeIndex, ID3D11Texture2D* colorTexture, const VamFrameGuides& guides, ID3D11Texture2D* outputTexture, UINT ow, UINT oh) {
        std::lock_guard lock(mutex);
        if (failed || eyeIndex > 1 || !colorTexture || !outputTexture || !guides.depth || !guides.motion) return false;
        const ULONGLONG now = GetTickCount64();
        RefreshSettings(now);
        try {
            if (settings.dlssPreset == 6) return SubmitRr(eyeIndex, guides, outputTexture, ow, oh);
            InitializeNgx();
            D3D11_TEXTURE2D_DESC cd{};
            colorTexture->GetDesc(&cd);
            const DXGI_FORMAT typed = TypedColor(cd.Format);
            if (typed == DXGI_FORMAT_UNKNOWN) throw std::runtime_error("unsupported eye color format " + std::to_string(cd.Format));
            D3D11_TEXTURE2D_DESC od{};
            outputTexture->GetDesc(&od);
            const DXGI_FORMAT outputTyped = TypedColor(od.Format);
            if (outputTyped == DXGI_FORMAT_UNKNOWN || od.Width != ow || od.Height != oh)
                throw std::runtime_error("unsupported DLSS output texture " + std::to_string(od.Format));
            auto& eye = eyes[eyeIndex];
            if (!Eligible(eyeIndex, cd.Width, cd.Height, ow, oh)) return false;
            try {
                EnsureFeature(eye, cd.Width, cd.Height, ow, oh, typed, outputTyped);
            } catch (const std::exception& e) {
                // This size or preset only: another one gets a new attempt.
                eye.badW = cd.Width; eye.badH = cd.Height; eye.badOW = ow; eye.badOH = oh; eye.badPreset = settings.dlssPreset;eye.badUntil=now+1000;
                OOVR_LOGF("VAM DLSS SR eye=%u: feature for %ux%u -> %ux%u not created (%s); FSR1 until the size or preset changes",
                    eyeIndex, cd.Width, cd.Height, ow, oh, e.what());
                return false;
            }
            immediate->CopyResource(eye.color.Get(), colorTexture);
            NVSDK_NGX_D3D11_DLSS_Eval_Params eval{};
            eval.Feature.pInColor = eye.color.Get();
            eval.Feature.pInOutput = eye.output.Get();
            eval.pInDepth = guides.depth;
            eval.pInMotionVectors = guides.motion;
            eval.InJitterOffsetX = settings.dlssJitterSignX * guides.jitterX;
            eval.InJitterOffsetY = settings.dlssJitterSignY * guides.jitterY;
            eval.InRenderSubrectDimensions = {cd.Width, cd.Height};
            const bool reset = guides.reset || eye.evaluatedSerial == 0 || guides.serial != eye.evaluatedSerial + 1;
            eval.InReset = reset ? 1 : 0;
            eval.InMVScaleX = eval.InMVScaleY = 1.f;
            eval.InFrameTimeDeltaInMsec = guides.frameDeltaMs;
            eval.InPreExposure = 1.f;
            eval.InExposureScale = 1.f;
            // Optional (DLSSHairBias): hair pixels lean on this frame's colour
            // instead of history - less smearing, more shimmer.
            eval.pInBiasCurrentColorMask = guides.hairMask;
            if (guides.hairMask && !hairBiasLogged) {
                hairBiasLogged = true;
                OOVR_LOGF("VAM DLSS SR: hair bias-current-colour mask in use (DLSSHairBias=%d%%)", settings.dlssHairBias);
            }
            if (on12) {
                EvaluateOn12(eye, eval);
            } else {
                ContextScope scope(context.Get(), state.Get());
                CheckNgx(NGX_D3D11_EVALUATE_DLSS_EXT(immediate.Get(), eye.feature, parameters, &eval), "NGX_D3D11_EVALUATE_DLSS_EXT");
            }
            // Menus: DLSS's result there replaced by EASU of this frame.
            const bool bypassed = guides.uiMask && guides.uiMask == eye.uiMask.Get() && BypassUi(eye, guides);
            // outputTexture is the upscaler's own texture or the swapchain image
            // itself (which may carry more mips): mip 0 only.
            immediate->CopySubresourceRegion(outputTexture, 0, 0, 0, 0, bypassed ? eye.composite.Get() : eye.output.Get(), 0, nullptr);
            eye.evaluatedSerial = guides.serial;
            lastEvaluation = now;
            if (reset) ++eye.resets;
            if (++eye.evaluations == 1) {
                OOVR_LOGF("VAM DLSS SR eye=%u first evaluation submitted: jitter=%.3f,%.3f reset=%d", eyeIndex, eval.InJitterOffsetX, eval.InJitterOffsetY, eval.InReset);
                WriteStatus("evaluating", "DLSS SR running");
            } else if (eye.evaluations % 1800 == 0) {
                OOVR_LOGF("VAM DLSS SR eye=%u: %llu evaluations, %llu resets, serial %llu", eyeIndex,
                    static_cast<unsigned long long>(eye.evaluations), static_cast<unsigned long long>(eye.resets),
                    static_cast<unsigned long long>(guides.serial));
            }
            UpdateState();
            return true;
        } catch (const std::exception& e) {
            Fail(e.what());
            return false;
        }
    }

    bool aheadFailed = false;
    bool UpscaledAhead(unsigned eyeIndex, uint64_t serial, ID3D11Texture2D* outputTexture, UINT ow, UINT oh) {
        std::lock_guard lock(mutex);
        if (failed || aheadFailed || eyeIndex > 1 || !outputTexture || !serial || settings.dlssPreset != 6) return false;
        try {
            VamFrameGuides guides;   // SubmitRr's ahead image reads only the serial
            guides.serial = serial;
            return SubmitRr(eyeIndex, guides, outputTexture, ow, oh, true);
        } catch (const std::exception& e) {
            aheadFailed = true;
            OOVR_LOGF("VAM RR eye=%u image ahead off for this run (NR after the upscaler per eye): %s", eyeIndex, e.what());
            return false;
        }
    }

    // OFXR DLSS Frame Generation. The layer is loaded by the OpenXR loader
    // long before the first frame; a build without the V3 export, or one not
    // asked for DLSS-G, ends the lookup for this process.
    OfxrPublishD3D11GuidesV3 publishFg = nullptr;
    bool fgLookupDone = false;
    ULONGLONG nextFgLookup = 0;
    uint64_t fgPublished = 0, fgRejected = 0;
    int fgEyesLogged = -1;
    uint64_t doubleCaptures = 0;
    bool hairBiasLogged = false;

    void PublishFrameGeneration(uint64_t swapchain, unsigned eye, const VamFrameGuides& guides) {
        std::lock_guard lock(mutex);
        if (!swapchain || eye > 1 || !guides.depth || !guides.motionHalf) return;
        RefreshSettings(GetTickCount64());
        // Diagnostic: an eye left out gets no guides, so OFXR gives it
        // optical flow; DLSS-G restarts that eye's history when it returns.
        if (settings.dlssFgEyes != fgEyesLogged) {
            static const char* const names[] = {"none (both eyes optical flow)", "left eye only", "right eye only", "both eyes"};
            OOVR_LOGF("VAM DLSS FG: DLSS Frame Generation on %s (DLSSFGEyes=%d)", names[settings.dlssFgEyes & 3], settings.dlssFgEyes);
            fgEyesLogged = settings.dlssFgEyes;
        }
        if ((settings.dlssFgEyes & (1 << eye)) == 0) return;
        if (!publishFg) {
            const ULONGLONG now = GetTickCount64();
            if (fgLookupDone || now < nextFgLookup) return;
            nextFgLookup = now + 2000;
            HMODULE layer = VamDesktopDisplay()?VamOn12Proxy():GetModuleHandleW(L"XR_APILAYER_XRFrameBridge_diagnostic.dll");
            if (!layer) return;
            fgLookupDone = true;
            auto version = reinterpret_cast<OfxrGetProviderApiVersionV3>(GetProcAddress(layer, VamDesktopDisplay()?"OFXR_GetProviderApiVersionV4":"OFXR_GetProviderApiVersionV3"));
            auto publish = reinterpret_cast<OfxrPublishD3D11GuidesV3>(GetProcAddress(layer, VamDesktopDisplay()?"OFXR_PublishD3D11GuidesV4":"OFXR_PublishD3D11GuidesV3"));
            if (!version || !publish) {
                OOVR_LOG("VAM DLSS FG: this OFXR layer has no DLSS Frame Generation guide export; frame generation keeps its own path");
                return;
            }
            if (version() != (VamDesktopDisplay()?4u:3u)) {
                OOVR_LOG("VAM DLSS FG: OFXR DLSS Frame Generation is not enabled (OFXR_DLSSG); guides not published");
                return;
            }
            publishFg = publish;
            OOVR_LOG("VAM DLSS FG: publishing per-eye depth, motion and camera to OFXR DLSS Frame Generation");
        }
        D3D11_TEXTURE2D_DESC dd{};
        guides.depth->GetDesc(&dd);
        OfxrGuidePublicationV4 packet{};
        auto& g=packet.base;
        if(VamDesktopDisplay()){g.structSize=sizeof(packet);g.apiVersion=4;}
        g.swapchain = swapchain;
        g.depth = guides.depth;
        g.motionVectors = guides.motionHalf;
        g.width = dd.Width;
        g.height = dd.Height;
        g.serial = guides.serial;
        g.reset = guides.reset ? 1 : 0;
        g.jitterX = settings.dlssJitterSignX * guides.jitterX;
        g.jitterY = settings.dlssJitterSignY * guides.jitterY;
        g.frameTimeDeltaMs = guides.frameDeltaMs;
        g.cameraNear = guides.nearPlane;
        g.cameraFar = guides.farPlane;
        const float* p = guides.viewToClip;
        if (p[5] != 0.f && p[0] != 0.f) {
            g.cameraFov = std::atan((1.f + p[6]) / p[5]) + std::atan((1.f - p[6]) / p[5]);
            g.cameraAspect = p[5] / p[0];
        }
        float t0[16], t1[16], viewToClip[16], clipToView[16], clipToPrev[16], prevToClip[16];
        Multiply(kGlToD3d, guides.viewToClip, t0);
        Multiply(t0, kFlipZ, viewToClip);
        Multiply(kGlToD3d, guides.clipToPrevClip, t1);
        Multiply(t1, kD3dToGl, clipToPrev);
        if (!Invert(viewToClip, clipToView) || !Invert(clipToPrev, prevToClip)) {
            if (++fgRejected == 1) OOVR_LOG("VAM DLSS FG: singular camera matrix; guides not published");
            return;
        }
        Transpose(viewToClip, g.viewToClip);
        Transpose(clipToView, g.clipToView);
        Transpose(clipToPrev, g.clipToPrevClip);
        Transpose(prevToClip, g.prevClipToClip);
        Column(guides.viewToWorld, 3, g.cameraPosition, false);
        Column(guides.viewToWorld, 0, g.cameraRight, true);
        Column(guides.viewToWorld, 1, g.cameraUp, true);
        Column(guides.viewToWorld, 2, g.cameraForward, true, -1.f);
        if (publishFg(&g)) {
            if (++fgPublished == 1)
                OOVR_LOGF("VAM DLSS FG: first guides published: %ux%u serial=%llu fov=%.3f aspect=%.3f near=%.3f far=%.1f",
                    g.width, g.height, static_cast<unsigned long long>(g.serial), g.cameraFov, g.cameraAspect, g.cameraNear, g.cameraFar);
        } else if (++fgRejected == 1 || fgRejected % 600 == 0) {
            OOVR_LOGF("VAM DLSS FG: OFXR rejected the guides (count=%llu)", static_cast<unsigned long long>(fgRejected));
        }
    }
};

namespace {
struct DesktopRenderHost {
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext> context;
    std::shared_ptr<VamNrPrepass> nr;
    std::unique_ptr<VamNis> nis;
    VamDlss* dlss = nullptr;
    D3D11EyeBlitter windowBlit;
    Ptr<ID3D11Texture2D> finalColor;
    Ptr<ID3D11RenderTargetView> finalView;
    std::atomic<UINT> width{0}, height{0};
    std::mutex captureMutex;
    std::wstring capturePath;
    ULONGLONG retryAt = 0;
    bool ready = false;

    void Initialize(ID3D11Texture2D* source) {
        if (ready || GetTickCount64() < retryAt) return;
        source->GetDevice(&device);
        device->GetImmediateContext(&context);
        VamInitializeRenderHost(device.Get(), nr, nis, dlss);
        Check(windowBlit.Initialize(device.Get(),context.Get()),"desktop window blitter");
        ready = dlss && nis;
        if (!ready) retryAt = GetTickCount64() + 1000;
        if (ready) OOVR_LOG("VAM desktop render host ready: mono view, shared SR/NR pipeline");
    }
    void CaptureOutput(ID3D11Texture2D* output, const D3D11_TEXTURE2D_DESC& desc) {
        std::wstring path;
        { std::lock_guard lock(captureMutex); path.swap(capturePath); }
        if (path.empty()) return;
        D3D11_TEXTURE2D_DESC staging = desc;
        staging.Usage = D3D11_USAGE_STAGING; staging.BindFlags = staging.MiscFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Ptr<ID3D11Texture2D> readback;
        Check(device->CreateTexture2D(&staging, nullptr, &readback), "desktop capture staging");
        context->CopyResource(readback.Get(), output);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "desktop capture readback");
        FILE* file = nullptr;
        _wfopen_s(&file, path.c_str(), L"wb");
        if (file) {
            fprintf(file, "P6\n%u %u\n255\n", desc.Width, desc.Height);
            std::vector<unsigned char> row(size_t(desc.Width)*3);
            const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || desc.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
            for (UINT y=0;y<desc.Height;y++) {
                const auto* src = static_cast<const unsigned char*>(mapped.pData)+size_t(y)*mapped.RowPitch;
                for (UINT x=0;x<desc.Width;x++) { row[x*3]=src[x*4+(bgra?2:0)]; row[x*3+1]=src[x*4+1]; row[x*3+2]=src[x*4+(bgra?0:2)]; }
                fwrite(row.data(), 1, row.size(), file);
            }
            fclose(file);
        }
        context->Unmap(readback.Get(), 0);
    }
    void Present(ID3D11Texture2D* source) {
        if (!ready) return;
        Ptr<ID3D11RenderTargetView> target;
        context->OMGetRenderTargets(1, &target, nullptr);
        if (!target) return; // no window target while Unity recreates it
        Ptr<ID3D11Resource> resource;
        Ptr<ID3D11Texture2D> output;
        target->GetResource(&resource);
        Check(resource.As(&output), "desktop backbuffer texture");
        D3D11_TEXTURE2D_DESC out{}, in{};
        output->GetDesc(&out); source->GetDesc(&in);
        static ID3D11Resource* lastTarget=nullptr;
        if(lastTarget!=resource.Get()){
            auto proxy=VamOn12Proxy();auto window=proxy?reinterpret_cast<int(*)(ID3D11Resource*)>(GetProcAddress(proxy,"VAM_DesktopIsWindowSurface")):nullptr;
            OOVR_LOGF("VAM DesktopPresent target=%p window=%d format=%u source=%p format=%u",resource.Get(),window?window(resource.Get()):-1,unsigned(out.Format),source,unsigned(in.Format));
            lastTarget=resource.Get();
        }
        width.store(out.Width); height.store(out.Height);
        auto& recommended=VamRecommendedSize();
        recommended.width=out.Width; recommended.fullWidth=out.Width;
        recommended.height=out.Height; recommended.fullHeight=out.Height;
        DXGI_FORMAT format=in.Format;
        if (format==DXGI_FORMAT_R8G8B8A8_TYPELESS || format==DXGI_FORMAT_R8G8B8A8_UNORM) format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if (format==DXGI_FORMAT_B8G8R8A8_TYPELESS || format==DXGI_FORMAT_B8G8R8A8_UNORM) format=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        // Unity's render texture has the same reversed Y as its stereo eye
        // texture. Crop/guide conversion must agree, then write upright output.
        const float bounds[]={0,1,1,0};
        const bool hdr=out.Format==DXGI_FORMAT_R16G16B16A16_TYPELESS || out.Format==DXGI_FORMAT_R16G16B16A16_FLOAT;
        if(hdr){
            D3D11_TEXTURE2D_DESC previous{};if(finalColor)finalColor->GetDesc(&previous);
            if(!finalView || previous.Width!=out.Width || previous.Height!=out.Height){
                finalView.Reset();finalColor.Reset();
                D3D11_TEXTURE2D_DESC d{};d.Width=out.Width;d.Height=out.Height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
                d.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
                Check(device->CreateTexture2D(&d,nullptr,&finalColor),"desktop upright output");
                D3D11_RENDER_TARGET_VIEW_DESC v{};v.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;v.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
                Check(device->CreateRenderTargetView(finalColor.Get(),&v,&finalView),"desktop upright output view");
            }
        }
        // Process in the same SDR output family as a VR eye. Unity's active
        // HDR screen buffer is inverted; its final screen resolve flips it back.
        nis->Process(source,0,bounds,format,in.Width,in.Height,hdr?finalView.Get():target.Get(),out.Width,out.Height,nr.get(),0,dlss,0x44534b5400000001ull);
        if(hdr){
            Check(windowBlit.Copy(finalColor.Get(),target.Get(),DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,out.Width,out.Height,0,bounds),"desktop HDR screen handoff");
            D3D11_TEXTURE2D_DESC d{};finalColor->GetDesc(&d);CaptureOutput(finalColor.Get(),d);
        }else CaptureOutput(output.Get(),out);
    }
};
DesktopRenderHost& DesktopHost() { static auto* host=new DesktopRenderHost; return *host; }

void __stdcall OnRenderEvent(int eventId, void* data) {
    if ((eventId != kCaptureEvent && eventId!=3 && eventId!=4 && eventId!=kOpaqueDepthEvent) || !data) return;
    FrameInfoV3 frame;
    std::memcpy(&frame, data, sizeof(frame));
    if (frame.base.magic != kFrameMagic || frame.base.size != sizeof(FrameInfoV3) || frame.base.version != kFrameVersion || frame.viewCount != VamViewCount()) return;
    if (eventId == kOpaqueDepthEvent) {
        auto* p = instance.load();
        if (!p) return;
        try { p->CaptureOpaque(frame); }
        catch (const std::exception& e) { std::lock_guard lock(p->mutex); p->Fail(e.what()); }
        catch (...) { std::lock_guard lock(p->mutex); p->Fail("unexpected opaque depth capture error"); }
        return;
    }
    if (VamDesktopDisplay() && frame.color) {
        try {
            auto* source=reinterpret_cast<ID3D11Texture2D*>(frame.color);
            auto& host=DesktopHost(); host.Initialize(source);
            if (eventId==4) { host.Present(source); return; }
        } catch(const std::exception& e) {
            DesktopHost().retryAt=GetTickCount64()+1000;
            OOVR_LOGF("VAM desktop event failed; retry in 1s: %s",e.what());
            return;
        }
    }
    auto* p = instance.load();
    if (!p) return;
    try {
        if(eventId==3) {
            if(p->FinishPathFrame(frame.base.serial))return;
            frame.base.flags&=~16u; // preparation/fallback capture is actually after alpha
        }
        if(eventId==kCaptureEvent && (frame.base.flags&16)) {
            auto skin=GetModuleHandleW(L"VaMSkinTransmissionNative.bin");
            auto active=skin?reinterpret_cast<int(__cdecl*)()>(GetProcAddress(skin,"ST_PathTracingEnabledV1")):nullptr;
            // Warmup still renders the complete original transparent pass;
            // its late event prepares the consumer for next frame's early PT.
            if(!active || !active())return;
        }
        const auto captured = p->captures;
        p->Capture(frame);
        if (p->captures != captured) if (auto callback = rrCallback.load()) callback();
    }
    catch (const std::exception& e) { std::lock_guard lock(p->mutex); p->Fail(e.what()); }
    catch (...) { std::lock_guard lock(p->mutex); p->Fail("unexpected capture error"); }
}
}

static void __stdcall OnRrToneMap(int id) {
    auto* p = instance.load();
    if (p && p->ToneRr(id)) return;
    if (rrToneMapEvent) rrToneMapEvent(id);
}
static void __stdcall OnRrPost(int id) {
    auto* p = instance.load();
    if (p) p->PostRr(id);
}
extern "C" {
__declspec(dllexport) void* VaMDLSS_GetPostEventFunc() { return reinterpret_cast<void*>(&OnRrPost); }
// RR's full output and the render size, for the bridge's image effect targets. Zero until RR has made them.
__declspec(dllexport) int VaMDLSS_GetPostSize(int* outputWidth, int* outputHeight, int* inputWidth, int* inputHeight) {
    auto* p = instance.load();
    if(!p || !outputWidth || !outputHeight || !inputWidth || !inputHeight) return 0;
    *outputWidth=static_cast<int>(rrPostSize[0].load()); *outputHeight=static_cast<int>(rrPostSize[1].load());
    *inputWidth=static_cast<int>(rrPostSize[2].load()); *inputHeight=static_cast<int>(rrPostSize[3].load());
    return *outputWidth>0 && *outputHeight>0;
}
__declspec(dllexport) int VaMDLSS_GetDesktopSize(int* width, int* height) {
    if(!width || !height || !VamDesktopDisplay()) return 0;
    *width=static_cast<int>(DesktopHost().width.load()); *height=static_cast<int>(DesktopHost().height.load());
    return *width>0 && *height>0;
}
__declspec(dllexport) int VaMDLSS_CaptureDesktop(const wchar_t* path) {
    if(!VamDesktopDisplay() || !path || !*path) return 0;
    auto& host=DesktopHost();std::lock_guard lock(host.captureMutex);
    if(!host.capturePath.empty()) return 0;
    host.capturePath=path;return 1;
}
__declspec(dllexport) void* VaMDLSS_WrapToneMapEvent(void* original) {
    rrToneMapEvent = reinterpret_cast<ToneMapEvent>(original); return reinterpret_cast<void*>(&OnRrToneMap);
}
__declspec(dllexport) int VaMDLSS_RegisterRr(void (__cdecl* callback)()) {
    auto* p = instance.load();
    if(p && VamDesktopDisplay() && p->rrFailed && GetTickCount64()>=p->rrRetryAt) p->rrFailed=false;
    const auto settings = ReadVamPerformanceSettings();
    const bool wanted=(settings.dlssPreset==6 && settings.dlss)||VamPathRequested();
    if (!p || !p->on12 || p->rrFailed || !wanted) callback = nullptr;
    auto previous = rrCallback.exchange(callback);
    if (!callback && previous && p) {
        p->ReleaseRr(); p->rrSource.Reset(); p->rrSourceSrv.Reset();
        OOVR_LOG("VAM RR inactive: both features and all RR textures released; zero RR dispatch");
    }
    return callback ? 1 : 0;
}
__declspec(dllexport) int VaMDLSS_Reconstruct(ID3D11Resource* color, ID3D11Resource* albedo, ID3D11Resource* normal,
    ID3D11Resource* spec, ID3D11Resource* depth, ID3D11Resource* reference, ID3D11Resource* opaque, float tolerance, float detail) {
    auto* p=instance.load();
    return p && rrCallback.load() && p->Reconstruct(color,albedo,normal,spec,depth,reference,opaque,tolerance,detail) ? 1 : 0;
}
__declspec(dllexport) int VaMDLSS_GetApiVersion() { return kApiVersion; }
__declspec(dllexport) void* VaMDLSS_GetRenderEventFunc() { return reinterpret_cast<void*>(&OnRenderEvent); }
// 0 until the compositor exists; see StateBits. kStateSrOff follows
// DLSSPreset=Off as it is now (VamLiveRenderSettings).
__declspec(dllexport) int VaMDLSS_GetState() {
    auto* p = instance.load();
    if (!p) return 0;
    // This bit is consumed by the HDR owner. Both reconstruction and native PT
    // denoising require HDR; it does not imply a change in the chosen upscaler.
    const bool path=VamPathRequested();
    const int bits = (p->stateBits.load()&~kStatePathRendering) | (path?kStatePathRendering:0) | (ReadVamPerformanceSettings().dlssPreset == 6 || path ? 16384 : 0);
    return (bits & kStateSelected) && VamLiveRenderSettings().srOff ? bits | kStateSrOff : bits;
}
// The live [Performance] RenderScale in percent of the output (100 with
// Upscaler=None or DLSSPreset=Off), re-read every 250 ms. The bridge renders
// at it (VaMDLSS_GetEyeTextureScale); frames are native only with
// kStateSrOff or while VaM's menus are open, 100% with a model is DLAA.
__declspec(dllexport) int VaMDLSS_GetRenderScalePercent() {
    return VamLiveRenderSettings().renderPercent;
}
// Main thread notification before Unity replaces its stereo render targets.
// The next frame prepares the consumer while the original route still draws.
__declspec(dllexport) void VaMDLSS_InvalidatePathConsumerV1() {
    if(auto skin=GetModuleHandleW(L"VaMSkinTransmissionNative.bin")) {
        auto ready=reinterpret_cast<void(__cdecl*)(int)>(GetProcAddress(skin,"ST_SetPathConsumerReadyV1"));if(ready)ready(0);
    }
}
// Unity's eye texture multiplier (XRSettings.eyeTextureResolutionScale,
// before VaM's own render scale) that renders `percent` of the output from
// the size VaM was given at start (the runtime's own size in the DLSS
// launcher, v18), the output being that size times the live OutputScale.
// For 100 it is the largest one within the output in both dimensions:
// those frames go out 1:1 (native) or through DLAA, and DLSS-G keeps its
// render-size guides. 0 before VaM asked for its size.
__declspec(dllexport) float VaMDLSS_GetEyeTextureScale(int percent) {
    const auto& r = VamRecommendedSize();
    const uint32_t w = r.width.load(), h = r.height.load(), fw = r.fullWidth.load(), fh = r.fullHeight.load();
    if (!w || !h || !fw || !fh) return 0.f;
    const int output = VamLiveRenderSettings().outputPercent;
    const float ow = static_cast<float>(VamOutputDimension(fw, output)), oh = static_cast<float>(VamOutputDimension(fh, output));
    percent = std::clamp(percent, 33, 100);
    // Just inside: whichever way Unity rounds, neither dimension passes the output.
    if (percent == 100) return std::min(ow / w, oh / h) * 0.99999f;
    return std::min(ow / w, oh / h) * (percent / 100.f);
}
// Halton phase count: 8 * (output / render)^2 per DLSS guide 3.7.1.
__declspec(dllexport) int VaMDLSS_GetJitterPhaseCount() {
    auto* p = instance.load();
    if (!p) return 32;
    const auto& e = p->eyes[0];
    const UINT w = ReadVamPerformanceSettings().dlssPreset == 6 ? p->rrEyes[0].w : e.fw,
               ow = ReadVamPerformanceSettings().dlssPreset == 6 ? p->rrEyes[0].ow : e.fow;
    if (!w || !ow) return 32;
    const float ratio = static_cast<float>(ow) / w;
    return std::clamp(static_cast<int>(std::ceil(8.f * ratio * ratio)), 8, 72);
}
}

VamDlss::VamDlss(ID3D11Device* device) : impl(std::make_unique<Impl>(device)) {}

VamDlss* VamDlss::GetIfEnabled(ID3D11Device* device) {
    if (!VamPerformanceStartup().enabled || Environment(L"VAM_DLSS_RUNTIME_PATH").empty()) return nullptr;
    static std::mutex mutex;
    static VamDlss* created = nullptr;
    static bool attempted = false;
    static ULONGLONG nextAttempt=0;
    std::lock_guard lock(mutex);
    if(VamDesktopDisplay() && attempted && !created && GetTickCount64()>=nextAttempt)attempted=false;
    if (attempted) return created && created->impl->device.Get() == device ? created : nullptr;
    attempted = true; nextAttempt=GetTickCount64()+1000;
    try {
        // Deliberately leaked: Unity may call the render-event export until exit.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&OnRenderEvent), &pinned);
        created = new VamDlss(device);
        instance.store(created->impl.get());
        created->impl->WriteStatus("waiting", "compositor ready; waiting for VaMDLSSBridge frames");
        OOVR_LOG("VAM DLSS bridge ready: waiting for VaMDLSSBridge render events (BepInEx)");
    } catch (const std::exception& e) {
        OOVR_LOGF("VAM DLSS initialization skipped: %s", e.what());
        created = nullptr;
    }
    return created;
}

bool VamDlss::PrepareGuides(ID3D11Texture2D* submitted, const float* bounds, unsigned eye, UINT width, UINT height, VamFrameGuides& guides, bool ahead) {
    try { return impl->PrepareGuides(submitted, bounds, eye, width, height, guides, ahead); }
    catch (const std::exception& e) { std::lock_guard lock(impl->mutex); impl->Fail(e.what()); return false; }
}

bool VamDlss::Upscale(unsigned eye, ID3D11Texture2D* color, const VamFrameGuides& guides, ID3D11Texture2D* output, UINT outputWidth, UINT outputHeight) {
    return impl->Upscale(eye, color, guides, output, outputWidth, outputHeight);
}

bool VamDlss::UpscaledAhead(unsigned eye, uint64_t serial, ID3D11Texture2D* output, UINT outputWidth, UINT outputHeight) {
    return impl->UpscaledAhead(eye, serial, output, outputWidth, outputHeight);
}

bool VamDlss::PrepareOutputGuides(unsigned eye, const VamFrameGuides& guides, UINT outputWidth, UINT outputHeight, VamFrameGuides& out) {
    return impl->PrepareOutputGuides(eye, guides, outputWidth, outputHeight, out);
}

void VamDlss::PublishFrameGeneration(uint64_t swapchain, unsigned eye, const VamFrameGuides& guides) {
    try { impl->PublishFrameGeneration(swapchain, eye, guides); }
    catch (...) {}
}

// OFXR DLSS Frame Generation brings up NGX D3D12 in this process, and NGX
// keeps one D3D12 setup per process: whichever initializes first decides the
// feature search path. Started lazily it came up after the NR prepass (whose
// path has no nvngx_dlssg.dll, so DLSS-G reported unavailable) and between
// the two eyes' DLSS SR features; asked for here, it is first.
bool VamDlss::PrepareFrameGenerationNgx() {
    static std::mutex onceMutex;
    static bool done = false, ready = false;
    std::lock_guard lock(onceMutex);
    if (done) return ready;
    done = true;
    try {
        // D3D11On12: NGX D3D12 must list DLSS SR's folder too, so it is set up here first.
        PrepareNgx12Folders();
        if (Environment(L"OFXR_DLSSG") != L"1") return false;
        HMODULE layer = VamDesktopDisplay()?VamOn12Proxy():GetModuleHandleW(L"XR_APILAYER_XRFrameBridge_diagnostic.dll");
        auto prepare = layer ? reinterpret_cast<OfxrPrepareFrameGenerationV3>(GetProcAddress(layer, "OFXR_PrepareFrameGenerationV3")) : nullptr;
        if (!prepare) {
            OOVR_LOG("VAM DLSS FG: OFXR_PrepareFrameGenerationV3 not found; NGX D3D12 starts with the first DLSS-G pair");
            return false;
        }
        ready = prepare() != 0;
        OOVR_LOGF("VAM DLSS FG: NGX D3D12 brought up first, by OFXR: %s", ready ? "DLSS Frame Generation ready" : "DLSS-G unavailable");
    } catch (...) {
        ready = false;
    }
    return ready;
}

void VamDlss::SetSelected(bool selected) {
    std::lock_guard lock(impl->mutex);
    if (impl->selected == selected) return;
    impl->selected = selected;
    impl->UpdateState();
}
