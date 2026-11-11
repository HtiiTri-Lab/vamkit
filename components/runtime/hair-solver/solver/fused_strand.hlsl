// Fused strand solver (P2).
//
// Everything in GPUTools' HairPhysicsWorld.DispatchPhysicsImpl that touches only one strand -- integration,
// point joints, distance joints (2 colours), compression joints (2 colours), spline joints, velocity, sphere /
// capsule collision -- runs with the strand's positions in groupshared memory, in the original order and with
// the original arithmetic (data\compute\*.asm). Per substep the original order is
//   A = integrate, point joints, distance x2, compression x2 | cling | B = distance x2, spline, distance x2, velocity
// and per outer pass End = collision reset, plane + sphere + capsule collision, point joints, velocity.
// Cling (nearby distance joints) is the only cross-strand step; three ways to do it:
//   clingMode 0  external: CSStrand dispatches B, End, A (each behind a flag) and the host runs GPUTools' own
//                NearbyDistanceJoints kernel colour by colour in between ("faithful", same result as GPUTools)
//   clingMode 1  Jacobi, synchronous: B starts by gathering every cling joint of its particle from the
//                positions all strands had after A (compact buffer written by A, ping-pong), still one
//                dispatch per substep but no separate cling dispatches
//   clingMode 2  one dispatch per physics step (CSStep): all substeps and outer passes in groupshared. Cling
//                is evaluated once, at the start of the step, on a snapshot of all positions (written at the end
//                of the previous step, ping-pong), so both ends of a joint see the same geometry and get equal
//                and opposite corrections. Per joint it removes the share of the error that m synchronous
//                substeps would have removed, 1/2 (1 - (1 - 2w)^m) per end, capped per particle at clingCap
//                (sum of those shares), and is applied in m equal parts where GPUTools runs cling (after A).
//                Neighbours are not extrapolated with their velocity: that feedback made the coupling unstable.
//                Without cling (no joints, or cling 0) strands do not interact and CSStep is exact: the default there.
// distanceRepeat / compressionRepeat / clingRepeat: how many times every distance / compression / cling block sweeps its
// colours. In the game it is 2 for all three: the joint builders read GroupedData.Data twice in OnBuild (length check,
// then the buffer), every read appends the colour groups to GroupsData again, and the kernels dispatch every entry of
// GroupsData (docs\P0-原求解器与新方案.md section 3b). The plugin reads the counts from the live kernels.
// State between dispatches of one step lives in `hot` (5 float4 planes per particle, coalesced); the GPUTools
// particle struct is read only when importStruct is set (first dispatch of a step) and written only when
// exportStruct is set (last dispatch), so the renderer sees exactly the fields GPUTools would have written.
//
// Layout: one thread per particle, strandsPerGroup whole strands per 256-thread group. The particle buffer is
// GPUTools' own (GPParticle, 116 bytes), so the result stays drop-in for its renderer.
//
// The rules are GPUTools' and they depend on the point spacing (stretch, bounce and damping all change with it), so
// render strands are simulated at their guides' spacing (docs\P0-原求解器与新方案.md section 4f). Tried and removed
// (2026-10-15): a direct per-strand length solve, long range attachments, DFTL, curl-spring compliance.

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
struct Mat { float4 c0, c1, c2, c3; };                    // Unity Matrix4x4, column-major
struct SphereD { float3 Position; float Radius; float Friction; float3 Delta; };
struct LineSphereD { float3 PositionA; float3 PositionB; float RadiusA; float RadiusB; float Friction; float3 DeltaA; float3 DeltaB; float3 Delta; };
struct ClingEntry { int Other; float Rest; float Elasticity; };

RWStructuredBuffer<Particle> particles : register(u0);
RWStructuredBuffer<float4> compact : register(u1);        // clingMode 1: positions after A, 2 x particleCount (ping-pong)
RWStructuredBuffer<float4> snapPos : register(u2);        // clingMode 2: positions at the end of a step, 2 x particleCount
RWStructuredBuffer<float4> hot : register(u3);            // 5 x particleCount: pos|drag, lastInner|hold, velocity|power,
                                                          // colliderDelta|sphere id, lastPosition|line-sphere id
StructuredBuffer<PointJoint> pointJoints : register(t0);
StructuredBuffer<Mat> transforms : register(t1);
StructuredBuffer<float2> restLengths : register(t2);      // x: |p(i-1) - p(i)| (distance + spline), y: compression rest of (i-2, i)
StructuredBuffer<SphereD> spheres : register(t3);
StructuredBuffer<LineSphereD> lineSpheres : register(t4);
StructuredBuffer<uint> clingOffsets : register(t5);       // particleCount + 1
StructuredBuffer<ClingEntry> clingEntries : register(t6);
StructuredBuffer<float4> planes : register(t7);           // GPUCollidersManager's plane colliders: normal, offset
StructuredBuffer<uint> positionOnly : register(t8);       // breast colliders: spheres, then capsules; 1 position only,
                                                          // 2 replaced by bodySpheres (the breast SoftBody draws)
StructuredBuffer<float4> bodySpheres : register(t9);      // per breast: bound (centre, radius), count (a float value) in
                                                          // x, then its surface spheres (centre, radius), position only

cbuffer Params : register(b0) {
    uint particleCount, segments, strandCount, strandsPerGroup;
    float DT; float3 accelDT2;
    float invDrag, weight, distanceScale, distancePower;
    float compressionScale, compressionPower, splinePower, dtrecipInner;
    float stepInner, dtrecipOuter, stepOuter, friction;
    float staticFriction, collisionPower; uint sphereCount, lineSphereCount;
    uint doB, doEnd, doA, doIntegrate;
    uint doCompression, doCollision, doReset, isFixed;
    float nearbyScale, nearbyPower, nearbyRolloff; uint clingMode;
    uint readParity, outer, iterations, distanceRepeat;
    float clingCap; uint importStruct, exportStruct, compressionRepeat;
    uint clingRepeat, planeCount, positionOnlyCount, positionOnlySpheres;
    uint bodyCount, bodyPad0, bodyPad1, bodyPad2;
};

#define GROUP 256
groupshared float3 P[GROUP];

// per-thread context
static uint lane, k, i;
static bool active;
static Particle p;
static PointJoint pj;
static float2 rest;

float3 PointTarget() {
    Mat m = transforms[pj.MatrixId];
    return float3(dot(float3(m.c0.x, m.c1.x, m.c2.x), pj.Point) + m.c3.x,
                  dot(float3(m.c0.y, m.c1.y, m.c2.y), pj.Point) + m.c3.y,
                  dot(float3(m.c0.z, m.c1.z, m.c2.z), pj.Point) + m.c3.z);
}

float3 ApplyPointJoint(float3 x) {                       // PointJoints
    float3 t = PointTarget();
    return (pj.Rigidity >= 1.0 || isFixed == 1) ? t : x - (x - t) * saturate(pj.Rigidity * weight);
}

void DistancePasses() {                                  // DistanceJoints, colour order: odd particle index, then even
    for (uint r = 0; r < distanceRepeat; r++) {
        [unroll] for (int parity = 1; parity >= 0; parity--) {
            if (active && k != 0 && (i & 1) == (uint)parity) {
                float3 p1 = P[lane - 1], p2 = P[lane];
                float3 d = p1 - p2;
                float len = sqrt(dot(d, d));
                float c = (rest.x * distanceScale - len) / len;
                d *= c;
                P[lane - 1] = d * distancePower + p1;
                P[lane] = -d * distancePower + p2;
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }
}

void PhaseA() {                                          // IntegrateIter, PointJoints, distance x2, compression x2
    if (active) {
        float3 x = P[lane];
        if (doIntegrate != 0)
            x = x + ((p.Velocity * invDrag) * DT + (1.0 - p.CollisionDrag) * accelDT2);
        P[lane] = ApplyPointJoint(x);
    }
    GroupMemoryBarrierWithGroupSync();
    DistancePasses();
    if (doCompression != 0) {                            // CompressionJoints: i%4 in {2,3} first, then the rest
        for (uint r = 0; r < compressionRepeat; r++) {
            [unroll] for (uint grp = 0; grp < 2; grp++) {
                bool first = (i & 3) >= 2;
                if (active && k >= 2 && first == (grp == 0)) {
                    float3 p1 = P[lane - 2], p2 = P[lane];
                    float3 d = p1 - p2;
                    float len = sqrt(dot(d, d));
                    float c = max(rest.y * compressionScale - len, 0.0) / len;
                    d *= c;
                    P[lane - 2] = d * compressionPower + p1;
                    P[lane] = -d * compressionPower + p2;
                }
                GroupMemoryBarrierWithGroupSync();
            }
        }
    }
}

// Cling as a Jacobi gather: every joint of this particle against the other end's position `other(j)`.
// Same per-joint correction as NearbyDistanceJoints (each end moves by d * c * power').
#define CLING_GATHER(OTHER)                                                             \
    if (active) {                                                                       \
        float3 x = P[lane], delta = 0;                                                  \
        uint e1 = clingOffsets[i + 1];                                                  \
        for (uint e = clingOffsets[i]; e < e1; e++) {                                   \
            ClingEntry en = clingEntries[e];                                            \
            float3 y = OTHER;                                                           \
            float3 d = x - y;                                                           \
            float len = sqrt(dot(d, d));                                                \
            float c = (en.Rest * nearbyScale - len) / len;                              \
            float w = (nearbyRolloff * (en.Elasticity - 1.0) + 1.0) * nearbyPower;      \
            delta += d * c * w;                                                         \
        }                                                                               \
        P[lane] = x + delta;                                                            \
    }                                                                                   \
    GroupMemoryBarrierWithGroupSync();

void PhaseB() {                                          // distance x2, SplineJoints, distance x2, IntegrateVelocityInner
    DistancePasses();
    if (active && k == 0) {                              // one thread walks its strand root -> tip
        for (uint s = 1; s < segments; s++) {
            float sr = restLengths[i + s].x;
            float3 a = P[lane + s - 1], b = P[lane + s];
            float3 d = a - b;
            float len = sqrt(dot(d, d));
            float c = (sr - len) / len;
            P[lane + s] = -(c * d) * splinePower + b;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    DistancePasses();
    if (active) {
        float3 x = P[lane];
        float3 r1 = x - p.LastPositionInner;
        float3 r2 = p.ColliderDelta * stepInner - r1;
        p.Velocity = (p.CollisionHold * r2 + r1) * dtrecipInner;
        p.LastPositionInner = x;
    }
}

bool MaskValid() { return positionOnlyCount == sphereCount + lineSphereCount && positionOnlySpheres == sphereCount; }

uint Contact(uint index) {                               // 0 original response, 1 position only, 2 not for the hair
    return MaskValid() && index < positionOnlyCount ? positionOnly[index] : 0;
}

void ProjectContact(inout float3 x, float3 d, float d2, float pen) {
    float3 n = d2 > 1e-12 ? d * rsqrt(d2) : float3(0, 1, 0);
    float3 correction = n * pen;
    x += correction;
    // Geometric separation must not become next step's launch velocity.
    p.LastPosition += correction;
    p.LastPositionInner += correction;
    // Zero restitution: remove only inward motion, retain tangential sliding.
    p.LastPosition += n * min(dot(x - p.LastPosition, n), 0.0);
    p.LastPositionInner += n * min(dot(x - p.LastPositionInner, n), 0.0);
}

void PhaseEnd() {                                        // collision reset, plane + sphere + capsule collision, point joints, velocity
    if (active) {
        float3 x = P[lane];
        p.CollisionDrag = 0; p.CollisionHold = 0; p.CollisionPower = 0;
        p.SphereCollisionID = -1; p.LineSphereCollisionID = -1;
        if (doCollision != 0 && p.CollisionEnabled != 0) {
            const float3 x0 = x;                                             // ParticlePlaneCollision: distance to every
            for (uint q = 0; q < planeCount; q++) {                          // plane from where the particle started,
                float4 pl = planes[q];                                       // pushed along the normal, velocity cut
                float dist = abs(dot(x0, pl.xyz) + pl.w);
                if (!(p.Radius >= dist)) continue;
                x = pl.xyz * (p.Radius - dist) + x;
                p.LastPosition = x;
            }
            for (uint s = 0; s < sphereCount; s++) {                         // ParticleSphereCollision
                SphereD sp = spheres[s];
                float3 d = x - sp.Position;
                float R = p.Radius + sp.Radius;
                float d2 = dot(d, d);
                if (R * R < d2) continue;
                float pen = R - sqrt(d2);
                uint kind = Contact(s);
                if (kind != 0) {
                    if (kind == 1) ProjectContact(x, d, d2, pen);
                    continue;
                }
                x = (d * rsqrt(d2) * pen) * collisionPower + x;
                float h = min(max(pen * 50.0, 0.0), 0.5);
                p.CollisionPower = staticFriction * sp.Friction + h * 50.0;
                p.CollisionHold = saturate(h + friction + 0.02);
                p.ColliderDelta = sp.Delta;
                p.CollisionDrag = friction;
                p.SphereCollisionID = (int)s;
            }
            for (uint s2 = 0; s2 < lineSphereCount; s2++) {                  // ParticleLineSphereCollision
                LineSphereD ls = lineSpheres[s2];
                float3 ab = ls.PositionA - ls.PositionB;
                float ab2 = dot(ab, ab);
                // Most particles miss most capsules. Read the classification only
                // after overlap, except for a degenerate capsule's guarded axis.
                float t = ab2 < 1e-12 && Contact(sphereCount + s2) != 0 ? 0.0 : saturate(dot(ab, ls.PositionA - x) / ab2);
                float3 c = t * (ls.PositionB - ls.PositionA) + ls.PositionA;
                float R = p.Radius + (t * (ls.RadiusB - ls.RadiusA) + ls.RadiusA);
                float3 d = x - c;
                float d2 = dot(d, d);
                if (R * R < d2) continue;
                float pen = R - sqrt(d2);
                uint kind = Contact(sphereCount + s2);
                if (kind != 0) {
                    if (kind == 1) ProjectContact(x, d, d2, pen);
                    continue;
                }
                x = (d * rsqrt(d2) * pen) * collisionPower + x;
                float h = min(max(pen * 50.0, 0.0), 0.5);
                p.CollisionPower = staticFriction * ls.Friction + h * 50.0;
                p.CollisionHold = saturate(h + friction + 0.02);
                p.ColliderDelta = ls.Delta;
                p.CollisionDrag = friction;
                p.SphereCollisionID = -1;
                p.LineSphereCollisionID = (int)s2;
            }
            // The breasts as drawn, in place of the colliders marked 2: inside a breast's bound, every surface sphere.
            for (uint g = 0; MaskValid() && g + 1 < bodyCount; ) {
                float4 b = bodySpheres[g];
                uint first = g + 2, end = min(first + (uint)bodySpheres[g + 1].x, bodyCount);
                g = end;
                float3 db = x - b.xyz;
                float Rb = p.Radius + b.w;
                if (dot(db, db) > Rb * Rb) continue;
                for (uint bs = first; bs < end; bs++) {
                    float4 sp = bodySpheres[bs];
                    float3 d = x - sp.xyz;
                    float R = p.Radius + sp.w;
                    float d2 = dot(d, d);
                    if (R * R < d2) continue;
                    ProjectContact(x, d, d2, R - sqrt(d2));
                }
            }
        }
        x = ApplyPointJoint(x);
        float3 r1 = x - p.LastPosition;                                      // IntegrateVelocity
        float3 r2 = p.ColliderDelta * stepOuter - r1;
        p.Velocity = (p.CollisionHold * r2 + r1) * dtrecipOuter;
        p.LastPosition = x; p.LastPositionInner = x; p.DrawPosition = x;
        P[lane] = x;
    }
    GroupMemoryBarrierWithGroupSync();
}

void Setup(uint3 gid, uint3 gtid) {
    lane = gtid.x;
    const uint local = lane / segments;
    k = lane - local * segments;
    const uint strand = gid.x * strandsPerGroup + local;
    active = local < strandsPerGroup && strand < strandCount;
    i = strand * segments + k;
    p = (Particle)0;
    pj = (PointJoint)0;
    rest = 0;
    if (active) {
        if (importStruct != 0) {
            p = particles[i];
        } else {
            const uint n = particleCount;
            float4 h0 = hot[i], h1 = hot[i + n], h2 = hot[i + 2 * n], h3 = hot[i + 3 * n];
            p.Position = h0.xyz; p.CollisionDrag = h0.w;
            p.LastPositionInner = h1.xyz; p.CollisionHold = h1.w;
            p.Velocity = h2.xyz; p.CollisionPower = h2.w;
            p.ColliderDelta = h3.xyz; p.SphereCollisionID = asint(h3.w);
            if (doEnd != 0) {
                float4 h4 = hot[i + 4 * n];
                p.LastPosition = h4.xyz; p.LineSphereCollisionID = asint(h4.w);
                p.Radius = particles[i].Radius;
                p.CollisionEnabled = particles[i].CollisionEnabled;
            }
        }
        pj = pointJoints[i];
        rest = restLengths[i];
    }
    P[lane] = p.Position;
}

void Store() {
    if (!active) return;
    p.Position = P[lane];
    if (exportStruct != 0) {
        particles[i].Position = p.Position;
        particles[i].LastPosition = p.LastPosition;
        particles[i].LastPositionInner = p.LastPositionInner;
        particles[i].DrawPosition = p.DrawPosition;
        particles[i].Velocity = p.Velocity;
        particles[i].ColliderDelta = p.ColliderDelta;
        particles[i].CollisionDrag = p.CollisionDrag;
        particles[i].CollisionHold = p.CollisionHold;
        particles[i].CollisionPower = p.CollisionPower;
        particles[i].SphereCollisionID = p.SphereCollisionID;
        particles[i].LineSphereCollisionID = p.LineSphereCollisionID;
    } else {
        const uint n = particleCount;
        hot[i] = float4(p.Position, p.CollisionDrag);
        hot[i + n] = float4(p.LastPositionInner, p.CollisionHold);
        hot[i + 2 * n] = float4(p.Velocity, p.CollisionPower);
        hot[i + 3 * n] = float4(p.ColliderDelta, asfloat(p.SphereCollisionID));
        if (importStruct != 0 || doEnd != 0)            // plane 4 is loaded / changed only with End
            hot[i + 4 * n] = float4(p.LastPosition, asfloat(p.LineSphereCollisionID));
    }
}

bool Reset() {                                           // frame < 10: ResetToPointJoints + ParticleCollisionReset
    if (doReset == 0) return false;
    if (active) {
        float3 t = PointTarget();
        p.Position = t; p.LastPosition = t; p.LastPositionInner = t; p.DrawPosition = t; p.Velocity = 0;
        p.CollisionDrag = 0; p.CollisionHold = 0; p.CollisionPower = 0;
        p.SphereCollisionID = -1; p.LineSphereCollisionID = -1;
        particles[i] = p;                                // reset dispatches always import the struct
        compact[i] = compact[i + particleCount] = float4(t, 0);
        snapPos[i] = snapPos[i + particleCount] = float4(t, 0);
    }
    return true;
}

// clingMode 0 / 1: one dispatch = [B] [End] [A], each behind its flag.
[numthreads(GROUP, 1, 1)]
void CSStrand(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    Setup(gid, gtid);
    if (Reset()) return;
    GroupMemoryBarrierWithGroupSync();
    if (doB != 0) {
        if (clingMode == 1) {                    // GPUTools sweeps the colours clingRepeat times; the other ends stay
            for (uint cr = 0; cr < clingRepeat; cr++) { CLING_GATHER(compact[en.Other + readParity * particleCount].xyz) }
        }
        PhaseB();
    }
    if (doEnd != 0) PhaseEnd();
    if (doA != 0) {
        PhaseA();
        if (clingMode == 1 && active) compact[i + (readParity ^ 1) * particleCount] = float4(P[lane], 0);
    }
    Store();
}

// clingMode 2 (or no cling): the whole physics step in one dispatch.
[numthreads(GROUP, 1, 1)]
void CSStep(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    Setup(gid, gtid);
    if (Reset()) return;
    const uint snapIn = readParity * particleCount, snapOut = (readParity ^ 1) * particleCount;
    const uint m = outer * iterations;
    float3 clingDelta = 0;
    if (nearbyPower != 0 && active) {
        float3 x0 = snapPos[i + snapIn].xyz;
        float shares = 0;
        uint e1 = clingOffsets[i + 1];
        for (uint e = clingOffsets[i]; e < e1; e++) {
            ClingEntry en = clingEntries[e];
            float3 d = x0 - snapPos[en.Other + snapIn].xyz;
            float len = sqrt(dot(d, d));
            float c = (en.Rest * nearbyScale - len) / len;
            float w = (nearbyRolloff * (en.Elasticity - 1.0) + 1.0) * nearbyPower;
            float left = 1.0;
            for (uint q = 0; q < m * clingRepeat; q++) left *= 1.0 - 2.0 * w;
            float share = 0.5 * (1.0 - left);
            clingDelta += d * c * share;
            shares += share;
        }
        if (shares > clingCap) clingDelta *= clingCap / shares;
        clingDelta /= m;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint o = 0; o < outer; o++) {
        for (uint it = 0; it < iterations; it++) {
            PhaseA();
            if (nearbyPower != 0) {                  // position only: shift the velocity references too
                if (active) {
                    P[lane] += clingDelta;
                    p.LastPositionInner += clingDelta;
                    p.LastPosition += clingDelta;
                }
                GroupMemoryBarrierWithGroupSync();
            }
            PhaseB();
        }
        PhaseEnd();
    }
    Store();
    if (active) snapPos[i + snapOut] = float4(P[lane], 0);
}
