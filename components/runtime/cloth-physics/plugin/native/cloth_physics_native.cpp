// VaMClothPhysicsNative.dll: GPUTools cloth physics on the fused solver (solver\cloth_fused.hlsl) inside VaM
// (docs\设计.md). The C# plugin (plugin\VaMClothPhysics.cs) takes over ClothPhysicsWorld.DispatchPhysicsImpl for the
// items it hands to the solver: at each item's own physics step it queues the step (the item's GPUTools buffers and
// DispatchPhysicsImpl's values), and at the end of the physics step (WaitForFixedUpdate, or before anything else
// touches a queued item's inputs or results) a solve job runs every queued step with the items' dispatches interleaved
// phase by phase: import all, then per outer step iterate all, collide all. The dispatches of different items do not
// depend on each other, so the GPU runs them side by side. On D3D11On12 (VaM with the D3DInterop proxy) that needs a
// Flush after every phase: D3D11On12 puts a UAV barrier before a dispatch whose UAVs were used earlier in the same
// command list, which would make each item wait for the one before; a Flush starts a new command list, and the command
// list boundary orders the phases. Jobs run on Unity's render thread (GL.IssuePluginEvent), in command order between
// Unity's own work, on Unity's D3D11 immediate context.
//
// The solver reads and writes GPUTools' particle and point joint buffers directly (UAVs on them) and reads the skin-wrap
// targets and colliders from GPUTools' buffers; its own buffers per item are the packed joints, the colour groups and
// the working state X / S. Compute bindings are cleared after each job; the C# side calls GL.InvalidateState.
//
// An item in device memory iterates in one thread group, one colour group after another; with large colour groups that
// one multiprocessor is the whole solve's critical path. The C# side gives such items a group count (CP_CreateItem's
// multiGroups): CSIterateMulti spreads their joints over that many thread groups, with a counter in the item's Sync
// buffer as the barrier between colour groups. Same joints, same order of the writes to each particle, so the same
// result bit for bit. It needs every group of the dispatch resident at once: one solve gives at most kMultiCap groups
// to multi items (the rest iterate in one group, same result), and a wait that never ends gives up in the shader and
// sets a flag in Sync, which this library reads back now and then; the item then iterates in one group for good.
//
// Diagnostics (VaMClothPhysics' Diagnostics setting): GPU timestamps around GPUTools' cloth steps and the solves, and an
// in-game check of one step of an item against GPUTools (both start from the same state).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d11on12.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// fxc /Fh (native\build.cmd): g_CSImport, g_CSIterateShared, g_CSIterate, g_CSIterateMulti, g_CSCollide
#include "cloth_fused_import.h"
#include "cloth_fused_iterate_shared.h"
#include "cloth_fused_iterate.h"
#include "cloth_fused_iterate_multi.h"
#include "cloth_fused_collide.h"

#define CP_API extern "C" __declspec(dllexport)

static const int kApiVersion = 4;
static const uint32_t kMaxShared = 2720;      // MAX_SHARED in cloth_fused.hlsl
static const uint32_t kMaxParticles = 65536;  // joints pack particle indices in 16 bits
static const uint32_t kMaxMulti = 16;         // thread groups per multi item
static const uint32_t kMultiCap = 32;         // thread groups of all multi items in one solve (RTX 5090 D: 170 SMs, one group each)
static const int kSyncEvery = 8;              // a multi item's gave-up flag is read back every this many of its solves

// One physics step of one item, as the C# side fills it (VaMClothPhysics.CPStep, Pack = 8).
#pragma pack(push, 8)
struct CPStep {
    ID3D11Buffer* particles;        // GPParticle, 116 B
    ID3D11Buffer* pointJoints;      // GPPointJoint, 24 B, particle i's joint at i
    ID3D11Buffer* cur;              // PreCalculatedPositions, float3
    ID3D11Buffer* old;              // OldPreCalculatedPositions, float3
    ID3D11Buffer* spheres;          // GPSphereWithDelta, 32 B (null = none)
    ID3D11Buffer* lineSpheres;      // GPLineSphereWithDelta, 72 B (null = none)
    ID3D11Buffer* planes;           // float4 (null = none)
    ID3D11Buffer* grab;             // GPGrabSphere, 24 B (null = none)
    int32_t sphereCount, lineSphereCount, planeCount, grabCount;
    int32_t iterations, outer, doIntegrate, doCollision;
    int32_t doStiffness, reserved;  // doStiffness: data.Stiffness > 0 (GPUTools runs the joint kernels)
    float DT, accelDT2[3];
    float invDrag, stiffness, compression, distanceScale;
    float jointStrength, jointPrediction, breakThreshold, friction;
    float staticFriction, collisionPower, dtrecipInner, stepInner;
    float dtrecipOuter, stepOuter;
};
#pragma pack(pop)
static_assert(sizeof(CPStep) == 176, "CPStep layout");

// cbuffer Params of cloth_fused.hlsl
struct FusedParams {
    uint32_t particleCount, groupCount, iterations, doIntegrate;
    uint32_t doCollision, sphereCount, lineSphereCount, planeCount;
    uint32_t grabCount, multiGroups, pad1, pad2;
    float DT, dtrecipInner, stepInner, invDrag;
    float accelDT2[3], stiffness;
    float compression, distanceScale, jointStrength, jointPrediction;
    float breakThreshold, friction, staticFriction, collisionPower;
    float dtrecipOuter, stepOuter, pad3, pad4;
};
static_assert(sizeof(FusedParams) == 128, "FusedParams layout");

// ---------------------------------------------------------------- errors

enum CPError {
    kOk = 0,
    kErrDevice = -1,       // no device / shader
    kErrBuffer = -2,       // a GPUTools buffer has an unexpected layout
    kErrUnsupported = -3,  // the item's data breaks a rule of the fused solver (CP_GetLastError says which)
    kErrResource = -4,     // creating a buffer or view failed
    kErrDeviceMismatch = -5,
};

static std::mutex gLock;
static std::string gLastError;

static void setLastError(const char* fmt, ...) {
    char buf[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof buf, fmt, a);
    va_end(a);
    std::lock_guard<std::mutex> l(gLock);
    gLastError = buf;
}

template <class T> static void safeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------- items

struct Verify {                     // check results, accumulated
    float lastMaxPos = 0, lastMeanPos = 0, lastMaxVel = 0;
    float worstPos = 0, worstVel = 0;
    double sumMeanPos = 0;
    int lastEqual = 0, nonFinite = 0, compares = 0;
};

// A view on one of GPUTools' buffers, recreated when the buffer changes (the view keeps the buffer alive).
struct ExtView {
    ID3D11Buffer* buffer = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    void release() { safeRelease(srv); safeRelease(uav); buffer = nullptr; }
};

struct Item {
    int id = 0;
    uint32_t n = 0;
    // until the add job ran: GPUTools' joint buffers and their colour groups ((start, count) as GroupsData lists them)
    ID3D11Buffer *stiffJoints = nullptr, *nearJoints = nullptr, *pointJointsAtAdd = nullptr;
    std::vector<int32_t> stiffGroups, nearGroups;
    // the solver's own buffers
    bool ready = false, shared = false;
    uint32_t groupCount = 0;
    ID3D11Buffer *joints = nullptr, *groups = nullptr, *X = nullptr, *S = nullptr, *cb = nullptr;
    ID3D11ShaderResourceView *jointsSrv = nullptr, *groupsSrv = nullptr;
    ID3D11UnorderedAccessView *xUav = nullptr, *sUav = nullptr;
    ExtView particles, pointJoints, cur, old, spheres, lineSpheres, planes, grab;
    // multi: thread groups for CSIterateMulti (0 = one group); Sync: arrivals, groups done, gave up (uint each)
    uint32_t multi = 0, runMulti = 0;           // runMulti: this solve's group count (0 = one group)
    ID3D11Buffer *sync = nullptr, *syncStage = nullptr;
    ID3D11UnorderedAccessView* syncUav = nullptr;
    int syncCheck = kSyncEvery;                  // solves until the next read-back; -1: a copy waits to be read
    std::atomic<bool> gaveUp{false};
    std::atomic<int> multiSolves{0};
    // queued step (render thread)
    bool queued = false;
    int outer = 1;
    // diagnostics
    ID3D11Buffer *snapP = nullptr, *snapJ = nullptr, *refP = nullptr, *refJ = nullptr, *stageA = nullptr, *stageB = nullptr;
    Verify verify;
    std::atomic<int> error{0};
    std::atomic<int> steps{0};
    ~Item() {
        for (ID3D11Buffer** b : {&stiffJoints, &nearJoints, &pointJointsAtAdd, &joints, &groups, &X, &S, &cb,
                                 &sync, &syncStage, &snapP, &snapJ, &refP, &refJ, &stageA, &stageB})
            safeRelease(*b);
        safeRelease(jointsSrv);
        safeRelease(groupsSrv);
        safeRelease(xUav);
        safeRelease(sUav);
        safeRelease(syncUav);
        for (ExtView* v : {&particles, &pointJoints, &cur, &old, &spheres, &lineSpheres, &planes, &grab}) v->release();
    }
};

static std::map<int, Item*> gItems;
static int gNextItem = 1;

// ---------------------------------------------------------------- job ring (main thread -> render thread)

enum JobKind { kJobAdd = 1, kJobQueue = 2, kJobSolve = 3, kJobRelease = 4, kJobTimer = 5, kJobCheck = 6 };
enum CheckOp { kCheckSnapshot = 0, kCheckSaveRef = 1, kCheckRestore = 2, kCheckCompare = 3 };
struct Job { int seq = -1; int kind = 0; int item = 0; int op = 0; void* buffer = nullptr; CPStep step = {}; };
static const int kRing = 1 << 14;
static Job gRing[kRing];
static int gNextSeq = 0;
static std::atomic<int> gLostJobs{0};   // overwritten before they ran (the render thread fell 16k jobs behind)

static int enqueue(const Job& j) {
    std::lock_guard<std::mutex> l(gLock);
    int seq = gNextSeq;
    gNextSeq = (gNextSeq + 1) & 0x3fffffff;
    Job& slot = gRing[seq & (kRing - 1)];
    slot = j;
    slot.seq = seq;
    return seq;
}

// ---------------------------------------------------------------- device and shaders

static ID3D11Device* gDevice = nullptr;           // Unity's device (referenced while we use it)
static ID3D11DeviceContext* gCtx = nullptr;
static bool gOn12 = false;                        // the device is D3D11On12: solve() flushes between phases
static std::atomic<bool> gPhasesOff{false};       // diagnostics: no flushes (0.1.0's solve), CP_SetPhases
static std::atomic<bool> gMultiOff{false};        // diagnostics: every item iterates in one group, CP_SetMulti
static ID3D11ComputeShader *gImport = nullptr, *gIterateShared = nullptr, *gIterate = nullptr, *gIterateMulti = nullptr,
                           *gCollide = nullptr;
static std::atomic<int> gDeviceError{0};
static std::atomic<int> gSolves{0}, gItemSteps{0}, gDispatches{0};
static std::vector<Item*> gQueue;                  // render thread: items with a queued step, in queue order

static bool ensureDevice(ID3D11Buffer* any) {
    if (gDeviceError.load()) return false;
    if (!any) return false;
    ID3D11Device* dev = nullptr;
    any->GetDevice(&dev);
    if (!dev) { gDeviceError = kErrDevice; setLastError("buffer has no device"); return false; }
    if (gDevice) {
        bool same = dev == gDevice;
        dev->Release();
        if (!same) { gDeviceError = kErrDeviceMismatch; setLastError("buffers from a second D3D11 device"); }
        return same;
    }
    gDevice = dev;   // keeps the reference
    gDevice->GetImmediateContext(&gCtx);
    ID3D11On12Device* on12 = nullptr;
    if (SUCCEEDED(gDevice->QueryInterface(__uuidof(ID3D11On12Device), (void**)&on12))) { gOn12 = true; on12->Release(); }
    if (FAILED(gDevice->CreateComputeShader(g_CSImport, sizeof g_CSImport, nullptr, &gImport)) ||
        FAILED(gDevice->CreateComputeShader(g_CSIterateShared, sizeof g_CSIterateShared, nullptr, &gIterateShared)) ||
        FAILED(gDevice->CreateComputeShader(g_CSIterate, sizeof g_CSIterate, nullptr, &gIterate)) ||
        FAILED(gDevice->CreateComputeShader(g_CSIterateMulti, sizeof g_CSIterateMulti, nullptr, &gIterateMulti)) ||
        FAILED(gDevice->CreateComputeShader(g_CSCollide, sizeof g_CSCollide, nullptr, &gCollide))) {
        gDeviceError = kErrDevice;
        setLastError("compute shaders failed (feature level 0x%x)", (unsigned)gDevice->GetFeatureLevel());
        return false;
    }
    return true;
}

// A GPUTools buffer's contents on the CPU (staging copy; stalls the render thread, used when an item is added).
static bool readBack(ID3D11Buffer* b, uint32_t stride, std::vector<int32_t>& out, const char* what) {
    out.clear();
    if (!b) return true;
    D3D11_BUFFER_DESC d = {};
    b->GetDesc(&d);
    if (!(d.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) || d.StructureByteStride != stride) {
        setLastError("%s buffer: stride %u (want %u)", what, d.StructureByteStride, stride);
        return false;
    }
    D3D11_BUFFER_DESC s = {};
    s.ByteWidth = d.ByteWidth;
    s.Usage = D3D11_USAGE_STAGING;
    s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* st = nullptr;
    if (FAILED(gDevice->CreateBuffer(&s, nullptr, &st))) { setLastError("%s: staging buffer failed", what); return false; }
    gCtx->CopyResource(st, b);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(gCtx->Map(st, 0, D3D11_MAP_READ, 0, &m))) { st->Release(); setLastError("%s: map failed", what); return false; }
    out.resize(d.ByteWidth / 4);
    memcpy(out.data(), m.pData, (size_t)out.size() * 4);
    gCtx->Unmap(st, 0);
    st->Release();
    return true;
}

static bool makeBuffer(uint32_t bytes, uint32_t stride, bool raw, bool uav, const void* init, ID3D11Buffer** out) {
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = std::max(bytes, 16u);
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
    d.MiscFlags = raw ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS : D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    d.StructureByteStride = raw ? 0 : stride;
    if (!raw) d.ByteWidth = std::max(bytes, stride);
    D3D11_SUBRESOURCE_DATA s = {init, 0, 0};
    std::vector<uint8_t> pad;
    if (init && bytes < d.ByteWidth) { pad.assign(d.ByteWidth, 0); memcpy(pad.data(), init, bytes); s.pSysMem = pad.data(); }
    return SUCCEEDED(gDevice->CreateBuffer(&d, init ? &s : nullptr, out));
}

static bool makeSrv(ID3D11Buffer* b, ID3D11ShaderResourceView** out) {
    D3D11_BUFFER_DESC d = {};
    b->GetDesc(&d);
    D3D11_SHADER_RESOURCE_VIEW_DESC v = {};
    v.Format = DXGI_FORMAT_UNKNOWN;
    v.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    v.Buffer.FirstElement = 0;
    v.Buffer.NumElements = d.StructureByteStride ? d.ByteWidth / d.StructureByteStride : 0;
    return SUCCEEDED(gDevice->CreateShaderResourceView(b, &v, out));
}

static bool makeUav(ID3D11Buffer* b, bool raw, ID3D11UnorderedAccessView** out) {
    D3D11_BUFFER_DESC d = {};
    b->GetDesc(&d);
    D3D11_UNORDERED_ACCESS_VIEW_DESC v = {};
    v.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    if (raw) {
        v.Format = DXGI_FORMAT_R32_TYPELESS;
        v.Buffer.NumElements = d.ByteWidth / 4;
        v.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    } else {
        v.Format = DXGI_FORMAT_UNKNOWN;
        v.Buffer.NumElements = d.StructureByteStride ? d.ByteWidth / d.StructureByteStride : 0;
    }
    return SUCCEEDED(gDevice->CreateUnorderedAccessView(b, &v, out));
}

// The view on a GPUTools buffer (structured, `stride` bytes per element), recreated when the buffer changes.
static bool extView(ExtView& v, ID3D11Buffer* b, uint32_t stride, bool uav, const char* what, int item) {
    if (b == v.buffer && (uav ? (void*)v.uav : (void*)v.srv)) return true;
    v.release();
    if (!b) return true;
    D3D11_BUFFER_DESC d = {};
    b->GetDesc(&d);
    if (!(d.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) || d.StructureByteStride != stride ||
        !(d.BindFlags & (uav ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_SHADER_RESOURCE))) {
        setLastError("item %d: %s buffer: stride %u bind 0x%x (want stride %u)", item, what, d.StructureByteStride, d.BindFlags, stride);
        return false;
    }
    bool ok = uav ? makeUav(b, false, &v.uav) : makeSrv(b, &v.srv);
    if (!ok) { setLastError("item %d: view on the %s buffer failed", item, what); return false; }
    v.buffer = b;
    return true;
}

// Builds the solver's buffers: joints packed as (A | B << 16, Rest), stiffness joints then nearby joints, the colour
// groups in GPUTools' dispatch order (empty groups dropped, as GPUTools dispatches nothing for them), and checks the
// rules the solver relies on: particle i's point joint at i, no particle twice in one colour group.
static int addItem(Item& it, ID3D11Buffer* particles) {
    if (!ensureDevice(particles)) return gDeviceError.load() ? gDeviceError.load() : kErrDevice;
    std::vector<int32_t> stiff, near_, pj;
    if (!readBack(it.stiffJoints, 16, stiff, "stiffness joints") || !readBack(it.nearJoints, 16, near_, "nearby joints") ||
        !readBack(it.pointJointsAtAdd, 24, pj, "point joints"))
        return kErrBuffer;
    safeRelease(it.stiffJoints);
    safeRelease(it.nearJoints);
    safeRelease(it.pointJointsAtAdd);
    const uint32_t n = it.n;
    if (pj.size() / 6 != n) { setLastError("item %d: %zu point joints for %u particles", it.id, pj.size() / 6, n); return kErrUnsupported; }
    for (uint32_t i = 0; i < n; i++)
        if (pj[(size_t)i * 6] != (int32_t)i) { setLastError("item %d: point joint %u moves particle %d", it.id, i, pj[(size_t)i * 6]); return kErrUnsupported; }
    const uint32_t stiffCount = (uint32_t)(stiff.size() / 4), nearCount = (uint32_t)(near_.size() / 4);
    std::vector<uint32_t> jd;
    jd.reserve(2 * (size_t)(stiffCount + nearCount));
    for (const std::vector<int32_t>* a : {&stiff, &near_})
        for (size_t j = 0; j + 3 < a->size(); j += 4) {
            int32_t A = (*a)[j], B = (*a)[j + 1];
            if (A < 0 || B < 0 || (uint32_t)A >= n || (uint32_t)B >= n) { setLastError("item %d: joint on particle %d / %d of %u", it.id, A, B, n); return kErrUnsupported; }
            jd.push_back((uint32_t)A | ((uint32_t)B << 16));
            jd.push_back((uint32_t)(*a)[j + 2]);
        }
    std::vector<int32_t> gd;
    std::vector<uint32_t> stamp(n, 0);
    uint32_t mark = 0;
    auto addGroups = [&](const std::vector<int32_t>& groups, uint32_t base, uint32_t count, const char* what) -> bool {
        for (size_t g = 0; g + 1 < groups.size(); g += 2) {
            int32_t start = groups[g], num = groups[g + 1];
            if (num <= 0) continue;
            if (start < 0 || (uint32_t)start + (uint32_t)num > count) { setLastError("item %d: %s group (%d, %d) of %u joints", it.id, what, start, num, count); return false; }
            mark++;
            for (int32_t j = start; j < start + num; j++) {
                uint32_t p = jd[2 * (size_t)(base + j)];
                uint32_t A = p & 0xffff, B = p >> 16;
                if (stamp[A] == mark || stamp[B] == mark) { setLastError("item %d: %s group (%d, %d) moves particle %u twice", it.id, what, start, num, stamp[A] == mark ? A : B); return false; }
                stamp[A] = stamp[B] = mark;
            }
            gd.push_back(start + (int32_t)base);
            gd.push_back(num);
        }
        return true;
    };
    if (!addGroups(it.stiffGroups, 0, stiffCount, "stiffness") || !addGroups(it.nearGroups, stiffCount, nearCount, "nearby"))
        return kErrUnsupported;
    it.groupCount = (uint32_t)(gd.size() / 2);
    it.shared = n <= kMaxShared;
    if (it.shared || it.multi < 2) it.multi = 0;
    if (it.multi) {
        const uint32_t zero[4] = {};
        D3D11_BUFFER_DESC sd = {};
        sd.ByteWidth = 16;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (!makeBuffer(16, 0, true, true, zero, &it.sync) || !makeUav(it.sync, true, &it.syncUav) ||
            FAILED(gDevice->CreateBuffer(&sd, nullptr, &it.syncStage)))
            it.multi = 0;   // one group, same result
    }
    D3D11_BUFFER_DESC cbd = {};   // DYNAMIC: on D3D11On12 an UpdateSubresource of a DEFAULT buffer stalls the GPU
    cbd.ByteWidth = sizeof(FusedParams);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!makeBuffer((uint32_t)(jd.size() * 4), 8, false, false, jd.empty() ? nullptr : jd.data(), &it.joints) ||
        !makeBuffer((uint32_t)(gd.size() * 4), 8, false, false, gd.empty() ? nullptr : gd.data(), &it.groups) ||
        !makeBuffer(16 * n, 0, true, true, nullptr, &it.X) || !makeBuffer(16 * 3 * n, 16, false, true, nullptr, &it.S) ||
        FAILED(gDevice->CreateBuffer(&cbd, nullptr, &it.cb)) ||
        !makeSrv(it.joints, &it.jointsSrv) || !makeSrv(it.groups, &it.groupsSrv) ||
        !makeUav(it.X, true, &it.xUav) || !makeUav(it.S, false, &it.sUav)) {
        setLastError("item %d: solver buffers for %u particles failed", it.id, n);
        return kErrResource;
    }
    it.ready = true;
    return kOk;
}

// The step's views and constants; the dispatches run in the next solve.
static int queueStep(Item& it, const CPStep& s) {
    if (!it.ready) { setLastError("item %d: not added", it.id); return kErrDevice; }
    if (!extView(it.particles, s.particles, 116, true, "particle", it.id) ||
        !extView(it.pointJoints, s.pointJoints, 24, true, "point joint", it.id) ||
        !extView(it.cur, s.cur, 12, false, "skin target", it.id) || !extView(it.old, s.old, 12, false, "old skin target", it.id) ||
        !extView(it.spheres, s.spheres, 32, false, "sphere", it.id) ||
        !extView(it.lineSpheres, s.lineSpheres, 72, false, "capsule", it.id) ||
        !extView(it.planes, s.planes, 16, false, "plane", it.id) || !extView(it.grab, s.grab, 24, false, "grab sphere", it.id))
        return kErrBuffer;
    if (!it.particles.uav || !it.pointJoints.uav || !it.cur.srv || !it.old.srv) { setLastError("item %d: no particle / target buffer", it.id); return kErrBuffer; }
    FusedParams p = {};
    p.particleCount = it.n;
    p.groupCount = s.doStiffness ? it.groupCount : 0;
    p.iterations = (uint32_t)std::max(s.iterations, 1);
    p.doIntegrate = s.doIntegrate ? 1 : 0;
    p.doCollision = s.doCollision ? 1 : 0;
    p.sphereCount = it.spheres.srv ? (uint32_t)std::max(s.sphereCount, 0) : 0;
    p.lineSphereCount = it.lineSpheres.srv ? (uint32_t)std::max(s.lineSphereCount, 0) : 0;
    p.planeCount = it.planes.srv ? (uint32_t)std::max(s.planeCount, 0) : 0;
    p.grabCount = it.grab.srv ? (uint32_t)std::max(s.grabCount, 0) : 0;
    p.multiGroups = std::max(it.multi, 1u);   // only CSIterateMulti reads it, dispatched with this many groups
    p.DT = s.DT; p.dtrecipInner = s.dtrecipInner; p.stepInner = s.stepInner; p.invDrag = s.invDrag;
    for (int a = 0; a < 3; a++) p.accelDT2[a] = s.accelDT2[a];
    p.stiffness = s.stiffness; p.compression = s.compression; p.distanceScale = s.distanceScale;
    p.jointStrength = s.jointStrength; p.jointPrediction = s.jointPrediction; p.breakThreshold = s.breakThreshold;
    p.friction = s.friction; p.staticFriction = s.staticFriction; p.collisionPower = s.collisionPower;
    p.dtrecipOuter = s.dtrecipOuter; p.stepOuter = s.stepOuter;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(gCtx->Map(it.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { setLastError("item %d: constant buffer map failed", it.id); return kErrResource; }
    memcpy(m.pData, &p, sizeof p);
    gCtx->Unmap(it.cb, 0);
    it.outer = std::max(s.outer, 1);
    if (!it.queued) { it.queued = true; gQueue.push_back(&it); }
    it.steps++;
    return kOk;
}

static void bindItem(Item& it) {
    gCtx->CSSetConstantBuffers(0, 1, &it.cb);
    ID3D11UnorderedAccessView* uav[] = {it.particles.uav, it.pointJoints.uav, it.xUav, it.sUav, it.syncUav};
    gCtx->CSSetUnorderedAccessViews(0, 5, uav, nullptr);
    ID3D11ShaderResourceView* srv[] = {it.cur.srv, it.old.srv, it.jointsSrv, it.groupsSrv,
                                       it.spheres.srv, it.lineSpheres.srv, it.planes.srv, it.grab.srv};
    gCtx->CSSetShaderResources(0, 8, srv);
}

static void unbindAll() {
    ID3D11UnorderedAccessView* noU[5] = {};
    ID3D11ShaderResourceView* noS[8] = {};
    ID3D11Buffer* noB[1] = {};
    gCtx->CSSetUnorderedAccessViews(0, 5, noU, nullptr);
    gCtx->CSSetShaderResources(0, 8, noS);
    gCtx->CSSetConstantBuffers(0, 1, noB);
    gCtx->CSSetShader(nullptr, nullptr, 0);
}

// A multi item's gave-up flag (Sync[2]): every kSyncEvery of its solves a copy to the staging buffer, read without
// waiting at a later solve.
static void checkSync(Item& it) {
    if (it.syncCheck < 0) {
        D3D11_MAPPED_SUBRESOURCE m;
        HRESULT hr = gCtx->Map(it.syncStage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
        if (SUCCEEDED(hr)) {
            bool gaveUp = ((const uint32_t*)m.pData)[2] != 0;
            gCtx->Unmap(it.syncStage, 0);
            if (gaveUp) {
                it.gaveUp = true;
                setLastError("item %d: a multi-group iterate (%u groups) gave up waiting; one group from now on", it.id, it.multi);
            }
        }
        it.syncCheck = kSyncEvery;
        return;
    }
    if (--it.syncCheck > 0) return;
    gCtx->CopyResource(it.syncStage, it.sync);
    it.syncCheck = -1;
}

// Every queued step: import all, then per outer step iterate all (long single-group dispatches first, then the multi
// items, largest first), collide all; on D3D11On12 with more than one item, a Flush after each phase but the last.
// Multi items get their groups while kMultiCap lasts.
static void solve() {
    if (!gDevice || gQueue.empty()) return;
    std::vector<Item*> q;
    q.swap(gQueue);
    std::stable_sort(q.begin(), q.end(), [](const Item* a, const Item* b) {
        int ra = a->shared ? 2 : a->multi ? 1 : 0, rb = b->shared ? 2 : b->multi ? 1 : 0;
        return ra != rb ? ra < rb : a->multi > b->multi;
    });
    int outer = 1, dispatches = 0;
    uint32_t multiLeft = gMultiOff.load() ? 0 : kMultiCap;
    for (Item* it : q) {
        it->queued = false;
        outer = std::max(outer, it->outer);
        it->runMulti = 0;
        if (it->multi && !it->gaveUp.load() && it->multi <= multiLeft) { it->runMulti = it->multi; multiLeft -= it->multi; }
    }
    const bool phases = gOn12 && !gPhasesOff.load() && q.size() > 1;
    gCtx->CSSetShader(gImport, nullptr, 0);
    for (Item* it : q) { bindItem(*it); gCtx->Dispatch((it->n + 255) / 256, 1, 1); dispatches++; }
    if (phases) gCtx->Flush();
    for (int o = 1; o <= outer; o++) {
        ID3D11ComputeShader* cur = nullptr;
        for (Item* it : q) {
            if (it->outer < o) continue;
            ID3D11ComputeShader* cs = it->runMulti ? gIterateMulti : it->shared ? gIterateShared : gIterate;
            if (cs != cur) { gCtx->CSSetShader(cs, nullptr, 0); cur = cs; }
            bindItem(*it);
            gCtx->Dispatch(it->runMulti ? it->runMulti : 1, 1, 1);
            dispatches++;
        }
        if (phases) gCtx->Flush();
        gCtx->CSSetShader(gCollide, nullptr, 0);
        for (Item* it : q) {
            if (it->outer < o) continue;
            bindItem(*it);
            gCtx->Dispatch((it->n + 255) / 256, 1, 1);
            dispatches++;
        }
        if (phases && o < outer) gCtx->Flush();
    }
    unbindAll();
    for (Item* it : q)
        if (it->runMulti) { it->multiSolves++; checkSync(*it); }
    gSolves++;
    gItemSteps += (int)q.size();
    gDispatches += dispatches;
}

// The item leaves the queue (released, or an error) without solving its step.
static void dequeue(Item& it) {
    if (!it.queued) return;
    gQueue.erase(std::remove(gQueue.begin(), gQueue.end(), &it), gQueue.end());
    it.queued = false;
}

// ---------------------------------------------------------------- diagnostics: GPU time

struct TimerSlot { ID3D11Query *disjoint = nullptr, *t0 = nullptr, *t1 = nullptr; int channel = -1; bool open = false, pending = false; };
static const int kTimerSlots = 512, kChannels = 2;   // 0 GPUTools' steps, 1 solves
static TimerSlot gTimers[kTimerSlots];
static int gOpenTimer[kChannels] = {-1, -1};
struct GpuTime { double sum = 0, max = 0; int count = 0, skipped = 0; };
static GpuTime gGpuTime[kChannels];

static void pollTimers() {
    for (TimerSlot& t : gTimers) {
        if (!t.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 a = 0, b = 0;
        if (gCtx->GetData(t.disjoint, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            gCtx->GetData(t.t0, &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            gCtx->GetData(t.t1, &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        t.pending = false;
        std::lock_guard<std::mutex> l(gLock);
        GpuTime& g = gGpuTime[t.channel];
        if (dj.Disjoint || b < a) { g.skipped++; continue; }
        double ms = double(b - a) * 1000.0 / double(dj.Frequency);
        g.sum += ms;
        g.max = std::max(g.max, ms);
        g.count++;
    }
}

static void runTimer(int channel, bool begin) {
    if (channel < 0 || channel >= kChannels || !gDevice) return;
    if (begin) {
        pollTimers();
        if (gOpenTimer[channel] >= 0) return;   // unmatched begin: keep the open one
        for (int q = 0; q < kTimerSlots; q++) {
            TimerSlot& t = gTimers[q];
            if (t.open || t.pending) continue;
            if (!t.disjoint) {
                D3D11_QUERY_DESC d = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
                if (FAILED(gDevice->CreateQuery(&d, &t.disjoint))) return;
                d.Query = D3D11_QUERY_TIMESTAMP;
                if (FAILED(gDevice->CreateQuery(&d, &t.t0)) || FAILED(gDevice->CreateQuery(&d, &t.t1))) return;
            }
            gCtx->Begin(t.disjoint);
            gCtx->End(t.t0);
            t.open = true;
            t.channel = channel;
            gOpenTimer[channel] = q;
            return;
        }
        std::lock_guard<std::mutex> l(gLock);
        gGpuTime[channel].skipped++;
    } else {
        int q = gOpenTimer[channel];
        if (q < 0) return;
        TimerSlot& t = gTimers[q];
        gCtx->End(t.t1);
        gCtx->End(t.disjoint);
        t.open = false;
        t.pending = true;
        gOpenTimer[channel] = -1;
    }
}

// ---------------------------------------------------------------- diagnostics: one step against GPUTools

static bool makeCopy(ID3D11Buffer* like, ID3D11Buffer** out, bool staging) {
    if (*out) return true;
    D3D11_BUFFER_DESC d = {};
    like->GetDesc(&d);
    if (staging) { d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.MiscFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.StructureByteStride = 0; }
    return SUCCEEDED(gDevice->CreateBuffer(&d, nullptr, out));
}

// snapshot: particles and point joints -> A; save-ref: -> B (GPUTools' result); restore: A -> buffers;
// compare: particles (ours, after the solve) against B, read back (stalls the render thread; diagnostics only)
static int runCheck(Item& it, int op, ID3D11Buffer* particles, ID3D11Buffer* pointJoints) {
    if (!ensureDevice(particles)) return kErrDevice;
    if (!makeCopy(particles, &it.snapP, false) || !makeCopy(particles, &it.refP, false) ||
        !makeCopy(pointJoints, &it.snapJ, false) || !makeCopy(pointJoints, &it.refJ, false)) {
        setLastError("item %d: diagnostics copies failed", it.id);
        return kErrResource;
    }
    switch (op) {
        case kCheckSnapshot: gCtx->CopyResource(it.snapP, particles); gCtx->CopyResource(it.snapJ, pointJoints); return kOk;
        case kCheckSaveRef: gCtx->CopyResource(it.refP, particles); gCtx->CopyResource(it.refJ, pointJoints); return kOk;
        case kCheckRestore: gCtx->CopyResource(particles, it.snapP); gCtx->CopyResource(pointJoints, it.snapJ); return kOk;
        default: break;
    }
    if (!makeCopy(particles, &it.stageA, true) || !makeCopy(particles, &it.stageB, true)) {
        setLastError("item %d: diagnostics staging buffers failed", it.id);
        return kErrResource;
    }
    gCtx->CopyResource(it.stageA, particles);
    gCtx->CopyResource(it.stageB, it.refP);
    D3D11_MAPPED_SUBRESOURCE ma, mb;
    if (FAILED(gCtx->Map(it.stageA, 0, D3D11_MAP_READ, 0, &ma))) return kOk;
    if (FAILED(gCtx->Map(it.stageB, 0, D3D11_MAP_READ, 0, &mb))) { gCtx->Unmap(it.stageA, 0); return kOk; }
    float maxPos = 0, maxVel = 0;
    double sumPos = 0;
    int equal = 0, nonFinite = 0;
    for (uint32_t i = 0; i < it.n; i++) {
        const float* a = (const float*)((const uint8_t*)ma.pData + (size_t)i * 116);
        const float* b = (const float*)((const uint8_t*)mb.pData + (size_t)i * 116);
        float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        float vx = a[12] - b[12], vy = a[13] - b[13], vz = a[14] - b[14];   // Velocity at byte 48
        float v = sqrtf(vx * vx + vy * vy + vz * vz);
        if (!std::isfinite(d) || !std::isfinite(v)) { nonFinite++; continue; }
        if (memcmp(a, b, 116) == 0) equal++;
        maxPos = std::max(maxPos, d);
        maxVel = std::max(maxVel, v);
        sumPos += d;
    }
    gCtx->Unmap(it.stageA, 0);
    gCtx->Unmap(it.stageB, 0);
    std::lock_guard<std::mutex> l(gLock);
    Verify& r = it.verify;
    r.lastMaxPos = maxPos;
    r.lastMeanPos = it.n ? float(sumPos / it.n) : 0.f;
    r.lastMaxVel = maxVel;
    r.lastEqual = equal;
    r.nonFinite += nonFinite;
    r.worstPos = std::max(r.worstPos, maxPos);
    r.worstVel = std::max(r.worstVel, maxVel);
    r.sumMeanPos += r.lastMeanPos;
    r.compares++;
    return kOk;
}

// ---------------------------------------------------------------- render thread

static void __stdcall OnRenderEvent(int eventId) {
    Job job;
    {
        std::lock_guard<std::mutex> l(gLock);
        Job& slot = gRing[eventId & (kRing - 1)];
        if (slot.seq != eventId) { gLostJobs++; return; }
        job = slot;
        slot.seq = -1;
    }
    if (job.kind == kJobTimer) {
        // the buffer only finds the device before the first item; later it may be gone (any item's particle buffer)
        if (!gDevice && job.buffer && !ensureDevice((ID3D11Buffer*)job.buffer)) return;
        runTimer(job.item, job.op != 0);
        return;
    }
    if (job.kind == kJobSolve) { solve(); return; }
    Item* it = nullptr;
    {
        std::lock_guard<std::mutex> l(gLock);
        auto f = gItems.find(job.item);
        if (f == gItems.end()) return;
        it = f->second;
        if (job.kind == kJobRelease) gItems.erase(f);
    }
    if (job.kind == kJobRelease) {
        dequeue(*it);
        delete it;
        return;
    }
    if (it->error.load()) return;
    int e = kOk;
    switch (job.kind) {
        case kJobAdd: e = addItem(*it, (ID3D11Buffer*)job.buffer); break;
        case kJobQueue: e = queueStep(*it, job.step); break;
        case kJobCheck: e = runCheck(*it, job.op, job.step.particles, job.step.pointJoints); break;
        default: break;
    }
    if (e) { it->error = e; dequeue(*it); }
}

// ---------------------------------------------------------------- exports

CP_API int CP_ApiVersion() { return kApiVersion; }

// Diagnostics: on = 1 / 0 turns the phase flushes on / off (they only run on D3D11On12), < 0 changes nothing.
// Returns bit 0: the device is D3D11On12 (known once the first item or timer ran), bit 1: flushes on.
CP_API int CP_SetPhases(int on) {
    if (on >= 0) gPhasesOff = on == 0;
    return (gOn12 ? 1 : 0) | (gPhasesOff.load() ? 0 : 2);
}
// Diagnostics: on = 1 / 0 lets multi items iterate over their groups / makes every item iterate in one group (same
// result), < 0 changes nothing. Returns 1 when multi is on.
CP_API int CP_SetMulti(int on) {
    if (on >= 0) gMultiOff = on == 0;
    return gMultiOff.load() ? 0 : 1;
}
CP_API int CP_StepSize() { return (int)sizeof(CPStep); }
CP_API void* CP_GetRenderEventFunc() { return (void*)&OnRenderEvent; }

// Registers an item: its particle count and particle buffer, GPUTools' point joint, stiffness joint and nearby joint
// buffers (read on the render thread when the returned add job runs) and the joints' colour groups as GroupsData lists
// them ((start, count) pairs), and the thread groups its iterate may spread over (multiGroups < 2: one; ignored for an
// item whose positions fit in group-shared memory). Returns the item id (> 0) or 0; *addEvent = the event id to pass to
// GL.IssuePluginEvent before the item's first step.
CP_API int CP_CreateItem(int particleCount, void* particles, void* pointJoints, void* stiffJoints, const int32_t* stiffGroups,
                         int stiffGroupCount, void* nearJoints, const int32_t* nearGroups, int nearGroupCount, int multiGroups,
                         int* addEvent) {
    if (particleCount <= 0 || (uint32_t)particleCount > kMaxParticles || !particles || !pointJoints || !addEvent ||
        stiffGroupCount < 0 || nearGroupCount < 0 || (stiffGroupCount && !stiffGroups) || (nearGroupCount && !nearGroups)) {
        setLastError("CP_CreateItem: bad arguments (%d particles)", particleCount);
        return 0;
    }
    Item* it = new Item();
    it->n = (uint32_t)particleCount;
    it->multi = multiGroups >= 2 ? std::min((uint32_t)multiGroups, kMaxMulti) : 0;
    it->stiffJoints = (ID3D11Buffer*)stiffJoints;
    it->nearJoints = (ID3D11Buffer*)nearJoints;
    it->pointJointsAtAdd = (ID3D11Buffer*)pointJoints;
    for (ID3D11Buffer* b : {it->stiffJoints, it->nearJoints, it->pointJointsAtAdd}) if (b) b->AddRef();
    if (stiffGroupCount) it->stiffGroups.assign(stiffGroups, stiffGroups + 2 * (size_t)stiffGroupCount);
    if (nearGroupCount) it->nearGroups.assign(nearGroups, nearGroups + 2 * (size_t)nearGroupCount);
    int id;
    {
        std::lock_guard<std::mutex> l(gLock);
        id = it->id = gNextItem++;
        gItems[id] = it;
    }
    Job j;
    j.kind = kJobAdd;
    j.item = id;
    j.buffer = particles;
    *addEvent = enqueue(j);
    return id;
}

// Queues one physics step of the item (the dispatches run in the next solve). Returns the event id or -1.
CP_API int CP_QueueStep(int item, const CPStep* step) {
    if (!step) return -1;
    {
        std::lock_guard<std::mutex> l(gLock);
        if (!gItems.count(item)) return -1;
    }
    Job j;
    j.kind = kJobQueue;
    j.item = item;
    j.step = *step;
    return enqueue(j);
}

// Solves every step queued since the last solve. Returns the event id.
CP_API int CP_QueueSolve() {
    Job j;
    j.kind = kJobSolve;
    return enqueue(j);
}

// The item is freed on the render thread, after every job queued before this one (a queued step of it is dropped).
CP_API int CP_QueueRelease(int item) {
    Job j;
    j.kind = kJobRelease;
    j.item = item;
    return enqueue(j);
}

// 0 fine, 1 fine but its multi-group iterate gave up (it iterates in one group now; CP_GetLastError says so), < 0 a
// CPError: the item stays stopped (the C# side hands it back to GPUTools and releases it); a device error stops every
// item.
CP_API int CP_ItemStatus(int item) {
    if (gDeviceError.load()) return gDeviceError.load();
    std::lock_guard<std::mutex> l(gLock);
    auto f = gItems.find(item);
    if (f == gItems.end()) return 0;
    int e = f->second->error.load();
    return e ? e : f->second->gaveUp.load() ? 1 : 0;
}

// Diagnostics: the item's thread groups for the multi-group iterate (0 = one group), its solves that used them, 1 if a
// wait gave up. Valid once its add job ran.
CP_API void CP_ItemMulti(int item, int* groups, int* solves, int* gaveUp) {
    int g = 0, s = 0, u = 0;
    {
        std::lock_guard<std::mutex> l(gLock);
        auto f = gItems.find(item);
        if (f != gItems.end()) { g = (int)f->second->multi; s = f->second->multiSolves.load(); u = f->second->gaveUp.load() ? 1 : 0; }
    }
    if (groups) *groups = g;
    if (solves) *solves = s;
    if (gaveUp) *gaveUp = u;
}

// Diagnostics: steps queued for the item so far, its colour groups, 1 if its positions stay in group-shared memory.
// Valid once its add job ran.
CP_API void CP_ItemCounters(int item, int* steps, int* groups, int* shared) {
    int s = 0, g = 0, h = 0;
    {
        std::lock_guard<std::mutex> l(gLock);
        auto f = gItems.find(item);
        if (f != gItems.end()) { s = f->second->steps.load(); g = (int)f->second->groupCount; h = f->second->shared ? 1 : 0; }
    }
    if (steps) *steps = s;
    if (groups) *groups = g;
    if (shared) *shared = h;
}

// Diagnostics: solves, item steps solved and dispatches so far.
CP_API void CP_SolveCounters(int* solves, int* itemSteps, int* dispatches) {
    if (solves) *solves = gSolves.load();
    if (itemSteps) *itemSteps = gItemSteps.load();
    if (dispatches) *dispatches = gDispatches.load();
}

// Diagnostics: GPU timestamp bracket (begin != 0 opens, 0 closes) on channel 0 (GPUTools' steps) or 1 (solves);
// anyBuffer = a buffer of Unity's device (for the device before the first item). Event id or -1.
CP_API int CP_QueueTimer(int channel, int begin, void* anyBuffer) {
    if (channel < 0 || channel >= kChannels) return -1;
    Job j;
    j.kind = kJobTimer;
    j.item = channel;
    j.op = begin ? 1 : 0;
    j.buffer = anyBuffer;
    return enqueue(j);
}

// GPU milliseconds per bracket on a channel (finished brackets so far); reset = 1 clears after reading.
CP_API void CP_GpuTime(int channel, double* sumMs, double* maxMs, int* count, int* skipped, int reset) {
    std::lock_guard<std::mutex> l(gLock);
    GpuTime z;
    GpuTime& g = (channel >= 0 && channel < kChannels) ? gGpuTime[channel] : z;
    if (sumMs) *sumMs = g.sum;
    if (maxMs) *maxMs = g.max;
    if (count) *count = g.count;
    if (skipped) *skipped = g.skipped;
    if (reset) g = GpuTime();
}

// Diagnostics: one check step on the item's GPUTools buffers (op: 0 snapshot, 1 save GPUTools' result, 2 restore,
// 3 compare after the solve). Event id or -1.
CP_API int CP_QueueCheck(int item, int op, void* particles, void* pointJoints) {
    if (op < kCheckSnapshot || op > kCheckCompare || !particles || !pointJoints) return -1;
    Job j;
    j.kind = kJobCheck;
    j.item = item;
    j.op = op;
    j.step.particles = (ID3D11Buffer*)particles;
    j.step.pointJoints = (ID3D11Buffer*)pointJoints;
    return enqueue(j);
}

// Diagnostics: the item's checks against GPUTools so far (positions in metres, velocities in m/s); reset = 1 clears.
// out[0] last max position difference, [1] last mean, [2] last max velocity difference, [3] worst max position,
// [4] worst max velocity, [5] mean of the means; counts[0] particles bit-identical (all 116 bytes) in the last check,
// [1] non-finite differences, [2] checks.
CP_API void CP_ItemVerify(int item, float* out6, int* counts3, int reset) {
    std::lock_guard<std::mutex> l(gLock);
    auto f = gItems.find(item);
    Verify z;
    Verify& r = f == gItems.end() ? z : f->second->verify;
    if (out6) {
        out6[0] = r.lastMaxPos; out6[1] = r.lastMeanPos; out6[2] = r.lastMaxVel;
        out6[3] = r.worstPos; out6[4] = r.worstVel; out6[5] = r.compares ? float(r.sumMeanPos / r.compares) : 0.f;
    }
    if (counts3) { counts3[0] = r.lastEqual; counts3[1] = r.nonFinite; counts3[2] = r.compares; }
    if (reset) r = Verify();
}

// Jobs that were overwritten before the render thread reached them.
CP_API int CP_LostJobs() { return gLostJobs.load(); }

// Last error text (UTF-8, truncated to cap - 1); returns its full length.
CP_API int CP_GetLastError(char* buf, int cap) {
    std::lock_guard<std::mutex> l(gLock);
    if (buf && cap > 0) {
        size_t m = std::min(gLastError.size(), (size_t)cap - 1);
        memcpy(buf, gLastError.data(), m);
        buf[m] = 0;
    }
    return (int)gLastError.size();
}
