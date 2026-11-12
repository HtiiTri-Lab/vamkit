// RtCore on VaM's D3D11On12 device (see rt.h). C++ COM (not CINTERFACE), D3D12 + 11on12 + RtCore.
//
// Per frame while the PT renders (RtFrame, the first camera's PreCull, render thread):
//  1. The plugin's draws grouped by owner (a skin, a clothing item, a MeshRenderer); every material seen gets an RtCore
//     material and its PT material.
//  2. D3D11: the copies VaM's buffers -> D3D12 buffers wrapped for D3D11 (skins and clothing every frame, meshes and
//     index buffers once), Release + Flush.
//  3. D3D12, the host's list on the 11on12 queue (in order with the frame, no fence wait): RtCore's Record, the copied
//     buffers back to COMMON (D3D11 takes them again), ExecuteCommandLists, Signal.
// Wrapped copies: InState COMMON, OutState NON_PIXEL_SHADER_RESOURCE (RtCore's inputs). A device failure pauses the PT
// (status last_error): the frames are VaM's own raster until it comes back.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../../rt-core/include/startup_display.h"
#include <d3d11_1.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <psapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <climits>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "rt.h"
#include "../../rt-core/include/rtcore.h"
#include "../../rt-core/include/path_tracing.h"

using Microsoft::WRL::ComPtr;

namespace {
std::atomic<bool> gPathSelected{false};
std::atomic<bool> gPathCollect{false};
std::atomic<bool> gPathRendering{false};
std::atomic<bool> gPathConsumerReady{false};
bool gPathFrameInvalid=false; // render thread; current stereo must not be published
bool gPathCleanupPending=false;
// Render thread. The PT textures stay unwrapped from the left eye's record to the right eye's finish: one
// unwrap / return per frame (each one is a 11on12 sync sequence on the queue, ~10 us of the render thread).
int gPathRecordedEye=-1;       // eye of the last successful record, -1 after a failed one
bool gPathFinishDeferred=false; // the left eye's finish kept the textures
// History breaks for RR/NRD/NR (managed: scene loading). Material and mesh
// discontinuities are content edits the reconstructors resolve per pixel.
std::atomic<uint64_t> gSceneRevision{0};
uint64_t gContentRevision=0; // diagnostic only
uint64_t gManagedSceneRevision=0;
using ClothPathFn=int(__cdecl*)(void*,UINT,float*);
ClothPathFn gClothPath=nullptr;

const unsigned kSlots = 4;          // command lists in flight
const int kEvictFrames = 120;       // an owner / index copy / material / hair not drawn for this long is released

struct Wrapped {
    ComPtr<ID3D12Resource> r12;
    ComPtr<ID3D11Resource> r11;
    UINT64 bytes = 0;
    bool acquired = false;  // by D3D11 (created released)
};

struct IndexCopy {
    ComPtr<ID3D11Buffer> source;
    Wrapped copy;
    bool ready = false;  // copied and handed to D3D12 (stays there, NON_PIXEL_SHADER_RESOURCE)
    int lastSeen = 0;
};
struct PathRasterDraw { uint32_t first,count;int32_t base;int kind;uint32_t programSet;uint64_t watched=0; }; // watched: 0.23.4, the material key of an RT_DRAW_WATCH draw
#include "path_program_sets.h"
bool PathProgramInSet(uint32_t set,uint64_t program) {
    if(!set||set>=std::size(kPathShaderProgramRanges))return false;
    const auto& range=kPathShaderProgramRanges[set];
    const auto* begin=kPathShaderPrograms+range[0];
    return std::binary_search(begin,begin+range[1],program);
}


struct Material {
    uint32_t id = 0;  // RtCore's
    uint64_t revision=0;
    rtc::MaterialDesc desc;
    ComPtr<ID3D11Resource> texture, alphaTexture;  // retain source identity until material eviction; no duplicate textures
    float color[3] = {1, 1, 1};
    uint64_t textureSerial=0;
    uint32_t textureVersion=0,alphaVersion=0;
    float alphaAdjust = 0;   // VaM's _AlphaAdjust (separate alpha maps)
    int lastSeen = 0;
    ComPtr<ID3D11Resource> pathSource[9]; // rtc::PathMaterial's eight maps, then its second colour layer
    rtc::PathMaterial path;
    RtSurfaceRole role=RtSurfaceRole::Generic;
    bool pathBound = false;
    bool pathSourcesReady = false;
};
struct PathTexture {
    ComPtr<ID3D11Resource> source;
    ComPtr<ID3D12Resource> underlying;
    bool taken = false;
    int lastSeen = 0;
    // path_textures.inl: our own D3D12 copy, bound instead of the borrowed original while it is up to date
    ComPtr<ID3D12Resource> copy;
    uint64_t copiedWrites = 0, bytes = 0;
    int kind = 0;  // 0 not looked at, 1 copied, 2 borrowed every frame
    bool watched = false;
};

struct Sub {
    ID3D11Buffer *indices;
    uint32_t start, count;
    int32_t base;
    bool i32;
    uint32_t material;  // RtCore's
    float tessAlpha;
    bool noOcclude;  // RT_DRAW_NO_OCCLUDE: its own geometry (kNoOcclude is per geometry)
    uint32_t primaryCull;
    bool cameraHidden;  // RT_DRAW_CAMERA_HIDDEN: its own geometry (kCameraHidden is per geometry)
    RtSurfaceRole role; // the PT skin entries can share a continuous surface
};
static bool PathSmoothSkin(RtSurfaceRole role) {
    return role==RtSurfaceRole::Skin || role==RtSurfaceRole::Lips;
}
static float PathSkinPhong(const std::vector<Sub>& subs) {
    float alpha=0;
    for(const Sub& sub:subs)if(PathSmoothSkin(sub.role))alpha=std::max(alpha,sub.tessAlpha);
    return alpha;
}
static float PathSurfacePhong(const Sub& sub,bool path,float skinPhong) {
    return path && PathSmoothSkin(sub.role) && skinPhong>0 ? skinPhong : sub.tessAlpha;
}
#ifdef RT_HOST_TEST
static bool TestSkinPhongSelection() {
    Sub shoulder{},arm{},face{},lip{},eye{},nail{};
    shoulder.role=arm.role=face.role=RtSurfaceRole::Skin;lip.role=RtSurfaceRole::Lips;
    eye.role=RtSurfaceRole::Eye;nail.role=RtSurfaceRole::Nails;
    shoulder.tessAlpha=.5f;eye.tessAlpha=.8f;nail.tessAlpha=.25f;
    std::vector<Sub> subs{shoulder,arm,face,lip,eye,nail};float shared=PathSkinPhong(subs);
    return shared==.5f && PathSurfacePhong(arm,true,shared)==.5f && PathSurfacePhong(lip,true,shared)==.5f &&
        PathSurfacePhong(eye,true,shared)==.8f && PathSurfacePhong(nail,true,shared)==.25f &&
        PathSurfacePhong(arm,false,shared)==0 && PathSurfacePhong(arm,true,0)==0;
}
#endif

struct Owner {
    uint64_t id = 0;
    uint32_t flags = 0;
    uint32_t lightLink = 0;  // RtDraw::lightLink (a renderer has one layer)
    ComPtr<ID3D11Buffer> vertexSource, normalSource, weightSource, tangentSource;
    uint32_t tangentOffset=0,tangentStride=0,tangentFormat=0;
    uint32_t meshRevision=0;
    ID3D11Buffer *uvSource = nullptr;  // UV0's vertex buffer (the vertex source itself, or a static copy in g.indices)
    uint32_t uvOffset = 0, uvStride = 0, pathNormalOffset=UINT32_MAX;
    rtc::VertexFormat uvFormat = rtc::VertexFormat::Float2;
    UINT64 uvBytes = 0;     // status: the UV buffer's size and the stride the rule found (0: none)
    uint32_t uvFound = 0;
    bool alpha = false;     // status: a Cutout / Translucent material among this frame's submeshes
    uint32_t stride = 0, vertexCount = 0;
    UINT64 sourceBytes = 0;
    Wrapped vertices, normals, weights, tangents;
    std::vector<float> boneMatrices;
    uint32_t boneCount=0,blendFrameCount=0;
    bool verticesReady = false;  // static owners: copied once
    std::vector<Sub> subs;
    RtReceiver *receiver = nullptr;
    float matrix[12] = {};
    int lastSeen = 0;
    bool Dynamic() const { return (flags & (RT_DRAW_SKIN | RT_DRAW_WRAP | RT_DRAW_DYNAMIC_MESH)) != 0; }
};

struct Slot {
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList4> list;
    UINT64 done = 0;
    bool timed = false;
};

struct Grave {
    UINT64 value;
    ComPtr<IUnknown> object;
};

}  // namespace
#ifdef RT_HOST_TEST
bool RtTestSkinPhongSelection() { return TestSkinPhongSelection(); }
#endif

struct RtBake {
    uint32_t triangleCount = 0;
    std::map<uint32_t, uint32_t> tessLevel;  // per submesh first index: the occluder copy's subdivision (mean edge / 5 mm)
    uint32_t vertexCount = 0;
    char summary[256] = "";
};

struct RtReceiver {
    uint64_t id = 0;
    void *key = nullptr;  // the mesh's index buffer
    int character = 0;
    uint32_t vertexCount = 0, triangleCount = 0;
    std::map<uint32_t, uint32_t> tessLevel;
    bool claimed = false;          // this frame, by a skin
};

namespace {

struct Host {
    bool tried = false, ok = false;
    int lastFrame = INT_MIN;
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11On12Device> on12;
    ComPtr<ID3D11On12Device2> pathOn12;
    ComPtr<ID3D12Fence> pathFence;
    uint64_t pathFenceValue=0, pathCalls=0;
    float pathLastJitter[2]={};
    bool pathRequested=false, pathThisFrame=false;
    std::unordered_map<ID3D11Resource *,PathTexture> pathTextures;
    ComPtr<ID3D11Resource> pathEnvironment[4];
    rtc::PathEnvironment pathSky;
    Wrapped pathHairCopy;
    int pathHairCopiedFrame=INT_MIN;
    uint64_t pathHairBuildBytes=0;
    Wrapped pathEyeCopy;      // 0.22.71: the characters' eye rigs PT reads (RecordPathEyes)
    uint32_t pathEyeCount=0;  // of them this frame
    std::vector<PathHairBuildItem> pathHairRegistry;
    std::vector<uint32_t> pathHairLinks;  // RtHairRec::lightLink, parallel to the registry
    PathHairItem pathHairDiagnostic[64]{};
    uint32_t pathHairDiagnosticCount=0;
    std::vector<ComPtr<ID3D11Resource>> pathHairSources;
    std::unordered_map<ID3D11Buffer*,std::vector<PathRasterDraw>> pathRasterBuffers;
    ComPtr<ID3D12Device5> d12;
    ComPtr<ID3D12CommandQueue> queue;
    // Named to UnwrapUnderlyingResource instead of `queue` until the proxy's probe has shown that this d3d11on12.dll
    // does not stop on its own queue (PathUnwrapQueue): on Windows 10 and older Windows 11 an unwrap onto 11on12's own
    // queue puts a Wait ahead of its Signal there and that queue stops for good (microsoft/D3D11On12 #51; the
    // compositor's VamOn12UnwrapQueue says more). It runs no work; the PT still runs on `queue`.
    ComPtr<ID3D12CommandQueue> unwrapQueue;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 fenceValue = 0;  // = RtCore's frame of the last Record
    UINT64 timestampFrequency = 0;
    Slot slots[kSlots];
    unsigned next = 0;
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> readback;
    const UINT64 *ticks = nullptr;
    double tickMs = 0, qpcMs = 0;
    std::unordered_map<uint64_t, Owner> owners;
    std::unordered_map<ID3D11Buffer *, IndexCopy> indices;  // static copies: index buffers, and skins' / clothing's UV vertex buffers
    std::unordered_map<uint64_t, Material> materials;
    std::unordered_map<void *, RtReceiver *> receivers;  // by mesh index buffer
    std::vector<RtReceiver *> allReceivers;
    uint64_t nextReceiverId = 1;
    bool reconstruction = false;
    uint32_t pathBounces = 0;  // RtSetPathBounces
    uint32_t authorRules = 0; float authorAmbient = 0, authorContact = .6f, authorAmbientRange = 1, authorEmitter = 1, backgroundNear = 0, backgroundFar = 0;  // RtSetPathAuthor
    uint32_t defaultMaterial = 0;
    std::vector<Grave> graves;
    std::vector<RtReceiver *> doomed;  // RtDestroyReceiver, any thread (under gLock)
    // per frame scratch
    std::vector<rtc::Geometry> geometries;
    std::vector<rtc::Submesh> submeshes;
    std::vector<rtc::Light> rtLights;
    RtHostStatus status;  // under gLock
    double cpuSum = 0, waitSum = 0, gpuSum = 0;
    int cpuSamples = 0;
} g;
std::mutex gLock;  // g.doomed, g.status

double NowMs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * g.qpcMs;
}

void SetReason(const char *fmt, ...) {
    char text[160];
    va_list a;
    va_start(a, fmt);
    vsnprintf(text, sizeof text, fmt, a);
    va_end(a);
    std::lock_guard<std::mutex> l(gLock);
    memcpy(g.status.reason, text, sizeof text);
}

// PT stops now (a failure, a pause, 光追 off): the draws fall back to raster and the hair plugin draws its strands.
void StopPathRendering() {
    gPathCleanupPending|=g.pathThisFrame;
    g.pathThisFrame=false;gPathRendering=false;gPathCollect=false;gPathConsumerReady=false;
    auto hair=GetModuleHandleW(L"VaMHairOnePassNative.bin");
    auto mode=hair?reinterpret_cast<void(__cdecl*)(int)>(GetProcAddress(hair,"HOP_PathModeV1")):nullptr;
    if(mode)mode(0);
}
void Fail(const char *what, HRESULT hr) {
    StopPathRendering();
    char text[160];
    snprintf(text, sizeof text, "%s: HRESULT 0x%08lx", what, (unsigned long)hr);
    std::lock_guard<std::mutex> l(gLock);
    memcpy(g.status.lastError, text, sizeof text);
    g.status.failures++;
    g.status.available = 0;
    g.ok = false;
}
// 0.22.62: one-time notes for the log (the plugin drains them, ST_PathNoticeV1): what PT adapted, and failures.
std::mutex gNoticeLock;
std::vector<std::string> gNotices;
std::map<std::string,bool> gNoticeSeen;
void PathNotice(const char *fmt,...) {
    char text[200];va_list a;va_start(a,fmt);vsnprintf(text,sizeof text,fmt,a);va_end(a);
    std::lock_guard<std::mutex> l(gNoticeLock);
    if(gNoticeSeen.size()<4096 && gNoticeSeen.emplace(text,true).second && gNotices.size()<64)gNotices.push_back(text);
}
// 0.22.62: a device failure pauses PT and retries (2, 4, ... 64 s at 30 fps when it repeats); the selection is
// the user's and is never cleared here. Nothing about one object pauses PT.
int gPathSuspendUntil=INT_MIN,gPathFailStreak=0,gPathLastFailure=INT_MIN;
void SuspendPath(const char *reason) {
    const int frame=g.lastFrame;
    gPathFailStreak=frame-gPathLastFailure<2000?std::min(gPathFailStreak+1,5):0;
    gPathLastFailure=frame;gPathSuspendUntil=frame+(60<<gPathFailStreak);
    PathNotice("PT paused for %d frames: %s",60<<gPathFailStreak,reason);
}
void PathFail(const char *what,HRESULT hr) {
    char text[160];snprintf(text,sizeof text,"%s: HRESULT 0x%08lx",what,(unsigned long)hr);
    SuspendPath(text);StopPathRendering();
    std::lock_guard<std::mutex> l(gLock);
    snprintf(g.status.lastError,sizeof g.status.lastError,"%s; existing lighting until the retry",text);
    g.status.failures++;
}
void InvalidatePathFrame(const char *reason) {
    gPathFrameInvalid=true;SuspendPath(reason);
    std::lock_guard<std::mutex> lock(gLock);
    snprintf(g.status.lastError,sizeof g.status.lastError,"%s; previous stereo frame retained, PT retries",reason);
    g.status.failures++;
}

HMODULE ProxyModule() {
    HMODULE modules[1024];
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) return nullptr;
    for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 1024; ++i)
        if (GetProcAddress(modules[i], "D3DInterop_GetOn12ForDevice")) return modules[i];
    return nullptr;
}

ComPtr<ID3D12Resource> Buffer12(UINT64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = std::max<UINT64>((bytes + 3) & ~3ull, 4);  // exact: a wrapped structured buffer is a whole number of elements
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    HRESULT hr = g.d12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r));
    if (FAILED(hr)) { Fail("CreateCommittedResource", hr); return nullptr; }
    return r;
}

// A D3D12 buffer D3D11 also sees, created released (D3D12 owns it, COMMON). like: the D3D11 buffer it is a copy of (its
// size, structured or not and stride).
bool WrapCopy(Wrapped &w, ID3D11Buffer *like) {
    D3D11_BUFFER_DESC d = {};
    like->GetDesc(&d);
    D3D11_RESOURCE_FLAGS f = {};
    f.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    f.MiscFlags = d.MiscFlags & (D3D11_RESOURCE_MISC_BUFFER_STRUCTURED | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS);
    f.StructureByteStride = (f.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) ? d.StructureByteStride : 0;
    w.r12 = Buffer12(d.ByteWidth, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    if (!w.r12) return false;
    HRESULT hr = g.on12->CreateWrappedResource(w.r12.Get(), &f, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                               IID_PPV_ARGS(&w.r11));
    if (FAILED(hr)) { Fail("CreateWrappedResource (copy)", hr); w.r12.Reset(); return false; }
    w.bytes = d.ByteWidth;
    w.acquired = false;
    return true;
}

void Bury(ComPtr<IUnknown> o) {
    if (o) g.graves.push_back({g.fenceValue + 1, o});  // freed once the next submission has finished
}
void BuryWrapped(Wrapped &w) {
    Bury(w.r11);
    Bury(w.r12);
    w = Wrapped();
}

void Collect() {
    const UINT64 done = g.fence ? g.fence->GetCompletedValue() : 0;
    if (done == UINT64_MAX) return;
    g.graves.erase(std::remove_if(g.graves.begin(), g.graves.end(), [done](const Grave &x) { return x.value <= done; }), g.graves.end());
}

bool WaitCpu(UINT64 value) {
    if (!value) return true;
    const UINT64 completed = g.fence->GetCompletedValue();
    if (completed == UINT64_MAX) { Fail("device removed", DXGI_ERROR_DEVICE_REMOVED); return false; }
    if (completed >= value) return true;
    if (FAILED(g.fence->SetEventOnCompletion(value, g.event)) || WaitForSingleObject(g.event, 5000) != WAIT_OBJECT_0) {
        Fail("fence wait", E_FAIL);
        return false;
    }
    return true;
}

void Barrier(std::vector<D3D12_RESOURCE_BARRIER> &b, ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to,
             UINT sub = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
    D3D12_RESOURCE_BARRIER x = {};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = sub;
    x.Transition.StateBefore = from;
    x.Transition.StateAfter = to;
    b.push_back(x);
}

UINT64 BufferBytes(ID3D11Buffer *b) {
    D3D11_BUFFER_DESC d = {};
    b->GetDesc(&d);
    return d.ByteWidth;
}

void DestroyReceiverNow(RtReceiver *r) {
    for (auto &kv : g.owners)
        if (kv.second.receiver == r) kv.second.receiver = nullptr;
    auto it = g.receivers.find(r->key);
    if (it != g.receivers.end() && it->second == r) g.receivers.erase(it);
    g.allReceivers.erase(std::remove(g.allReceivers.begin(), g.allReceivers.end(), r), g.allReceivers.end());
    delete r;
}

void DropOwner(Owner &o) {
    BuryWrapped(o.vertices);
    BuryWrapped(o.normals);
    Bury(o.vertexSource);
    Bury(o.normalSource);
    BuryWrapped(o.tangents);Bury(o.tangentSource);
    BuryWrapped(o.weights);Bury(o.weightSource);o.boneMatrices.clear();
}

void ReadTiming(Slot &s, unsigned index) {
    if (!s.timed || !g.ticks) return;
    s.timed = false;
    const UINT64 *t = g.ticks + 2 * index;
    if (t[1] <= t[0]) return;
    const double ms = (double)(t[1] - t[0]) * g.tickMs;
    std::lock_guard<std::mutex> l(gLock);
    g.status.samples++;
    g.gpuSum += ms;
    g.status.gpuMs = g.gpuSum / g.status.samples;
}

}  // namespace

// ---- receivers ------------------------------------------------------------------------------------------------------
void RtInvalidatePathFrame(const char *reason) { InvalidatePathFrame(reason); }

RtBake *RtBakeReceiver(const float *pos, const float *uv, int vertexCount, const int *indices, int subCount, const int *subStart,
                       const int *subCount3, const int *gpuStart, const uint8_t *parts, std::string &error) {
    if (!pos || !uv || vertexCount <= 0 || !indices || subCount <= 0) { error = "rt bake: empty input"; return nullptr; }
    // the mesh's triangles in the index buffer's order: triangle t = gpu index / 3
    uint64_t total = 0;
    for (int s = 0; s < subCount; s++) total = std::max<uint64_t>(total, (uint64_t)gpuStart[s] + (uint64_t)subCount3[s]);
    const uint32_t T = (uint32_t)((total + 2) / 3);
    if (!T) { error = "rt bake: no triangles"; return nullptr; }
    std::unique_ptr<RtBake> b(new RtBake);
    b->vertexCount = (uint32_t)vertexCount;
    for (int s = 0; s < subCount; s++) {
        if (gpuStart[s] % 3 || subCount3[s] % 3) continue;  // not a triangle list's start: VaM's meshes never
        const uint32_t n = (uint32_t)subCount3[s] / 3;
        double edges = 0;
        for (uint32_t t = 0; t < n; t++) {
            const int *src = indices + subStart[s] + 3 * t;
            if (src[0] < 0 || src[1] < 0 || src[2] < 0 || src[0] >= vertexCount || src[1] >= vertexCount || src[2] >= vertexCount) continue;
            for (int k = 0; k < 3; k++) {
                const float *a = pos + 3ll * src[k], *c = pos + 3ll * src[(k + 1) % 3];
                edges += std::sqrt((double)(a[0] - c[0]) * (a[0] - c[0]) + (double)(a[1] - c[1]) * (a[1] - c[1]) + (double)(a[2] - c[2]) * (a[2] - c[2]));
            }
        }
        const double mean = n ? edges / (3.0 * n) : 0.0;
        b->tessLevel[(uint32_t)gpuStart[s]] = (uint32_t)std::min(8.0, std::max(1.0, std::floor(mean / 0.005 + 0.5)));
    }
    b->triangleCount = T;
    snprintf(b->summary, sizeof b->summary, "PT geometry metadata only, %u triangles", T);
    return b.release();
}

void RtFreeBake(RtBake *b) { delete b; }
const char *RtBakeSummary(const RtBake *b) { return b ? b->summary : ""; }

RtReceiver *RtCreateReceiver(RtBake *bake, void *indexBuffer, int character) {
    std::unique_ptr<RtBake> b(bake);
    if (!g.ok || !b || !indexBuffer) return nullptr;
    RtReceiver *r = new RtReceiver;
    r->id = g.nextReceiverId++;
    r->key = indexBuffer;
    r->character = character;
    r->vertexCount = b->vertexCount;
    r->triangleCount = b->triangleCount;
    r->tessLevel = b->tessLevel;
    {
        std::lock_guard<std::mutex> l(gLock);
        snprintf(g.status.bakes, sizeof g.status.bakes, "%s", b->summary);
    }
    auto old = g.receivers.find(indexBuffer);
    if (old != g.receivers.end()) old->second->key = nullptr;  // the newer one takes the mesh (the older one goes with its entries)
    g.receivers[indexBuffer] = r;
    g.allReceivers.push_back(r);
    return r;
}

void RtDestroyReceiver(RtReceiver *r) {
    if (!r) return;
    std::lock_guard<std::mutex> l(gLock);
    g.doomed.push_back(r);
}

// ---- init -----------------------------------------------------------------------------------------------------------

bool RtInit(ID3D11Device *device) {
    if (g.tried) return g.ok;
    g.tried = true;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g.qpcMs = 1000.0 / (double)f.QuadPart;
    if (!device) { SetReason("no device"); return false; }
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&g.on12)))) {
        SetReason("Unity's device is real D3D11 (no D3DInterop d3d11.dll, or D3DINTEROP_MODE is not on12)");
        return false;
    }
    HMODULE proxy = ProxyModule();
    using GetForDevice = HRESULT(WINAPI *)(ID3D11Device *, ID3D12Device **, ID3D12CommandQueue **);
    auto get = proxy ? reinterpret_cast<GetForDevice>(GetProcAddress(proxy, "D3DInterop_GetOn12ForDevice")) : nullptr;
    ComPtr<ID3D12Device> d;
    if (!get || FAILED(get(device, &d, &g.queue)) || !d || !g.queue) { SetReason("11on12 device without the D3DInterop proxy's queue"); return false; }
    if (FAILED(d.As(&g.d12))) { SetReason("no ID3D12Device5"); return false; }
    // The PT bridge is made whenever the device can trace; the plugin selects the renderer while playing
    // (ST_SetPathTracingV1, [RayTracing] Renderer). Selecting RR never selects a renderer.
    g.pathRequested=true;
    if(g.pathRequested) {
        D3D12_COMMAND_QUEUE_DESC uq{};uq.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        if(FAILED(device->QueryInterface(IID_PPV_ARGS(&g.pathOn12))) ||
           FAILED(g.d12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&g.pathFence))) ||
           FAILED(g.d12->CreateCommandQueue(&uq,IID_PPV_ARGS(&g.unwrapQueue)))) {
            g.pathRequested=false;gPathSelected=false; // existing RT/RR remains available
        } else g.unwrapQueue->SetName(L"VaM skin PT unwrap queue (runs no work)");
    }
    g.d11 = device;
    gClothPath=(ClothPathFn)GetProcAddress(GetModuleHandleW(L"VaMClothShadingNative.bin"),"CS_GetPathMaterialV1");
    char err[160] = "";
    if (!rtc::Init(g.d12.Get(), err, sizeof err)) { SetReason("RtCore: %s", err); return false; }
    rtc::Status cs = rtc::GetStatus();
    if (cs.apiVersion != rtc::kApiVersion) { SetReason("RtCore API %u, host %u", cs.apiVersion, rtc::kApiVersion); return false; }
    if(g.pathRequested && !rtc::PathTracingAvailable()){g.pathRequested=false;gPathSelected=false;}
    HRESULT hr = g.d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence));
    g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(hr) || !g.event) { SetReason("fence"); return false; }
    D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * kSlots, 0};
    UINT64 freq = 0;
    if (SUCCEEDED(g.d12->CreateQueryHeap(&qd, IID_PPV_ARGS(&g.queries))) && SUCCEEDED(g.queue->GetTimestampFrequency(&freq)) && freq) {
        g.readback = Buffer12(8 * 2 * kSlots, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        void *p = nullptr;
        if (g.readback && SUCCEEDED(g.readback->Map(0, nullptr, &p))) g.ticks = (const UINT64 *)p;
        g.tickMs = 1000.0 / (double)freq;
        g.timestampFrequency = freq;
    }
    rtc::MaterialDesc md;  // the records' material 0: opaque mid grey
    g.defaultMaterial = rtc::CreateMaterial(md);
    g.ok = true;
    std::lock_guard<std::mutex> l(gLock);
    g.status.available = 1;
    snprintf(g.status.reason, sizeof g.status.reason, "D3D11On12 + RtCore (API %u); path_bridge=%d", rtc::kApiVersion,g.pathFence?1:0);
    return true;
}

bool RtAvailable() { return g.ok; }
// 0.22.62: 光追 off in the menu skips RtFrame, so nothing else ended a PT frame: PT kept its last state and the hair
// plugin kept leaving its strands to PT (only their shadows showed).
void RtFrameOff() {
    if(g.pathThisFrame || gPathRendering.load())StopPathRendering();
    std::lock_guard<std::mutex> l(gLock);
    g.status.pathTracing=0;
}
bool RtMaybe() { return g.ok || !g.tried; }  // two bools written once on the render thread: a stale read only costs a bake

// ---- the frame ------------------------------------------------------------------------------------------------------

#include "path_textures.inl"

// No fences: the PT ran on the 11on12 queue itself (unwrapped naming PathUnwrapQueue()), already executed, so D3D11's later
// use of these textures is ordered after it. A fence here makes 11on12 put a queue wait before that use.
static bool ReturnPathTextures() {
    gPathFinishDeferred=false;
    if(!g.pathOn12)return true;
    HRESULT failed=S_OK;
    for(auto &kv:g.pathTextures)if(kv.second.taken) {
        HRESULT hr=g.pathOn12->ReturnUnderlyingResource(kv.second.source.Get(),0,nullptr,nullptr);
        if(SUCCEEDED(hr))kv.second.taken=false;
        else if(SUCCEEDED(failed))failed=hr;
    }
    if(FAILED(failed)){Fail("PT texture return",failed);return false;}
    return true;
}

// 0.22.71: an eyeball's draw this frame (RtDraw::pathFlags 1073741824, RtScene.cs), and the characters' eye rigs PT's
// eyelid band reads (path_trace.hlsl PtEyeBand): each rig's gather on the skin's vertices of this frame (RtPathEye),
// once a character, with its header into one buffer of rtc::kPathEyeRigBytes records - D3D12's, wrapped, written here by
// D3D11 (section 2) and read by the trace promoted from COMMON. Up to kPathEyeRigs characters.
struct PathEyeDraw { ID3D11Buffer *indices, *vertices; uint32_t start; float offset[3]; };
constexpr uint32_t kPathEyeRigs=8;
void RecordPathEyes(ID3D11DeviceContext *c,const std::vector<PathEyeDraw> &draws) {
    RtPathEyeRig rigs[kPathEyeRigs];const float *offsets[kPathEyeRigs];uint32_t n=0;
    for(const PathEyeDraw &d:draws) {
        RtPathEyeRig r;
        if(!RtPathEye(c,d.indices,d.start,d.vertices,r))continue;
        bool seen=false;for(uint32_t k=0;k<n;k++)seen|=rigs[k].rig==r.rig;
        if(seen)continue;
        if(n==kPathEyeRigs)break;
        rigs[n]=r;offsets[n]=d.offset;n++;
    }
    g.pathEyeCount=0;
    if(!n)return;
    if(!g.pathEyeCopy.r12) {
        D3D11_RESOURCE_FLAGS f={};f.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        g.pathEyeCopy.r12=Buffer12(kPathEyeRigs*rtc::kPathEyeRigBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON);
        if(!g.pathEyeCopy.r12)return;
        HRESULT hr=g.on12->CreateWrappedResource(g.pathEyeCopy.r12.Get(),&f,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COMMON,
                                                 IID_PPV_ARGS(&g.pathEyeCopy.r11));
        if(FAILED(hr)){Fail("CreateWrappedResource (eye rigs)",hr);g.pathEyeCopy.r12.Reset();return;}
        g.pathEyeCopy.bytes=kPathEyeRigs*rtc::kPathEyeRigBytes;
    }
    ID3D11Resource *wrapped=g.pathEyeCopy.r11.Get();
    g.on12->AcquireWrappedResources(&wrapped,1);
    for(uint32_t k=0;k<n;k++) {
        const UINT at=k*rtc::kPathEyeRigBytes;
        uint32_t head[12]={rigs[k].points[0],rigs[k].points[1]};
        memcpy(head+4,offsets[k],12);memcpy(head+8,rigs[k].occlusion,16);
        const D3D11_BOX box={at,0,0,at+48,1,1};
        c->UpdateSubresource(wrapped,0,&box,head,0,0);
        const D3D11_BOX rig={0,0,0,rtc::kPathEyeRigBytes-48,1,1};
        c->CopySubresourceRegion(wrapped,0,at+48,0,0,rigs[k].rig,0,&rig);
    }
    g.on12->ReleaseWrappedResources(&wrapped,1);
    g.pathEyeCount=n;
}

void RtFrame(ID3D11DeviceContext *c, const void *records, int frame) {
    // A compositor exception between the left eye's record and finish leaves the textures unwrapped; VaM draws
    // with them from here on.
    if(gPathFinishDeferred || gPathRecordedEye>=0){ReturnPathTextures();gPathRecordedEye=-1;}
    PathCollectGraves();
    {
        std::vector<RtReceiver *> doomed;
        {
            std::lock_guard<std::mutex> l(gLock);
            doomed.swap(g.doomed);
        }
        for (RtReceiver *r : doomed) DestroyReceiverNow(r);
        std::lock_guard<std::mutex> l(gLock);
        g.status.receivers = (int)g.allReceivers.size();  // also on frames that trace nothing
    }
    if (!g.ok || !c || !records || frame == g.lastFrame) return;
    const RtSceneHeader *h = (const RtSceneHeader *)records;
    if (h->version != kRtSceneVersion || h->draws < 0 || h->lights < 0 || h->hairs < 0) return;
    g.lastFrame = frame;
    gPathFrameInvalid=false;
    if(gManagedSceneRevision!=h->sceneRevision){gManagedSceneRevision=h->sceneRevision;gSceneRevision++;}
    uint32_t coverage;memcpy(&coverage,h->pathEnvironmentScale+3,4);
    const bool staging=(coverage&0x40000000u)!=0;
    const bool prepared=(coverage&0x80000000u)!=0 && !staging;
    // Scene restore is a transaction: partially instantiated geometry is not
    // a complete PT snapshot. Keep the user's selection and render the original
    // route until the loader has committed and the registry was rebuilt.
    // 0.22.62: no object refuses the scene (the plugin adapts each one in PT and
    // logs it); the old coverage bits that cleared the selection are gone.
    const bool requested=g.pathRequested && g.reconstruction && gPathSelected.load() && frame>=gPathSuspendUntil;
    gPathCollect=requested;
    const bool pathNow=requested && prepared && gPathConsumerReady.load();
    const bool leavingPath=(g.pathThisFrame || gPathCleanupPending) && !pathNow;
    if(leavingPath) {
        gPathCleanupPending=false;
        rtc::ClearPathMaterials();
        for(auto &kv:g.pathTextures)PathForget(kv.second);
        g.pathTextures.clear();
        for(auto &cube:g.pathEnvironment)cube.Reset();
        BuryWrapped(g.pathHairCopy);g.pathHairCopiedFrame=INT_MIN;
        g.pathHairBuildBytes=0;
        BuryWrapped(g.pathEyeCopy);g.pathEyeCount=0;
        for(auto &kv:g.materials) {
            auto &m=kv.second;m.pathBound=m.pathSourcesReady=false;
            for(auto &source:m.pathSource)source.Reset();
        }
        for(auto it=g.owners.begin();it!=g.owners.end();) {
            BuryWrapped(it->second.tangents);Bury(it->second.tangentSource);++it;
        }
    }
    g.pathThisFrame=pathNow;
    gPathRendering=pathNow;
    // The hair plugin draws its strands exactly when PT does not trace them.
    // 0.22.62: before the return below (0.22.50 returned first: a PT launch
    // waiting or paused showed raster frames whose hair only cast shadows).
    if(g.pathRequested) {
        using HairMode=void(__cdecl*)(int);
        auto mode=(HairMode)GetProcAddress(GetModuleHandleW(L"VaMHairOnePassNative.bin"),"HOP_PathModeV1");
        if(mode)mode(pathNow?1:0);
    }
    // Nothing is traced while PT warms up, is paused or is off: ordinary raster.
    if(!pathNow)return;
    if(pathNow) {
        for(int k=0;k<2;k++)g.pathEnvironment[k]=reinterpret_cast<ID3D11Resource *>(h->pathEnvironment[k]);
        g.pathEnvironment[2]=reinterpret_cast<ID3D11Resource *>(h->pathIndoorEnvironment);
        g.pathEnvironment[3]=reinterpret_cast<ID3D11Resource *>(h->pathProbe);
        memcpy(g.pathSky.probeDecode,h->pathProbeDecode,64);
        g.pathSky.indoorScale=std::max(h->pathIndoorStrength,0.f);
        memcpy(g.pathSky.worldToSky,h->pathWorldToSky,48);
        g.pathSky.lightingScale=std::max(h->pathEnvironmentScale[0],0.f);
        g.pathSky.backgroundScale=std::max(h->pathEnvironmentScale[1],0.f);
        // Bit 0 retains API 25's Skyshop decode; bit 1 exempts a linear captured background.
        g.pathSky.rgbm=(h->pathEnvironmentScale[2]!=0?1u:0u) | (h->pathBackgroundColor[3]>0?2u:0u);
        g.pathSky.diffuseScale=std::max(h->pathEnvironmentDiffuse,0.f);
        g.pathSky.diffuseBounces=g.pathBounces;
        g.pathSky.authorRules=g.authorRules;g.pathSky.authorAmbient=g.authorAmbient;
        g.pathSky.authorContact=g.authorContact;g.pathSky.authorAmbientRange=g.authorAmbientRange;g.pathSky.authorEmitter=g.authorEmitter;
        g.pathSky.backgroundNear=g.backgroundNear;g.pathSky.backgroundFar=g.backgroundFar;
        memcpy(g.pathSky.backgroundToSky,h->pathBackgroundToSky,48);
        for(int k=0;k<3;k++)g.pathSky.backgroundColor[k]=std::max(h->pathBackgroundColor[k],0.f);
    }
    const double t0 = NowMs();
    Collect();
    const RtDraw *draws = (const RtDraw *)(h + 1);
    const RtLightRec *lights = (const RtLightRec *)(draws + h->draws);
    const RtHairRec *hairs = (const RtHairRec *)(lights + h->lights);
    g.pathHairRegistry.clear();g.pathHairLinks.clear();g.pathHairSources.clear();g.pathRasterBuffers.clear();
    if(pathNow) {
        // One fiber BLAS per light-link class and camera visibility (rtc::RecordPathHair): keep each
        // contiguous, in the plugin's order within it.
        auto group=[&](int i){return (hairs[i].lightLink&0x1fu)|((hairs[i].flags&RT_HAIR_CAMERA_HIDDEN)?kPathHairCameraHidden:0u);};
        std::vector<int> order;
        for(int i=0;i<h->hairs;i++)order.push_back(i);
        std::stable_sort(order.begin(),order.end(),[&](int a,int b){return group(a)<group(b);});
        for(int i:order) {
            const auto &hair=hairs[i].path;
            if(!hair.particles || !hair.barycentrics || !hair.indices || !hair.indexCount)continue;
            g.pathHairRegistry.push_back(hair);g.pathHairLinks.push_back(group(i));
            g.pathRasterBuffers[reinterpret_cast<ID3D11Buffer*>(hair.indices)].push_back({0,0,0,2,0,0});
            for(auto pointer:{hair.particles,hair.barycentrics,hair.indices,hair.noise,hair.color})
                g.pathHairSources.emplace_back(reinterpret_cast<ID3D11Resource*>(pointer));
        }
    }

    // 1. owners, their submeshes and materials
    for (RtReceiver *r : g.allReceivers) r->claimed = false;
    std::vector<Owner *> live;
    std::vector<PathEyeDraw> pathEyeDraws;
    for (int i = 0; i < h->draws; i++) {
        const RtDraw &d = draws[i];

        if (!d.vertices || !d.indices || !d.indexCount || !d.vertexCount) continue;
        if(pathNow) {
            auto& ranges=g.pathRasterBuffers[reinterpret_cast<ID3D11Buffer*>(d.indices)];
            // 0.23.4: an author's effect (RT_DRAW_EFFECT) is replayed over PT whether or not it is also traced
            const int kind=(d.flags&RT_DRAW_DISCARD)?4:(d.flags&RT_DRAW_EFFECT)?5:(d.pathFlags&32)?3:1;
            const uint64_t watched=(d.flags&RT_DRAW_WATCH)?d.material:0;
            auto same=std::find_if(ranges.begin(),ranges.end(),[&](const PathRasterDraw& x){return x.first==d.indexStart && x.count==d.indexCount && x.base==d.baseVertex && x.kind==kind && x.programSet==d.pathProgramSet;});
            if(same==ranges.end())ranges.push_back({d.indexStart,d.indexCount,d.baseVertex,kind,d.pathProgramSet,watched});
            else if(watched)same->watched=watched;
        }
        if(d.flags&RT_DRAW_DISCARD)continue; // no surface, transport or depth contribution
        if(d.pathFlags&32)continue; // editor overlays never become physical occluders
        auto *vb = reinterpret_cast<ID3D11Buffer *>(d.vertices);
        auto *nb = reinterpret_cast<ID3D11Buffer *>(d.normals);
        auto *ib = reinterpret_cast<ID3D11Buffer *>(d.indices);
        if(pathNow && (d.pathFlags&1073741824u))pathEyeDraws.push_back({ib,vb,d.indexStart,{d.matrix[3],d.matrix[7],d.matrix[11]}});
        Owner &o = g.owners[d.owner];
        if (o.lastSeen != frame || o.id != d.owner) {
            o.id = d.owner;
            o.lastSeen = frame;
            o.subs.clear();
            o.alpha = false;
            live.push_back(&o);
            const UINT64 bytes = BufferBytes(vb);
            const uint32_t stride = d.vertexStride ? d.vertexStride : (uint32_t)(bytes / std::max<uint32_t>(1, d.vertexCount));
            if (o.vertexSource.Get() != vb || o.normalSource.Get() != nb || o.stride != stride || o.sourceBytes != bytes ||
                (o.flags & ~(RT_DRAW_INDEX32 | RT_DRAW_NO_OCCLUDE | RT_DRAW_CAMERA_HIDDEN | RT_DRAW_EFFECT | RT_DRAW_WATCH)) !=
                (d.flags & ~(RT_DRAW_INDEX32 | RT_DRAW_NO_OCCLUDE | RT_DRAW_CAMERA_HIDDEN | RT_DRAW_EFFECT | RT_DRAW_WATCH))) {
                DropOwner(o);
                o.vertexSource = vb;
                o.normalSource = nb;
                o.stride = stride;
                o.sourceBytes = bytes;
                o.verticesReady = false;
            }
            o.flags = d.flags & ~(RT_DRAW_NO_OCCLUDE | RT_DRAW_CAMERA_HIDDEN | RT_DRAW_EFFECT | RT_DRAW_WATCH);  // per submesh (Sub::noOcclude, cameraHidden)
            o.lightLink = d.lightLink;
            o.pathNormalOffset=d.pathNormalOffset;
            if(o.meshRevision!=d.pathLayoutFlags) {
                o.meshRevision=d.pathLayoutFlags;o.verticesReady=false;
                if(d.uvs)g.indices[reinterpret_cast<ID3D11Buffer*>(d.uvs)].ready=false;
                g.indices[ib].ready=false;
            }
            auto *tb=reinterpret_cast<ID3D11Buffer*>(d.pathTangents);
            if(!g.pathThisFrame)tb=nullptr;
            const uint64_t tangentBytes=tb?BufferBytes(tb):0;
            uint32_t tangentStride=d.pathTangentStride?d.pathTangentStride:
                d.vertexCount && tangentBytes%d.vertexCount==0?(uint32_t)(tangentBytes/d.vertexCount):0;
            const uint32_t tangentElement=d.pathTangentFormat==2?8:16;
            if(tb && (!tangentStride || d.pathTangentOffset+tangentElement>tangentStride ||
                tangentBytes<uint64_t(d.vertexCount)*tangentStride)) {
                // 0.22.62: this owner only (it used to clear the PT selection): its UV derivative basis.
                PathNotice("PT tangent layout invalid for owner %llu: its UV derivative basis is used",(unsigned long long)o.id);
                tb=nullptr;tangentStride=0;
            }
            if(o.tangentSource.Get()!=tb){BuryWrapped(o.tangents);o.tangentSource=tb;}
            o.tangentOffset=d.pathTangentOffset;o.tangentFormat=d.pathTangentFormat;o.tangentStride=tangentStride;
            o.vertexCount = (uint32_t)(bytes / std::max<uint32_t>(4, stride));
            memcpy(o.matrix, d.matrix, sizeof o.matrix);
            if((d.flags&RT_DRAW_CUA_SKIN) && d.boneCount && d.boneCount<=16384 && d.skinWeights && d.skinMatrices) {
                auto *weights=reinterpret_cast<ID3D11Buffer*>(d.skinWeights);
                if(o.weightSource.Get()!=weights){BuryWrapped(o.weights);o.weightSource=weights;}
                const float *rows=reinterpret_cast<const float*>(d.skinMatrices);
                o.boneCount=d.boneCount;o.blendFrameCount=d.blendFrameCount;
                o.boneMatrices.assign(rows,rows+size_t(d.boneCount)*12+d.blendFrameCount);
            }
            // UV0 (the alpha test): only a stream of whole vertices that holds every vertex (the plugin's layout guess
            // for a Unity mesh is checked against the buffer's size)
            auto *ub = reinterpret_cast<ID3D11Buffer *>(d.uvs);
            o.uvSource = nullptr;
            const UINT64 ubBytes = ub ? BufferBytes(ub) : 0;
            // the mesh's own vertex count, not the draw buffer's (0.22.0 SimXR: VaM's padded clothing buffers left every
            // clothing item without UVs, alpha 1: lace and chokers blocked as solid sheets)
            const uint32_t uvVerts = d.vertexCount;
            const bool uvHalf = (d.uvStride & 0x80000000u) != 0;const uint32_t uvGiven = d.uvStride & 0x7fffffffu, uvElement = uvHalf ? 4 : 8;
            const uint32_t us = uvGiven ? uvGiven : uvVerts && ubBytes % uvVerts == 0 ? (uint32_t)(ubBytes / uvVerts) : 0;
            o.uvBytes = ubBytes;
            o.uvFound = us;
            if (ub && us >= uvElement && d.uvOffset + uvElement <= us && ubBytes % us == 0 && ubBytes / us >= uvVerts && uvVerts <= o.vertexCount) {
                o.uvSource = ub;
                o.uvOffset = d.uvOffset;
                o.uvStride = us;
                o.uvFormat = uvHalf ? rtc::VertexFormat::Half2 : rtc::VertexFormat::Float2;
                if (ub != vb) {
                    IndexCopy &uc = g.indices[ub];
                    if (uc.source.Get() != ub) { BuryWrapped(uc.copy); uc.source = ub; uc.ready = false; }
                    uc.lastSeen = frame;
                }
            }
            // a receiver's skin or clothing item: the first one drawn this frame with the receiver's mesh (another one
            // with the same mesh is an occluder like any other)
            o.receiver = nullptr;
            if ((d.flags & (RT_DRAW_SKIN | RT_DRAW_WRAP)) && nb) {
                auto it = g.receivers.find((void *)ib);
                if (it != g.receivers.end() && !it->second->claimed && o.vertexCount >= it->second->vertexCount) {
                    o.receiver = it->second;
                    o.receiver->claimed = true;
                }
            }
        }
        IndexCopy &ic = g.indices[ib];
        if (ic.source.Get() != ib) { BuryWrapped(ic.copy); ic.source = ib; ic.ready = false; }
        ic.lastSeen = frame;
        uint32_t mat = g.defaultMaterial;
        RtSurfaceRole role=RtSurfaceRole::Generic;
        if (d.material) {
            float cloth[8]={};
            const bool fabric=pathNow && (d.flags&RT_DRAW_WRAP) && (d.pathFlags&33554432u) &&
                !(d.pathFlags&(32u|1024u)) && gClothPath && gClothPath(ib,d.indexStart,cloth)==1;
            float diffuseFactor=-1;
            role=pathNow && (d.flags&RT_DRAW_SKIN)?RtPathSurfaceRole(ib,d.indexStart,diffuseFactor):RtSurfaceRole::Generic;
            if(fabric)role=RtSurfaceRole::Fabric;
            // One source material may be reused on surfaces with distinct
            // roles. Their converted closures must not overwrite each other.
            // 11-05: a person's skin and skin-wrap draws (clothing, mesh hair) carry PT_MODE_PERSON (256): the PT guides
            // keep their depth and motion even where they are see-through (path_trace.hlsl PtPrimaryGuides).
            const uint32_t pathMode=d.pathMode|((d.flags&(RT_DRAW_SKIN|RT_DRAW_WRAP))?256u:0u);
            const uint64_t materialKey=d.material^(uint64_t(role)<<56)^(uint64_t(pathMode>>8&1)<<55);
            Material &m = g.materials[materialKey];
            m.role=role;
            ID3D11Resource *texture=reinterpret_cast<ID3D11Resource*>(d.texture);
            ID3D11Resource *alpha=reinterpret_cast<ID3D11Resource*>(d.alphaTexture);
            const bool newTexture=m.texture.Get()!=texture;
            if(newTexture) {
                m.texture=texture;m.textureVersion=d.textureVersion;m.textureSerial++;
            }
            const auto kind=(rtc::MaterialKind)std::min<uint32_t>(d.kind&0xff,3);
            const float adjust=std::isfinite(d.alphaAdjust)?std::clamp(d.alphaAdjust,-1.f,1.f):0;
            const bool alphaChanged=m.alphaTexture.Get()!=alpha || (!alpha && newTexture) ||
                m.alphaVersion!=d.alphaVersion || m.alphaAdjust!=adjust || m.desc.kind!=kind;
            if(alphaChanged) {
                m.alphaTexture=alpha;m.alphaVersion=d.alphaVersion;m.alphaAdjust=adjust;
            }
            if (!m.id || m.revision!=d.materialRevision || newTexture || alphaChanged) {
                if(pathNow && m.id) {
                    const uint64_t textures[]={d.texture,d.pathNormal,d.pathSurface,d.pathEmission,d.pathGloss,d.pathDetailNormal,d.pathDetailMask,d.alphaTexture?d.alphaTexture:d.texture,d.pathSecond};
                    bool discontinuity=((m.path.flags^d.pathFlags)&(8u|1024u|2048u|1048576u|2097152u|8388608u))!=0 || m.path.mode!=pathMode;
                    for(int k=0;k<9;k++)discontinuity|=m.pathSource[k].Get()!=reinterpret_cast<ID3D11Resource*>(textures[k]);
                    if(discontinuity)gContentRevision++;
                }
                m.pathBound=m.pathSourcesReady=false;
                m.revision=d.materialRevision;
                m.desc.kind = (rtc::MaterialKind)std::min<uint32_t>(d.kind & 0xff, 3);
                // version 6: the ambient its shader draws (its bounce radiance's)
                m.desc.environment = (rtc::EnvironmentShader)std::min<uint32_t>((d.kind >> 12) & 3, 2);
                for (int k = 0; k < 3; k++) m.desc.subsurface[k] = std::isfinite(d.subsurface[k]) ? std::min(std::max(d.subsurface[k], 0.0f), 1.0f) : 0.0f;
                m.desc.iblFilter = std::isfinite(d.iblFilter) ? std::min(std::max(d.iblFilter, 0.0f), 1.0f) : 0.0f;
                m.alphaAdjust = std::isfinite(d.alphaAdjust) ? std::min(std::max(d.alphaAdjust, -1.0f), 1.0f) : 0.0f;
                for (int k = 0; k < 3; k++) m.color[k] = std::min(std::max(d.color[k], 0.0f), 4.0f);
                for(int k=0;k<3;k++)m.desc.averageColor[k]=m.color[k];
                m.desc.alphaCutoff = d.cutoff;
                m.desc.solidShadow = (d.kind & 0x400) != 0;  // API 43: RtScene KindSolidShadow
                m.desc.opacity = d.opacity;
                memcpy(m.desc.uvScaleOffset, d.st, sizeof d.st);
                if(!m.id)m.id = rtc::CreateMaterial(m.desc);else rtc::SetMaterial(m.id,m.desc);

            }
            if(pathNow && !m.pathSourcesReady) {
                    m.path=rtc::PathMaterial{};
                    uint64_t sources[]={d.texture,d.pathNormal,d.pathSurface,d.pathEmission,d.pathGloss,d.pathDetailNormal,d.pathDetailMask,d.alphaTexture?d.alphaTexture:d.texture,d.pathSecond};
                    for(int k=0;k<9;k++)m.pathSource[k]=reinterpret_cast<ID3D11Resource *>(sources[k]);
                    memcpy(m.path.baseColor,d.color,12);m.path.baseColor[3]=d.opacity;
                    // Material-family authoring response, independently of
                    // skin/cloth role. Physical Standard materials stay 1:1.
                    // VaM's Subsurface family multiplies DIRECT diffuse by
                    // 2(1-S/2), Skyshop's by 2; ambient and bounces see the
                    // albedo itself, so the gain is not baked into the colour.
                    if(d.pathTransmission<.999f && !(d.pathFlags&1024u))for(int k=0;k<3;k++) {
                        if(d.pathFlags&33554432u)m.path.directGain[k]=2-m.desc.subsurface[k];
                        else if(d.pathFlags&67108864u)m.path.directGain[k]=2;
                    }
                    m.path.directGain[3]=d.pathModeParam;m.path.mode=pathMode;
                    memcpy(m.path.secondST,d.pathSecondST,16);m.path.secondSampling=d.pathSecondSampling;
                    memcpy(m.path.specular,d.pathSpecular,12);m.path.roughness=d.pathRoughness;
                    memcpy(m.path.emissionColor,d.pathEmissionColor,12);m.path.metallic=d.pathMetallic;
                    memcpy(m.path.uvST,d.pathST,16);memcpy(m.path.glossParams,d.pathGlossParams,12);
                    m.path.glossParams[3]=d.pathLegacyFresnel;
                    m.path.normalScale=d.pathNormalScale;m.path.ior=d.pathIor;m.path.transmission=d.pathTransmission;
                    m.path.flags=d.pathFlags;
                    memcpy(m.path.mapST,d.pathMapST,64);memcpy(m.path.mapST+16,d.pathDetailST,16);memcpy(m.path.bump,d.pathBump,16);
                    memcpy(m.path.alphaST,d.st,16);m.path.alphaAdjust=m.alphaAdjust;
                    memcpy(m.path.materialControls,d.pathMaterialControls,16);
                    memcpy(m.path.sampling,d.pathSampling,sizeof d.pathSampling);
                    m.pathSourcesReady=true;m.pathBound=false;
            }
            m.lastSeen = frame;
            if(fabric) {
                constexpr uint32_t fabricFlag=1u<<24;
                if(!(m.path.flags&fabricFlag)||memcmp(m.path.skin,cloth,sizeof cloth)!=0) {
                    gContentRevision++;
                    memcpy(m.path.skin,cloth,sizeof cloth);m.path.flags|=fabricFlag;m.pathBound=false;
                }
                for(int k=0;k<3;k++){m.path.baseColor[k]=m.color[k];m.path.directGain[k]=2-m.desc.subsurface[k];}
            }
            if(g.pathThisFrame && (d.flags&RT_DRAW_SKIN)) {
                float skin[28];
                bool isSkin=RtPathSkinParameters(ib,d.indexStart,m.pathSource[4].Get(),skin);
                uint32_t flags=(m.path.flags&~8u)|(isSkin?8u:0u);
                if(flags!=m.path.flags || memcmp(skin,m.path.skin,sizeof skin)!=0) {
                    // Bake completion and explicit gloss/SSS changes are
                    // material discontinuities, separate from scene animation.
                    gContentRevision++;
                    memcpy(m.path.skin,skin,sizeof skin);m.path.flags=flags;m.pathBound=false;
                }
                // Convert source authoring reflectance, not exposure. The
                // raster family multiplies (1-S/2)*albedo by TWO after the
                // diffuse factor, for direct light only (teeth: their own S
                // and gain). Reapply after every route rebuild.
                bool baseChanged=false;
                for(int k=0;k<3;k++) {
                    const float gain=diffuseFactor>=0?diffuseFactor:(d.pathFlags&33554432u)?2-m.desc.subsurface[k]:(d.pathFlags&67108864u)?2:1;
                    baseChanged|=m.color[k]!=m.path.baseColor[k] || gain!=m.path.directGain[k];
                    m.path.baseColor[k]=m.color[k];m.path.directGain[k]=gain;
                }
                if(baseChanged){gContentRevision++;m.pathBound=false;}
            }
            if (m.id) mat = m.id;
            o.alpha |= m.desc.kind == rtc::MaterialKind::Cutout || m.desc.kind == rtc::MaterialKind::Translucent;
        }
        o.subs.push_back({ib, d.indexStart, d.indexCount, d.baseVertex, (d.flags & RT_DRAW_INDEX32) != 0, mat, d.tessAlpha,
                          (d.flags & RT_DRAW_NO_OCCLUDE) != 0,g.pathThisFrame && d.pathTransmission<.999f ?
                          ((d.pathFlags&524288u)?1u:(d.pathFlags&134217728u)?2u:0u):0u,
                          pathNow && (d.flags & RT_DRAW_CAMERA_HIDDEN) != 0,role});
    }
    // evict what was not drawn for a while
    for (auto it = g.owners.begin(); it != g.owners.end();) {
        if (frame - it->second.lastSeen > kEvictFrames) { DropOwner(it->second); it = g.owners.erase(it); }
        else ++it;
    }
    for (auto it = g.indices.begin(); it != g.indices.end();) {
        if (frame - it->second.lastSeen > kEvictFrames) { BuryWrapped(it->second.copy); Bury(it->second.source); it = g.indices.erase(it); }
        else ++it;
    }
    for (auto it = g.materials.begin(); it != g.materials.end();) {
        if (frame - it->second.lastSeen > kEvictFrames) {
            if (it->second.id) rtc::DestroyMaterial(it->second.id);
            it = g.materials.erase(it);
        } else ++it;
    }
    // 2. D3D11: the copies
    std::vector<ID3D11Resource *> acquire, release;
    std::vector<Wrapped *> cycling;  // back to COMMON at the end of the list (D3D11 takes them again)
    UINT64 copied = 0;
    auto Need = [&](Wrapped &w, ID3D11Buffer *src) {
        if (!w.r12 && !WrapCopy(w, src)) return false;
        if (!w.acquired) { acquire.push_back(w.r11.Get()); w.acquired = true; }
        return true;
    };
    for (Owner *o : live) {
        if(!o->Dynamic()) {
            IndexCopy& vc=g.indices[o->vertexSource.Get()];
            if(vc.source.Get()!=o->vertexSource.Get()){BuryWrapped(vc.copy);vc.source=o->vertexSource;vc.ready=false;}
            vc.lastSeen=frame;
        }
        if(o->weightSource && !o->weights.r12 && !Need(o->weights,o->weightSource.Get()))return;
        if(o->tangentSource && o->tangentSource.Get()!=o->vertexSource.Get() && (o->Dynamic() || !o->tangents.r12))
            if(!Need(o->tangents,o->tangentSource.Get()))return;
        if (o->Dynamic()) {
            if (!Need(o->vertices, o->vertexSource.Get())) return;
            if (o->normalSource && !Need(o->normals, o->normalSource.Get())) return;
        }
    }
    for (auto &kv : g.indices) {
        IndexCopy &ic = kv.second;
        if (ic.ready || ic.lastSeen != frame) continue;
        if (!Need(ic.copy, ic.source.Get())) return;
    }
    if (!acquire.empty()) g.on12->AcquireWrappedResources(acquire.data(), (UINT)acquire.size());
    auto Copy = [&](Wrapped &w, ID3D11Buffer *src, bool cycle) {
        const D3D11_BOX box = {0, 0, 0, (UINT)std::min<UINT64>(w.bytes, BufferBytes(src)), 1, 1};
        c->CopySubresourceRegion(w.r11.Get(), 0, 0, 0, 0, src, 0, &box);
        release.push_back(w.r11.Get());
        w.acquired = false;
        if (cycle) cycling.push_back(&w);
        copied += box.right;
    };
    for (Owner *o : live) {
        if (o->vertices.acquired) Copy(o->vertices, o->vertexSource.Get(), o->Dynamic());
        if (o->normals.acquired) Copy(o->normals, o->normalSource.Get(), o->Dynamic());
        if(o->tangents.acquired)Copy(o->tangents,o->tangentSource.Get(),o->Dynamic());
        if(o->weights.acquired)Copy(o->weights,o->weightSource.Get(),false);
        o->verticesReady = true;
    }
    for (auto &kv : g.indices) {
        IndexCopy &ic = kv.second;
        if (!ic.copy.acquired) continue;
        Copy(ic.copy, ic.source.Get(), false);
        ic.ready = true;
    }
    for(Owner* o:live)if(!o->Dynamic())o->vertices=g.indices[o->vertexSource.Get()].copy;
    if (!release.empty()) g.on12->ReleaseWrappedResources(release.data(), (UINT)release.size());
    if(pathNow)RecordPathEyes(c,pathEyeDraws);else g.pathEyeCount=0;
    c->Flush();

    // 3. D3D12 on the 11on12 queue
    const unsigned si = g.next;
    Slot &s = g.slots[si];
    const double w0 = NowMs();
    if (!WaitCpu(s.done)) return;
    const double waited = NowMs() - w0;
    ReadTiming(s, si);
    HRESULT hr;
    if (!s.alloc) {
        hr = g.d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.alloc));
        if (SUCCEEDED(hr)) hr = g.d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.alloc.Get(), nullptr, IID_PPV_ARGS(&s.list));
        if (FAILED(hr)) { Fail("command list", hr); return; }
    } else {
        hr = s.alloc->Reset();
        if (SUCCEEDED(hr)) hr = s.list->Reset(s.alloc.Get(), nullptr);
        if (FAILED(hr)) { Fail("command list reset", hr); return; }
    }
    ID3D12GraphicsCommandList4 *l = s.list.Get();
    if (g.queries) l->EndQuery(g.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * si);
    std::vector<D3D12_RESOURCE_BARRIER> bars;
    // RtCore's frame
    g.geometries.clear();
    g.submeshes.clear();
    size_t subTotal = 0;
    for (Owner *o : live) subTotal += o->subs.size();
    g.submeshes.reserve(subTotal);
    for (Owner *o : live) {
        if (o->subs.empty() || !o->vertices.r12 || (o->stride & 3) || o->stride < 12) continue;
        // PT traces the tessellated mesh itself. Adjacent material regions of
        // one skin must use the same subdivision: different polygonal
        // approximations of their shared Phong edge leave camera-visible gaps.
        uint32_t pathTessLevel = 1;
        float pathSkinPhong = 0;
        if (pathNow && o->receiver) {
            pathSkinPhong=PathSkinPhong(o->subs);
            for (const Sub &sub : o->subs) {
                if (sub.tessAlpha <= 0) continue;
                auto level = o->receiver->tessLevel.find(sub.start);
                if (level != o->receiver->tessLevel.end()) pathTessLevel = std::max(pathTessLevel, level->second);
            }
        }
        // one geometry per owner and index buffer / base vertex
        std::vector<size_t> order(o->subs.size());
        for (size_t k = 0; k < order.size(); k++) order[k] = k;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const Sub &x = o->subs[a], &y = o->subs[b];
            return x.indices != y.indices ? x.indices < y.indices : x.base != y.base ? x.base < y.base :
                x.noOcclude!=y.noOcclude?x.noOcclude<y.noOcclude:x.primaryCull!=y.primaryCull?x.primaryCull<y.primaryCull:
                x.cameraHidden<y.cameraHidden;
        });
        for (size_t k = 0; k < order.size();) {
            const Sub &first = o->subs[order[k]];
            IndexCopy &ic = g.indices[first.indices];
            const size_t from = g.submeshes.size();
            size_t e = k;
            for (; e < order.size() && o->subs[order[e]].indices == first.indices && o->subs[order[e]].base == first.base &&
                          o->subs[order[e]].noOcclude == first.noOcclude && o->subs[order[e]].primaryCull==first.primaryCull &&
                          o->subs[order[e]].cameraHidden==first.cameraHidden; e++) {
                const Sub &x = o->subs[order[e]];
                rtc::Submesh sm;
                sm.start = x.start;
                sm.count = x.count / 3 * 3;
                sm.material = x.material;
                // Some skins use a tessellated shoulder material beside an
                // untessellated forearm/face material. Equal subdivisions do
                // not join those edges unless both also use the same Phong
                // surface. Extend the existing skin setting only to skin and
                // lip entries; eyes, teeth, nails and other meshes keep theirs.
                const float tess=PathSurfacePhong(x,pathNow,pathSkinPhong);
                if (tess > 0 && o->receiver) {
                    auto tl = o->receiver->tessLevel.find(x.start);
                    sm.tess = rtc::Tess::Phong;
                    sm.tessAlpha = tess;
                    sm.tessLevel = pathNow ? pathTessLevel : (tl != o->receiver->tessLevel.end() ? tl->second : 1);
                }
                g.submeshes.push_back(sm);
            }
            if (ic.ready && ic.copy.r12) {
                rtc::Geometry x;
                x.id = o->id * 0x9E3779B97F4A7C15ull ^ ((uint64_t)(uintptr_t)first.indices) ^ ((uint64_t)(uint32_t)first.base << 40) ^
                       (first.noOcclude ? 0x8000000000000000ull : 0) ^ (uint64_t(first.primaryCull)<<61) ^
                       (first.cameraHidden ? 0x5bd1e9955bd1e995ull : 0);
                x.flags = o->Dynamic() ? rtc::kSkinned : rtc::kRigid;
                if (first.noOcclude) x.flags |= rtc::kNoOcclude;
                if (first.cameraHidden) x.flags |= rtc::kCameraHidden;
                if(first.primaryCull==1)x.flags|=rtc::kPrimaryCullBack;
                if(first.primaryCull==2)x.flags|=rtc::kPrimaryCullFront;
                if(o->flags&RT_DRAW_SHADOW_ONLY)x.flags|=rtc::kShadowOnly;
                if(o->flags&RT_DRAW_CAMERA_ONLY)x.flags|=rtc::kCameraOnly;  // 0.23.5
                if(o->flags&RT_DRAW_NO_CAST)x.flags|=rtc::kNoCast;  // API 43
                x.lightLink = o->lightLink;
                x.position = {o->vertices.r12.Get(), 0, o->stride, rtc::VertexFormat::Float3};
                if (o->normals.r12) x.normal = {o->normals.r12.Get(), 0, 12, rtc::VertexFormat::Float3};
                else if(o->pathNormalOffset!=UINT32_MAX && o->pathNormalOffset+12<=o->stride)
                    x.normal = {o->vertices.r12.Get(), o->pathNormalOffset, o->stride, rtc::VertexFormat::Float3};
                if(o->tangentSource) {
                    auto *buffer=o->tangentSource.Get()==o->vertexSource.Get()?o->vertices.r12.Get():o->tangents.r12.Get();
                    x.tangent={buffer,o->tangentOffset,o->tangentStride,(rtc::VertexFormat)o->tangentFormat};
                }
                if (o->uvSource == o->vertexSource.Get() && o->uvSource) {
                    x.uv = {o->vertices.r12.Get(), o->uvOffset, o->uvStride, o->uvFormat};
                } else if (o->uvSource) {
                    auto uc = g.indices.find(o->uvSource);
                    if (uc != g.indices.end() && uc->second.ready && uc->second.copy.r12)
                        x.uv = {uc->second.copy.r12.Get(), o->uvOffset, o->uvStride, o->uvFormat};
                }
                x.indexBuffer = ic.copy.r12.Get();
                x.indexOffset = 0;
                x.index32 = first.i32 ? 1 : 0;
                x.baseVertex = first.base;
                memcpy(x.matrix, o->matrix, sizeof x.matrix);
                x.vertexCount = o->vertexCount;
                if(o->weights.r12 && !o->boneMatrices.empty()) {
                    x.skinWeights=o->weights.r12.Get();x.skinMatrices=o->boneMatrices.data();
                    x.boneCount=o->boneCount;x.blendFrameCount=o->blendFrameCount;
                }
                x.submeshes = g.submeshes.data() + from;
                x.submeshCount = (uint32_t)(g.submeshes.size() - from);
                g.geometries.push_back(x);
            }
            k = e;
        }
    }
    g.rtLights.clear();
    for (int i = 0; i < h->lights; i++) {
        const RtLightRec &r = lights[i];
        rtc::Light L;
        L.id = r.id;
        L.type = (rtc::LightType)std::min<uint32_t>(r.type, 2);
        memcpy(L.position, r.position, 12);
        memcpy(L.direction, r.direction, 12);
        memcpy(L.color, r.color, 12);
        L.range = r.range;
        L.spotCosOuter = r.spotCos;
        L.radius = std::max(r.radius,0.f);
        L.angularRadius = std::max(r.angular,0.f);
        L.shadowStrength = ((r.shadows >> 8) & 255u) / 255.f;
        L.shadowNear = ((r.shadows >> 16) & 4095u) / 1024.f;
        L.unityShadows = (r.shadows >> 28) & 1u ? 0u : 1u;
        L.excludedLinks = r.excludedLinks;
        g.rtLights.push_back(L);
    }
    rtc::Frame f;
    f.frame = g.fenceValue + 1;
    f.completedFrame = g.fence->GetCompletedValue();
    if (f.completedFrame == UINT64_MAX) { Fail("device removed", DXGI_ERROR_DEVICE_REMOVED); l->Close(); return; }
    // Core frame N is signaled BEFORE compositor PT N. On the common queue,
    // core N completion only proves that PT N-1 has finished. Include that tail
    // when retiring RtCore descriptors, resources and upload-ring slices.
    if(g.pathRequested && f.completedFrame) --f.completedFrame;
    f.geometries = g.geometries.data();
    f.geometryCount = (uint32_t)g.geometries.size();
    f.lights = g.rtLights.data();
    f.lightCount = (uint32_t)g.rtLights.size();
    memcpy(f.ambientSh, h->sh, sizeof f.ambientSh);
    for (int k = 0; k < 27; k++) f.skySh[k] = std::isfinite(h->skySh[k]) ? h->skySh[k] : 0.0f;
    f.ambientLM = std::isfinite(h->ambientLM) ? h->ambientLM : 1.0f;
    f.exposure = std::isfinite(h->exposure) ? h->exposure : 1.0f;
    f.timestampFrequency = g.timestampFrequency;
    const bool recorded = rtc::Record(l, f);
    // back to COMMON: what D3D11 takes again
    for (Wrapped *w : cycling) Barrier(bars, w->r12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (!bars.empty()) l->ResourceBarrier((UINT)bars.size(), bars.data());
    if (g.queries) {
        l->EndQuery(g.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * si + 1);
        l->ResolveQueryData(g.queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * si, 2, g.readback.Get(), 16ull * si);
        s.timed = true;
    }
    hr = l->Close();
    if (FAILED(hr)) { Fail("command list close", hr); return; }
    ID3D12CommandList *lists[] = {l};
    g.queue->ExecuteCommandLists(1, lists);
    s.done = ++g.fenceValue;
    hr = g.queue->Signal(g.fence.Get(), s.done);
    if (FAILED(hr)) { Fail("signal", hr); return; }
    g.next = (g.next + 1) % kSlots;

    const double ms = NowMs() - t0;
    std::lock_guard<std::mutex> lk(gLock);
    g.status.frames++;
    g.status.records = h->draws;
    g.status.owners = (int)g.owners.size();
    g.status.uvOwners = g.status.noOccludeOwners = 0;
    for (Owner *o : live) g.status.uvOwners += o->uvSource != nullptr, g.status.noOccludeOwners += std::any_of(o->subs.begin(), o->subs.end(), [](const Sub &x) { return x.noOcclude; });
    {
        std::string a;
        for (Owner *o : live) {
            if (!o->alpha || a.size() > 440) continue;
            char t[96];
            snprintf(t, sizeof t, "%s%u/%llu/%u/%u%s", a.empty() ? "" : " ", o->vertexCount, (unsigned long long)o->uvBytes, o->uvOffset,
                     o->uvFound, o->uvSource ? "" : "-");
            a += t;
        }
        snprintf(g.status.alphaUv, sizeof g.status.alphaUv, "%s", a.c_str());
    }
    g.status.geometries = (int)g.geometries.size();
    g.status.receivers = (int)g.allReceivers.size();
    g.status.lights = h->lights;
    g.status.materials = (int)g.materials.size();
    g.status.pathTracing=g.pathThisFrame?1:0;
    g.status.pathCalls=g.pathCalls;
    memcpy(g.status.pathJitter,g.pathLastJitter,sizeof g.status.pathJitter);
    g.status.pathTextures=(int)g.pathTextures.size();
    g.status.pathMaterials=0;
    rtc::GetPathHairStatus(g.status.pathHairSegments,g.status.pathHairBytes);
    rtc::GetPathLightingStatus(g.status.pathEmitters,g.status.pathLightingBytes);
    g.status.pathSer=rtc::PathUsesSer()?1u:0u;
    g.status.pathHairBytes+=g.pathHairCopy.bytes+g.pathHairBuildBytes;
    g.status.pathEyeRigs=g.pathEyeCount;
    for(auto &kv:g.materials)g.status.pathMaterials+=kv.second.pathBound?1:0;
    g.status.indexCopies = (int)g.indices.size();
    g.status.copiedBytes = copied;
    if (!recorded) {
        rtc::Status cs = rtc::GetStatus();
        snprintf(g.status.lastError, sizeof g.status.lastError, "RtCore: %s %s", cs.reason, cs.lastError);
        g.status.failures++;
    }
    g.cpuSum += ms - waited;
    g.waitSum += waited;
    g.cpuSamples++;
    g.status.cpuMs = g.cpuSum / g.cpuSamples;
    g.status.waitMs = g.waitSum / g.cpuSamples;
}

void RtRelease() {
    gPathCollect=false;
    gPathRendering=false;
    using HairMode=void(__cdecl*)(int);
    auto mode=(HairMode)GetProcAddress(GetModuleHandleW(L"VaMHairOnePassNative.bin"),"HOP_PathModeV1");
    if(mode)mode(0);
    if(g.pathFence && g.pathFenceValue && g.pathFence->GetCompletedValue()<g.pathFenceValue) {
        g.pathFence->SetEventOnCompletion(g.pathFenceValue,g.event); WaitForSingleObject(g.event,INFINITE);
    }
    for(auto &kv:g.pathTextures)PathForget(kv.second);
    gPathGraves.clear();
    PathUnhook();
    if (g.fence && g.fenceValue) WaitCpu(g.fenceValue);
    g.graves.clear();
    if (g.ok) rtc::Shutdown();
    if (g.event) CloseHandle(g.event);
    RtHostStatus status;
    std::vector<RtReceiver *> doomed;
    {
        std::lock_guard<std::mutex> l(gLock);
        status = g.status;
        doomed.swap(g.doomed);
    }
    for (RtReceiver *r : doomed) delete r;
    status.available = 0;
    snprintf(status.reason, sizeof status.reason, "released");
    std::vector<RtReceiver *> keep = g.allReceivers;  // the plugin's entries still point at them; they stay inert
    g = Host();
    g.tried = true;  // not again on this device
    g.allReceivers = keep;
    std::lock_guard<std::mutex> l(gLock);
    g.status = status;
}

RtHostStatus RtGetStatus() {
    std::lock_guard<std::mutex> l(gLock);
    return g.status;
}

void RtResetTiming() {
    {
        std::lock_guard<std::mutex> l(gLock);
        g.gpuSum = g.cpuSum = g.waitSum = 0;
        g.cpuSamples = 0;
        g.status.samples = 0;
        g.status.gpuMs = g.status.cpuMs = g.status.waitMs = 0;
    }
    if (g.ok) rtc::ResetTiming();
}

void RtSetReconstruction(bool enabled) { g.reconstruction = enabled;if(!enabled)gPathConsumerReady=false; }
bool RtPathSelected() { return g.pathRequested && g.ok && gPathSelected.load(); }
bool RtPathRendering() {return g.ok && gPathRendering.load();}
int RtPathRasterKind(ID3D11Buffer *indices,uint32_t first,uint32_t count,int32_t base,uint64_t program,uint64_t *watched) {
    if(watched)*watched=0;
    if(!g.ok || !g.pathThisFrame)return 0;
    auto i=g.pathRasterBuffers.find(indices);if(i==g.pathRasterBuffers.end())return 0;
    int kind=-1;bool conflict=false;
    for(const auto& r:i->second) {
        if(r.kind==2)return 2;
        if(r.first!=first||r.count!=count||r.base!=base)continue;
        if(program && !PathProgramInSet(r.programSet,program))continue;
        if(kind<0)kind=r.kind;else if(kind!=r.kind)conflict=true;
        if(watched && r.watched)*watched=r.watched;
    }
    if(kind<0)return 0;
    if(!program && (conflict||kind==4))return -1; // resolve with the actual PS
    return conflict?0:kind;
}
void RtPathDrawDiagnostic(ID3D11DeviceContext* context,uint32_t count,uint32_t first,int32_t base,uint64_t program,uint32_t instances) {
    // Explicit test-run diagnostics only, once when the strict reconciler fails.
    wchar_t directory[MAX_PATH]={};if(!GetEnvironmentVariableW(L"VAM_PT_DUMP",directory,MAX_PATH))return;
    wchar_t file[MAX_PATH];swprintf_s(file,L"%s/unknown-draw-%d.json",directory,g.lastFrame);
    FILE* f=_wfopen(file,L"wb");if(!f)return;
    ID3D11Buffer* indices=nullptr;DXGI_FORMAT format;UINT offset;
    context->IAGetIndexBuffer(&indices,&format,&offset);
    LARGE_INTEGER ticks,frequency;QueryPerformanceCounter(&ticks);QueryPerformanceFrequency(&frequency);
    fprintf(f,"{\"frame\":%d,\"qpc_s\":%.9f,\"instances\":%u,\"index_buffer\":\"%llx\",\"format\":%u,\"offset\":%u,\"first\":%u,\"count\":%u,\"base\":%d,\"program\":\"%016llx\",\"owners\":[",
        g.lastFrame,double(ticks.QuadPart)/frequency.QuadPart,instances,(unsigned long long)(uintptr_t)indices,unsigned(format),offset,first,count,base,(unsigned long long)program);
    bool comma=false;
    for(const auto& kv:g.owners) {
        const auto& owner=kv.second;if(owner.lastSeen!=g.lastFrame)continue;
        for(const auto& sub:owner.subs) {
            fprintf(f,"%s{\"owner\":%lld,\"index_buffer\":\"%llx\",\"first\":%u,\"count\":%u,\"base\":%d,\"material\":%u}",
                comma?",":"",(long long)owner.id,(unsigned long long)(uintptr_t)sub.indices,sub.start,sub.count,sub.base,sub.material);comma=true;
        }
    }
    fprintf(f,"],\"registered\":[");comma=false;
    auto found=g.pathRasterBuffers.find(indices);
    if(found!=g.pathRasterBuffers.end())for(const auto& range:found->second) {
        fprintf(f,"%s{\"first\":%u,\"count\":%u,\"base\":%d,\"kind\":%d,\"program_set\":%u}",comma?",":"",range.first,range.count,range.base,range.kind,range.programSet);comma=true;
    }
    fprintf(f,"]}\n");fclose(f);if(indices)indices->Release();
}
void RtSetPathBounces(int bounces) { g.pathBounces = (uint32_t)std::min(std::max(bounces, 0), 3); }
void RtSetPathAuthor(uint32_t rules,float ambient,float contact,float ambientRange,float emitter,float backgroundNear,float backgroundFar) {
    g.authorRules=rules;g.authorAmbient=ambient;g.authorContact=contact;g.authorAmbientRange=ambientRange;g.authorEmitter=emitter;
    g.backgroundNear=backgroundNear;g.backgroundFar=backgroundFar;
}

void RtSetTest(unsigned layers) {
    if (!g.ok) return;
    rtc::SetTestLayers(layers);
}

namespace {
const GUID kPathTextureFormat={0x5b8ab339,0x137c,0x48fe,{0xb3,0xdc,0x4f,0xc8,0x99,0x6a,0x4b,0x80}};
DXGI_FORMAT PathFormat(DXGI_FORMAT f,bool color) {
    switch(f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:return color?DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:return color?DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_BC1_TYPELESS:return color?DXGI_FORMAT_BC1_UNORM_SRGB:DXGI_FORMAT_BC1_UNORM;
    case DXGI_FORMAT_BC2_TYPELESS:return color?DXGI_FORMAT_BC2_UNORM_SRGB:DXGI_FORMAT_BC2_UNORM;
    case DXGI_FORMAT_BC3_TYPELESS:return color?DXGI_FORMAT_BC3_UNORM_SRGB:DXGI_FORMAT_BC3_UNORM;
    case DXGI_FORMAT_BC4_TYPELESS:return DXGI_FORMAT_BC4_UNORM;
    case DXGI_FORMAT_BC5_TYPELESS:return DXGI_FORMAT_BC5_UNORM;
    case DXGI_FORMAT_BC6H_TYPELESS:return DXGI_FORMAT_BC6H_UF16;
    case DXGI_FORMAT_BC7_TYPELESS:return color?DXGI_FORMAT_BC7_UNORM_SRGB:DXGI_FORMAT_BC7_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default:return f;
    }
}
}
void RtRememberTextureFormat(ID3D11Resource *texture,uint32_t format) {
    UINT bytes=sizeof(uint32_t);uint32_t old=0;
    if(format && FAILED(texture->GetPrivateData(kPathTextureFormat,&bytes,&old)))
        texture->SetPrivateData(kPathTextureFormat,sizeof format,&format);
}

// Experimental compositor bridge. Resources are unwrapped and returned on the
// common 11on12 queue; there are no full-resolution duplicate texture copies.
// Material textures D3D11 cannot render into have their own D3D12 copy (path_textures.inl).
extern "C" __declspec(dllexport) int __cdecl ST_PathTracingEnabledV1() {
    return g.ok && g.pathThisFrame && g.pathOn12 && g.pathFence ? 1 : 0;
}
extern "C" __declspec(dllexport) void __cdecl ST_SetPathTracingV1(int enabled) {
    gPathSelected=enabled!=0;
    if(enabled)gPathSuspendUntil=INT_MIN; // a fresh request retries a paused PT now
    if(!enabled)gPathCollect=false;
}
extern "C" __declspec(dllexport) int __cdecl ST_PathSceneActiveV1() { return gPathCollect.load()?1:0; }
extern "C" __declspec(dllexport) int __cdecl ST_PathRenderingActiveV1() { return gPathRendering.load()?1:0; }
// The live selection (the compositor reads it every frame: RR F for the PT, else SR); 0 on a device that cannot
// path trace (RtInit done without the PT bridge).
extern "C" __declspec(dllexport) int __cdecl ST_PathTracingRequestedV1() { return gPathSelected.load() && (!g.tried || (g.ok && g.pathRequested))?1:0; }
extern "C" __declspec(dllexport) void __cdecl ST_SetPathConsumerReadyV1(int ready) {gPathConsumerReady=ready!=0;}
extern "C" __declspec(dllexport) uint64_t __cdecl ST_PathSceneRevisionV1() { return gSceneRevision.load(); }
extern "C" __declspec(dllexport) int __cdecl ST_PathFrameValidV1() {return !gPathFrameInvalid;}
// 0.22.62: the next one-time PT note (bytes written, 0: none); the plugin logs each.
extern "C" __declspec(dllexport) int __cdecl ST_PathNoticeV1(char *buffer,int size) {
    if(!buffer || size<2)return 0;
    std::lock_guard<std::mutex> l(gNoticeLock);
    if(gNotices.empty())return 0;
    const int n=std::min<int>(int(gNotices.front().size()),size-1);
    memcpy(buffer,gNotices.front().data(),n);buffer[n]=0;gNotices.erase(gNotices.begin());
    return n;
}
void RtPathNotice(const char *text) { PathNotice("%s",text); }
// Explicit diagnostic, after this frame's material texture bindings are ready.
// The first loading frame is not a valid substitute for the captured scene.
extern "C" __declspec(dllexport) int __cdecl ST_WritePathSceneV1(const wchar_t* file,uint64_t serial) {
    if(!file || !g.pathThisFrame)return 0;
    FILE* f=_wfopen(file,L"wb");if(!f)return 0;
    auto array=[&](const char* name,const float* values,unsigned count) {
        fprintf(f,",\"%s\":[",name);for(unsigned i=0;i<count;i++)fprintf(f,"%s%.9g",i?",":"",values[i]);fprintf(f,"]");
    };
    fprintf(f,"{\"serial\":%llu,\"revision\":%llu,\"content_revision\":%llu,\"geometries\":[",(unsigned long long)serial,(unsigned long long)gSceneRevision.load(),(unsigned long long)gContentRevision);
    unsigned gi=0;
    for(const auto& geo:g.geometries) {
        fprintf(f,"%s{\"instance\":%u,\"id\":\"%llu\",\"flags\":%u,\"position_stride\":%u,\"normal_stride\":%u,\"uv_stride\":%u",
            gi?",":"",gi,(unsigned long long)geo.id,geo.flags,geo.position.stride,geo.normal.stride,geo.uv.stride);
        array("matrix",geo.matrix,12);fprintf(f,",\"materials\":[");
        for(unsigned k=0;k<geo.submeshCount;k++)fprintf(f,"%s%u",k?",":"",geo.submeshes[k].material);
        fprintf(f,"]}");gi++;
    }
    fprintf(f,"],\"materials\":[");bool first=true;
    for(const auto& kv:g.materials) {
        const auto& material=kv.second;if(material.lastSeen!=g.lastFrame)continue;
        const auto& m=material.path;
        fprintf(f,"%s{\"id\":%u,\"key\":\"%llu\",\"source_key\":\"%llu\",\"role\":%u,\"flags\":%u,\"roughness\":%g,\"ior\":%g,\"transmission\":%g,\"metallic\":%g",
            first?"":",",material.id,(unsigned long long)kv.first,(unsigned long long)(kv.first&0x00ffffffffffffffull),unsigned(material.role),m.flags,m.roughness,m.ior,m.transmission,m.metallic);
        array("base",m.baseColor,4);array("specular",m.specular,3);array("emission",m.emissionColor,3);
        array("gloss",m.glossParams,4);array("skin",m.skin,28);array("bump",m.bump,4);array("controls",m.materialControls,4);
        fprintf(f,",\"formats\":[");for(unsigned k=0;k<8;k++)fprintf(f,"%s%u",k?",":"",m.formats[k]);
        fprintf(f,"],\"mode\":%u,\"second\":%u",m.mode,m.secondFormat);array("direct_gain",m.directGain,4);array("second_st",m.secondST,4);
        fprintf(f,"}");first=false;
    }
    fprintf(f,"],\"lights\":[");
    for(size_t i=0;i<g.rtLights.size();i++) {
        const auto& light=g.rtLights[i];
        fprintf(f,"%s{\"id\":%llu,\"type\":%u,\"radius\":%g,\"angular_radius\":%g,\"range\":%g,\"spot_cos\":%g",i?",":"",(unsigned long long)light.id,unsigned(light.type),light.radius,light.angularRadius,light.range,light.spotCosOuter);
        array("position",light.position,3);array("direction",light.direction,3);array("color",light.color,3);fprintf(f,"}");
    }
    fprintf(f,"],\"environment\":{\"lighting_scale\":%g,\"diffuse_scale\":%g,\"background_scale\":%g",g.pathSky.lightingScale,g.pathSky.diffuseScale,g.pathSky.backgroundScale);
    array("background_color",g.pathSky.backgroundColor,3);array("background_to_sky",g.pathSky.backgroundToSky,12);array("world_to_sky",g.pathSky.worldToSky,12);
    fprintf(f,"},\"hair\":[");
    for(uint32_t i=0;i<g.pathHairDiagnosticCount;i++) {
        const auto& material=g.pathHairDiagnostic[i];
        fprintf(f,"%s{\"guide_id\":%d,\"strands\":%u,\"segments\":%u,\"roughness\":%g",i?",":"",-1-int(i),material.strands,material.segments,material.roughness);
        array("reflection",material.reflection,3);
        if(i<g.pathHairRegistry.size())array("authored_specular",g.pathHairRegistry[i].specularColor,3);
        fprintf(f,"}");
    }
    fprintf(f,"]}\n");int error=ferror(f);fclose(f);return error?0:1;
}
static int RecordPathTraceWithResponse(ID3D12GraphicsCommandList *base,const rtc::PathView *view,const rtc::PathResponsivity *response) {
    gPathRecordedEye=-1;
    if(gPathFrameInvalid)return -1;
    if(!ST_PathTracingEnabledV1() || !base || !view)return 0;
    ComPtr<ID3D12GraphicsCommandList4> list;
    if(FAILED(base->QueryInterface(IID_PPV_ARGS(&list))))return -1;
    for(auto &kv:g.materials) {
        Material &m=kv.second;
        if(m.lastSeen!=g.lastFrame)continue;
        ID3D12Resource **dest[]={&m.path.base,&m.path.normal,&m.path.surface,&m.path.emission,&m.path.gloss,&m.path.detailNormal,&m.path.detailMask,&m.path.alpha,&m.path.second};
        for(int k=0;k<9;k++) {
            auto *source=m.pathSource[k].Get();
            if(!source)continue;
            PathTexture &t=g.pathTextures[source];t.source=source;t.lastSeen=g.lastFrame;
            ID3D12Resource *resource=PathTextureFor(list.Get(),t);
            if(!resource)return -1;
            if(*dest[k]!=resource)m.pathBound=false;
            *dest[k]=resource;
            if(k==7) {
                // RtCore's PT micromaps read the alpha map while this stays (path_omm.inl): our copy as of the writes
                // it was copied after; 0 for a borrowed original, which D3D11 may write any frame
                const uint64_t version=resource==t.copy.Get()?t.copiedWrites+1:0;
                if(m.path.alphaVersion!=version){m.path.alphaVersion=version;m.pathBound=false;}
            }
            uint32_t &slotFormat=k<8?m.path.formats[k]:m.path.secondFormat;
            if(!m.pathBound || !slotFormat) {
                uint32_t format=0;UINT bytes=sizeof format;
                if(FAILED(source->GetPrivateData(kPathTextureFormat,&bytes,&format)) || !format)
                    format=(uint32_t)PathFormat(t.underlying->GetDesc().Format,k==0||k==3||k==8);
                slotFormat=format;
            }
        }
        if(!m.pathBound) {
            if(!rtc::SetPathMaterial(m.id,m.path))return -1;
            m.pathBound=true;
        }
    }
    ID3D12Resource **cubes[]={&g.pathSky.lighting,&g.pathSky.background,&g.pathSky.indoor,&g.pathSky.probe};
    for(int k=0;k<4;k++) {
        *cubes[k]=nullptr;
        auto *source=g.pathEnvironment[k].Get();
        if(!source)continue;
        PathTexture &t=g.pathTextures[source];t.source=source;t.lastSeen=g.lastFrame;
        *cubes[k]=PathTextureFor(list.Get(),t);
        if(!*cubes[k])return -1;
        g.pathSky.formats[k]=(uint32_t)PathFormat(t.underlying->GetDesc().Format,false);
    }
    if(!rtc::SetPathEnvironment(g.pathSky))return -1;
    PathHairItem hairItems[64];PathHairSource hair;hair.items=hairItems;hair.capacity=64;
    using HairBuild=int(__cdecl*)(const PathHairBuildItem*,uint32_t,uint64_t,PathHairSource*);
    auto hairBuild=(HairBuild)GetProcAddress(GetModuleHandleW(L"VaMHairOnePassNative.bin"),"HOP_BuildPathHairV2");
    if(!g.pathHairRegistry.empty() && !hairBuild)return -1;
    if(hairBuild && hairBuild(g.pathHairRegistry.data(),(uint32_t)g.pathHairRegistry.size(),(uint64_t)g.lastFrame,&hair)<0)return -1;
    for(uint32_t i=0;i<hair.count && i<g.pathHairLinks.size();i++)hairItems[i].pad=g.pathHairLinks[i];  // HOP keeps the registry's order
    static const bool diagnostic=[](){return GetEnvironmentVariableW(L"VAM_PT_DUMP",nullptr,0)!=0;}();
    if(diagnostic) {g.pathHairDiagnosticCount=hair.count;memcpy(g.pathHairDiagnostic,hairItems,hair.count*sizeof(PathHairItem));}
    g.pathHairBuildBytes=hair.allocationBytes;
    ID3D12Resource *hairResource=nullptr;
    bool copiedHair=false;
    if(hair.points && hair.count) {
        // UnwrapUnderlyingResource does not support D3D11 buffers. Copy only
        // once per frame into the existing shared-queue wrapped-buffer scheme.
        ComPtr<ID3D11Buffer> source;
        if(FAILED(hair.points->QueryInterface(IID_PPV_ARGS(&source))))return -1;
        if(g.pathHairCopy.bytes!=BufferBytes(source.Get())) {
            BuryWrapped(g.pathHairCopy);g.pathHairCopiedFrame=INT_MIN;
            if(!WrapCopy(g.pathHairCopy,source.Get()))return -1;
        }
        if(g.pathHairCopiedFrame!=g.lastFrame) {
            ComPtr<ID3D11DeviceContext> context;g.d11->GetImmediateContext(&context);
            ID3D11Resource *wrapped=g.pathHairCopy.r11.Get();g.on12->AcquireWrappedResources(&wrapped,1);
            context->CopyResource(wrapped,source.Get());g.on12->ReleaseWrappedResources(&wrapped,1);context->Flush();
            g.pathHairCopiedFrame=g.lastFrame;copiedHair=true;
        }
        hairResource=g.pathHairCopy.r12.Get();
    } else if(g.pathHairCopy.r12) {
        BuryWrapped(g.pathHairCopy);g.pathHairCopiedFrame=INT_MIN;
    }
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    barriers.reserve(g.pathTextures.size());
    for(auto &kv:g.pathTextures)if(kv.second.taken)
        Barrier(barriers,kv.second.underlying.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if(hairResource && !copiedHair)Barrier(barriers,hairResource,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if(!barriers.empty())list->ResourceBarrier((UINT)barriers.size(),barriers.data());
    rtc::SetPathEyeRigs(g.pathEyeCount?g.pathEyeCopy.r12.Get():nullptr,g.pathEyeCount);
    bool ok=rtc::RecordPathHair(list.Get(),hairResource,hairItems,hair.count,hair.serial) && rtc::RecordPathTrace(list.Get(),*view,response);
    for(auto &b:barriers)std::swap(b.Transition.StateBefore,b.Transition.StateAfter);
    if(hairResource && copiedHair)Barrier(barriers,hairResource,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    if(!barriers.empty())list->ResourceBarrier((UINT)barriers.size(),barriers.data());
    if(ok){g.pathCalls++;memcpy(g.pathLastJitter,view->jitter,sizeof g.pathLastJitter);gPathRecordedEye=(int)view->eye;}
    return ok?1:-1;
}
extern "C" __declspec(dllexport) int __cdecl ST_RecordPathTraceV1(ID3D12GraphicsCommandList *base,const rtc::PathView *view) {
    return RecordPathTraceWithResponse(base,view,nullptr);
}
extern "C" __declspec(dllexport) int __cdecl ST_RecordPathTraceResponsiveV1(ID3D12GraphicsCommandList *base,const rtc::PathView *view,const rtc::PathResponsivity *response) {
    return RecordPathTraceWithResponse(base,view,response);
}
extern "C" __declspec(dllexport) int __cdecl ST_FinishPathTraceV1() {
    if(!g.pathFence || !g.pathOn12)return 0;
    uint64_t value=++g.pathFenceValue;
    HRESULT hr=g.queue->Signal(g.pathFence.Get(),value);
    if(FAILED(hr)){Fail("PT completion signal",hr);return -1;}
    // The compositor records the right eye next, with no D3D11 use of these textures in between (vam_rr.inl). A
    // second finish (its exception path) or one after a failed record returns them.
    if(VamViewCount()==2 && gPathRecordedEye==0 && !gPathFinishDeferred){gPathFinishDeferred=true;return 1;}
    gPathRecordedEye=-1;
    int borrowed=0;
    for(auto &kv:g.pathTextures)borrowed+=kv.second.taken;
    if(!ReturnPathTextures())return -1;
    PathTextureLog(borrowed);
    for(auto it=g.pathTextures.begin();it!=g.pathTextures.end();) {
        if(g.lastFrame-it->second.lastSeen>kEvictFrames){PathForget(it->second);it=g.pathTextures.erase(it);}else ++it;
    }
    return 1;
}
