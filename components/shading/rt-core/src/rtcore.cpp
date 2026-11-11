// RtCore's D3D12 side (include\rtcore.h): resources, acceleration structures, the per-frame recording.
//
// Record(list, frame) records the path tracer's scene, in order:
//   uploads (records), the tessellated occluder copies, bottom levels (skins and clothing updated every frame, rebuilt
//   every kRebuildFrames; scene meshes once), the top level; timestamps go to a readback ring read once the host
//   reports the frame complete. RecordPathTrace (below) traces it.
// Every pass is one of tools\gen_shaders.py's entry points on one root signature; descriptors live in RtCore's own
// shader-visible heap: a static part (the host's buffers, the path tracer's) and a per-frame ring part (this frame's
// record buffer views, the micromap bakes').
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#pragma warning(push, 0)
#include <dxgi.h>
#include "nvapi.h"  // opacity micromaps on NVIDIA (<repo>\third_party\nvapi; builds add /I and nvapi64.lib)
#pragma warning(pop)
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "../include/rtcore.h"
#include "../include/path_tracing.h"
#include "rtc_shaders.h"

namespace rtc {
namespace {

using Microsoft::WRL::ComPtr;

constexpr uint32_t kRing = 8;                     // frames of per-frame data (descriptors, uploads, readback)
// 0.22.21: 65536 (was 8192), the host's buffers' views among them (HostSrv). Static indices stay below 2^16: passes pack
// two receiver views into one constant.
constexpr uint32_t kStaticDescriptors = 65536;
constexpr uint32_t kFrameDescriptors = 4096;      // the records' views and the micromap bakes'
constexpr uint64_t kUploadBytes = 8ull << 20;     // per frame of the ring
constexpr uint64_t kRebuildFrames = 90;           // skins and clothing: a full rebuild this often, updated in between
constexpr uint64_t kEvictFrames = 120;            // geometry / hair state not submitted for this long is released
constexpr uint64_t kStaticTrianglesPerFrame = 400000;
constexpr uint64_t kScratchBatch = 64ull << 20;
constexpr uint32_t kStamps = 2;                   // start, end (after the top level)
constexpr uint32_t kReadbackStride = 8 * kStamps;
constexpr uint32_t kMaxGroups = 65535;
// opacity micromaps (path_omm.hlsl PathOmmBake): one per baked triangle, level 4 (256 micro-triangles), 4-state
constexpr uint32_t kOmmLevel = 4, kOmmWords = 16, kOmmBytes = 64;

enum Pipe { pClear, pTess, pPathTrace, pPathHairPrepare, pPathSkin,
            pPathEnvironmentCdf,pPathEmitterCdf,pPathEmitterRanges,pPathTraceRaygen,pPathTraceSplit,pPathTraceSplitRaygen,pPathOmmBake,
            pPathTraceSss,pPathTraceSssRaygen,pPathScatterX,pPathScatterY,pCount };
static_assert(sizeof(rtc_shaders::kAll) / sizeof(rtc_shaders::kAll[0]) == pCount, "rtc_shaders.h entry order");

// ---- GPU records (rtc_common.hlsli) ---------------------------------------------------------------------------
struct LightGpu {
    float position[3]; uint32_t type;
    float direction[3]; float range;
    float color[3]; float spotCos;
    float radius, tanAngular; uint32_t traced, excludedLinks;  // API 18: Light::excludedLinks
};
struct FrameCb {
    float sh[7][4];
    float sky[7][4];  // API 8: Frame::skySh
    uint32_t geometries, submeshes, materials;
    float ambientLM;
};
static_assert(sizeof(LightGpu) == 64 && sizeof(FrameCb) == 240, "Frame cbuffer layout");
struct GeoGpu {
    uint32_t posBuf, posOffset, posStride, posFormat;
    uint32_t nrmBuf, nrmOffset, nrmStride, nrmFormat;
    uint32_t uvBuf, uvOffset, uvStride, uvFormat;
    uint32_t idxBuf, idxOffset, index32, baseVertex;
    uint32_t firstSub, subCount, receiver, receiverTriBase;
    uint32_t flags, lightLink, pad1, pad2;  // API 18: Geometry::lightLink
    float m[12];
    uint32_t tanBuf, tanOffset, tanStride, tanFormat;
};
struct SubGpu {
    uint32_t material, triStart, triCount, tessLevel;
    float tessAlpha; uint32_t blasTess, tessBuf, pad0;
};
struct MatGpu {
    uint32_t kind, alphaSlice, hasAlpha, alphaFine;  // hasAlpha bit 2 (API 26): solid shadow, its cutoff above bit 16; alphaSlice, alphaFine: 0
    float color[4];
    float uvST[4];
    float env1[4], env2[4];  // API 8: SurfaceAmbient's (f1 rgb, 1 - _IBLFilter), (f2 rgb, EnvironmentShader)
};
struct PtMaterialGpu {
    uint32_t textures[4] = {UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    uint32_t extra[4] = {UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    float base[4] = {1,1,1,1}, spec[4] = {.04f,.04f,.04f,.5f};
    float emission[4] = {}, uv[4] = {1,1,0,0}, params[4] = {1,1.5f,0,0};
    float gloss[4] = {0,8,0,0};
    float skin[28]={};
    float mapST[20]={1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,0},bump[4]={1,1,1,0};
    float alphaST[4]={1,1,0,0};
    float materialControls[4]={0,1,0,0};
    uint32_t sampling[8]={};
    uint32_t second[4]={UINT32_MAX,0,0,0}; // texture, mode, sampling, reserved
    float secondST[4]={1,1,0,0};
    float directGain[4]={1,1,1,0};
};
static_assert(sizeof(PtMaterialGpu)==448,"path material layout");
constexpr int kPtTextures=9;
struct PtMaterialState {
    PtMaterialGpu gpu;
    ComPtr<ID3D12Resource> resources[kPtTextures];
    uint32_t formats[kPtTextures]={};
    // API 20 (path_omm.inl): PathMaterial::alphaVersion; PtOmmKey of the coverage inputs and the frame it last changed
    uint64_t alphaVersion=0, ommKey=0, ommSince=0;
    uint32_t &Index(int k) {return k<4?gpu.textures[k]:k<8?gpu.extra[k-4]:gpu.second[0];}
};
static_assert(sizeof(GeoGpu) == 160 && sizeof(SubGpu) == 32 && sizeof(MatGpu) == 80, "records");

struct PassCb {
    uint32_t receiver = 0, light = 0, count = 0, seed = 0;
    uint32_t a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, g = 0, h = 0;
};

constexpr uint32_t kNone = 0xffffffffu;

// ---- resources --------------------------------------------------------------------------------------------------
struct Res {
    ComPtr<ID3D12Resource> r;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    uint64_t bytes = 0;
    ID3D12Resource *Get() const { return r.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS Va() const { return r ? r->GetGPUVirtualAddress() : 0; }
};

struct Barriers {
    std::vector<D3D12_RESOURCE_BARRIER> b;
    void To(Res &x, D3D12_RESOURCE_STATES s) {
        if (!x.r || x.state == s) return;
        D3D12_RESOURCE_BARRIER t = {};
        t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        t.Transition.pResource = x.Get();
        t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        t.Transition.StateBefore = x.state;
        t.Transition.StateAfter = s;
        b.push_back(t);
        x.state = s;
    }
    void Uav() {
        D3D12_RESOURCE_BARRIER t = {};
        t.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.push_back(t);
    }
    void Flush(ID3D12GraphicsCommandList4 *l) {
        if (!b.empty()) l->ResourceBarrier((UINT)b.size(), b.data());
        b.clear();
    }
};

struct Grave {
    uint64_t frame;
    ComPtr<IUnknown> object;
    uint32_t descriptor;  // kNone: none
};

struct Material {
    bool live = false;
    MaterialDesc d;
};

struct TessCopy {
    Res buf;
    uint32_t uav = kNone;
    uint32_t level = 0, triangles = 0;
};

struct GeoState {
    uint64_t id = 0, signature = 0, lastSeen = 0, builtFrame = 0;
    ComPtr<ID3D12Resource> blas;
    uint64_t blasBytes = 0, scratchBytes = 0, updateBytes = 0, triangles = 0;
    bool built = false, dirty = true;
    std::vector<TessCopy> tess;  // per host submesh
    // opacity micromaps: per OMM-attached triangle its micromap or uniform state (ommIndex, read by the builds) and the
    // baked micromaps (ommArray, read while tracing); both kept as long as a bottom level built on them
    Res ommIndex, ommArray;
    bool ommReady = false;  // baked for the current signature
    bool ommPlain = false;  // 0.22.21: the bottom level built without the micromaps it wants (the frame had no descriptors left)
};

// A host buffer's view (HostSrv); the reference keeps the resource, so the address names only it
struct HostView {
    ComPtr<ID3D12Resource> r;
    uint32_t srv = kNone;
    uint64_t lastSeen = 0;
};

// A submesh's opacity micromaps this frame: mode 0 none (plain triangles), 1 one uniform state for every triangle
// (`special`: an NVAPI special index), 2 baked (one micromap per triangle from `first` on); `at`: its first entry in
// the geometry's OMM index buffer.
struct SubOmm {
    uint32_t mode, special, first, at;
};
// API 20: a PT-baked submesh's inputs (path_omm.inl, shaders\path_omm.hlsl PtOmmParams)
struct PtOmmParamsGpu {
    uint32_t texture, sampling, rules; float cutoff;
    float st[4];
    float adjust, base, offset, pad;
};
static_assert(sizeof(PtOmmParamsGpu) == 48, "path_omm.hlsl PtOmmParams");

struct Readback {
    uint64_t frame = 0;
    bool pending = false, timed = false;
    uint64_t frequency = 0;
};

struct Core {
    bool ok = false;
    bool omm = false;  // opacity micromaps: NVAPI's OMM caps, the pipelines made with them
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipes[pCount];
    ComPtr<ID3D12StateObject> pathSerState;
    Res pathShaderTable;
    uint32_t pathVariant=0; // the trace's program: 0 as ever, 1 split (NRD), 2 the skin's scattering in screen space (PT_SSS)
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT descriptorSize = 0;
    std::vector<uint32_t> freeDescriptors;
    uint32_t nextStatic = 0;
    uint32_t frameDescriptors = 0;  // used in this frame's segment
    Res upload;                     // kRing x kUploadBytes, mapped
    uint8_t *uploadCpu = nullptr;
    uint64_t uploadUsed = 0;
    Res records, scratch, tlas;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasPrebuild = {};
    uint32_t tlasPrebuildCount = UINT32_MAX;
    ComPtr<ID3D12QueryHeap> queries;
    Res readback;
    const uint8_t *readbackCpu = nullptr;
    Readback slots[kRing];
    std::vector<Material> materials;  // id = index + 1
    std::unordered_map<uint32_t,PtMaterialState> pathMaterials;
    std::vector<PtMaterialGpu> pathMaterialRecords;
    FrameCb pathFrame = {};
    uint64_t pathFrameValid = 0;
    std::vector<LightGpu> pathLights;
    float pathExposure = 1;
    PathEnvironment pathEnvironment;
    ID3D12Resource *pathEyeRigs = nullptr;  // SetPathEyeRigs
    uint32_t pathEyeRigCount = 0;
    ComPtr<ID3D12Resource> pathCubes[4];
    uint32_t pathCubeSrv[4]={kNone,kNone,kNone,kNone};
    struct PathLightingState {
        Res cdf[2];uint32_t srv[2]={kNone,kNone},uav[2]={kNone,kNone};
        struct Range {uint32_t instance,geometry,first,count;};
        std::vector<Range> ranges;std::vector<uint32_t> lookup;
        uint32_t triangles=0;uint64_t frame=UINT64_MAX;
    } pathLighting;
    struct PathHairState {
        // One fiber BLAS per light-link class (PathHairItem::pad): unlinked + five, each seen
        // or hidden by camera rays (API 21).
        static constexpr uint32_t kGroups=12;
        Res vertices,indices,tlas,scratch,blas[kGroups];
        uint32_t vertexSrv=kNone,vertexUav=kNone,indexUav=kNone,recordSrv=kNone;
        uint32_t count=0,segments=0,groups=0;
        uint64_t serial=UINT64_MAX,frame=0,signature=0,lastBuild=0;
        uint64_t scratchBytes[kGroups]={},updateBytes[kGroups]={};
        bool active=false,built=false,previousValid=false;
    } pathHair;
    bool pathLssChecked=false,pathLss=false;
    uint32_t pathNvNull=kNone;
    // Per-pixel light visibility (11-07): 12 bytes a pixel per eye, see path_trace.hlsl PtVisRate.
    Res pathVis[2];uint32_t pathVisUav[2]={kNone,kNone};uint32_t pathVisSize[2][2]={};
    // The skin's scattering (11-10, path_scatter.hlsl): per eye its light (E) and the across blur, RGBA16F at the input size.
    Res pathScatter[2][2];uint32_t pathScatterUav[2][2]={{kNone,kNone},{kNone,kNone}};uint32_t pathScatterSize[2][2]={};
    struct PathSkinState { Res vertices; uint32_t uav=kNone;uint64_t lastSeen=0; };
    std::unordered_map<uint64_t,PathSkinState> pathSkins;
    std::vector<Geometry> pathSkinGeometries;
    struct PathMotionState {
        Res positions[2];uint32_t srv[2]={kNone,kNone},current=0;
        Res normals[2];uint32_t normalSrv[2]={kNone,kNone};
        uint64_t lastSeen=0,signature=0;float matrix[12]={};
    };
    struct PathMotionGpu {
        uint32_t position=kNone,valid=0,offset=0,stride=0;float matrix[12]={};uint32_t format=0,pad[3]={};
        uint32_t normal=kNone,normalOffset=0,normalStride=0,normalFormat=0;
    };
    static_assert(sizeof(PathMotionGpu)==96,"path motion shader stride");
    std::unordered_map<uint64_t,PathMotionState> pathMotion;
    std::vector<PathMotionGpu> pathMotionRecords;
    std::unordered_map<uint64_t, GeoState> geos;
    std::vector<Grave> graves;
    uint64_t frame = 0;               // last recorded
    uint32_t layers = 0xffffffffu;    // every layer and cost bit (SetTestLayers)
    // per-frame scratch (capacity kept)
    std::vector<GeoGpu> geoRecords;
    std::vector<SubGpu> subRecords;
    std::vector<MatGpu> matRecords;
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geoDescs;
    std::vector<NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX> geoDescsEx;           // a bottom level with micromaps
    std::vector<NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_USAGE_COUNT> ommUsage;  // its baked submeshes' counts
    std::vector<SubOmm> subOmm;         // per SubGpu record
    std::vector<PtOmmParamsGpu> subOmmPath;  // per SubGpu record: the PT bake's inputs (path_omm.inl)
    uint32_t pathOmmUniform = 0, pathOmmBaked = 0;  // Status::pathOmm*
    uint64_t pathOmmTriangles = 0;
    std::vector<uint32_t> geoOmm;       // per frame geometry: OMM-attached triangles (index entries)
    std::vector<uint32_t> geoOmmBaked;  // per frame geometry: baked micromaps
    std::unordered_map<ID3D12Resource *, HostView> hostViews;
    std::vector<uint32_t> geoSubFirst;  // per frame geometry: first SubGpu record
    std::vector<int32_t> geoSubOfHost;  // per frame geometry x host submesh -> SubGpu record (-1: none)
    std::vector<uint32_t> geoSubOfHostFirst;
    Barriers frameBarriers;
    std::vector<GeoState *> frameStates;
    std::vector<uint8_t> frameUsable;
    Status status;
    std::mutex statusLock;
    double gpuSum = 0, cpuSum = 0;
    uint64_t cpuSamples = 0;
    double qpcMs = 0;
    bool markers = false;                   // RTC_MARKERS=1 at Init: a debug event per pass (profilers' ranges)
};
Core *g = nullptr;

double NowMs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * g->qpcMs;
}

void SetText(char *dst, size_t bytes, const char *fmt, va_list a) { vsnprintf(dst, bytes, fmt, a); }
void Reason(const char *fmt, ...) {
    std::lock_guard<std::mutex> l(g->statusLock);
    va_list a;
    va_start(a, fmt);
    SetText(g->status.reason, sizeof g->status.reason, fmt, a);
    va_end(a);
}
void Error(const char *fmt, ...) {
    std::lock_guard<std::mutex> l(g->statusLock);
    va_list a;
    va_start(a, fmt);
    SetText(g->status.lastError, sizeof g->status.lastError, fmt, a);
    va_end(a);
    g->status.failures++;
}
void InitError(char *error, int bytes, const char *fmt, ...) {
    if (!error || bytes <= 0) return;
    va_list a;
    va_start(a, fmt);
    vsnprintf(error, (size_t)bytes, fmt, a);
    va_end(a);
}

// ---- creation ---------------------------------------------------------------------------------------------------
bool Buffer(Res &out, uint64_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initial) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = std::max<uint64_t>((bytes + 255) & ~255ull, 256);
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    // buffers are created COMMON (promoted on first use) unless they are an acceleration structure, an upload heap
    // (GENERIC_READ) or a readback heap (COPY_DEST)
    D3D12_RESOURCE_STATES s = initial;
    if (heap == D3D12_HEAP_TYPE_UPLOAD) s = D3D12_RESOURCE_STATE_GENERIC_READ;
    else if (heap == D3D12_HEAP_TYPE_READBACK) s = D3D12_RESOURCE_STATE_COPY_DEST;
    else if (s != D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) s = D3D12_RESOURCE_STATE_COMMON;
    HRESULT hr = g->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, s, nullptr, IID_PPV_ARGS(&out.r));
    if (FAILED(hr)) { Error("buffer %llu bytes: 0x%08lx", (unsigned long long)bytes, (unsigned long)hr); out = Res(); return false; }
    out.state = s;
    out.bytes = rd.Width;
    return true;
}

bool Texture(Res &out, D3D12_RESOURCE_DIMENSION dim, DXGI_FORMAT format, uint32_t w, uint32_t h, uint32_t depth,
             D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initial) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = dim;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = (UINT16)depth; rd.MipLevels = 1;
    rd.Format = format;
    rd.SampleDesc.Count = 1;
    rd.Flags = flags;
    HRESULT hr = g->device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initial, nullptr, IID_PPV_ARGS(&out.r));
    if (FAILED(hr)) { Error("texture %ux%ux%u: 0x%08lx", w, h, depth, (unsigned long)hr); out = Res(); return false; }
    out.state = initial;
    out.bytes = g->device->GetResourceAllocationInfo(0,1,&rd).SizeInBytes;
    return true;
}

void Bury(ComPtr<IUnknown> o, uint32_t descriptor = kNone) {
    if (o || descriptor != kNone) g->graves.push_back({g->frame, o, descriptor});
}
void BuryRes(Res &r) {
    if (r.r) Bury(r.r);
    r = Res();
}
void FreeDescriptor(uint32_t &d) {
    if (d != kNone) g->graves.push_back({g->frame, nullptr, d});
    d = kNone;
}
void Collect(uint64_t completed) {
    auto &v = g->graves;
    size_t w = 0;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i].frame <= completed) {
            if (v[i].descriptor != kNone) g->freeDescriptors.push_back(v[i].descriptor);
            continue;
        }
        if (w != i) v[w] = std::move(v[i]);
        w++;
    }
    v.resize(w);
}

// ---- descriptors ------------------------------------------------------------------------------------------------
D3D12_CPU_DESCRIPTOR_HANDLE Cpu(uint32_t i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = g->heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)i * g->descriptorSize;
    return h;
}
uint32_t StaticDescriptor() {
    if (!g->freeDescriptors.empty()) { uint32_t d = g->freeDescriptors.back(); g->freeDescriptors.pop_back(); return d; }
    if (g->nextStatic >= kStaticDescriptors) { Error("static descriptors exhausted"); return kNone; }
    return g->nextStatic++;
}
uint32_t FrameDescriptor() {
    if (g->frameDescriptors >= kFrameDescriptors) return kNone;
    return kStaticDescriptors + (uint32_t)(g->frame % kRing) * kFrameDescriptors + g->frameDescriptors++;
}

void RawSrv(uint32_t i, ID3D12Resource *r, uint64_t firstByte, uint64_t bytes) {
    D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Buffer.FirstElement = firstByte / 4;
    d.Buffer.NumElements = (UINT)(bytes / 4);
    d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    g->device->CreateShaderResourceView(r, &d, Cpu(i));
}
void RawUav(uint32_t i, ID3D12Resource *r, uint64_t bytes) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = (UINT)(bytes / 4);
    d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    g->device->CreateUnorderedAccessView(r, nullptr, &d, Cpu(i));
}void TexUav(uint32_t i, ID3D12Resource *r, D3D12_UAV_DIMENSION dim, DXGI_FORMAT f, uint32_t slices = 1) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d = {};
    d.Format = f;
    d.ViewDimension = dim;
    if (dim == D3D12_UAV_DIMENSION_TEXTURE2DARRAY) d.Texture2DArray.ArraySize = slices;
    else if (dim == D3D12_UAV_DIMENSION_TEXTURE3D) d.Texture3D.WSize = slices;
    g->device->CreateUnorderedAccessView(r, nullptr, &d, Cpu(i));
}

// A whole-buffer raw SRV of a host buffer, in the static part while frames name the buffer (released kRing frames after
// the last: Record). 0.22.21: made each frame in the frame part before - 2 to 4 a mesh, a scene of ~2900 meshes (the
// largest seen) needed ~6000 of the 4096: the meshes past them silently left out, then every frame failed on its records.
uint32_t HostSrv(ID3D12Resource *r) {
    if (!r) return kNone;
    HostView &v = g->hostViews[r];
    v.lastSeen = g->frame;
    if (v.srv != kNone) return v.srv;
    const D3D12_RESOURCE_DESC rd = r->GetDesc();
    if (rd.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return kNone;
    v.srv = StaticDescriptor();
    if (v.srv == kNone) return kNone;
    v.r = r;
    RawSrv(v.srv, r, 0, rd.Width & ~3ull);
    return v.srv;
}

// ---- upload ring ------------------------------------------------------------------------------------------------
struct Alloc {
    uint8_t *cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
    uint64_t offset = 0;  // in the upload buffer
};
bool Upload(uint64_t bytes, uint64_t align, Alloc &a) {
    const uint64_t base = (g->frame % kRing) * kUploadBytes;
    uint64_t at = (g->uploadUsed + align - 1) / align * align;
    if (at + bytes > kUploadBytes) { Error("upload ring full (%llu bytes)", (unsigned long long)(at + bytes)); return false; }
    g->uploadUsed = at + bytes;
    a.offset = base + at;
    a.cpu = g->uploadCpu + a.offset;
    a.gpu = g->upload.Va() + a.offset;
    return true;
}

// ---- misc -------------------------------------------------------------------------------------------------------
uint64_t Mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}
uint64_t Mix(uint64_t h, const float *v, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t u;
        memcpy(&u, v + i, 4);
        h = Mix(h, u);
    }
    return h;
}

DXGI_FORMAT BlasVertexFormat(VertexFormat f) {
    if (f == VertexFormat::Float3) return DXGI_FORMAT_R32G32B32_FLOAT;
    if (f == VertexFormat::Half4) return DXGI_FORMAT_R16G16B16A16_FLOAT;
    return DXGI_FORMAT_UNKNOWN;
}

// 1D work in 64-thread groups, rows of kMaxGroups (rtc_common.hlsli Linear()).
void Dispatch(ID3D12GraphicsCommandList4 *l, Pipe p, const PassCb &cb, uint32_t items, uint32_t size = 64) {
    if (!items) return;
    const uint32_t groups = (items + size - 1) / size;
    l->SetPipelineState(g->pipes[p].Get());
    l->SetComputeRoot32BitConstants(0, 12, &cb, 0);
    l->Dispatch(std::min(groups, kMaxGroups), (groups + kMaxGroups - 1) / kMaxGroups, 1);
}
void Dispatch2D(ID3D12GraphicsCommandList4 *l, Pipe p, const PassCb &cb, uint32_t w, uint32_t h) {
    l->SetPipelineState(g->pipes[p].Get());
    l->SetComputeRoot32BitConstants(0, 12, &cb, 0);
    l->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
}

void Stamp(ID3D12GraphicsCommandList4 *l, uint32_t k) {
    if (g->queries) l->EndQuery(g->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, (UINT)((g->frame % kRing) * kStamps + k));
}

void ReadCompleted(uint64_t completed) {
    for (uint32_t s = 0; s < kRing; s++) {
        Readback &rb = g->slots[s];
        if (!rb.pending || rb.frame > completed || !g->readbackCpu) continue;
        rb.pending = false;
        const uint8_t *p = g->readbackCpu + s * kReadbackStride;
        uint64_t t[kStamps];
        memcpy(t, p, sizeof t);
        if (!rb.timed || !rb.frequency) continue;
        if (t[1] <= t[0]) continue;
        const double ms = (double)(t[1] - t[0]) * 1000.0 / (double)rb.frequency;
        std::lock_guard<std::mutex> l(g->statusLock);
        Status &st = g->status;
        st.samples++;
        g->gpuSum += ms;
        st.gpuMs = st.buildMs = g->gpuSum / st.samples;
        st.gpuMaxMs = std::max(st.gpuMaxMs, ms);
    }
}

void DropGeo(GeoState &s) {
    if (s.blas) Bury(s.blas);
    s.blas.Reset();
    for (TessCopy &t : s.tess) { BuryRes(t.buf); FreeDescriptor(t.uav); }
    s.tess.clear();
    BuryRes(s.ommIndex);
    BuryRes(s.ommArray);
    s.ommReady = false;
}

}  // namespace

// ---- API --------------------------------------------------------------------------------------------------------
void RetireCompleted(uint64_t completedFrame) { if(g)Collect(completedFrame); }
bool Init(ID3D12Device5 *device, char *error, int errorBytes) {
    if (g) { InitError(error, errorBytes, "already initialised"); return false; }
    if (!device) { InitError(error, errorBytes, "no device"); return false; }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5)) || o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
        InitError(error, errorBytes, "no DXR 1.1 (tier %d)", (int)o5.RaytracingTier);
        return false;
    }
    g = new Core;
    char markers[8] = {};
    g->markers = GetEnvironmentVariableA("RTC_MARKERS", markers, sizeof markers) == 1 && markers[0] == '1';
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g->qpcMs = 1000.0 / (double)f.QuadPart;
    g->device = device;
    auto Fail = [&](const char *what, HRESULT hr) {
        InitError(error, errorBytes, "%s: 0x%08lx", what, (unsigned long)hr);
        delete g;
        g = nullptr;
        return false;
    };
    const auto &first = rtc_shaders::kAll[0];
    HRESULT hr = device->CreateRootSignature(0, first.code, first.size, IID_PPV_ARGS(&g->root));
    if (FAILED(hr)) return Fail("root signature", hr);
    // Opacity micromaps (NVIDIA, NVAPI; the in-box D3D12 has DXR 1.2 but no SM 6.9 compiler here): pipelines that trace
    // must be created with OMM support. The option is the device's, global: on around RtCore's own pipelines only.
    // NVAPI stays loaded (NvAPI_Unload before the device is released crashed the replay's exit).
    NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_CAPS ommCaps = NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_CAP_NONE;
    NVAPI_D3D12_SET_CREATE_PIPELINE_STATE_OPTIONS_PARAMS po = {NVAPI_D3D12_SET_CREATE_PIPELINE_STATE_OPTIONS_PARAMS_VER,
                                                              NVAPI_D3D12_PIPELINE_CREATION_STATE_FLAGS_ENABLE_OMM_SUPPORT};
    g->omm = NvAPI_Initialize() == NVAPI_OK &&
             NvAPI_D3D12_GetRaytracingCaps(device, NVAPI_D3D12_RAYTRACING_CAPS_TYPE_OPACITY_MICROMAP, &ommCaps, sizeof ommCaps) == NVAPI_OK &&
             (ommCaps & NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_CAP_STANDARD) &&
             NvAPI_D3D12_SetCreatePipelineStateOptions(device, &po) == NVAPI_OK;
    int failed = -1;
    for (int i = 0; i < pPathTrace && failed < 0; i++) {  // the scene passes; the PT's are made on first use
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = g->root.Get();
        pd.CS = {rtc_shaders::kAll[i].code, rtc_shaders::kAll[i].size};
        hr = device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g->pipes[i]));
        if (FAILED(hr)) failed = i;
        else {  // the pass's name in profilers (Nsight Graphics GPU Trace: docs\hardware-audit.md)
            wchar_t name[32];
            swprintf(name, 32, L"RtCore %hs", rtc_shaders::kAll[i].name);
            g->pipes[i]->SetName(name);
        }
    }
    if (g->omm) {
        po.flags = NVAPI_D3D12_PIPELINE_CREATION_STATE_FLAGS_NONE;
        NvAPI_D3D12_SetCreatePipelineStateOptions(device, &po);
    }
    if (failed >= 0) return Fail(rtc_shaders::kAll[failed].name, hr);
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kStaticDescriptors + kRing * kFrameDescriptors;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g->heap));
    if (FAILED(hr)) return Fail("descriptor heap", hr);
    g->descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!Buffer(g->upload, kRing * kUploadBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ))
        return Fail("upload ring", E_OUTOFMEMORY);
    void *p = nullptr;
    D3D12_RANGE none = {0, 0};
    hr = g->upload.r->Map(0, &none, &p);
    if (FAILED(hr)) return Fail("upload map", hr);
    g->uploadCpu = (uint8_t *)p;
    D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kStamps * kRing, 0};
    if (FAILED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g->queries)))) g->queries.Reset();
    if (!Buffer(g->readback, kRing * kReadbackStride, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST))
        return Fail("readback", E_OUTOFMEMORY);
    hr = g->readback.r->Map(0, nullptr, &p);
    if (FAILED(hr)) return Fail("readback map", hr);
    g->readbackCpu = (const uint8_t *)p;
    // material 0: the default (opaque mid grey)
    g->materials.resize(1);
    g->materials[0].live = true;
    g->ok = true;
    std::lock_guard<std::mutex> l(g->statusLock);
    g->status.ok = 1;
    snprintf(g->status.reason, sizeof g->status.reason, "DXR tier %d.%d%s", (int)o5.RaytracingTier / 10, (int)o5.RaytracingTier % 10,
             g->omm ? ", opacity micromaps" : "");
    return true;
}

void Shutdown() {
    if (!g) return;
    if (g->upload.r) g->upload.r->Unmap(0, nullptr);
    if (g->readback.r) g->readback.r->Unmap(0, nullptr);
    delete g;
    g = nullptr;
}

uint32_t CreateMaterial(const MaterialDesc &m) {
    if (!g) return 0;
    const Material made{true, m};
    for (size_t i = 1; i < g->materials.size(); i++)
        if (!g->materials[i].live) { g->materials[i] = made; return (uint32_t)i; }
    g->materials.push_back(made);
    return (uint32_t)(g->materials.size() - 1);
}

void SetMaterial(uint32_t material, const MaterialDesc &d) {
    if (!g || material == 0 || material >= g->materials.size() || !g->materials[material].live) return;
    g->materials[material].d = d;
}

void DestroyMaterial(uint32_t material) {
    if (!g || material == 0 || material >= g->materials.size()) return;
    auto p=g->pathMaterials.find(material);
    if(p!=g->pathMaterials.end()) {
        for(int k=0;k<kPtTextures;k++) { Bury(p->second.resources[k]); FreeDescriptor(p->second.Index(k)); }
        g->pathMaterials.erase(p);
    }
    g->materials[material] = Material{};
}

Status GetStatus() {
    Status s;
    if (!g) { snprintf(s.reason, sizeof s.reason, "not initialised"); return s; }
    std::lock_guard<std::mutex> l(g->statusLock);
    return g->status;
}

void ResetTiming() {
    if (!g) return;
    std::lock_guard<std::mutex> l(g->statusLock);
    g->gpuSum = g->cpuSum = 0;
    g->cpuSamples = 0;
    Status &s = g->status;
    s.samples = 0;
    s.gpuMs = s.gpuMaxMs = s.buildMs = s.recordCpuMs = 0;
}

void SetTestLayers(uint32_t mask) {
    if (g) g->layers = mask;
}

// ---- Record -----------------------------------------------------------------------------------------------------
#include "path_cpu_profile.inl"
namespace {

#include "path_skinning.inl"
#include "path_motion.inl"
#include "path_omm.inl"
}  // namespace

bool Record(ID3D12GraphicsCommandList4 *l, const Frame &input) {
    Frame f=input;
    if (!g || !g->ok) return false;
    if (!l) { Error("no command list"); return false; }
    if (f.frame <= g->frame) { Error("frame %llu not after %llu", (unsigned long long)f.frame, (unsigned long long)g->frame); return false; }
    if (f.frame - f.completedFrame >= kRing) {
        Reason("GPU %llu frames behind", (unsigned long long)(f.frame - f.completedFrame));
        return false;
    }
    const double t0 = NowMs();
    PathCpuProfile cpuProfile(true,t0);
    Collect(f.completedFrame);
    ReadCompleted(f.completedFrame);
    g->frame = f.frame;
    g->frameDescriptors = 0;
    g->uploadUsed = 0;
    if(!PreparePathSkinning(l,f))return false;
    if(!PreparePathMotion(l,f))return false;
    for (auto it = g->hostViews.begin(); it != g->hostViews.end();) {
        if (f.frame - it->second.lastSeen > kRing) { Bury(it->second.r, it->second.srv); it = g->hostViews.erase(it); }
        else ++it;
    }
    const uint32_t layers = g->layers;
    const uint32_t si = (uint32_t)(f.frame % kRing);
    auto &bar = g->frameBarriers;
    bar.b.clear();

    // ---- frame constants and lights
    cpuProfile.Mark(0);
    FrameCb cb = {};
    g->pathLights.clear();
    {
        // PT samples one light from the entire submitted set. No silent eight-light cap.
        for(uint32_t i=0;i<f.lightCount;i++) {
            const Light &L=f.lights[i]; LightGpu o{};
            o.type=(uint32_t)L.type; memcpy(o.position,L.position,12);
            float len=std::sqrt(L.direction[0]*L.direction[0]+L.direction[1]*L.direction[1]+L.direction[2]*L.direction[2]);
            for(int k=0;k<3;k++)o.direction[k]=len>0?L.direction[k]/len:(k==1?-1.f:0.f);
            memcpy(o.color,L.color,12);o.range=L.range;o.spotCos=L.spotCosOuter;
            o.radius=L.radius;o.tanAngular=std::tan(std::min(L.angularRadius,1.2f));
            // API 26: PT's `traced` carries Unity's shadow settings: 0-7 strength, 8-19 near plane (1/1024 m), 31 casts.
            o.traced=(uint32_t)std::lround(std::clamp(L.shadowStrength,0.f,1.f)*255.f)|
                ((uint32_t)std::lround(std::clamp(L.shadowNear,0.f,3.99f)*1024.f)<<8)|(L.unityShadows?0x80000000u:0u);
            o.excludedLinks=L.excludedLinks&0x1fu;
            g->pathLights.push_back(o);
        }
    }
    g->pathExposure=f.exposure;
    for (int i = 0; i < 6; i++) memcpy(cb.sh[i], f.ambientSh + 4 * i, 16), memcpy(cb.sky[i], f.skySh + 4 * i, 16);
    memcpy(cb.sh[6], f.ambientSh + 24, 12);
    memcpy(cb.sky[6], f.skySh + 24, 12);
    cb.ambientLM = f.ambientLM;

    // ---- records
    cpuProfile.Mark(1);
    g->geoRecords.clear();
    g->subRecords.clear();
    g->matRecords.clear();
    for (const Material &m : g->materials) {
        MatGpu r = {};
        r.kind = (uint32_t)m.d.kind;
        // API 26: Unity's shadow map takes this sheer scene material as a cutout (bit 2; its threshold above bit 16).
        if (m.d.kind == MaterialKind::Translucent && m.d.solidShadow) {
            const float c = std::isfinite(m.d.alphaCutoff) ? std::min(std::max(m.d.alphaCutoff, 0.0f), 1.0f) : 0.5f;
            r.hasAlpha |= 4u | ((uint32_t)(c * 65535.0f + 0.5f) << 16);
        }
        memcpy(r.color, m.d.averageColor, 12);
        r.color[3] = m.d.kind == MaterialKind::Cutout ? m.d.alphaCutoff : m.d.kind == MaterialKind::Translucent ? m.d.opacity : 1.0f;
        memcpy(r.uvST, m.d.uvScaleOffset, 16);
        // API 8: VaM's skin family weights the SH's first and second bands by _SubdermisColor (its disassembly)
        for (int c = 0; c < 3; c++) {
            const float s = m.d.environment == EnvironmentShader::VamSkin ? m.d.subsurface[c] : 0.0f;
            const float a = 1 - s / 3, b = 1 - 0.75f * s;
            r.env1[c] = a + s * (a * a - a);
            r.env2[c] = b + s * (b * b - b);
        }
        r.env1[3] = m.d.environment == EnvironmentShader::VamSkin ? 1 - m.d.iblFilter : 1.0f;
        r.env2[3] = (float)(uint32_t)m.d.environment;
        g->matRecords.push_back(r);
    }
    auto &states = g->frameStates;
    auto &usable = g->frameUsable;
    states.assign(f.geometryCount, nullptr);
    usable.assign(f.geometryCount, 0);
    g->subOmm.clear();
    g->subOmmPath.clear();
    g->geoOmm.assign(f.geometryCount, 0);
    g->geoOmmBaked.assign(f.geometryCount, 0);
    g->pathOmmUniform = g->pathOmmBaked = 0;
    g->pathOmmTriangles = 0;
    const bool pathOmm = g->omm && (layers & kPathOmmLayer);
    uint64_t triangles = 0, tessTriangles = 0;
    for (uint32_t i = 0; i < f.geometryCount; i++) {
        const Geometry &x = f.geometries[i];
        GeoGpu r = {};
        r.firstSub = (uint32_t)g->subRecords.size();
        r.receiver = kNone;
        const bool posOk = x.position.buffer && BlasVertexFormat(x.position.format) != DXGI_FORMAT_UNKNOWN && x.indexBuffer && x.vertexCount;
        r.posBuf = HostSrv(x.position.buffer);
        r.posOffset = (uint32_t)x.position.offset; r.posStride = x.position.stride; r.posFormat = (uint32_t)x.position.format;
        r.nrmBuf = HostSrv(x.normal.buffer);
        r.nrmOffset = (uint32_t)x.normal.offset; r.nrmStride = x.normal.stride; r.nrmFormat = (uint32_t)x.normal.format;
        r.uvBuf = HostSrv(x.uv.buffer);
        r.uvOffset = (uint32_t)x.uv.offset; r.uvStride = x.uv.stride; r.uvFormat = (uint32_t)x.uv.format;
        r.tanBuf=HostSrv(x.tangent.buffer);
        r.tanOffset=(uint32_t)x.tangent.offset;r.tanStride=x.tangent.stride;r.tanFormat=(uint32_t)x.tangent.format;
        r.idxBuf = HostSrv(x.indexBuffer);
        r.idxOffset = (uint32_t)x.indexOffset; r.index32 = x.index32; r.baseVertex = (uint32_t)std::max(0, x.baseVertex);
        r.flags = x.flags;
        r.lightLink = x.lightLink & 0x1fu;
        memcpy(r.m, x.matrix, sizeof r.m);  // skinned: identity, or VaM's drawOffset translation
        GeoState &s = g->geos[x.id];
        if (s.id != x.id) { s = GeoState(); s.id = x.id; }
        s.lastSeen = f.frame;
        states[i] = &s;
        if (s.tess.size() < x.submeshCount) s.tess.resize(x.submeshCount);
        uint64_t sig = Mix(Mix(Mix(0, x.flags & (kSkinned | kRigid | kNoOcclude)), x.vertexCount), (uint64_t)(uintptr_t)x.position.buffer);
        sig = Mix(Mix(Mix(sig, x.position.offset), x.position.stride), (uint64_t)x.position.format);
        sig = Mix(Mix(Mix(sig, (uint64_t)(uintptr_t)x.indexBuffer), x.indexOffset), x.index32 | ((uint64_t)(uint32_t)x.baseVertex << 1));
        sig = Mix(sig, layers & 64u);
        uint64_t tris = 0;
        uint32_t ommTris = 0, ommBaked = 0;
        for (uint32_t k = 0; k < x.submeshCount; k++) {
            const Submesh &sm = x.submeshes[k];
            const uint32_t n = sm.count / 3;
            if (!n) continue;
            SubGpu sr = {};
            sr.material = sm.material < g->materials.size() && g->materials[sm.material].live ? sm.material : 0;
            sr.triStart = sm.start / 3;
            sr.triCount = n;
            const bool tess = sm.tess == Tess::Phong && sm.tessLevel > 1 && (layers & 64u) && x.normal.buffer;
            sr.tessLevel = tess ? std::min(sm.tessLevel, 8u) : 1;
            sr.tessAlpha = sm.tessAlpha;
            sr.blasTess = tess ? 1u : 0u;
            sr.tessBuf = kNone;
            if (tess) {
                TessCopy &tc = s.tess[k];
                const uint32_t L = sr.tessLevel;
                const uint64_t bytes = 36ull * n * L * L;
                if (!tc.buf.r || tc.buf.bytes < bytes) {
                    BuryRes(tc.buf);
                    FreeDescriptor(tc.uav);
                    if (Buffer(tc.buf, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON)) {
                        tc.uav = StaticDescriptor();
                        if (tc.uav != kNone) RawUav(tc.uav, tc.buf.Get(), tc.buf.bytes);
                    }
                }
                tc.level = L;
                tc.triangles = n;
                if (!tc.buf.r || tc.uav == kNone) { sr.blasTess = 0; sr.tessLevel = 1; }
                else sr.tessBuf = tc.uav;
            }
            const MaterialKind kind = g->materials[sr.material].d.kind;
            sig = Mix(Mix(Mix(Mix(sig, sm.start), sm.count), (uint64_t)(kind == MaterialKind::Cutout || kind == MaterialKind::Translucent)), sr.blasTess * sr.tessLevel);
            SubOmm so = {0, 0, 0, 0};
            PtOmmParamsGpu pp = {};
            if (pathOmm) {
                so = PathOmm(sr, g->matRecords[sr.material], kind, r, f.frame, pp, sig);
                if (so.mode) {
                    so.at = ommTris;
                    ommTris += n;
                    if (so.mode == 2) so.first = ommBaked, ommBaked += n;
                    sig = Mix(Mix(sig, so.mode), so.special);
                    (so.mode == 2 ? g->pathOmmBaked : g->pathOmmUniform)++;
                    g->pathOmmTriangles += n;
                }
            }
            g->subOmm.push_back(so);
            g->subOmmPath.push_back(pp);
            tris += (uint64_t)n * (sr.blasTess ? sr.tessLevel * sr.tessLevel : 1);
            if (sr.blasTess) tessTriangles += (uint64_t)n * sr.tessLevel * sr.tessLevel;
            g->subRecords.push_back(sr);
        }
        r.subCount = (uint32_t)g->subRecords.size() - r.firstSub;
        if (ommBaked) sig = Mix(Mix(Mix(Mix(sig, (uint64_t)(uintptr_t)x.uv.buffer), x.uv.offset), x.uv.stride), (uint64_t)x.uv.format);
        // any change rebuilds the bottom level and bakes its micromaps again (PathOmmBake reads the index buffer, the uvs,
        // the alpha map)
        if (sig != s.signature) { s.signature = sig; s.dirty = true; s.ommReady = false; }
        g->geoOmm[i] = ommTris;
        g->geoOmmBaked[i] = ommBaked;
        if (!ommTris && s.ommIndex.r) {  // the bottom level is rebuilt without them before it is used again
            BuryRes(s.ommIndex);
            BuryRes(s.ommArray);
        }
        s.triangles = tris;
        usable[i] = posOk && r.posBuf != kNone && r.idxBuf != kNone && r.subCount > 0 ? 1 : 0;
        if (!usable[i]) r.subCount = 0;  // nothing in it can be read: not traced against
        g->geoRecords.push_back(r);
    }
    // evict geometry state not submitted for a while
    for (auto it = g->geos.begin(); it != g->geos.end();) {
        if (f.frame - it->second.lastSeen > kEvictFrames) { DropGeo(it->second); it = g->geos.erase(it); }
        else ++it;
    }

    // ---- upload the records into the default records buffer
    cpuProfile.Mark(2);
    auto Align = [](uint64_t x) { return (x + 255) & ~255ull; };
    const uint64_t geoBytes = Align(std::max<size_t>(1, g->geoRecords.size()) * sizeof(GeoGpu));
    const uint64_t subBytes = Align(std::max<size_t>(1, g->subRecords.size()) * sizeof(SubGpu));
    const uint64_t matBytes = Align(std::max<size_t>(1, g->matRecords.size()) * sizeof(MatGpu));
    const uint64_t recordBytes = geoBytes + subBytes + matBytes;
    Alloc cbA, recA;
    if (!Upload(sizeof cb, 256, cbA) || !Upload(recordBytes, 256, recA)) return false;
    memset(recA.cpu, 0, (size_t)recordBytes);
    memcpy(recA.cpu, g->geoRecords.data(), g->geoRecords.size() * sizeof(GeoGpu));
    memcpy(recA.cpu + geoBytes, g->subRecords.data(), g->subRecords.size() * sizeof(SubGpu));
    memcpy(recA.cpu + geoBytes + subBytes, g->matRecords.data(), g->matRecords.size() * sizeof(MatGpu));
    if (!g->records.r || g->records.bytes < recordBytes) {
        BuryRes(g->records);
        if (!Buffer(g->records, std::max<uint64_t>(recordBytes * 2, 1 << 20), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON)) return false;
    }
    const uint32_t dGeo = FrameDescriptor(), dSub = FrameDescriptor(), dMat = FrameDescriptor();
    if (dMat == kNone) { Error("frame descriptors exhausted"); return false; }
    RawSrv(dGeo, g->records.Get(), 0, geoBytes);
    RawSrv(dSub, g->records.Get(), geoBytes, subBytes);
    RawSrv(dMat, g->records.Get(), geoBytes + subBytes, matBytes);
    cb.geometries = dGeo; cb.submeshes = dSub; cb.materials = dMat;
    cpuProfile.Mark(3);
    memcpy(cbA.cpu, &cb, sizeof cb);
    g->pathFrame=cb;

    // ---- recording: bindings
    cpuProfile.Mark(4);
    ID3D12DescriptorHeap *heaps[] = {g->heap.Get()};
    l->SetDescriptorHeaps(1, heaps);
    l->SetComputeRootSignature(g->root.Get());
    l->SetComputeRootConstantBufferView(1, cbA.gpu);
    const D3D12_GPU_DESCRIPTOR_HANDLE table = g->heap->GetGPUDescriptorHandleForHeapStart();
    l->SetComputeRootDescriptorTable(3, table);
    l->SetComputeRootDescriptorTable(4, table);
    // the scene slot is set again after this frame's top level; the passes before it do not trace
    if (!g->tlas.r && !Buffer(g->tlas, 64 << 10, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                              D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)) return false;
    l->SetComputeRootShaderResourceView(2, g->tlas.Va());
    const bool timed = g->queries && f.timestampFrequency;
    if (timed) Stamp(l, 0);
    // RTC_MARKERS: each pass group in its own debug event (PIX's unicode form), the last one closed on any return
    struct Marks {
        ID3D12GraphicsCommandList4 *l; bool on, open = false;
        void Next(const wchar_t *name) {
            if (!on) return;
            if (open) l->EndEvent();
            l->BeginEvent(0, name, (UINT)((wcslen(name) + 1) * sizeof(wchar_t)));
            open = true;
        }
        ~Marks() { if (open) l->EndEvent(); }
    } mark{l, g->markers};
    mark.Next(L"RtCore build");

    // uploads: records
    bar.To(g->records, D3D12_RESOURCE_STATE_COPY_DEST);
    bar.Flush(l);
    l->CopyBufferRegion(g->records.Get(), 0, g->upload.Get(), recA.offset, recordBytes);
    const auto NPSR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto UAVS = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bar.To(g->records, NPSR);
    for (GeoState *s : states)
        for (TessCopy &t : s->tess)
            if (t.buf.r) bar.To(t.buf, UAVS);
    bar.Flush(l);
    bar.Uav();
    bar.Flush(l);

    // ---- tessellated occluder copies
    for (uint32_t i = 0; i < f.geometryCount; i++) {
        if (usable[i] != 1) continue;
        const GeoGpu &r = g->geoRecords[i];
        const Geometry &x = f.geometries[i];
        uint32_t sub = r.firstSub;
        for (uint32_t k = 0; k < x.submeshCount; k++) {
            if (x.submeshes[k].count / 3 == 0) continue;
            const SubGpu &sr = g->subRecords[sub];
            if (sr.blasTess) {
                PassCb p;
                p.a = i; p.b = sub; p.c = sr.tessBuf; p.count = sr.triCount * sr.tessLevel * sr.tessLevel;
                Dispatch(l, pTess, p, p.count);
            }
            sub++;
        }
    }
    bar.Uav();
    for (GeoState *s : states)
        for (TessCopy &t : s->tess)
            if (t.buf.r) bar.To(t.buf, NPSR);
    bar.Flush(l);

    // ---- bottom levels
    cpuProfile.Mark(5);
    uint64_t scratchUsed = 0, staticBuilt = 0;
    bool periodic = false;
    auto Scratch = [&](uint64_t bytes, D3D12_GPU_VIRTUAL_ADDRESS &at) {
        bytes = (bytes + 255) & ~255ull;
        if (scratchUsed && scratchUsed + bytes > kScratchBatch) { bar.Uav(); bar.Flush(l); scratchUsed = 0; }
        if (!g->scratch.r || g->scratch.bytes < scratchUsed + bytes) {
            if (scratchUsed) { bar.Uav(); bar.Flush(l); scratchUsed = 0; }
            BuryRes(g->scratch);
            if (!Buffer(g->scratch, std::max<uint64_t>(bytes, 32ull << 20), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON)) return false;
            bar.To(g->scratch, UAVS);
            bar.Flush(l);
        }
        at = g->scratch.Va() + scratchUsed;
        scratchUsed += bytes;
        return true;
    };
    // A geometry's opacity micromaps, before a bottom level is built on them: the OMM index (uniform submeshes' special
    // index, ClearBuffer; baked ones' micromaps, PathOmmBake), then the micromap array. False: allocation failed (the
    // frame fails), or NVAPI refused (micromaps off from here on, the geometry skipped this frame).
    auto BakeOmm = [&](uint32_t i, GeoState &s) {
        const GeoGpu &r = g->geoRecords[i];
        const uint32_t tris = g->geoOmm[i], baked = g->geoOmmBaked[i];
        const auto DEF = D3D12_HEAP_TYPE_DEFAULT;
        const auto UAVF = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (!s.ommIndex.r || s.ommIndex.bytes < 4ull * tris) {
            BuryRes(s.ommIndex);
            if (!Buffer(s.ommIndex, 4ull * tris, DEF, UAVF, D3D12_RESOURCE_STATE_COMMON)) return false;
        }
        BuryRes(s.ommArray);
        Res data, descs;  // the array build's inputs, released once the frame is done
        if (baked) {
            if (!Buffer(data, (uint64_t)kOmmBytes * baked, DEF, UAVF, D3D12_RESOURCE_STATE_COMMON) ||
                !Buffer(descs, 8ull * baked, DEF, UAVF, D3D12_RESOURCE_STATE_COMMON)) return false;
            Bury(data.r);
            Bury(descs.r);
        }
        const uint32_t uIndex = FrameDescriptor(), uData = baked ? FrameDescriptor() : kNone, uDescs = baked ? FrameDescriptor() : kNone;
        if (uIndex == kNone || (baked && uDescs == kNone)) { Error("frame descriptors exhausted"); return false; }
        RawUav(uIndex, s.ommIndex.Get(), s.ommIndex.bytes);
        if (baked) {
            RawUav(uData, data.Get(), data.bytes);
            RawUav(uDescs, descs.Get(), descs.bytes);
        }
        // the PT's bakes (path_omm.inl): their submeshes' inputs, and the pass made on first use
        uint32_t sParams = kNone;
        if (baked) {
            if (!g->pipes[pPathOmmBake]) {
                D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
                pd.pRootSignature = g->root.Get();
                pd.CS = {rtc_shaders::kAll[pPathOmmBake].code, rtc_shaders::kAll[pPathOmmBake].size};
                if (FAILED(g->device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g->pipes[pPathOmmBake])))) {
                    Error("PT micromap pass: micromaps off");
                    g->omm = false;
                    return false;
                }
                g->pipes[pPathOmmBake]->SetName(L"RtCore PathOmmBake");
            }
            const uint64_t bytes = sizeof(PtOmmParamsGpu) * r.subCount;
            Alloc pa;
            sParams = FrameDescriptor();
            if (sParams == kNone) { Error("frame descriptors exhausted"); return false; }
            if (!Upload(bytes, 256, pa)) return false;
            memcpy(pa.cpu, g->subOmmPath.data() + r.firstSub, bytes);
            RawSrv(sParams, g->upload.Get(), pa.offset, bytes);
        }
        bar.To(s.ommIndex, UAVS);
        bar.To(data, UAVS);
        bar.To(descs, UAVS);
        bar.Flush(l);
        for (uint32_t k = r.firstSub; k < r.firstSub + r.subCount; k++) {
            const SubOmm &so = g->subOmm[k];
            PassCb p;
            if (so.mode == 1) {
                p.a = uIndex; p.b = so.special; p.c = so.at; p.count = g->subRecords[k].triCount;
                Dispatch(l, pClear, p, p.count);
            } else if (so.mode == 2) {
                p.a = i; p.b = k; p.c = uData; p.d = uDescs; p.e = uIndex; p.f = so.first; p.g = so.at;
                p.count = g->subRecords[k].triCount * kOmmWords;
                p.h = sParams; p.light = k - r.firstSub;
                Dispatch(l, pPathOmmBake, p, p.count);
            }
        }
        bar.Uav();
        bar.To(s.ommIndex, NPSR);
        bar.To(data, NPSR);
        bar.To(descs, NPSR);
        bar.Flush(l);
        if (baked) {
            NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_USAGE_COUNT usage = {baked, kOmmLevel, NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE};
            NVAPI_D3D12_BUILD_RAYTRACING_OPACITY_MICROMAP_ARRAY_INPUTS oi = {};
            oi.flags = NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_BUILD_FLAG_PREFER_FAST_TRACE;
            oi.numOMMUsageCounts = 1;
            oi.pOMMUsageCounts = &usage;
            oi.inputBuffer = data.Va();
            oi.perOMMDescs = {descs.Va(), 8};
            NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_PREBUILD_INFO pi = {};
            NVAPI_GET_RAYTRACING_OPACITY_MICROMAP_ARRAY_PREBUILD_INFO_PARAMS pp = {NVAPI_GET_RAYTRACING_OPACITY_MICROMAP_ARRAY_PREBUILD_INFO_PARAMS_VER, &oi, &pi};
            if (NvAPI_D3D12_GetRaytracingOpacityMicromapArrayPrebuildInfo(g->device.Get(), &pp) != NVAPI_OK || !pi.resultDataMaxSizeInBytes) {
                Error("micromap array prebuild refused: micromaps off");
                g->omm = false;
                return false;
            }
            if (!Buffer(s.ommArray, pi.resultDataMaxSizeInBytes, DEF, UAVF, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)) return false;
            NVAPI_D3D12_BUILD_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC bd = {s.ommArray.Va(), oi, 0};
            if (!Scratch(pi.scratchDataSizeInBytes, bd.scratchOpacityMicromapArrayData)) return false;
            NVAPI_BUILD_RAYTRACING_OPACITY_MICROMAP_ARRAY_PARAMS bp = {NVAPI_BUILD_RAYTRACING_OPACITY_MICROMAP_ARRAY_PARAMS_VER, &bd, 0, nullptr};
            if (NvAPI_D3D12_BuildRaytracingOpacityMicromapArray(l, &bp) != NVAPI_OK) {
                Error("micromap array build refused: micromaps off");
                g->omm = false;
                return false;
            }
            bar.Uav();  // the array before a bottom level reads it
            bar.Flush(l);
        }
        s.ommReady = true;
        return true;
    };
    g->instances.clear();
    int builds = 0, updates = 0;
    uint32_t ommPending = 0;
    for (uint32_t i = 0; i < f.geometryCount; i++) {
        const Geometry &x = f.geometries[i];
        if (usable[i] != 1) continue;
        GeoState &s = *states[i];
        const GeoGpu &r = g->geoRecords[i];
        g->geoDescs.clear();
        uint32_t sub = r.firstSub;
        for (uint32_t k = 0; k < x.submeshCount; k++) {
            const Submesh &sm = x.submeshes[k];
            if (sm.count / 3 == 0) continue;
            const SubGpu &sr = g->subRecords[sub++];
            D3D12_RAYTRACING_GEOMETRY_DESC d = {};
            d.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            const MaterialKind kind = g->materials[sr.material].d.kind;
            d.Flags = (kind == MaterialKind::Cutout || kind == MaterialKind::Translucent)
                          ? D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            if (sr.blasTess) {
                const TessCopy &tc = s.tess[k];
                d.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                d.Triangles.VertexCount = 3 * sr.triCount * sr.tessLevel * sr.tessLevel;
                d.Triangles.VertexBuffer.StartAddress = tc.buf.Va();
                d.Triangles.VertexBuffer.StrideInBytes = 12;
            } else {
                const uint32_t base = (uint32_t)std::max(0, x.baseVertex);
                d.Triangles.IndexFormat = x.index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
                d.Triangles.IndexCount = sr.triCount * 3;
                d.Triangles.IndexBuffer = x.indexBuffer->GetGPUVirtualAddress() + x.indexOffset + (uint64_t)sm.start * (x.index32 ? 4 : 2);
                d.Triangles.VertexFormat = BlasVertexFormat(x.position.format);
                d.Triangles.VertexCount = x.vertexCount > base ? x.vertexCount - base : 0;
                d.Triangles.VertexBuffer.StartAddress = x.position.buffer->GetGPUVirtualAddress() + x.position.offset + (uint64_t)base * x.position.stride;
                d.Triangles.VertexBuffer.StrideInBytes = x.position.stride;
            }
            g->geoDescs.push_back(d);
        }
        if (g->geoDescs.empty()) continue;
        const bool dynamic = (x.flags & kSkinned) != 0;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        if (dynamic) in.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
        in.NumDescs = (UINT)g->geoDescs.size();
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.pGeometryDescs = g->geoDescs.data();
        const bool rigidChanged = !dynamic && (x.flags & kDirty);
        // micromaps: a bake takes four of the frame's descriptors (one without a baked submesh). 0.22.21: while the
        // frame has none left the bottom level is built without them - the same picture, the passes' alpha tests stay -
        // and built again with them in a later frame (a scene's ~1400 bakes at once failed that frame and every next one,
        // all of them baked afresh each time)
        const bool want = g->geoOmm[i] > 0;
        const bool room = g->frameDescriptors + (g->geoOmmBaked[i] ? 4u : 1u) <= kFrameDescriptors;
        if (want && g->omm && s.ommPlain && s.built && !s.dirty && room &&
            (dynamic || staticBuilt == 0 || staticBuilt + s.triangles <= kStaticTrianglesPerFrame))
            s.dirty = true;  // built afresh with them (their size differs; a refit takes the build's descriptions)
        const bool due = dynamic && s.built && !s.dirty && f.frame - s.builtFrame >= kRebuildFrames && !periodic;
        const bool rebuild = s.dirty || !s.built || due || rigidChanged;
        periodic |= due;
        if (rebuild && !dynamic) {
            if (staticBuilt > 0 && staticBuilt + s.triangles > kStaticTrianglesPerFrame) continue;  // next frame
            staticBuilt += s.triangles;
        }
        // with micromaps: the same descriptions through NVAPI (build, prebuild and the refits; a refit takes the same
        // index buffer and array as the build)
        bool omm = want;
        if (omm && !g->omm) { s.dirty = true; continue; }  // NVAPI refused earlier this frame: built plain from the next
        NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS_EX inEx = {};
        if (omm && !rebuild && s.built && s.ommPlain) omm = false;  // a refit of the plain build
        else if (omm && (dynamic || rebuild) && !s.ommReady) {
            if (!room) omm = false;
            else if (!BakeOmm(i, s)) {
                if (!g->omm) continue;
                return false;
            }
        }
        if (omm && (dynamic || rebuild)) {
            g->geoDescsEx.clear();
            g->ommUsage.clear();
            g->ommUsage.reserve(g->geoDescs.size());  // the descriptions point into it
            for (uint32_t k = 0; k < r.subCount; k++) {
                const D3D12_RAYTRACING_GEOMETRY_DESC &d = g->geoDescs[k];
                const SubOmm &so = g->subOmm[r.firstSub + k];
                NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX e = {};
                e.flags = d.Flags;
                if (!so.mode) {
                    e.type = NVAPI_D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES_EX;
                    e.triangles = d.Triangles;
                } else {
                    e.type = NVAPI_D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES_EX;
                    e.ommTriangles.triangles = d.Triangles;
                    NVAPI_D3D12_RAYTRACING_GEOMETRY_OMM_ATTACHMENT_DESC &a = e.ommTriangles.ommAttachment;
                    a.opacityMicromapIndexBuffer = {s.ommIndex.Va() + 4ull * so.at, 4};
                    a.opacityMicromapIndexFormat = DXGI_FORMAT_R32_UINT;
                    a.opacityMicromapArray = s.ommArray.Va();  // 0: no baked submesh in the geometry
                    if (so.mode == 2) {
                        g->ommUsage.push_back({g->subRecords[r.firstSub + k].triCount, kOmmLevel, NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE});
                        a.numOMMUsageCounts = 1;
                        a.pOMMUsageCounts = &g->ommUsage.back();
                    }
                }
                g->geoDescsEx.push_back(e);
            }
            inEx.type = in.Type;
            inEx.flags = (NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS_EX)in.Flags;  // the same bits
            inEx.numDescs = in.NumDescs;
            inEx.descsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            inEx.geometryDescStrideInBytes = sizeof(NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX);
            inEx.pGeometryDescs = g->geoDescsEx.data();
        }
        if (dynamic || rebuild) {
            if (s.dirty || !s.blas) {
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi = {};
                if (omm) {
                    NVAPI_GET_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_EX_PARAMS pp = {NVAPI_GET_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_EX_PARAMS_VER, &inEx, &pi};
                    if (NvAPI_D3D12_GetRaytracingAccelerationStructurePrebuildInfoEx(g->device.Get(), &pp) != NVAPI_OK) pi = {};
                } else {
                    g->device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &pi);
                }
                if (!pi.ResultDataMaxSizeInBytes) continue;
                if (pi.ResultDataMaxSizeInBytes > s.blasBytes) {
                    if (s.blas) Bury(s.blas);
                    s.blas.Reset();
                    Res b;
                    if (!Buffer(b, pi.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)) return false;
                    s.blas = b.r;
                    s.blasBytes = pi.ResultDataMaxSizeInBytes;
                }
                s.scratchBytes = pi.ScratchDataSizeInBytes;
                s.updateBytes = pi.UpdateScratchDataSizeInBytes;
                s.built = false;
            }
            const bool update = !rebuild && s.built;
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
            if (!Scratch(update ? s.updateBytes : s.scratchBytes, bd.ScratchAccelerationStructureData)) return false;
            bd.Inputs = in;
            if (update) {
                bd.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
                bd.SourceAccelerationStructureData = s.blas->GetGPUVirtualAddress();
            }
            bd.DestAccelerationStructureData = s.blas->GetGPUVirtualAddress();
            if (omm) {
                NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC_EX be = {bd.DestAccelerationStructureData, inEx,
                                                                                bd.SourceAccelerationStructureData,
                                                                                bd.ScratchAccelerationStructureData};
                be.inputs.flags = (NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS_EX)bd.Inputs.Flags;
                NVAPI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_EX_PARAMS bp = {NVAPI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_EX_PARAMS_VER, &be, 0, nullptr};
                if (NvAPI_D3D12_BuildRaytracingAccelerationStructureEx(l, &bp) != NVAPI_OK) {
                    Error("bottom level with micromaps refused: micromaps off");
                    g->omm = false;
                    s.dirty = true;
                    s.built = false;
                    continue;
                }
            } else {
                l->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
            }
            if (update) updates++;
            else { builds++; s.built = true; s.builtFrame = f.frame; s.dirty = false; s.ommPlain = want && !omm; }
        }
        if (!s.built) continue;
        ommPending += s.ommPlain ? 1u : 0u;
        D3D12_RAYTRACING_INSTANCE_DESC inst = {};
        memcpy(inst.Transform, r.m, sizeof inst.Transform);
        inst.InstanceID = i;
        // Path tracing: 0x02 seen by camera rays, 0x01 by every other ray (BSDF
        // continuations, environment/emitter NEE), 0x04 blocks analytic lights; a
        // shadow-only proxy is 0x04 alone, camera-hidden geometry (API 21: a first-
        // person plugin's head) has no 0x02.
        // A light-link class blocks with its own bit (0x08 << class) instead of
        // 0x04, so a light's shadow ray leaves out the classes it does not light.
        // API 26: kNoCast has none of those bits while authorRules asks for it.
        const uint32_t link=x.lightLink&0x1fu,blocks=(x.flags&kNoCast)&&(g->pathEnvironment.authorRules&kPathAuthorCull)?0u:link?link<<3:0x04u;
        // API 24: camera-only geometry is 0x02 alone (no 0x01, blocks nothing).
        inst.InstanceMask=(x.flags&kShadowOnly)?blocks:(x.flags&kCameraOnly)?((x.flags&kCameraHidden)?0u:0x02u):
            blocks|0x01u|((x.flags&kCameraHidden)?0u:0x02u);
        inst.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        if(x.flags&(kPrimaryCullBack|kPrimaryCullFront)) {
            // DXR defines triangle facing in object space; TLAS mirroring does
            // NOT flip it (DirectX Raytracing Functional Spec, instance flags).
            // Preserve the authored face; only an explicit Cull Front reverses it.
            const bool reversed=(x.flags&kPrimaryCullFront)!=0;
            inst.Flags=reversed?D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE:0;
        }
        inst.AccelerationStructure = s.blas->GetGPUVirtualAddress();
        g->instances.push_back(inst);
        triangles += s.triangles;
    }
    if (scratchUsed) { bar.Uav(); bar.Flush(l); scratchUsed = 0; }

    // ---- top level (always one, possibly empty: the passes need a scene)
    cpuProfile.Mark(6);
    {
        Alloc ia;
        const uint64_t ib = std::max<size_t>(1, g->instances.size()) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
        if (!Upload(ib, 16, ia)) return false;
        if (!g->instances.empty()) memcpy(ia.cpu, g->instances.data(), g->instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        in.NumDescs = (UINT)g->instances.size();
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.InstanceDescs = ia.gpu;
        // Type, flags and layout are fixed; instance contents/address do not affect the sizing query.
        // Cache only the exact count, including zero. Scene changes immediately invalidate this key.
        if (g->tlasPrebuildCount != in.NumDescs) {
            g->device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &g->tlasPrebuild);
            g->tlasPrebuildCount = in.NumDescs;
        }
        const auto &pi = g->tlasPrebuild;
        if (!g->tlas.r || pi.ResultDataMaxSizeInBytes > g->tlas.bytes) {
            BuryRes(g->tlas);
            if (!Buffer(g->tlas, std::max<uint64_t>(pi.ResultDataMaxSizeInBytes * 2, 64 << 10), D3D12_HEAP_TYPE_DEFAULT,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)) return false;
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
        if (!Scratch(pi.ScratchDataSizeInBytes, bd.ScratchAccelerationStructureData)) return false;
        bd.Inputs = in;
        bd.DestAccelerationStructureData = g->tlas.Va();
        l->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
        bar.Uav();
        bar.Flush(l);
        l->SetComputeRootShaderResourceView(2, g->tlas.Va());
    }
    cpuProfile.Mark(7);
    if (timed) {
        Stamp(l, 1);
        l->ResolveQueryData(g->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, si * kStamps, kStamps, g->readback.Get(), (uint64_t)si * kReadbackStride);
    }
    Readback &slot = g->slots[si];
    slot.frame = f.frame; slot.pending = true; slot.timed = timed; slot.frequency = f.timestampFrequency;

    const double ms = NowMs() - t0;
    cpuProfile.Finish(f.frame,f.geometryCount);
    std::lock_guard<std::mutex> lk(g->statusLock);
    Status &st = g->status;
    st.frames++;
    g->pathFrameValid=f.frame;
    st.ok = 1;
    st.geometries = f.geometryCount;
    st.triangles = triangles;
    st.tessellatedTriangles = tessTriangles;
    st.ommPending = ommPending;
    st.pathOmmUniform = g->pathOmmUniform;
    st.pathOmmBaked = g->pathOmmBaked;
    st.pathOmmTriangles = g->pathOmmTriangles;
    st.staticDescriptors = g->nextStatic - (uint32_t)g->freeDescriptors.size();
    st.frameDescriptors = g->frameDescriptors;
    g->cpuSum += ms;
    st.recordCpuMs = g->cpuSum / (double)++g->cpuSamples;
    (void)builds;
    (void)updates;
    return true;
}

#include "path_hair.inl"
#include "path_lighting.inl"
bool PathTracingAvailable() {return g && g->ok && PathLssCaps();}
void GetPathLightingStatus(uint32_t &emitters,uint64_t &bytes) {
    emitters=0;bytes=0;if(!g)return;
    emitters=(uint32_t)g->pathLighting.ranges.size();
    for(auto &r:g->pathLighting.cdf)bytes+=r.bytes;
}

// The skin's scattering in screen space (11-10, docs\pt-skin-sss-ss-20261110, shaders\path_scatter.hlsl).
static void ClearPathScatter() {
    for(int e=0;e<2;e++)for(int k=0;k<2;k++){BuryRes(g->pathScatter[e][k]);FreeDescriptor(g->pathScatterUav[e][k]);}
    for(auto &s:g->pathScatterSize)s[0]=s[1]=0;
    for(Pipe p:{pPathScatterX,pPathScatterY}){Bury(g->pipes[p]);g->pipes[p].Reset();}
}
// d'Eon & Luebke's skin profile (GPU Gems 3 ch. 14) as the raster route blurs it (the skin component's native\sss.cpp FillCb
// at width 1), in profile millimetres: 1 / 2 var per Gaussian, their weights in red, green, blue, 21 taps at R s|s| (s -1..1,
// R 3 sigma of the widest), each Gaussian's share of the interval half way to the neighbouring taps (the outer ones to infinity).
struct PathScatterTable { float invTwoVar[6],weights[18],offsets[21],shares[21*6]; };
static_assert(sizeof(PathScatterTable)==171*4,"path_scatter.hlsl table");
static const PathScatterTable &ScatterTable() {
    static const PathScatterTable table=[] {
        const double var[6]={0.0064,0.0484,0.187,0.567,1.99,7.41};
        const double w[3][6]={{0.233,0.100,0.118,0.113,0.358,0.078},{0.455,0.336,0.198,0.007,0.004,0.000},
                              {0.649,0.344,0.000,0.007,0.000,0.000}};
        PathScatterTable r{};double sigma[6],o[21],widest=0;
        for(int k=0;k<6;k++) {
            sigma[k]=std::sqrt(var[k]);widest=std::max(widest,sigma[k]);r.invTwoVar[k]=(float)(1/(2*var[k]));
            for(int c=0;c<3;c++)r.weights[6*c+k]=(float)w[c][k];
        }
        for(int i=0;i<21;i++){const double s=(i-10)/10.0;o[i]=3*widest*s*std::fabs(s);r.offsets[i]=(float)o[i];}
        auto phi=[](double x){return .5*std::erfc(-x/std::sqrt(2.0));};
        for(int i=0;i<21;i++) {
            const double lo=i==0?-1e30:.5*(o[i-1]+o[i]),hi=i==20?1e30:.5*(o[i]+o[i+1]);
            for(int k=0;k<6;k++)r.shares[6*i+k]=(float)(phi(hi/sigma[k])-phi(lo/sigma[k]));
        }
        return r;
    }();
    return table;
}
// The image plane's axis (towards the scene) and metres per pixel per metre of depth along it, from the view's unprojection
// (row-major GL clip): exact for a pinhole, asymmetric eye frusta too. The scattering steps its taps by them.
static bool PathScatterView(const PathView &v,float forward[3],float &perPixel) {
    auto point=[&](double x,double y,double out[3]) {
        const double p[4]={x,y,1,1};double r[4];
        for(int i=0;i<4;i++)r[i]=v.clipToWorld[4*i]*p[0]+v.clipToWorld[4*i+1]*p[1]+v.clipToWorld[4*i+2]*p[2]+v.clipToWorld[4*i+3]*p[3];
        if(!(std::fabs(r[3])>1e-12))return false;
        for(int k=0;k<3;k++)out[k]=r[k]/r[3];
        return true;
    };
    double a[3],b[3],c[3];
    if(!point(0,0,a) || !point(1,0,b) || !point(0,1,c))return false;
    double u[3],w[3],n[3],to[3];
    for(int k=0;k<3;k++){u[k]=b[k]-a[k];w[k]=c[k]-a[k];to[k]=a[k]-v.camera[k];}
    n[0]=u[1]*w[2]-u[2]*w[1];n[1]=u[2]*w[0]-u[0]*w[2];n[2]=u[0]*w[1]-u[1]*w[0];
    double len=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
    if(!(len>0))return false;
    if(n[0]*to[0]+n[1]*to[1]+n[2]*to[2]<0)len=-len;
    for(int k=0;k<3;k++)n[k]/=len;
    const double depth=n[0]*to[0]+n[1]*to[1]+n[2]*to[2],step=std::sqrt(w[0]*w[0]+w[1]*w[1]+w[2]*w[2])*2/v.height;
    if(!(depth>0) || !(step>0) || !std::isfinite(step/depth))return false;
    for(int k=0;k<3;k++)forward[k]=(float)n[k];
    perPixel=(float)(step/depth);
    return true;
}

bool SetPathMaterial(uint32_t material,const PathMaterial &d) {
    if(!g || !g->ok || material==0 || material>=g->materials.size() || !g->materials[material].live)return false;
    PtMaterialState next;
    auto &old=g->pathMaterials[material];
    bool owned[kPtTextures]={};
    auto &r=next.gpu;
    memcpy(r.base,d.baseColor,16); memcpy(r.spec,d.specular,12); r.spec[3]=d.roughness;
    memcpy(r.emission,d.emissionColor,12); r.emission[3]=d.metallic;
    memcpy(r.uv,d.uvST,16); r.params[0]=d.normalScale; r.params[1]=d.ior; r.params[2]=d.transmission;
    memcpy(r.params+3,&d.flags,4);
    memcpy(r.gloss,d.glossParams,16);
    memcpy(r.skin,d.skin,sizeof r.skin);
    memcpy(r.mapST,d.mapST,sizeof r.mapST);memcpy(r.bump,d.bump,sizeof r.bump);
    memcpy(r.alphaST,d.alphaST,16);r.bump[3]=d.alphaAdjust;
    memcpy(r.materialControls,d.materialControls,16);
    memcpy(r.sampling,d.sampling,sizeof r.sampling);
    r.second[1]=d.mode;r.second[2]=d.secondSampling;
    memcpy(r.secondST,d.secondST,16);memcpy(r.directGain,d.directGain,16);
    ID3D12Resource *textures[]={d.base,d.normal,d.surface,d.emission,d.gloss,d.detailNormal,d.detailMask,d.alpha,d.second};
    static_assert(sizeof textures/sizeof *textures==kPtTextures,"path material slots");
    auto discard=[&](){for(int k=0;k<kPtTextures;k++)if(owned[k])FreeDescriptor(next.Index(k));};
    for(int k=0;k<kPtTextures;k++) {
        if(!textures[k])continue;
        const auto td=textures[k]->GetDesc();
        if(td.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || td.DepthOrArraySize!=1 || td.SampleDesc.Count!=1) {
            discard();
            return false;
        }
        const uint32_t requested=k<8?d.formats[k]:d.secondFormat;
        const uint32_t format=requested?requested:(uint32_t)td.Format;
        const uint32_t before=old.Index(k);
        if(old.resources[k].Get()==textures[k] && old.formats[k]==format && before!=kNone) {
            next.Index(k)=before;next.resources[k]=old.resources[k];next.formats[k]=format;continue;
        }
        next.Index(k)=StaticDescriptor();
        if(next.Index(k)==kNone) { discard(); return false; }
        owned[k]=true;next.formats[k]=format;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format=(DXGI_FORMAT)format;
        sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels=td.MipLevels;
        g->device->CreateShaderResourceView(textures[k],&sv,Cpu(next.Index(k)));
        next.resources[k]=textures[k];
    }
    for(int k=0;k<kPtTextures;k++) {
        if(old.resources[k].Get()!=next.resources[k].Get())Bury(old.resources[k]);
        uint32_t &before=old.Index(k);
        if(before!=next.Index(k))FreeDescriptor(before);
    }
    // path_omm.inl: the coverage's inputs; their micromaps wait until these have stayed the same a few frames
    next.alphaVersion=d.alpha?d.alphaVersion:0;
    next.ommKey=PtOmmKey(r,next.alphaVersion);
    next.ommSince=old.ommKey==next.ommKey && old.ommKey?old.ommSince:g->frame;
    old=std::move(next);
    g->pathLighting.frame=UINT64_MAX;
    return true;
}

void ClearPathMaterials() {
    if(!g)return;
    for(auto &kv:g->pathMaterials)for(int k=0;k<kPtTextures;k++) {
        Bury(kv.second.resources[k]); FreeDescriptor(kv.second.Index(k));
    }
    g->pathMaterials.clear();
    g->pathMaterialRecords.clear();
    g->pathLights.clear();
    for(int k=0;k<4;k++) {
        Bury(g->pathCubes[k]);g->pathCubes[k].Reset();
        FreeDescriptor(g->pathCubeSrv[k]);g->pathCubeSrv[k]=kNone;
    }
    g->pathEnvironment=PathEnvironment{};
    ClearPathHair();
    ClearPathSkinning();
    ClearPathMotion();
    ClearPathLighting();
    FreeDescriptor(g->pathNvNull);g->pathNvNull=kNone;
    for(int e=0;e<2;e++){BuryRes(g->pathVis[e]);FreeDescriptor(g->pathVisUav[e]);g->pathVisUav[e]=kNone;g->pathVisSize[e][0]=g->pathVisSize[e][1]=0;}
    Bury(g->pipes[pPathTrace]);g->pipes[pPathTrace].Reset();
    Bury(g->pathSerState);g->pathSerState.Reset();BuryRes(g->pathShaderTable);
    Bury(g->pipes[pPathTraceSplit]);g->pipes[pPathTraceSplit].Reset();
    Bury(g->pipes[pPathTraceSss]);g->pipes[pPathTraceSss].Reset();
    ClearPathScatter();
}

void SetPathEyeRigs(ID3D12Resource *rigs,uint32_t count) {
    if(!g)return;
    g->pathEyeRigs=count?rigs:nullptr;g->pathEyeRigCount=rigs?count:0;
}
bool SetPathEnvironment(const PathEnvironment &d) {
    if(!g || !g->ok)return false;
    if(memcmp(&d,&g->pathEnvironment,sizeof d))g->pathLighting.frame=UINT64_MAX;
    ID3D12Resource *resources[]={d.lighting,d.background,d.indoor,d.probe};
    for(int k=0;k<4;k++) {
        if(resources[k]==g->pathCubes[k].Get() && d.formats[k]==g->pathEnvironment.formats[k])continue;
        uint32_t index=kNone;
        if(resources[k]) {
            const auto td=resources[k]->GetDesc();
            if(td.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || td.DepthOrArraySize!=6 || td.SampleDesc.Count!=1)return false;
            index=StaticDescriptor();if(index==kNone)return false;
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format=d.formats[k]?(DXGI_FORMAT)d.formats[k]:td.Format;
            sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBE;
            sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.TextureCube.MipLevels=td.MipLevels;
            g->device->CreateShaderResourceView(resources[k],&sv,Cpu(index));
        }
        Bury(g->pathCubes[k]);FreeDescriptor(g->pathCubeSrv[k]);
        g->pathCubes[k]=resources[k];g->pathCubeSrv[k]=index;
    }
    g->pathEnvironment=d;
    return true;
}

bool RecordPathTrace(ID3D12GraphicsCommandList4 *l,const PathView &v,const PathResponsivity *response) {
    if(!g || !g->ok || !l || !g->pathFrameValid || g->pathFrameValid!=g->frame)return false;
    if(!v.width || !v.height || v.width>16384 || v.height>16384 || !v.samples || v.samples>8 ||
       !v.maxBounces || v.maxBounces>8)return false;
    ID3D12Resource *outputs[]={v.color,v.diffuse,v.normal,v.specular,v.depth,v.hitDistance};
    for(auto *r:outputs) {
        if(!r)return false;
        const auto d=r->GetDesc();
        if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width!=v.width || d.Height!=v.height ||
           d.DepthOrArraySize!=1 || !(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))return false;
    }
    if(response && response->mask) {
        const auto d=response->mask->GetDesc();
        if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width!=v.width || d.Height!=v.height ||
            d.DepthOrArraySize!=1 || d.MipLevels!=1 || d.SampleDesc.Count!=1 || d.Format!=DXGI_FORMAT_R16_FLOAT ||
            !(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) || !v.motion ||
            !std::isfinite(response->strength) || response->strength<0 || response->strength>1 ||
            !std::isfinite(response->motionScale[0]) || !std::isfinite(response->motionScale[1]) ||
            response->motionScale[0]<=0 || response->motionScale[1]<=0)return false;
        for(auto *r:outputs)if(response->mask==r)return false;
        if(response->mask==v.motion || response->mask==v.detailReference ||
            response->mask==v.diffuseRadiance || response->mask==v.specularRadiance)return false;
    }
    if(bool(v.diffuseRadiance)!=bool(v.specularRadiance))return false;
    const bool split=v.diffuseRadiance!=nullptr;
    // 11-10 (docs\pt-skin-sss-ss-20261110): skin with a scattering strength (VaM's Subsurface Strength above 0) has its
    // direct diffuse light spread over the skin profile in screen space after the trace (PT_SSS, path_scatter.hlsl), before
    // RR; no rays. Without such skin the trace is the one before, with no added work.
    float scatterUnit=0,scatterForward[3]={},scatterPixel=0;
    if(!split)for(auto &kv:g->pathMaterials) {
        if(kv.first>=g->materials.size() || !g->materials[kv.first].live)continue;
        const PtMaterialGpu &m=kv.second.gpu;uint32_t flags;memcpy(&flags,&m.params[3],4);
        if((flags&8) && m.skin[0]>0 && m.skin[21]>0 && m.skin[20]>scatterUnit)scatterUnit=m.skin[20];
    }
    if(scatterUnit>0 && !PathScatterView(v,scatterForward,scatterPixel))scatterUnit=0;
    const uint32_t variant=split?1:scatterUnit>0?2:0;
    const Pipe tracePipe=variant==1?pPathTraceSplit:variant==2?pPathTraceSss:pPathTrace;
    if(g->pathVariant!=variant) {
        const Pipe old=g->pathVariant==1?pPathTraceSplit:g->pathVariant==2?pPathTraceSss:pPathTrace;
        Bury(g->pipes[old]);g->pipes[old].Reset();
        Bury(g->pathSerState);g->pathSerState.Reset();BuryRes(g->pathShaderTable);
        if(g->pathVariant==2)ClearPathScatter();
        g->pathVariant=variant;
    }
    if(!g->pipes[tracePipe] && !g->pathSerState) {
        if(!PathLssCaps())return false;
        NVAPI_D3D12_SET_CREATE_PIPELINE_STATE_OPTIONS_PARAMS opt={NVAPI_D3D12_SET_CREATE_PIPELINE_STATE_OPTIONS_PARAMS_VER,
            (NvU32)(NVAPI_D3D12_PIPELINE_CREATION_STATE_FLAGS_ENABLE_LSS_SUPPORT |
                (g->omm?NVAPI_D3D12_PIPELINE_CREATION_STATE_FLAGS_ENABLE_OMM_SUPPORT:0))};
        if(NvAPI_D3D12_SetCreatePipelineStateOptions(g->device.Get(),&opt)!=NVAPI_OK)return false;
        if(NvAPI_D3D12_SetNvShaderExtnSlotSpaceLocalThread(g->device.Get(),0,8)!=NVAPI_OK) {
            opt.flags=(NVAPI_D3D12_PIPELINE_CREATION_STATE_FLAGS)0;
            NvAPI_D3D12_SetCreatePipelineStateOptions(g->device.Get(),&opt);
            return false;
        }
        NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAPS caps=NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAP_NONE;
        bool ser=NvAPI_D3D12_GetRaytracingCaps(g->device.Get(),NVAPI_D3D12_RAYTRACING_CAPS_TYPE_THREAD_REORDERING,&caps,sizeof caps)==NVAPI_OK &&
            (caps&NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAP_STANDARD)!=0 && !GetEnvironmentVariableW(L"RTC_PATH_COMPUTE",nullptr,0);
        HRESULT hr=E_FAIL;
        if(ser) {
            D3D12_EXPORT_DESC exp={L"PathTraceRaygen",nullptr,D3D12_EXPORT_FLAG_NONE};
            const auto& code=rtc_shaders::kAll[variant==1?pPathTraceSplitRaygen:variant==2?pPathTraceSssRaygen:pPathTraceRaygen];
            D3D12_DXIL_LIBRARY_DESC lib{{code.code,code.size},1,&exp};
            D3D12_GLOBAL_ROOT_SIGNATURE root{g->root.Get()};
            D3D12_RAYTRACING_SHADER_CONFIG shader{0,8};D3D12_RAYTRACING_PIPELINE_CONFIG pipeline{1};
            D3D12_STATE_SUBOBJECT objects[]={
                {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,&lib},
                {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,&root},
                {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,&shader},
                {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,&pipeline}};
            D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,4,objects};
            hr=g->device->CreateStateObject(&desc,IID_PPV_ARGS(&g->pathSerState));
        }
        if(!g->pathSerState) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=g->root.Get();
            const auto& code=rtc_shaders::kAll[tracePipe];
            pd.CS={code.code,code.size};
            hr=g->device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&g->pipes[tracePipe]));
        }
        opt.flags=(NVAPI_D3D12_PIPELINE_CREATION_STATE_FLAGS)0;
        NvAPI_D3D12_SetNvShaderExtnSlotSpaceLocalThread(g->device.Get(),UINT32_MAX,0);
        NvAPI_D3D12_SetCreatePipelineStateOptions(g->device.Get(),&opt);
        if(FAILED(hr)){Error("path tracing pipeline 0x%08lx",(unsigned long)hr);return false;}
        if(g->pathSerState) {
            ComPtr<ID3D12StateObjectProperties> props;
            if(FAILED(g->pathSerState.As(&props)) || !props->GetShaderIdentifier(L"PathTraceRaygen"))return false;
            if(!Buffer(g->pathShaderTable,64,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ))return false;
            void *mapped=nullptr;if(FAILED(g->pathShaderTable.r->Map(0,nullptr,&mapped)))return false;
            memcpy(mapped,props->GetShaderIdentifier(L"PathTraceRaygen"),D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
            g->pathShaderTable.r->Unmap(0,nullptr);
        }
        if(g->pathNvNull==kNone)g->pathNvNull=StaticDescriptor();if(g->pathNvNull==kNone)return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC nullUav{};nullUav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
        nullUav.Buffer.NumElements=1;nullUav.Buffer.StructureByteStride=256;
        g->device->CreateUnorderedAccessView(nullptr,nullptr,&nullUav,Cpu(g->pathNvNull));
    }
    g->pathMaterialRecords.resize(g->materials.size());
    for(uint32_t i=0;i<g->materials.size();i++) {
        auto it=g->pathMaterials.find(i);
        PtMaterialGpu r;
        if(it!=g->pathMaterials.end())r=it->second.gpu;
        else {
            const MaterialDesc &m=g->materials[i].d;
            memcpy(r.base,m.averageColor,12);
            if(m.kind==MaterialKind::Emissive)memcpy(r.emission,m.averageColor,12);
        }
        g->pathMaterialRecords[i]=r;
    }
    uint32_t lightRanges=kNone;
    if(!PreparePathLighting(lightRanges))return false;
    struct Camera {
        float clipToWorld[16],worldToClip[16],camera[4],jitter[4];
        uint32_t width,height,samples,bounces;
        uint32_t cubes[4];
        float worldToSky[12],skyScale[4];
        uint32_t hair[4];
        float previousWorldToClip[16];
        uint32_t motion[4];
        uint32_t geometryMotion[4];
        uint32_t lighting[4];
        uint32_t signals[4];
        float backgroundToSky[12],backgroundColor[4];
        uint32_t path[4]; // diffuse bounce limit, test layers (SetTestLayers: the PT's bits 20-26), eye rigs (raw view, count)
        uint32_t authorRules; float authorUnused,authorAmbient,authorContact; // API 26, path_trace.hlsl PtAuthor
        float authorAmbientRange,authorEmitter,authorPad[2];
        uint32_t probe[4]; // API 27: the probe's view
        float probeDecode[4],probeMin[4],probeMax[4],probeCenter[4];
        uint32_t scatter[4]; // 11-10, PT_SSS: the skin light's UAV; the image plane's axis (floats)
    } camera{};
    static_assert(sizeof(Camera)==608,"path camera layout");
    memcpy(camera.clipToWorld,v.clipToWorld,64); memcpy(camera.worldToClip,v.worldToClip,64);
    memcpy(camera.camera,v.camera,12); camera.camera[3]=v.exposure*g->pathExposure;
    memcpy(camera.jitter,v.jitter,8);
    camera.jitter[2]=std::clamp(v.textureLodBias,-4.f,0.f);
    camera.width=v.width; camera.height=v.height; camera.samples=v.samples; camera.bounces=v.maxBounces;
    camera.path[0]=g->pathEnvironment.diffuseBounces;
    camera.authorRules=g->pathEnvironment.authorRules;
    camera.authorAmbient=g->pathEnvironment.authorAmbient;camera.authorContact=g->pathEnvironment.authorContact;
    camera.authorAmbientRange=g->pathEnvironment.authorAmbientRange;camera.authorEmitter=g->pathEnvironment.authorEmitter;
    camera.authorPad[0]=g->pathEnvironment.backgroundNear;camera.authorPad[1]=g->pathEnvironment.backgroundFar;
    camera.path[1]=g->layers;
    camera.path[2]=kNone;camera.path[3]=0;
    if(g->pathEyeRigs) {
        camera.path[2]=FrameDescriptor();if(camera.path[2]==kNone)return false;
        RawSrv(camera.path[2],g->pathEyeRigs,0,(uint64_t)g->pathEyeRigCount*kPathEyeRigBytes);camera.path[3]=g->pathEyeRigCount;
    }
    camera.probe[0]=g->pathCubeSrv[3];memcpy(camera.probeDecode,g->pathEnvironment.probeDecode,64);
    camera.cubes[0]=g->pathCubeSrv[0];camera.cubes[1]=g->pathCubeSrv[1];camera.cubes[2]=g->pathEnvironment.rgbm;camera.cubes[3]=g->pathCubeSrv[2];
    memcpy(camera.worldToSky,g->pathEnvironment.worldToSky,48);
    camera.skyScale[0]=g->pathEnvironment.lightingScale;camera.skyScale[1]=g->pathEnvironment.backgroundScale;
    camera.skyScale[2]=g->pathEnvironment.diffuseScale;camera.skyScale[3]=g->pathEnvironment.indoorScale;
    memcpy(camera.backgroundToSky,g->pathEnvironment.backgroundToSky,48);
    memcpy(camera.backgroundColor,g->pathEnvironment.backgroundColor,12);
    camera.backgroundColor[3]=(float)(v.eye&1u); // per-eye selection for stereo panoramas
    camera.hair[0]=g->pathHair.active?g->pathHair.count:0;
    camera.hair[1]=g->pathHair.vertexSrv;camera.hair[2]=g->pathHair.recordSrv;
    // Per-pixel light visibility (test bit 31 cleared: off, as 0.23.9). Made (all slots "visible") at a size change.
    camera.hair[3]=kNone;bool visFresh=false;const uint32_t visEye=v.eye&1;
    if(g->layers&(1u<<31)) {
        if(g->pathVisSize[visEye][0]!=v.width || g->pathVisSize[visEye][1]!=v.height || g->pathVisUav[visEye]==kNone) {
            BuryRes(g->pathVis[visEye]);FreeDescriptor(g->pathVisUav[visEye]);g->pathVisUav[visEye]=kNone;
            const uint64_t bytes=uint64_t(v.width)*v.height*12;
            if(Buffer(g->pathVis[visEye],bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON)) {
                g->pathVisUav[visEye]=StaticDescriptor();
                if(g->pathVisUav[visEye]!=kNone){RawUav(g->pathVisUav[visEye],g->pathVis[visEye].Get(),bytes);visFresh=true;
                    g->pathVisSize[visEye][0]=v.width;g->pathVisSize[visEye][1]=v.height;}
            }
        }
        camera.hair[3]=g->pathVisUav[visEye];
    }
    camera.scatter[0]=kNone;
    if(variant==2) {
        for(Pipe p:{pPathScatterX,pPathScatterY})if(!g->pipes[p]) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=g->root.Get();
            pd.CS={rtc_shaders::kAll[p].code,rtc_shaders::kAll[p].size};
            if(FAILED(g->device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&g->pipes[p]))))return false;
        }
        auto &size=g->pathScatterSize[visEye];auto &uav=g->pathScatterUav[visEye];
        if(size[0]!=v.width || size[1]!=v.height || uav[0]==kNone || uav[1]==kNone) {
            size[0]=size[1]=0;
            for(int k=0;k<2;k++) {
                BuryRes(g->pathScatter[visEye][k]);FreeDescriptor(uav[k]);
                if(!Texture(g->pathScatter[visEye][k],D3D12_RESOURCE_DIMENSION_TEXTURE2D,DXGI_FORMAT_R16G16B16A16_FLOAT,v.width,v.height,1,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS))return false;
                uav[k]=StaticDescriptor();if(uav[k]==kNone)return false;
                TexUav(uav[k],g->pathScatter[visEye][k].Get(),D3D12_UAV_DIMENSION_TEXTURE2D,DXGI_FORMAT_R16G16B16A16_FLOAT);
            }
            size[0]=v.width;size[1]=v.height;
        }
        camera.scatter[0]=uav[0];memcpy(&camera.scatter[1],scatterForward,12);
    }
    memcpy(camera.previousWorldToClip,v.previousWorldToClip,64);
    camera.motion[0]=kNone;camera.motion[1]=g->pathHair.previousValid?1u:0u;
    camera.geometryMotion[0]=kNone;
    camera.geometryMotion[2]=kNone;
    camera.geometryMotion[3]=g->pathLighting.srv[0];
    camera.lighting[0]=lightRanges;camera.lighting[1]=(uint32_t)g->pathLighting.ranges.size();
    camera.lighting[2]=g->pathLighting.srv[1];camera.lighting[3]=g->pathLighting.triangles;
    camera.signals[0]=camera.signals[1]=camera.signals[2]=kNone;
    if(response && response->mask) {
        camera.signals[2]=FrameDescriptor();if(camera.signals[2]==kNone)return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=DXGI_FORMAT_R16_FLOAT;u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        g->device->CreateUnorderedAccessView(response->mask,nullptr,&u,Cpu(camera.signals[2]));
        memcpy(&camera.signals[3],&response->strength,sizeof(float));
        memcpy(&camera.motion[2],response->motionScale,2*sizeof(float));
    }
    ID3D12Resource *signals[]={v.diffuseRadiance,v.specularRadiance};
    if(split)for(int k=0;k<2;k++) {
        const auto d=signals[k]->GetDesc();
        if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width!=v.width || d.Height!=v.height ||
            d.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT || !(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))return false;
        camera.signals[k]=FrameDescriptor();if(camera.signals[k]==kNone)return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=d.Format;u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        g->device->CreateUnorderedAccessView(signals[k],nullptr,&u,Cpu(camera.signals[k]));
    }
    if(v.detailReference) {
        const auto d=v.detailReference->GetDesc();
        if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width!=v.width || d.Height!=v.height ||
           d.DepthOrArraySize!=1 || d.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT || !(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))return false;
        camera.geometryMotion[2]=FrameDescriptor();if(camera.geometryMotion[2]==kNone)return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=d.Format;u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        g->device->CreateUnorderedAccessView(v.detailReference,nullptr,&u,Cpu(camera.geometryMotion[2]));
    }
    if(!g->pathMotionRecords.empty()) {
        Alloc records;const uint64_t bytes=g->pathMotionRecords.size()*sizeof(Core::PathMotionGpu);
        if(!Upload(bytes,256,records))return false;
        memcpy(records.cpu,g->pathMotionRecords.data(),(size_t)bytes);
        camera.geometryMotion[0]=FrameDescriptor();camera.geometryMotion[1]=(uint32_t)g->pathMotionRecords.size();
        if(camera.geometryMotion[0]==kNone)return false;
        RawSrv(camera.geometryMotion[0],g->upload.Get(),records.offset,bytes);
    }
    if(v.motion) {
        const auto d=v.motion->GetDesc();
        if(d.Width!=v.width || d.Height!=v.height || d.Format!=DXGI_FORMAT_R32G32_FLOAT || !(d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))return false;
        camera.motion[0]=FrameDescriptor();if(camera.motion[0]==kNone)return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=d.Format;u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        g->device->CreateUnorderedAccessView(v.motion,nullptr,&u,Cpu(camera.motion[0]));
    }
    Alloc ca,ma,fa,la;
    const uint64_t materialBytes=std::max<size_t>(1,g->pathMaterialRecords.size())*sizeof(PtMaterialGpu);
    const uint64_t lightBytes=std::max<size_t>(1,g->pathLights.size())*sizeof(LightGpu);
    if(!Upload(sizeof camera,256,ca) || !Upload(materialBytes,256,ma) || !Upload(sizeof(FrameCb),256,fa) || !Upload(lightBytes,256,la))return false;
    memcpy(ca.cpu,&camera,sizeof camera);
    memcpy(ma.cpu,g->pathMaterialRecords.data(),g->pathMaterialRecords.size()*sizeof(PtMaterialGpu));
    memcpy(fa.cpu,&g->pathFrame,sizeof(FrameCb));
    PassCb pass{}; pass.seed=v.seed+v.eye*0x68e31da4u;
    pass.light=FrameDescriptor();if(pass.light==kNone)return false;
    // Standalone callers may use Record without the geometry-only switch.
    if(!g->pathLights.empty())memcpy(la.cpu,g->pathLights.data(),g->pathLights.size()*sizeof(LightGpu));
    pass.count=(uint32_t)g->pathLights.size();
    RawSrv(pass.light,g->upload.Get(),la.offset,lightBytes);
    uint32_t *descriptors[]={&pass.a,&pass.b,&pass.c,&pass.d,&pass.e,&pass.f,&pass.g,&pass.h};
    for(auto *d:descriptors) { *d=FrameDescriptor(); if(*d==kNone)return false; }
    RawSrv(pass.a,g->upload.Get(),ca.offset,sizeof camera);
    RawSrv(pass.b,g->upload.Get(),ma.offset,materialBytes);
    for(int k=0;k<6;k++) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format=outputs[k]->GetDesc().Format; u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        g->device->CreateUnorderedAccessView(outputs[k],nullptr,&u,Cpu(*descriptors[k+2]));
    }
    PassCb scatter{};
    if(variant==2) {
        Alloc ta;const PathScatterTable &table=ScatterTable();
        if(!Upload(sizeof table,256,ta))return false;
        memcpy(ta.cpu,&table,sizeof table);
        scatter.a=g->pathScatterUav[visEye][0];scatter.b=g->pathScatterUav[visEye][1];scatter.c=pass.c;scatter.d=pass.d;
        scatter.e=FrameDescriptor();if(scatter.e==kNone)return false;
        RawSrv(scatter.e,g->upload.Get(),ta.offset,sizeof table);
        memcpy(&scatter.f,&scatterUnit,4);memcpy(&scatter.g,&scatterPixel,4);scatter.h=v.width|(v.height<<16);
    }
    ID3D12DescriptorHeap *heaps[]={g->heap.Get()}; l->SetDescriptorHeaps(1,heaps);
    l->SetComputeRootSignature(g->root.Get());
    l->SetComputeRootConstantBufferView(1,fa.gpu);
    l->SetComputeRootShaderResourceView(2,g->tlas.Va());
    l->SetComputeRootShaderResourceView(5,g->pathHair.active?g->pathHair.tlas.Va():g->tlas.Va());
    auto nv=g->heap->GetGPUDescriptorHandleForHeapStart();nv.ptr+=uint64_t(g->pathNvNull)*g->descriptorSize;
    l->SetComputeRootDescriptorTable(6,nv);
    auto table=g->heap->GetGPUDescriptorHandleForHeapStart();
    l->SetComputeRootDescriptorTable(3,table); l->SetComputeRootDescriptorTable(4,table);
    if(visFresh) {
        PassCb c{};c.a=g->pathVisUav[visEye];c.b=0xffffffffu;c.c=0;c.count=v.width*v.height*3;
        Dispatch(l,pClear,c,c.count);
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;l->ResourceBarrier(1,&b);
    }
    RecordPathLighting(l,pass);
    if(g->pathSerState) {
        l->SetPipelineState1(g->pathSerState.Get());l->SetComputeRoot32BitConstants(0,12,&pass,0);
        D3D12_DISPATCH_RAYS_DESC rays{};
        rays.RayGenerationShaderRecord={g->pathShaderTable.Va(),D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES};
        rays.Width=v.width;rays.Height=v.height;rays.Depth=1;l->DispatchRays(&rays);
    } else Dispatch2D(l,tracePipe,pass,v.width,v.height);
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
    l->ResourceBarrier(1,&barrier);
    if(variant==2) {
        Dispatch2D(l,pPathScatterX,scatter,v.width,v.height);l->ResourceBarrier(1,&barrier);
        Dispatch2D(l,pPathScatterY,scatter,v.width,v.height);l->ResourceBarrier(1,&barrier);
    }
    return true;
}

bool PathUsesSer() { return g && g->pathSerState; }

}  // namespace rtc
