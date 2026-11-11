// Fused GPUTools cloth step. Same physics as ClothPhysicsWorld.DispatchPhysicsImpl (frame > 20, usesPreCalcVerts,
// no neighbour collision), same order and arithmetic as GPUTools' kernels (docs\P0-原布料管线.md section 1), in
// 1 + 2 * outer dispatches per step instead of outer * (iterations * (3 + groups) + 6):
//   CSImport    (one thread per particle)  GPUTools' particle buffer -> X / S
//   per outer step o:
//     CSIterate (ONE thread group per item) for each iteration: IntegrateIter, PointJointsPreCalculated (own particle),
//               then every joint colour group with a group barrier in between (GPUTools: one dispatch per group),
//               then IntegrateVelocityInner
//     CSCollide (one thread per particle)  ParticleCollisionReset, ParticlePlaneCollision, ParticleSphereCollision,
//               ParticleLineSphereCollision, GrabJoints, IntegrateVelocity; writes GPUTools' particle buffer back
// A particle's point joint is handled by the thread that owns the particle: the plugin only takes items where every
// particle has exactly one point joint with BodyId = its index (all recorded items do).
// SHARED: CSIterate keeps positions in groupshared memory (items up to MAX_SHARED particles).
// Joints of one colour group never share a particle (checked by tools\check_records.py), so a thread may load several
// of them before storing any (JOINTS_IN_FLIGHT): same results, fewer memory round trips on the one multiprocessor.
// Joints are packed as (A | B << 16, Rest): items have fewer than 65536 particles; Elasticity is not used by GPUTools.
// MULTI: CSIterateMulti spreads an item in device memory over multiGroups thread groups (one dispatch of that many), with
// a counter in Sync standing in for the group barrier between colour groups (every group waits until all have arrived;
// the plugin keeps the groups of all items of a solve well under the multiprocessor count, so they are all resident).
// Same joints per colour group, same arithmetic: only which thread solves which joint changes. X is globallycoherent so
// that positions stored by one group are seen by the others; a wait that never ends gives up after SPIN_LIMIT polls and
// sets Sync[2] (the native library then goes back to one group).

struct Particle {
    float3 Position;
    float3 LastPosition;
    float3 LastPositionInner;
    float3 DrawPosition;
    float3 Velocity;
    float Radius;
    float3 ColliderDelta;
    float CollisionDrag;
    float CollisionHold;
    float CollisionPower;
    float Strength;
    int CollisionEnabled;
    int SphereCollisionID;
    int LineSphereCollisionID;
    int GrabID;
    float GrabDistance;
    float AuxData;
};
struct PointJoint { int BodyId; int MatrixId; float3 Point; float Rigidity; };
struct SphereD { float3 Position; float Radius; float Friction; float3 Delta; };
struct LineSphereD { float3 PositionA; float3 PositionB; float RadiusA; float RadiusB; float Friction; float3 DeltaA; float3 DeltaB; float3 Delta; };
struct GrabSphere { int ID; float3 Position; float Radius; int Grab; };

cbuffer Params : register(b0) {
    uint particleCount, groupCount, iterations, doIntegrate;
    uint doCollision, sphereCount, lineSphereCount, planeCount;
    uint grabCount, multiGroups, pad1, pad2;
    float DT, dtrecipInner, stepInner, invDrag;
    float3 accelDT2; float stiffness;
    float compression, distanceScale, jointStrength, jointPrediction;
    float breakThreshold, friction, staticFriction, collisionPower;
    float dtrecipOuter, stepOuter, pad3, pad4;
};

RWStructuredBuffer<Particle> particles : register(u0);
RWStructuredBuffer<PointJoint> pointJoints : register(u1);
#ifdef MULTI
globallycoherent RWByteAddressBuffer X : register(u2);
globallycoherent RWByteAddressBuffer Sync : register(u4);    // arrivals, groups done, gave up
#else
RWByteAddressBuffer X : register(u2);
#endif
                                                             // X: float4 per particle: position, joint weight
                                                             // Strength * (1 + CollisionPower)
RWStructuredBuffer<float4> S : register(u3);                 // 3 x particleCount: LastPositionInner | CollisionDrag,
                                                             // Velocity | CollisionHold, ColliderDelta | CollisionPower
StructuredBuffer<float3> cur : register(t0);                 // skin-wrap targets this step
StructuredBuffer<float3> old : register(t1);                 // and the step before
StructuredBuffer<uint2> joints : register(t2);               // stiffness joints, then nearby joints: A | B << 16, Rest
StructuredBuffer<int2> groups : register(t3);                // (start, count) in solve order, GPUTools' GroupsData
StructuredBuffer<SphereD> spheres : register(t4);
StructuredBuffer<LineSphereD> lineSpheres : register(t5);
StructuredBuffer<float4> planes : register(t6);
StructuredBuffer<GrabSphere> grabSpheres : register(t7);

#define ITERATE_THREADS 1024
#define MAX_SHARED 2720
#ifndef JOINTS_IN_FLIGHT
#define JOINTS_IN_FLIGHT 1
#endif

float4 LoadS(uint p, uint k) { return S[k * particleCount + p]; }
void StoreS(uint p, uint k, float4 v) { S[k * particleCount + p] = v; }
float4 LoadX(uint p) { return asfloat(X.Load4(p * 16)); }
void StoreX(uint p, float4 v) { X.Store4(p * 16, asuint(v)); }
void StoreXPos(uint p, float3 v) { X.Store3(p * 16, asuint(v)); }

[numthreads(256, 1, 1)]
void CSImport(uint3 id : SV_DispatchThreadID) {
    uint p = id.x;
    if (p >= particleCount) return;
    Particle q = particles[p];
    StoreX(p, float4(q.Position, q.CollisionPower * q.Strength + q.Strength));
    StoreS(p, 0, float4(q.LastPositionInner, q.CollisionDrag));
    StoreS(p, 1, float4(q.Velocity, q.CollisionHold));
    StoreS(p, 2, float4(q.ColliderDelta, q.CollisionPower));
}

// IntegrateIter + PointJointsPreCalculated (isFixed = 0) for particle p, given its Velocity and CollisionDrag
float3 Predict(uint p, float3 x, float3 vel, float drag) {
    if (doIntegrate != 0) {
        float3 r1 = vel * invDrag;
        float3 r2 = (1.0 - drag) * accelDT2;
        x = x + (r1 * DT + r2);
    }
    PointJoint pj = pointJoints[p];
    float3 c = cur[pj.MatrixId];
    float3 o = old[pj.MatrixId];
    float rig = pj.Rigidity;
    float3 t = (c - o) * jointPrediction + c;
    if (rig < 1.0) {
        float3 d = x - t;
        d = saturate(rig) * d;
        float l1 = abs(d.y) + abs(d.x);
        l1 = abs(d.z) + l1;
        l1 = l1 - breakThreshold;
        float m = max(l1 * 1000000000.0, 0.0);
        m = min(rig, m);
        pointJoints[p].Rigidity = rig - m;
        t = -d * jointStrength + x;
    }
    return t;
}

// StiffnessJoints for one joint (GPUTools' arithmetic order)
void SolveJoint(float rest, inout float3 a, inout float3 b, float wA, float wB) {
    float3 d = a - b;
    float len = sqrt(dot(d, d));
    float den = max(rest * distanceScale * 0.05, len);
    float c = rest * distanceScale - den;
    c = min(c, c * compression);
    c = c / den;
    float3 corr = (c * d) * stiffness;
    float rw = 1.0 / (wB + wA);
    a = corr * (wB * rw) + a;
    b = -corr * (wA * rw) + b;
}

#ifdef SHARED
groupshared float3 sh[MAX_SHARED];
#define POS_LOAD(p) sh[p]
#define POS_STORE(p, v) sh[p] = (v)
#define WEIGHT(p) asfloat(X.Load(p * 16 + 12))
#define BARRIER GroupMemoryBarrierWithGroupSync()
#else
#define POS_LOAD(p) asfloat(X.Load3(p * 16))
#define POS_STORE(p, v) StoreXPos(p, v)
#define BARRIER DeviceMemoryBarrierWithGroupSync()
#endif

// every joint colour group in order (StiffnessJoints dispatches), a group barrier after each
void SolveGroups(uint tid) {
    for (uint g = 0; g < groupCount; g++) {
        int2 gr = groups[g];
        for (uint base = 0; base < (uint)gr.y; base += ITERATE_THREADS * JOINTS_IN_FLIGHT) {
            uint2 jt[JOINTS_IN_FLIGHT];
            float4 a[JOINTS_IN_FLIGHT], b[JOINTS_IN_FLIGHT];
            [unroll] for (uint k = 0; k < JOINTS_IN_FLIGHT; k++) {
                uint j = base + k * ITERATE_THREADS + tid;
                jt[k] = j < (uint)gr.y ? joints[gr.x + j] : uint2(0, 0);
            }
            [unroll] for (uint k2 = 0; k2 < JOINTS_IN_FLIGHT; k2++) {
                uint ia = jt[k2].x & 0xffff, ib = jt[k2].x >> 16;
#ifdef SHARED
                a[k2] = float4(sh[ia], WEIGHT(ia));
                b[k2] = float4(sh[ib], WEIGHT(ib));
#else
                a[k2] = asfloat(X.Load4(ia * 16));
                b[k2] = asfloat(X.Load4(ib * 16));
#endif
            }
            [unroll] for (uint k3 = 0; k3 < JOINTS_IN_FLIGHT; k3++) {
                if (base + k3 * ITERATE_THREADS + tid < (uint)gr.y) {
                    uint ia = jt[k3].x & 0xffff, ib = jt[k3].x >> 16;
                    float3 pa = a[k3].xyz, pb = b[k3].xyz;
                    SolveJoint(asfloat(jt[k3].y), pa, pb, a[k3].w, b[k3].w);
                    POS_STORE(ia, pa);
                    POS_STORE(ib, pb);
                }
            }
        }
        BARRIER;
    }
}

// IntegrateVelocityInner for particle p at x; returns Velocity, CollisionDrag for the next IntegrateIter
float4 InnerVelocity(uint p, float3 x) {
    float4 s0 = LoadS(p, 0), s1 = LoadS(p, 1), s2 = LoadS(p, 2);
    float3 r1 = x - s0.xyz;
    float3 r2 = s2.xyz * stepInner - r1;
    r1 = s1.w * r2 + r1;
    float3 vel = r1 * dtrecipInner;
    StoreS(p, 1, float4(vel, s1.w));
    StoreS(p, 0, float4(x, s0.w));
    return float4(vel, s0.w);
}

[numthreads(ITERATE_THREADS, 1, 1)]
void CSIterate(uint3 gtid : SV_GroupThreadID) {
    uint tid = gtid.x;
    for (uint p = tid; p < particleCount; p += ITERATE_THREADS) {
        float3 x = Predict(p, asfloat(X.Load3(p * 16)), LoadS(p, 1).xyz, LoadS(p, 0).w);
        POS_STORE(p, x);
    }
    BARRIER;
    for (uint it = 1; it < iterations; it++) {
        SolveGroups(tid);
        // IntegrateVelocityInner, then IntegrateIter + PointJointsPreCalculated of the next iteration: own particle only
        for (uint p2 = tid; p2 < particleCount; p2 += ITERATE_THREADS) {
            float3 x = POS_LOAD(p2);
            float4 vd = InnerVelocity(p2, x);
            POS_STORE(p2, Predict(p2, x, vd.xyz, vd.w));
        }
        BARRIER;
    }
    SolveGroups(tid);
    for (uint p3 = tid; p3 < particleCount; p3 += ITERATE_THREADS) {
        float3 x = POS_LOAD(p3);
        InnerVelocity(p3, x);
#ifdef SHARED
        StoreXPos(p3, x);
#endif
    }
}

#ifdef MULTI
#ifndef SPIN_LIMIT
#define SPIN_LIMIT 50000
#endif
// every thread of every group of the dispatch has stored before any loads past it (the colour group barrier across groups).
// fxc merges two group barriers of the same kind with only UAV work between them into one, so the two here differ and
// each carries a group-shared value that the code after it uses: thread 0 counts the group in only after the last
// thread's mark (the stores of the whole group are done), and the group goes on only with thread 0's release.
// A group that has waited SPIN_LIMIT polls, or sees that another one gave up, stops waiting for the rest of the dispatch.
groupshared uint gsMark, gsRelease, gsQuit;

void GridSync(uint tid, inout uint epoch) {
    epoch += 1;
    if (tid == ITERATE_THREADS - 1) gsMark = epoch;
    AllMemoryBarrierWithGroupSync();
    if (tid == 0) {
        uint seen;
        Sync.InterlockedAdd(0, gsMark - epoch + 1, seen);
        seen += 1;
        uint target = epoch * multiGroups, spins = 0;
        [allow_uav_condition] while (seen < target && gsQuit == 0) {
            if ((++spins & 255) == 0) {
                uint flag;
                Sync.InterlockedOr(8, 0, flag);
                if (flag != 0 || spins > SPIN_LIMIT) { Sync.InterlockedOr(8, 1, flag); gsQuit = 1; }
            }
            Sync.InterlockedOr(0, 0, seen);
        }
        gsRelease = epoch;
    }
    GroupMemoryBarrierWithGroupSync();
    epoch = gsRelease;
}

void SolveGroupsMulti(uint tid, uint gt, inout uint epoch) {
    uint all = ITERATE_THREADS * multiGroups;
    for (uint g = 0; g < groupCount; g++) {
        int2 gr = groups[g];
        for (uint j = gt; j < (uint)gr.y; j += all) {
            uint2 jt = joints[gr.x + j];
            uint ia = jt.x & 0xffff, ib = jt.x >> 16;
            float4 a = asfloat(X.Load4(ia * 16));
            float4 b = asfloat(X.Load4(ib * 16));
            float3 pa = a.xyz, pb = b.xyz;
            SolveJoint(asfloat(jt.y), pa, pb, a.w, b.w);
            StoreXPos(ia, pa);
            StoreXPos(ib, pb);
        }
        GridSync(tid, epoch);
    }
}

// CSIterate over multiGroups thread groups (device memory only)
[numthreads(ITERATE_THREADS, 1, 1)]
void CSIterateMulti(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    uint tid = gtid.x, gt = gid.x * ITERATE_THREADS + tid, all = ITERATE_THREADS * multiGroups, epoch = 0;
    if (tid == 0) gsQuit = 0;
    for (uint p = gt; p < particleCount; p += all) {
        float3 x = Predict(p, asfloat(X.Load3(p * 16)), LoadS(p, 1).xyz, LoadS(p, 0).w);
        StoreXPos(p, x);
    }
    GridSync(tid, epoch);
    for (uint it = 1; it < iterations; it++) {
        SolveGroupsMulti(tid, gt, epoch);
        for (uint p2 = gt; p2 < particleCount; p2 += all) {
            float3 x = asfloat(X.Load3(p2 * 16));
            float4 vd = InnerVelocity(p2, x);
            StoreXPos(p2, Predict(p2, x, vd.xyz, vd.w));
        }
        GridSync(tid, epoch);
    }
    SolveGroupsMulti(tid, gt, epoch);
    for (uint p3 = gt; p3 < particleCount; p3 += all)
        InnerVelocity(p3, asfloat(X.Load3(p3 * 16)));
    // the last group out resets the counters for the next dispatch (the next one of this item comes after a collide)
    if (tid == 0) {
        uint done;
        Sync.InterlockedAdd(4, 1, done);
        if (done == multiGroups - 1) { Sync.Store(0, 0); Sync.Store(4, 0); }
    }
}
#endif

[numthreads(256, 1, 1)]
void CSCollide(uint3 id : SV_DispatchThreadID) {
    uint p = id.x;
    if (p >= particleCount) return;
    Particle q = particles[p];
    float4 x4 = LoadX(p), s0 = LoadS(p, 0), s1 = LoadS(p, 1), s2 = LoadS(p, 2);
    q.Position = x4.xyz;
    q.LastPositionInner = s0.xyz; q.CollisionDrag = s0.w;
    q.Velocity = s1.xyz; q.CollisionHold = s1.w;
    q.ColliderDelta = s2.xyz; q.CollisionPower = s2.w;
    if (doCollision != 0) {
        q.CollisionDrag = 0; q.CollisionHold = 0; q.CollisionPower = 0;                // ParticleCollisionReset
        q.SphereCollisionID = -1; q.LineSphereCollisionID = -1;
        if (q.CollisionEnabled != 0) {
            const float3 x0 = q.Position;                                             // ParticlePlaneCollision
            for (uint k = 0; k < planeCount; k++) {
                float4 pl = planes[k];
                float dist = dot(x0, pl.xyz) + pl.w;
                if (q.Radius >= abs(dist)) {
                    float3 nx = pl.xyz * (q.Radius - abs(dist)) + q.Position;
                    q.Position = nx;
                    q.LastPosition = nx;
                }
            }
            for (uint s = 0; s < sphereCount; s++) {                                  // ParticleSphereCollision
                SphereD sp = spheres[s];
                float3 d = q.Position - sp.Position;
                float R = q.Radius + sp.Radius;
                float d2 = dot(d, d);
                if (R * R < d2) continue;
                float pen = R - sqrt(d2);
                q.Position = ((rsqrt(d2) * d) * pen) * collisionPower + q.Position;
                float h = min(max(pen * 50.0, 0.0), 0.5);
                q.CollisionPower = staticFriction * sp.Friction + h * 50.0;
                q.CollisionHold = saturate(h + friction + 0.02);
                q.ColliderDelta = sp.Delta;
                q.CollisionDrag = friction;
                q.SphereCollisionID = (int)s;
            }
            for (uint c = 0; c < lineSphereCount; c++) {                              // ParticleLineSphereCollision
                LineSphereD ls = lineSpheres[c];
                float3 ab = ls.PositionA - ls.PositionB;
                float t = saturate(dot(ab, ls.PositionA - q.Position) / dot(ab, ab));
                float3 cp = t * (ls.PositionB - ls.PositionA) + ls.PositionA;
                float R = q.Radius + (t * (ls.RadiusB - ls.RadiusA) + ls.RadiusA);
                float3 d = q.Position - cp;
                float d2 = dot(d, d);
                if (R * R < d2) continue;
                float pen = R - sqrt(d2);
                q.Position = ((rsqrt(d2) * d) * pen) * collisionPower + q.Position;
                float h = min(max(pen * 50.0, 0.0), 0.5);
                q.CollisionPower = staticFriction * ls.Friction + h * 50.0;
                q.CollisionHold = saturate(h + friction + 0.02);
                q.ColliderDelta = ls.Delta;
                q.CollisionDrag = friction;
                q.SphereCollisionID = -1;
                q.LineSphereCollisionID = (int)c;
            }
        }
        if (grabCount != 0) {                                                         // GrabJoints
            for (uint g = 0; g < grabCount; g++) {
                GrabSphere gs = grabSpheres[g];
                if (gs.Grab == 0) continue;
                float3 d = q.Position - gs.Position;
                float R = q.Radius + gs.Radius;
                float d2 = dot(d, d);
                if (R * R < d2) continue;
                q.GrabDistance = max(sqrt(d2), 0.000001);
                q.GrabID = gs.ID;
            }
            if (q.GrabID != -1) {
                bool held = false;
                for (uint h2 = 0; h2 < grabCount; h2++) {
                    GrabSphere gs = grabSpheres[h2];
                    if (gs.ID != q.GrabID) continue;
                    float3 d = q.Position - gs.Position;
                    float len = max(sqrt(dot(d, d)), 0.000001);
                    q.Position = d * ((q.GrabDistance - len) / len) + q.Position;
                    q.CollisionPower = 100.0;
                    held = true;
                    break;
                }
                if (!held) q.GrabID = -1;
            }
        }
    }
    float3 r1 = q.Position - q.LastPosition;                                          // IntegrateVelocity
    float3 r2 = q.ColliderDelta * stepOuter - r1;
    r1 = q.CollisionHold * r2 + r1;
    q.Velocity = r1 * dtrecipOuter;
    q.LastPosition = q.Position;
    q.LastPositionInner = q.Position;
    q.DrawPosition = q.Position;
    particles[p] = q;
    StoreX(p, float4(q.Position, q.CollisionPower * q.Strength + q.Strength));
    StoreS(p, 0, float4(q.Position, q.CollisionDrag));
    StoreS(p, 1, float4(q.Velocity, q.CollisionHold));
    StoreS(p, 2, float4(q.ColliderDelta, q.CollisionPower));
}

