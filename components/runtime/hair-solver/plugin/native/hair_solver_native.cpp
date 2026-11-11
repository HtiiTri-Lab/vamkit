// VaMHairSolverNative.dll: runs solver\fused_strand.hlsl for GPUTools hair items inside VaM (docs\P0-原求解器与新方案.md
// section 8, P3). The C# plugin (plugin\VaMHairSolver.cs) takes over HairPhysicsWorld.DispatchPhysicsImpl, queues one
// job per physics step here and issues GL.IssuePluginEvent(HS_GetRenderEventFunc(), job); Unity's render thread then
// calls OnRenderEvent in command order, between GPUTools' own compute dispatches, on Unity's D3D11 immediate context.
//
// The particle, point-joint, scalp-matrix and collider buffers are GPUTools' own ComputeBuffers
// (ComputeBuffer.GetNativeBufferPtr); the solver reads and writes them in place, so GPUTools' renderer, and GPUTools'
// solver whenever an item falls back, see exactly what GPUTools would have left there. Per item this DLL only adds the
// rest lengths and the cling adjacency (built by the C# side from GPUTools' joint data) and the solver's scratch state.
//
// Everything that touches D3D runs in OnRenderEvent (Unity may create its device single-threaded). Compute-stage state
// is saved and restored around every job; graphics-stage views of the particle buffer that binding it for writing
// unbinds are put back too. The C# side also calls GL.InvalidateState after every solver event.
//
// Diagnostics (VaMHairSolver's Diagnostics setting): GPU timestamps around a physics step, whichever solver ran it
// (HS_QueueTimer brackets GPUTools' own dispatches the same way), and an in-game check of one step against GPUTools
// (ops kOpSnapshot .. kOpCompare: both solvers start from the same particle state, the results are compared).
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d11on12.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "fused_strand_step.h"     // g_CSStep   (fxc /Fh, Build-HairSolver.ps1)
#include "fused_strand_strand.h"   // g_CSStrand

#define HS_API extern "C" __declspec(dllexport)

static const int kApiVersion = 5;   // 3: position-only breast collider mask; 4: HS_SetOptions; 5: the drawn breasts' spheres

// HS_SetOptions bits (all on by default; 0 = 0.1.2's behaviour, 3 = 0.1.6's, for timing them in game).
//   kOptDynamicConstants  the constants are written before every dispatch. On D3D11On12, UpdateSubresource of a DEFAULT
//                         constant buffer (0.1.2) puts a copy and two state transitions on the GPU each time, and with
//                         them a wait for all earlier work; DYNAMIC + Map(WRITE_DISCARD) has none of that.
//   kOptBatch             step jobs wait for a solve event (HS_QueueSolve) and run together, phase by phase (solveBatch).
//   kOptRepeat            an item's next step joins the waiting batch too, in the phases after its step before (0.1.7).
//                         With two hair steps in a frame (HairBudget following a body that took two), 0.1.6 ran the
//                         batch at every item's second step: 2n + 1 solves one after another instead of two phases.
enum HSOption { kOptDynamicConstants = 1, kOptBatch = 2, kOptRepeat = 4 };

// ---------------------------------------------------------------- interface shared with the C# side

#pragma pack(push, 8)
struct HSStep {
    // GPUTools' buffers (ID3D11Buffer*), 0 = none
    void* particles;            // GPParticle, 116 B
    void* pointJoints;          // GPPointJoint, 24 B, one per particle in particle order
    void* transforms;           // Matrix4x4, 64 B
    void* spheres;              // GPSphereWithDelta, 32 B (GPUCollidersManager's processed spheres)
    void* lineSpheres;          // GPLineSphereWithDelta, 72 B
    void* planes;               // Vector4 (normal, offset), 16 B (GPUCollidersManager's planes; 0 = none)
    int32_t op;                 // HSOp
    int32_t flags;              // op kOpStrand: kFlag*
    float DT, accelDT2[3];
    float invDrag, weight, distanceScale, distancePower;
    float compressionScale, compressionPower, splinePower, dtrecipInner;
    float stepInner, dtrecipOuter, stepOuter, friction;
    float staticFriction, collisionPower;
    int32_t doIntegrate, doCompression, doCollision, isFixed;
    float nearbyScale, nearbyPower, nearbyRolloff, clingCap;
    int32_t outer, iterations, distanceRepeat, compressionRepeat, clingRepeat;
    int32_t reserved;
    void* positionOnly;         // uint mask: spheres, then capsules (0 = original contact, 1 position only, 2 replaced)
    void* bodySpheres;          // Vector4, 16 B: per breast SoftBody draws, a bound, its count, its surface spheres
    int32_t bodyCount;          // entries of bodySpheres in use
    int32_t bodyReserved;
};
#pragma pack(pop)
static_assert(sizeof(HSStep) == 208, "HSStep layout");

enum HSOp {
    kOpReset = 0,    // frame < 10: ResetToPointJoints + ParticleCollisionReset
    kOpStep = 1,     // whole physics step in one dispatch (CSStep): exact when the strands do not interact
    kOpJacobi = 2,   // outer x iterations + 1 dispatches (CSStrand), cling gathered inside (clingMode 1)
    kOpStrand = 3,   // one CSStrand dispatch with the flags below; the C# side runs GPUTools' cling kernel in between
    // in-game check against GPUTools: snapshot, GPUTools' step, save its result, restore, our step, compare
    kOpSnapshot = 4, // particles -> copy A
    kOpSaveRef = 5,  // particles -> copy B (GPUTools' result)
    kOpRestore = 6,  // copy A -> particles
    kOpCompare = 7,  // particles (ours) vs copy B, read back (stalls the render thread; diagnostics only)
};
enum HSFlag { kFlagB = 1, kFlagEnd = 2, kFlagA = 4, kFlagImport = 8, kFlagExport = 16 };

struct HSClingEntry { int32_t other; float rest, elasticity; };   // 12 B, solver ClingEntry

// solver\fused_strand.hlsl cbuffer Params
struct FusedCB {
    uint32_t particleCount, segments, strandCount, strandsPerGroup;
    float DT, accelDT2[3];
    float invDrag, weight, distanceScale, distancePower;
    float compressionScale, compressionPower, splinePower, dtrecipInner;
    float stepInner, dtrecipOuter, stepOuter, friction;
    float staticFriction, collisionPower;
    uint32_t sphereCount, lineSphereCount;
    uint32_t doB, doEnd, doA, doIntegrate;
    uint32_t doCompression, doCollision, doReset, isFixed;
    float nearbyScale, nearbyPower, nearbyRolloff;
    uint32_t clingMode;
    uint32_t readParity, outer, iterations, distanceRepeat;
    float clingCap;
    uint32_t importStruct, exportStruct, compressionRepeat;
    uint32_t clingRepeat, planeCount, positionOnlyCount, positionOnlySpheres;
    uint32_t bodyCount, bodyPad[3];
};
static_assert(sizeof(FusedCB) == 208, "cbuffer layout");

// ---------------------------------------------------------------- errors

enum HSError {
    kOk = 0,
    kErrDevice = -1,       // no device / shaders / constant buffer
    kErrBuffer = -2,       // a GPUTools buffer has an unexpected layout or size
    kErrCreate = -3,       // creating our own buffers or views failed
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

// ---------------------------------------------------------------- items

template <class T> static void safeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct View {                       // our view of one external buffer, recreated when the pointer changes
    ID3D11Buffer* buffer = nullptr; // not owned (the view holds the reference)
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    uint32_t count = 0;
    void reset() { safeRelease(srv); safeRelease(uav); buffer = nullptr; count = 0; }
};

struct Owned {
    ID3D11Buffer* buffer = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    void reset() { safeRelease(srv); safeRelease(uav); safeRelease(buffer); }
};

struct Verify {                     // kOpCompare results, accumulated
    float lastMaxPos = 0, lastMeanPos = 0, lastMaxVel = 0;
    float worstPos = 0, worstVel = 0;
    double sumMeanPos = 0;
    int lastEqual = 0, nonFinite = 0, compares = 0;
};

struct Item {
    int id = 0;
    uint32_t n = 0, seg = 0, strands = 0;
    std::vector<float> rest2;                 // 2n
    std::vector<uint32_t> clingOffsets;       // n + 1
    std::vector<HSClingEntry> clingEntries;
    bool created = false;
    Owned bRest2, bOffsets, bEntries, bCompact, bSnap, bHot;
    View vParticles, vPointJoints, vTransforms, vSpheres, vLineSpheres, vPlanes, vPositionOnly, vBodySpheres;
    uint32_t parity = 0;
    ID3D11Buffer *copyA = nullptr, *copyB = nullptr, *stageA = nullptr, *stageB = nullptr;   // diagnostics
    Verify verify;
    std::atomic<int> error{0};
    std::atomic<int> steps{0};
    std::atomic<int> dispatches{0};
    ~Item() {
        for (Owned* o : {&bRest2, &bOffsets, &bEntries, &bCompact, &bSnap, &bHot}) o->reset();
        for (View* v : {&vParticles, &vPointJoints, &vTransforms, &vSpheres, &vLineSpheres, &vPlanes, &vPositionOnly, &vBodySpheres}) v->reset();
        for (ID3D11Buffer** b : {&copyA, &copyB, &stageA, &stageB}) safeRelease(*b);
    }
};

static std::map<int, Item*> gItems;
static int gNextItem = 1;

// ---------------------------------------------------------------- job ring (main thread -> render thread)

enum JobKind { kJobStep = 1, kJobRelease = 2, kJobTimer = 3, kJobSolve = 4 };
struct Job { int seq = -1; int kind = 0; int item = 0; HSStep step = {}; };
static const int kRing = 1 << 14;
static Job gRing[kRing];
static int gNextSeq = 0;
static std::atomic<int> gLostJobs{0};   // overwritten before they ran (the render thread fell 16k jobs behind)

static int enqueue(int kind, int item, const HSStep* step) {
    std::lock_guard<std::mutex> l(gLock);
    int seq = gNextSeq;
    gNextSeq = (gNextSeq + 1) & 0x3fffffff;
    Job& j = gRing[seq & (kRing - 1)];
    j.seq = seq;
    j.kind = kind;
    j.item = item;
    if (step) j.step = *step;
    return seq;
}

// ---------------------------------------------------------------- device-wide resources

static ID3D11Device* gDevice = nullptr;           // Unity's device (referenced while we use it)
static ID3D11DeviceContext* gCtx = nullptr;
static ID3D11ComputeShader* gCsStrand = nullptr;
static ID3D11ComputeShader* gCsStep = nullptr;
static ID3D11Buffer* gCb = nullptr;               // DYNAMIC, Map(WRITE_DISCARD) before every dispatch
static ID3D11Buffer* gCbDefault = nullptr;        // diagnostics: 0.1.2's DEFAULT buffer and UpdateSubresource
static std::atomic<int> gDeviceError{0};
static std::atomic<int> gOptions{kOptDynamicConstants | kOptBatch | kOptRepeat};
static bool gOn12 = false;                        // Unity's device is D3D11On12 (VaM with the DLSS bridge)

static bool ensureDevice(ID3D11Buffer* any) {
    if (gDeviceError.load()) return false;
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
    HRESULT hr = gDevice->CreateComputeShader(g_CSStrand, sizeof g_CSStrand, nullptr, &gCsStrand);
    if (SUCCEEDED(hr)) hr = gDevice->CreateComputeShader(g_CSStep, sizeof g_CSStep, nullptr, &gCsStep);
    if (SUCCEEDED(hr)) {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = sizeof(FusedCB);
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = gDevice->CreateBuffer(&d, nullptr, &gCb);
        d.Usage = D3D11_USAGE_DEFAULT;
        d.CPUAccessFlags = 0;
        if (SUCCEEDED(hr)) hr = gDevice->CreateBuffer(&d, nullptr, &gCbDefault);
    }
    if (FAILED(hr)) {
        gDeviceError = kErrDevice;
        setLastError("creating the solver shaders failed: 0x%08x (feature level 0x%x)", (unsigned)hr, (unsigned)gDevice->GetFeatureLevel());
        return false;
    }
    return true;
}

static bool makeOwned(Owned& o, uint32_t stride, uint32_t count, const void* init, bool uav) {
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = stride * std::max(count, 1u);
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
    d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    d.StructureByteStride = stride;
    std::vector<uint8_t> zero;
    if (!init || !count) { zero.assign(d.ByteWidth, 0); init = zero.data(); }
    D3D11_SUBRESOURCE_DATA sd = {init, 0, 0};
    if (FAILED(gDevice->CreateBuffer(&d, &sd, &o.buffer))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC s = {};
    s.Format = DXGI_FORMAT_UNKNOWN;
    s.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    s.Buffer.NumElements = std::max(count, 1u);
    if (FAILED(gDevice->CreateShaderResourceView(o.buffer, &s, &o.srv))) return false;
    if (uav) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC u = {};
        u.Format = DXGI_FORMAT_UNKNOWN;
        u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        u.Buffer.NumElements = std::max(count, 1u);
        if (FAILED(gDevice->CreateUnorderedAccessView(o.buffer, &u, &o.uav))) return false;
    }
    return true;
}

// View of a GPUTools buffer: structured, the expected stride, at least minCount elements.
static int bindView(View& v, void* ptr, uint32_t stride, uint32_t minCount, bool uav, const char* what) {
    ID3D11Buffer* b = (ID3D11Buffer*)ptr;
    if (v.buffer == b && b) return kOk;
    v.reset();
    if (!b) return minCount ? kErrBuffer : kOk;
    D3D11_BUFFER_DESC d = {};
    b->GetDesc(&d);
    uint32_t count = d.StructureByteStride ? d.ByteWidth / d.StructureByteStride : 0;
    if (!(d.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) || d.StructureByteStride != stride || count < minCount ||
        !(d.BindFlags & D3D11_BIND_SHADER_RESOURCE) || (uav && !(d.BindFlags & D3D11_BIND_UNORDERED_ACCESS))) {
        setLastError("%s buffer: stride %u (want %u), %u elements (want >= %u), bind 0x%x, misc 0x%x", what,
                     d.StructureByteStride, stride, count, minCount, d.BindFlags, d.MiscFlags);
        return kErrBuffer;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC s = {};
    s.Format = DXGI_FORMAT_UNKNOWN;
    s.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    s.Buffer.NumElements = count;
    if (uav) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC u = {};
        u.Format = DXGI_FORMAT_UNKNOWN;
        u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        u.Buffer.NumElements = count;
        if (FAILED(gDevice->CreateUnorderedAccessView(b, &u, &v.uav))) { setLastError("%s: UAV failed", what); return kErrCreate; }
    } else if (FAILED(gDevice->CreateShaderResourceView(b, &s, &v.srv))) {
        setLastError("%s: SRV failed", what);
        return kErrCreate;
    }
    v.buffer = b;
    v.count = count;
    return kOk;
}

static int createItemResources(Item& it) {
    const uint32_t n = it.n;
    if (!makeOwned(it.bRest2, 8, n, it.rest2.data(), false) ||
        !makeOwned(it.bOffsets, 4, n + 1, it.clingOffsets.data(), false) ||
        !makeOwned(it.bEntries, 12, (uint32_t)it.clingEntries.size(), it.clingEntries.empty() ? nullptr : it.clingEntries.data(), false) ||
        !makeOwned(it.bCompact, 16, 2 * n, nullptr, true) || !makeOwned(it.bSnap, 16, 2 * n, nullptr, true) ||
        !makeOwned(it.bHot, 16, 5 * n, nullptr, true)) {
        setLastError("item %d: creating solver buffers for %u particles failed", it.id, n);
        return kErrCreate;
    }
    it.created = true;
    return kOk;
}

// ---------------------------------------------------------------- state save / restore

static const UINT kSrvSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;   // 128
static const UINT kUavSlots = D3D11_PS_CS_UAV_REGISTER_COUNT;                // 8

struct SavedState {
    ID3D11ComputeShader* cs = nullptr;
    ID3D11ClassInstance* inst[256] = {};
    UINT nInst = 0;
    ID3D11Buffer* cb = nullptr;
    ID3D11ShaderResourceView* srv[kSrvSlots] = {};
    ID3D11UnorderedAccessView* uav[kUavSlots] = {};
    struct Gfx { int stage; UINT slot; ID3D11ShaderResourceView* v; };
    std::vector<Gfx> gfx;   // graphics-stage views of the buffers we write (unbound by D3D when we bind them as UAV)

    void save(ID3D11DeviceContext* c, ID3D11Buffer* const* written, size_t writtenCount) {
        nInst = 256;
        c->CSGetShader(&cs, inst, &nInst);
        c->CSGetConstantBuffers(0, 1, &cb);
        c->CSGetShaderResources(0, kSrvSlots, srv);
        c->CSGetUnorderedAccessViews(0, kUavSlots, uav);
        ID3D11ShaderResourceView* tmp[kSrvSlots];
        for (int stage = 0; stage < 5; stage++) {
            memset(tmp, 0, sizeof tmp);
            switch (stage) {
                case 0: c->VSGetShaderResources(0, kSrvSlots, tmp); break;
                case 1: c->HSGetShaderResources(0, kSrvSlots, tmp); break;
                case 2: c->DSGetShaderResources(0, kSrvSlots, tmp); break;
                case 3: c->GSGetShaderResources(0, kSrvSlots, tmp); break;
                case 4: c->PSGetShaderResources(0, kSrvSlots, tmp); break;
            }
            for (UINT s = 0; s < kSrvSlots; s++) {
                if (!tmp[s]) continue;
                ID3D11Resource* r = nullptr;
                tmp[s]->GetResource(&r);
                bool hit = r && std::find(written, written + writtenCount, (ID3D11Buffer*)r) != written + writtenCount;
                if (r) r->Release();
                if (hit) gfx.push_back({stage, s, tmp[s]});
                else tmp[s]->Release();
            }
        }
    }
    void restore(ID3D11DeviceContext* c) {
        UINT keep[kUavSlots];
        for (UINT& k : keep) k = (UINT)-1;
        c->CSSetUnorderedAccessViews(0, kUavSlots, uav, keep);   // replaces ours first, so the SRVs below can bind
        c->CSSetShaderResources(0, kSrvSlots, srv);
        c->CSSetConstantBuffers(0, 1, &cb);
        c->CSSetShader(cs, nInst ? inst : nullptr, nInst);
        for (auto& g : gfx) {
            switch (g.stage) {
                case 0: c->VSSetShaderResources(g.slot, 1, &g.v); break;
                case 1: c->HSSetShaderResources(g.slot, 1, &g.v); break;
                case 2: c->DSSetShaderResources(g.slot, 1, &g.v); break;
                case 3: c->GSSetShaderResources(g.slot, 1, &g.v); break;
                case 4: c->PSSetShaderResources(g.slot, 1, &g.v); break;
            }
            g.v->Release();
        }
        gfx.clear();
        safeRelease(cs);
        for (UINT q = 0; q < 256; q++) safeRelease(inst[q]);
        safeRelease(cb);
        for (auto& v : srv) safeRelease(v);
        for (auto& v : uav) safeRelease(v);
    }
};

// ---------------------------------------------------------------- diagnostics: GPU time per physics step

struct TimerSlot { ID3D11Query *disjoint = nullptr, *t0 = nullptr, *t1 = nullptr; int channel = -1; bool open = false, pending = false; };
static const int kTimerSlots = 256, kChannels = 2;
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

static void runTimer(int channel, bool begin, ID3D11Buffer* any) {
    if (channel < 0 || channel >= kChannels || !any || !ensureDevice(any)) return;
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

static int runCheckOp(Item& it, int op, ID3D11Buffer* particles) {
    if (!makeCopy(particles, &it.copyA, false) || !makeCopy(particles, &it.copyB, false)) {
        setLastError("item %d: diagnostics copies failed", it.id);
        return kErrCreate;
    }
    switch (op) {
        case kOpSnapshot: gCtx->CopyResource(it.copyA, particles); return kOk;
        case kOpSaveRef: gCtx->CopyResource(it.copyB, particles); return kOk;
        case kOpRestore: gCtx->CopyResource(particles, it.copyA); return kOk;
        default: break;
    }
    if (!makeCopy(particles, &it.stageA, true) || !makeCopy(particles, &it.stageB, true)) {
        setLastError("item %d: diagnostics staging buffers failed", it.id);
        return kErrCreate;
    }
    gCtx->CopyResource(it.stageA, particles);
    gCtx->CopyResource(it.stageB, it.copyB);
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
        if (memcmp(a, b, 12) == 0) equal++;
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

// ---------------------------------------------------------------- one job

static void dispatch(Item& it, FusedCB& p, ID3D11ComputeShader* cs, bool doB, bool doEnd, bool doA, bool reset,
                     bool importStruct, bool exportStruct, uint32_t groups) {
    p.doB = doB; p.doEnd = doEnd; p.doA = doA; p.doReset = reset;
    p.importStruct = importStruct || reset || p.clingMode == 0;
    p.exportStruct = exportStruct || reset || p.clingMode == 0;
    p.readParity = it.parity;
    ID3D11Buffer* cb = gCb;
    if (gOptions.load() & kOptDynamicConstants) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(gCtx->Map(gCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { it.error = kErrDevice; setLastError("item %d: mapping the constants failed", it.id); return; }
        memcpy(m.pData, &p, sizeof p);
        gCtx->Unmap(gCb, 0);
    } else {
        cb = gCbDefault;
        gCtx->UpdateSubresource(cb, 0, nullptr, &p, 0, 0);
    }
    ID3D11UnorderedAccessView* uav[4] = {it.vParticles.uav, it.bCompact.uav, it.bSnap.uav, it.bHot.uav};
    UINT keep[4] = {(UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1};
    gCtx->CSSetUnorderedAccessViews(0, 4, uav, keep);
    ID3D11ShaderResourceView* srv[10] = {it.vPointJoints.srv, it.vTransforms.srv, it.bRest2.srv, it.vSpheres.srv,
                                         it.vLineSpheres.srv, it.bOffsets.srv, it.bEntries.srv, it.vPlanes.srv, it.vPositionOnly.srv,
                                         it.vBodySpheres.srv};
    gCtx->CSSetShaderResources(0, 10, srv);
    gCtx->CSSetShader(cs, nullptr, 0);
    gCtx->CSSetConstantBuffers(0, 1, &cb);
    gCtx->Dispatch(groups, 1, 1);
    it.dispatches++;
}

// A step job's dispatches, in order.
struct Planned {
    struct D { ID3D11ComputeShader* cs; bool b, end, a, reset, import, exp, flip; };
    Item* it = nullptr;
    ID3D11Buffer* particles = nullptr;
    uint32_t groups = 0;
    FusedCB p = {};
    std::vector<D> d;
};

// Views of the item's buffers, the constants and the dispatch list of one step (the device is known).
static int plan(Item& it, const HSStep& s, Planned& q) {
    ID3D11Buffer* particles = (ID3D11Buffer*)s.particles;
    if (!it.created) { int e = createItemResources(it); if (e) return e; }
    int e;
    if ((e = bindView(it.vParticles, s.particles, 116, it.n, true, "particles")) ||
        (e = bindView(it.vPointJoints, s.pointJoints, 24, it.n, false, "point joints")) ||
        (e = bindView(it.vTransforms, s.transforms, 64, 1, false, "scalp matrices")) ||
        (e = bindView(it.vSpheres, s.spheres, 32, 0, false, "spheres")) ||
        (e = bindView(it.vLineSpheres, s.lineSpheres, 72, 0, false, "line spheres")) ||
        (e = bindView(it.vPlanes, s.planes, 16, 0, false, "planes")) ||
        (e = bindView(it.vPositionOnly, s.positionOnly, 4, 0, false, "position-only mask")) ||
        (e = bindView(it.vBodySpheres, s.bodySpheres, 16, 0, false, "body spheres")))
        return e;

    const uint32_t perGroup = 256 / it.seg;
    q.it = &it;
    q.particles = particles;
    q.groups = (it.strands + perGroup - 1) / perGroup;
    q.d.clear();
    FusedCB& p = q.p;
    p = FusedCB();
    p.particleCount = it.n;
    p.segments = it.seg;
    p.strandCount = it.strands;
    p.strandsPerGroup = perGroup;
    p.DT = s.DT;
    memcpy(p.accelDT2, s.accelDT2, sizeof p.accelDT2);
    p.invDrag = s.invDrag; p.weight = s.weight; p.distanceScale = s.distanceScale; p.distancePower = s.distancePower;
    p.compressionScale = s.compressionScale; p.compressionPower = s.compressionPower; p.splinePower = s.splinePower;
    p.dtrecipInner = s.dtrecipInner; p.stepInner = s.stepInner; p.dtrecipOuter = s.dtrecipOuter; p.stepOuter = s.stepOuter;
    p.friction = s.friction; p.staticFriction = s.staticFriction; p.collisionPower = s.collisionPower;
    p.sphereCount = it.vSpheres.count;
    p.lineSphereCount = it.vLineSpheres.count;
    p.planeCount = it.vPlanes.count;
    p.positionOnlyCount = it.vPositionOnly.count;
    p.positionOnlySpheres = (uint32_t)s.reserved;
    p.bodyCount = it.vBodySpheres.srv ? std::min((uint32_t)std::max(s.bodyCount, 0), it.vBodySpheres.count) : 0;
    p.doIntegrate = s.doIntegrate != 0; p.doCompression = s.doCompression != 0; p.doCollision = s.doCollision != 0;
    p.isFixed = s.isFixed;
    p.nearbyScale = s.nearbyScale; p.nearbyPower = s.nearbyPower; p.nearbyRolloff = s.nearbyRolloff;
    p.clingCap = s.clingCap;
    p.outer = (uint32_t)std::max(1, s.outer);
    p.iterations = (uint32_t)std::max(1, s.iterations);
    p.distanceRepeat = (uint32_t)std::max(1, s.distanceRepeat);
    p.compressionRepeat = (uint32_t)std::max(1, s.compressionRepeat);
    p.clingRepeat = (uint32_t)std::max(1, s.clingRepeat);

    // dispatch list: shader, B / End / A, reset, import / export the GPUTools struct, parity flips after it
    switch (s.op) {
        case kOpReset:
            p.clingMode = 0;
            q.d.push_back({gCsStrand, false, false, false, true, true, true, false});
            break;
        case kOpStep:
            p.clingMode = 2;
            q.d.push_back({gCsStep, false, false, false, false, true, true, true});
            break;
        case kOpJacobi: {   // as harness\hair_harness.cpp FusedSolver::step, mode 1
            p.clingMode = 1;
            bool pendingB = false, pendingEnd = false, first = true;
            for (uint32_t o = 0; o < p.outer; o++) {
                for (uint32_t i = 0; i < p.iterations; i++) {
                    q.d.push_back({gCsStrand, pendingB, pendingEnd, true, false, first, false, true});   // B of the next dispatch reads what this A wrote
                    first = false;
                    pendingEnd = false;
                    pendingB = true;
                }
                pendingEnd = true;
            }
            q.d.push_back({gCsStrand, true, true, false, false, false, true, false});
            break;
        }
        case kOpStrand:
            p.clingMode = 0;
            q.d.push_back({gCsStrand, (s.flags & kFlagB) != 0, (s.flags & kFlagEnd) != 0, (s.flags & kFlagA) != 0, false, true, true, false});
            break;
        default:
            setLastError("item %d: unknown op %d", it.id, s.op);
            return kErrBuffer;
    }
    return kOk;
}

static void runPlanned(Planned& q, size_t k) {
    const Planned::D& x = q.d[k];
    dispatch(*q.it, q.p, x.cs, x.b, x.end, x.a, x.reset, x.import, x.exp, q.groups);
    if (x.flip) q.it->parity ^= 1;
}

static int runStep(Item& it, const HSStep& s) {
    ID3D11Buffer* particles = (ID3D11Buffer*)s.particles;
    if (!particles) { setLastError("item %d: no particle buffer", it.id); return kErrBuffer; }
    if (!ensureDevice(particles)) return gDeviceError.load();
    if (s.op >= kOpSnapshot && s.op <= kOpCompare) return runCheckOp(it, s.op, particles);
    Planned q;
    int e = plan(it, s, q);
    if (e) return e;
    SavedState saved;
    saved.save(gCtx, &particles, 1);
    for (size_t k = 0; k < q.d.size(); k++) runPlanned(q, k);
    saved.restore(gCtx);
    it.steps++;
    return kOk;
}

// kOptBatch: the steps queued since the last solve, all items' first dispatches, then all second dispatches... On
// D3D11On12 a Flush between those phases starts a new command list, so the translation layer puts no UAV barrier
// between one item's dispatch and the next item's (it only does for resources used as UAV earlier in the same command
// list, and on NVIDIA such a barrier waits for all earlier work); each item's own dispatches stay in order. A later
// step of an item (kOptRepeat) starts in the phase after its step before ends.
static std::vector<Planned> gBatch;          // render thread only
static std::atomic<int> gBatches{0}, gBatchItems{0}, gBatchPhases{0};

static void solveBatch() {
    if (gBatch.empty()) return;
    std::vector<ID3D11Buffer*> written;
    std::vector<size_t> first(gBatch.size(), 0);   // the phase of each step's first dispatch
    size_t phases = 0;
    for (size_t i = 0; i < gBatch.size(); i++) {
        Planned& q = gBatch[i];
        for (size_t j = i; j-- > 0;)
            if (gBatch[j].it == q.it) { first[i] = first[j] + gBatch[j].d.size(); break; }
        if (std::find(written.begin(), written.end(), q.particles) == written.end()) written.push_back(q.particles);
        phases = std::max(phases, first[i] + q.d.size());
    }
    const bool flush = gOn12 && written.size() > 1;
    SavedState saved;
    saved.save(gCtx, written.data(), written.size());
    for (size_t k = 0; k < phases; k++) {
        for (size_t i = 0; i < gBatch.size(); i++) {
            Planned& q = gBatch[i];
            if (k >= first[i] && k - first[i] < q.d.size() && !q.it->error.load()) runPlanned(q, k - first[i]);
        }
        if (flush && k + 1 < phases) gCtx->Flush();
    }
    saved.restore(gCtx);
    for (Planned& q : gBatch) q.it->steps++;
    gBatches++;
    gBatchItems += (int)gBatch.size();
    gBatchPhases += (int)phases;
    gBatch.clear();
}

// The step reads the buffers the item's views show now (planning it with others would rebind the views its waiting
// steps run with).
static bool sameViews(const Item& it, const HSStep& s) {
    return it.vParticles.buffer == s.particles && it.vPointJoints.buffer == s.pointJoints &&
           it.vTransforms.buffer == s.transforms && it.vSpheres.buffer == s.spheres &&
           it.vLineSpheres.buffer == s.lineSpheres && it.vPlanes.buffer == s.planes &&
           it.vPositionOnly.buffer == s.positionOnly && it.vBodySpheres.buffer == s.bodySpheres;
}

static void __stdcall OnRenderEvent(int eventId) {
    Job job;
    {
        std::lock_guard<std::mutex> l(gLock);
        Job& slot = gRing[eventId & (kRing - 1)];
        if (slot.seq != eventId) { gLostJobs++; return; }
        job = slot;
        slot.seq = -1;
    }
    Item* it = nullptr;
    if (job.kind == kJobStep || job.kind == kJobRelease) {
        {
            std::lock_guard<std::mutex> l(gLock);
            auto f = gItems.find(job.item);
            if (f != gItems.end()) it = f->second;
        }
        if (!it) { solveBatch(); return; }
    }
    // a step that can wait joins the batch (a second step of the same item too with kOptRepeat, unless it reads other
    // buffers than the item's waiting steps: then the batch runs first); anything else runs after the batch
    const int op = job.step.op, options = gOptions.load();
    if (job.kind == kJobStep && (options & kOptBatch) && (op == kOpReset || op == kOpStep || op == kOpJacobi)) {
        if (it->error.load()) return;
        for (Planned& q : gBatch)
            if (q.it == it) { if (!(options & kOptRepeat) || !sameViews(*it, job.step)) solveBatch(); break; }
        ID3D11Buffer* particles = (ID3D11Buffer*)job.step.particles;
        int e = !particles ? kErrBuffer : !ensureDevice(particles) ? gDeviceError.load() : kOk;
        if (!e) {
            gBatch.emplace_back();
            e = plan(*it, job.step, gBatch.back());
            if (e) gBatch.pop_back();
        }
        if (e) { if (!particles) setLastError("item %d: no particle buffer", it->id); it->error = e; }
        return;
    }
    solveBatch();
    if (job.kind == kJobSolve) return;
    if (job.kind == kJobTimer) { runTimer(job.item, job.step.op != 0, (ID3D11Buffer*)job.step.particles); return; }
    if (job.kind == kJobRelease) {
        {
            std::lock_guard<std::mutex> l(gLock);
            gItems.erase(job.item);
        }
        delete it;
        return;
    }
    if (it->error.load()) return;
    int e = runStep(*it, job.step);
    if (e) it->error = e;
}

// ---------------------------------------------------------------- exports

HS_API int HS_ApiVersion() { return kApiVersion; }
HS_API int HS_StepSize() { return (int)sizeof(HSStep); }
HS_API void* HS_GetRenderEventFunc() { return (void*)&OnRenderEvent; }

// Copies the item's rest lengths (float2 per particle: x distance / spline rest of (i-1, i), y compression rest of
// (i-2, i)) and cling adjacency (CSR over particles, both ends of every joint). Returns the item id (> 0) or 0.
HS_API int HS_CreateItem(int particleCount, int segments, const float* rest2, const uint32_t* clingOffsets,
                         const HSClingEntry* clingEntries, int clingEntryCount) {
    if (particleCount <= 0 || segments < 2 || segments > 256 || particleCount % segments || !rest2 || !clingOffsets ||
        clingEntryCount < 0 || (clingEntryCount > 0 && !clingEntries)) {
        setLastError("HS_CreateItem: bad arguments (%d particles, %d points per strand)", particleCount, segments);
        return 0;
    }
    const uint32_t strands = (uint32_t)(particleCount / segments), perGroup = 256 / (uint32_t)segments;
    if ((strands + perGroup - 1) / perGroup > D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION) {
        setLastError("HS_CreateItem: %u strands need more than 65535 thread groups", strands);
        return 0;
    }
    if (clingOffsets[particleCount] != (uint32_t)clingEntryCount) {
        setLastError("HS_CreateItem: cling offsets end at %u, %d entries", clingOffsets[particleCount], clingEntryCount);
        return 0;
    }
    Item* it = new Item();
    it->n = (uint32_t)particleCount;
    it->seg = (uint32_t)segments;
    it->strands = strands;
    it->rest2.assign(rest2, rest2 + 2 * (size_t)particleCount);
    it->clingOffsets.assign(clingOffsets, clingOffsets + particleCount + 1);
    if (clingEntryCount) it->clingEntries.assign(clingEntries, clingEntries + clingEntryCount);
    std::lock_guard<std::mutex> l(gLock);
    it->id = gNextItem++;
    gItems[it->id] = it;
    return it->id;
}

// Queues one job for the item; returns the event id to pass to GL.IssuePluginEvent, or -1.
HS_API int HS_QueueStep(int item, const HSStep* step) {
    if (!step) return -1;
    {
        std::lock_guard<std::mutex> l(gLock);
        if (!gItems.count(item)) return -1;
    }
    return enqueue(kJobStep, item, step);
}

// The item is freed on the render thread, after every job queued before this one.
HS_API int HS_QueueRelease(int item) { return enqueue(kJobRelease, item, nullptr); }

// 0 fine, < 0 an HSError (the item stays stopped: the C# side hands it back to GPUTools and releases it).
HS_API int HS_ItemStatus(int item) {
    if (gDeviceError.load()) return gDeviceError.load();
    std::lock_guard<std::mutex> l(gLock);
    auto f = gItems.find(item);
    return f == gItems.end() ? 0 : f->second->error.load();
}

// Jobs and dispatches run so far for the item (diagnostics).
HS_API void HS_ItemCounters(int item, int* steps, int* dispatches) {
    std::lock_guard<std::mutex> l(gLock);
    auto f = gItems.find(item);
    if (steps) *steps = f == gItems.end() ? 0 : f->second->steps.load();
    if (dispatches) *dispatches = f == gItems.end() ? 0 : f->second->dispatches.load();
}

// Diagnostics: GPU timestamp bracket around one physics step (begin != 0 opens, 0 closes) on channel 0 (GPUTools) or
// 1 (this solver); anyBuffer = one of the item's GPUTools buffers (for the device). Returns the event id or -1.
HS_API int HS_QueueTimer(int channel, int begin, void* anyBuffer) {
    if (channel < 0 || channel >= kChannels || !anyBuffer) return -1;
    HSStep st = {};
    st.op = begin ? 1 : 0;
    st.particles = anyBuffer;
    return enqueue(kJobTimer, channel, &st);
}

// GPU milliseconds per bracket on a channel (finished brackets so far); reset = 1 clears after reading.
HS_API void HS_GpuTime(int channel, double* sumMs, double* maxMs, int* count, int* skipped, int reset) {
    std::lock_guard<std::mutex> l(gLock);
    GpuTime z;
    GpuTime& g = (channel >= 0 && channel < kChannels) ? gGpuTime[channel] : z;
    if (sumMs) *sumMs = g.sum;
    if (maxMs) *maxMs = g.max;
    if (count) *count = g.count;
    if (skipped) *skipped = g.skipped;
    if (reset) g = GpuTime();
}

// Diagnostics: the item's checks against GPUTools so far (positions in metres, velocities in m/s); reset = 1 clears.
// out[0] last max position difference, [1] last mean, [2] last max velocity difference, [3] worst max position,
// [4] worst max velocity, [5] mean of the means; counts[0] particles bit-identical in the last check, [1] non-finite
// differences, [2] checks.
HS_API void HS_ItemVerify(int item, float* out6, int* counts3, int reset) {
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

// Jobs that were overwritten before the render thread reached them (each one is a skipped physics step).
HS_API int HS_LostJobs() { return gLostJobs.load(); }

// Runs the step jobs queued so far (kOptBatch); returns the event id or -1.
HS_API int HS_QueueSolve() { return enqueue(kJobSolve, 0, nullptr); }

// Diagnostics: HSOption bits (options < 0 only reads them) for the jobs the render thread runs from then on; returns them.
// Turning kOptBatch off needs a solve event afterwards for the steps already waiting. Bit 8 of the result: D3D11On12.
HS_API int HS_SetOptions(int options) {
    if (options >= 0) gOptions = options & (kOptDynamicConstants | kOptBatch | kOptRepeat);
    return gOptions.load() | (gOn12 ? 256 : 0);
}

// Diagnostics: solves so far, the step jobs and phases in them; reset = 1 clears after reading.
HS_API void HS_BatchStats(int* solves, int* steps, int* phases, int reset) {
    if (solves) *solves = gBatches.load();
    if (steps) *steps = gBatchItems.load();
    if (phases) *phases = gBatchPhases.load();
    if (reset) { gBatches = 0; gBatchItems = 0; gBatchPhases = 0; }
}

// Last error text (UTF-8, truncated to cap - 1); returns its full length.
HS_API int HS_GetLastError(char* buf, int cap) {
    std::lock_guard<std::mutex> l(gLock);
    if (buf && cap > 0) {
        size_t m = std::min(gLastError.size(), (size_t)cap - 1);
        memcpy(buf, gLastError.data(), m);
        buf[m] = 0;
    }
    return (int)gLastError.size();
}
