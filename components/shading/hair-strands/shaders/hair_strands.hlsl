// VaM hair strands (hair-strands component): GPUTools/MeshedVR/HairOpt hair drawn through a visibility buffer instead of
// Unity's tessellation passes.
//
// The original draws every hair item once per pass, light and eye: VS -> HS isolines (hairMultiplier strands per scalp
// triangle, curveDensity segments per strand) -> DS (strand point, lighting per point) -> GS (a camera-facing quad per
// segment) -> PS. At VR resolutions nearly every quad is thinner than a pixel, so the GPU spends the pass setting up
// triangles that cover no pixel centre (a measured case: 23 M triangles in the eye camera for 35 k hair pixels).
//
// Here:
//   HsBuild   (compute, once per item and frame): every strand point as the original DS computes it, and what its
//             lighting needs (tangent, normal, colour, specular shift).
//   HsRaster  (compute, per item and eye): the GS's quads rasterised in software with D3D11's rules (16.8 fixed point,
//             top-left fill rule, cull mode, depth clip) into a visibility buffer: HS_PASS_DEPTH keeps the nearest
//             depth of every pixel (atomic), HS_PASS_ID the item and segment that has it.
//   HsResolve (pixel shader over the eye target, once per pass): for the pixel's segment, the original's per-point
//             lighting at both ends, interpolated perspective-correct as the rasteriser would, then (ForwardAdd) each
//             light's per-pixel attenuation and shadow; writes the colour and the depth.
// The lighting arithmetic is HairRender\onepass\shaders\hair_onepass.hlsl's (checked instruction by instruction against
// the game's DXBC).

#define HS_MAX_LIGHTS 16    // lights in one compatible resolve batch
#define HS_ITEM_SHIFT 26    // visibility id = item << 26 | (segment + 1)
#define HS_SEG_MASK ((1u << HS_ITEM_SHIFT) - 1u)
#include "../shaders/hs_dispatch.hlsl"
#ifndef HS_CLIPPED
#define HS_CLIPPED 0
#endif

#define HS_LIGHT_DIRECTIONAL 0
#define HS_LIGHT_POINT 1
#define HS_LIGHT_SPOT 2
#define HS_LIGHT_RADIUS 0.25  // m: the lights' size for the transmission lobe (the skin's light disc)
#define HS_SHADOW_NONE 0
#define HS_SHADOW_DEPTH 1
#define HS_SHADOW_DEPTH_SOFT 2
#define HS_SHADOW_CUBE 3
#define HS_SHADOW_CUBE_SOFT 4
#define HS_SHADOW_SCREEN 5

// One ForwardAdd light as its pass had it ($Globals / UnityLighting / UnityShadows; matrices one register per column).
struct HsLight {
    float4 pos;               // _WorldSpaceLightPos0
    float4 color;             // _LightColor0
    float4 worldToLight[4];   // unity_WorldToLight
    float4 worldToShadow[4];  // unity_WorldToShadow[0]
    float4 shadowData;        // _LightShadowData
    float4 positionRange;     // _LightPositionRange
    float4 projectionParams;  // _LightProjectionParams
    uint4 kind;               // x HS_LIGHT_*, y HS_SHADOW_*
    uint4 scissor;            // ForwardAdd scissor: eye 0 x left | right << 16, y top | bottom << 16; eye 1 z, w
};

// One hair item (layout shared with native\hs_item.h).
struct HsItem {
    float4 tessFactor;        // _TessFactor
    float4 length;            // _Length
    float4 mat0;              // _MaxSpread _RandomBarycentric _StandWidth _Diffuse
    float4 mat1;              // _FresnelPower _FresnelAtten _SpecularShift _PrimarySpecular
    float4 mat2;              // _SecondarySpecular _RandomTexColorPower _RandomTexColorOffset _IBLFactor
    float4 specularColor;
    float4 exposureIBL;
    float4 exposureLM;
    float4 skyMatrix[4];
    float4 sh[9];
    float4 ambient;           // glstate_lightmodel_ambient
    float4 mainLightPos;      // ForwardBase _WorldSpaceLightPos0
    float4 mainLightColor;
    float4 objectToWorld[4];
    float4 worldToObject[4];
    float4 stereoMatrixVP[8];
    float4 stereoCameraPos[2];
    float4 stereoScaleOffset[2];
    float4 projectionParams;  // _ProjectionParams
    uint4 counts;             // x patches, y strands per patch, z segments per strand, w first point
    uint4 flags;              // x left-eye light mask; y bit 0 screen shadows, bits 8-9 eyes, bits 16-31 right-eye mask; z first index; w 16-bit indices
};

// One eye's raster state (the item's ForwardBase draw for that eye).
struct HsView {
    float4 viewport;          // left, top, width, height
    float4 depth;             // min depth, max depth
    int4 clip;                // pixels drawn: left, top, right, bottom (exclusive; viewport and scissor)
    uint4 mode;               // x D3D11_CULL_MODE, y front counter-clockwise, z D3D11_COMPARISON_FUNC (depth), w eye
};

cbuffer HsDispatch : register(b0) {
    uint4 gDispatch;          // x item, y view, z first thread, w threads
    uint4 gResolve;           // x eye 1 starts at this pixel column, y depth func of the resolve, z 1 = transmission (2.1.0)
};

// The frame's segments numbered across items (raster: one dispatch for every item and both eyes).
cbuffer HsFrame : register(b1) {
    uint4 gFrame;             // x items, y segments
    uint4 gSegStart[16];      // item i's first segment (i = 0..items; entry `items` = segments)
};
uint SegStart(uint i) { return gSegStart[i >> 2][i & 3]; }
uint FindItem(uint g) {
    uint lo = 0, hi = gFrame.x;  // SegStart(lo) <= g < SegStart(hi)
    [loop] while (hi - lo > 1) {
        uint mid = (lo + hi) >> 1;
        if (SegStart(mid) <= g) lo = mid;
        else hi = mid;
    }
    return lo;
}

StructuredBuffer<float> gDomain : register(t5);       // isoline domain: u of detail S (k = 0..S), then v of density L
StructuredBuffer<HsItem> gItems : register(t6);
ByteAddressBuffer gPoints : register(t7);
StructuredBuffer<HsView> gViews : register(t8);

uint DomainU(uint S, uint k) { return (S - 1) * (S + 2) / 2 + k; }  // sum of (s + 1) for s < S
uint DomainV(uint L, uint j) { return 2144 + L * (L - 1) / 2 + j; }  // after the 64 u rows (2144 values)

// ---- strand point (as the original DS) ------------------------------------------------------------------------------

struct TessRenderParticle {
    float3 Position;
    float3 Velocity;
    float3 LightCenter;
    float3 Color;
    float Interpolation;
    int RootIndex;
};

struct HairStrandPoint {
    float3 pos;
    float3 tangent;
    float3 normal;  // from the strand's light centre
    float3 color;   // particle colour (build: before the noise gradient; after: with it)
    float shift;    // specular shift (the _ColorTex sample)
};

// 32 points per tile, with three contiguous 12-byte planes: position, tangent,
// appearance. All nine words retain their original precision and values. Consecutive
// threads now write/read adjacent words instead of scattering across 36-byte records.
uint3 PointAddresses(uint index) {
    uint base = (index >> 5) * (32u * 36u) + (index & 31u) * 12u;
    return base + uint3(0u, 32u * 12u, 64u * 12u);
}

#ifdef HS_BUILD
Texture2D<float4> _NoiseTex : register(t0);
Texture2D<float4> _ColorTex : register(t1);
StructuredBuffer<TessRenderParticle> _Particles : register(t2);
StructuredBuffer<float3> _Barycentrics : register(t3);
ByteAddressBuffer gIndices : register(t4);
SamplerState sampler_ColorTex : register(s0);
SamplerState sampler_NoiseTex : register(s1);
RWByteAddressBuffer gPointsOut : register(u0);

static float _MaxSpread, _RandomBarycentric;
static float4 _TessFactor, _Length;

float3 HairSpread(float3 root, float3 p) {
    float3 d = p - root;
    float len = sqrt(dot(d, d));
    return d * (min(len, _MaxSpread) / max(len, 0.0001)) + root;
}

float3 HairInterpolate(int i0, int i1, int i2, float3 b, float interpolation) {
    float3 p0 = _Particles[i0].Position;
    float3 q1 = HairSpread(p0, _Particles[i1].Position);
    float3 q2 = HairSpread(p0, _Particles[i2].Position);
    float3 bary = q1 * b.y;
    bary = p0 * b.x + bary;
    bary = q2 * b.z + bary;
    float3 sum = q1 + p0;
    sum = sum + q2;
    return interpolation * (sum * (1.0 / 3.0) - bary) + bary;
}

HairStrandPoint HairComputePoint(uint3 guides, float u, float v) {
    float segments = _TessFactor.y;  // particles per guide strand
    float t = (segments - 0.001) * u;
    float3 b0 = _Barycentrics[(uint)(v * 64.0)];
    t = t * dot(_Length.xyz, b0);
    float segment = trunc(t);
    int seg = (int)t;
    int i0 = (int)((float)guides.x * segments + segment);
    int i1 = (int)((float)guides.y * segments + segment);
    int i2 = (int)((float)guides.z * segments + segment);
    TessRenderParticle root = _Particles[i0];
    float3 b1 = _Barycentrics[(int)(((float)root.RootIndex + v) * 64.0)];
    float3 b = _RandomBarycentric * (b1 - b0) + b0;
    HairStrandPoint s;
    s.pos = HairInterpolate(i0, i1, i2, b, root.Interpolation);
    // Tangent toward the tip from the neighbouring particle (the next one at the root).
    bool first = seg == max(seg - 1, 0);
    int step = first ? 1 : -1;
    float3 n = HairInterpolate(i0 + step, i1 + step, i2 + step, b, _Particles[i0 + step].Interpolation);
    s.tangent = normalize(s.pos - n) * (first ? -1.0 : 1.0);
    float3 c = _Particles[i1].Color * b0.y;
    c = root.Color * b0.x + c;
    c = _Particles[i2].Color * b0.z + c;
    s.color = root.Interpolation * (root.Color - c) + c;
    s.normal = normalize(s.pos - root.LightCenter);
    s.shift = 0.0;
    return s;
}

uint Index(uint first, bool short16, uint i) {
    if (!short16) return gIndices.Load((first + i) * 4);
    uint w = gIndices.Load(((first + i) * 2) & ~3u);
    return ((first + i) & 1) ? w >> 16 : w & 0xffff;
}

uint OctEncode(float3 n) {
    float2 p = n.xy / (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0) p = (1.0 - abs(p.yx)) * (p.xy >= 0.0 ? 1.0 : -1.0);
    int2 q = (int2)round(clamp(p, -1.0, 1.0) * 32767.0);
    return (uint)(q.x & 0xffff) | ((uint)q.y << 16);
}

// One point per thread: thread i = (patch * strands + strand) * (segments + 1) + k.
[numthreads(64, 1, 1)]
void HsBuild(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x + gDispatch.z;
    if (tid.x >= gDispatch.w) return;
    HsItem it = gItems[gDispatch.x];
    _TessFactor = it.tessFactor;
    _Length = it.length;
    _MaxSpread = it.mat0.x;
    _RandomBarycentric = it.mat0.y;
    uint L = it.counts.y, S = it.counts.z;
    uint perPatch = L * (S + 1);
    uint patch = i / perPatch, r = i - patch * perPatch, j = r / (S + 1), k = r - j * (S + 1);
    uint first = it.flags.z + patch * 3;
    bool short16 = it.flags.w != 0;
    uint3 guides = uint3(Index(first, short16, 0), Index(first, short16, 1), Index(first, short16, 2));
    float u = gDomain[DomainU(S, k)], v = gDomain[DomainV(L, j)];
    HairStrandPoint s = HairComputePoint(guides, u, v);
    // HairNoiseColor and the specular shift, as the DS
    float3 g = _NoiseTex.SampleLevel(sampler_NoiseTex, float2(0.5, v), 0).xyz;
    g = g + it.mat2.z;
    g = g + -1.0;
    g = it.mat2.y * g + 1.0;
    float3 color = s.color * g;
    float shift = saturate(_ColorTex.SampleLevel(sampler_ColorTex, float2(v, u), 0).x - 0.5);
    uint3 a = PointAddresses(it.counts.w + i);
    gPointsOut.Store3(a.x, asuint(s.pos));
    gPointsOut.Store3(a.y, asuint(s.tangent));
    gPointsOut.Store3(a.z, uint3(OctEncode(s.normal), f32tof16(color.x) | (f32tof16(color.y) << 16),
                                f32tof16(color.z) | (f32tof16(shift) << 16)));
}
#endif  // HS_BUILD

// ---- points, quads, coverage (raster and resolve compute them the same way) -------------------------------------------

float3 OctDecode(uint e) {
    float2 p = float2((float)((int)(e << 16) >> 16), (float)((int)e >> 16)) / 32767.0;
    float3 n = float3(p, 1.0 - abs(p.x) - abs(p.y));
    float t = saturate(-n.z);
    n.xy += n.xy >= 0.0 ? -t : t;
    return normalize(n);
}

HairStrandPoint LoadPoint(uint index) {
    uint3 a = PointAddresses(index);
    HairStrandPoint s;
    s.pos = asfloat(gPoints.Load3(a.x));
    s.tangent = asfloat(gPoints.Load3(a.y));
    uint3 appearance = gPoints.Load3(a.z);
    s.normal = OctDecode(appearance.x);
    uint2 c = appearance.yz;
    s.color = float3(f16tof32(c.x), f16tof32(c.x >> 16), f16tof32(c.y));
    s.shift = f16tof32(c.y >> 16);
    return s;
}

// The DS's side vector (half the quad's width across the strand, facing the eye) at a point.
float3 Side(HsItem it, uint eye, HairStrandPoint s, float u) {
    float3 toCamera = it.stereoCameraPos[eye].xyz - s.pos;
    float invDistance = rsqrt(dot(toCamera, toCamera));
    float3 view = invDistance * toCamera;
    return normalize(cross(s.tangent, view)) * ((1.0 - saturate(u - 0.9) * 10.0) * it.mat0.z);
}

// UnityObjectToClipPos(mul(unity_WorldToObject, world)) for the eye, as the ForwardBase GS.
float4 Clip(HsItem it, uint eye, float3 wp) {
    float3 o = it.worldToObject[1].xyz * wp.y;
    o = it.worldToObject[0].xyz * wp.x + o;
    o = it.worldToObject[2].xyz * wp.z + o;
    o = o + it.worldToObject[3].xyz;
    float4 w = it.objectToWorld[1] * o.y;
    w = it.objectToWorld[0] * o.x + w;
    w = it.objectToWorld[2] * o.z + w;
    w = w + it.objectToWorld[3];
    float4 c = it.stereoMatrixVP[eye * 4 + 1] * w.y;
    c = it.stereoMatrixVP[eye * 4 + 0] * w.x + c;
    c = it.stereoMatrixVP[eye * 4 + 2] * w.z + c;
    return it.stereoMatrixVP[eye * 4 + 3] * w.w + c;
}

// A segment's quad as the GS emits it: corners (p0 + side0, p1 + side1, p0 - side0, p1 - side1); triangles (0 1 2) and
// (2 1 3). World positions too (the resolve interpolates them).
struct Quad {
    float4 clip[4];
    float3 world[4];
};

Quad MakeQuad(HsItem it, uint eye, HairStrandPoint a, HairStrandPoint b, float ua, float ub) {
    float3 sa = Side(it, eye, a, ua), sb = Side(it, eye, b, ub);
    Quad q;
    q.world[0] = a.pos + sa;
    q.world[1] = b.pos + sb;
    q.world[2] = a.pos - sa;
    q.world[3] = b.pos - sb;
    [unroll] for (int i = 0; i < 4; i++) q.clip[i] = Clip(it, eye, q.world[i]);
    return q;
}

// A triangle set up for coverage: vertices in 16.8 fixed point relative to v0, oriented clockwise on the target
// (y down), edge bias for the top-left rule. ok = false: culled, degenerate or not drawable here.
struct Tri {
    bool ok;
    bool flipped;    // vertices 1 and 2 swapped for the orientation
    bool exact;      // integer edge functions (bounding box under 100 pixels: no overflow)
    int2 p0;         // v0 (absolute, 1/256 pixel)
    int2 d1, d2;     // v1 - v0, v2 - v0
    int area;        // twice the area, > 0
    int3 bias;       // 0 or 1 per edge (01 12 20): 1 = pixel centres on it are outside
    float2 f0, f1, f2;  // the same in float (large triangles)
    float3 z;        // depth (z / w) at v0 v1 v2 (after the swap)
    float3 invW;
    int4 box;        // pixels: x0 y0 x1 y1 (inclusive)
};

bool TopLeft(int2 d) { return d.y < 0 || (d.y == 0 && d.x > 0); }

// A quad corner on the target. Unsafe projections use the homogeneous clipping path;
// they are not grounds for discarding the visible part of the triangle.
struct Corner {
    bool ok;
    float3 ndc;   // after the divide
    int2 p;       // 1/256 pixel
    float invW;
};

Corner Project(HsView v, float4 c) {
    Corner k = (Corner)0;
    k.ok = c.w > 1e-6;
    k.ndc = float3(c.x / c.w, c.y / c.w, c.z / c.w);
    float sx = (k.ndc.x * 0.5 + 0.5) * v.viewport.z + v.viewport.x;
    float sy = (0.5 - k.ndc.y * 0.5) * v.viewport.w + v.viewport.y;
    k.ok = k.ok && abs(sx) <= 32768.0 && abs(sy) <= 32768.0;
    k.p = (int2)round(float2(sx, sy) * 256.0);
    k.invW = 1.0 / c.w;
    return k;
}

// Pixels whose centres (x + 0.5) can be inside the box lo..hi (1/256 pixel), within the drawable rectangle.
int4 CentreBox(HsView v, int2 lo, int2 hi) {
    int4 box = int4((lo - 128 + 255) >> 8, (hi - 128) >> 8);
    box.xy = max(box.xy, v.clip.xy);
    box.zw = min(box.zw, v.clip.zw - 1);
    return box;
}

Tri SetupTriC(HsView v, Corner k0, Corner k1, Corner k2) {
    Tri t = (Tri)0;
    t.ok = false;
    if (!k0.ok || !k1.ok || !k2.ok) return t;
    float3 x = float3(k0.ndc.x, k1.ndc.x, k2.ndc.x), y = float3(k0.ndc.y, k1.ndc.y, k2.ndc.y), z = float3(k0.ndc.z, k1.ndc.z, k2.ndc.z);
    // all three on the outside of one frustum plane: nothing to draw
    if (all(x < -1.0) || all(x > 1.0) || all(y < -1.0) || all(y > 1.0) || all(z < 0.0) || all(z > 1.0)) return t;
    int2 p0 = k0.p, p1 = k1.p, p2 = k2.p;
    float3 invW = float3(k0.invW, k1.invW, k2.invW);
    int2 lo = min(min(p0, p1), p2), hi = max(max(p0, p1), p2);
    t.exact = all(hi - lo < 100 * 256);
    float area;
    if (t.exact) {
        int2 d1 = p1 - p0, d2 = p2 - p0;
        t.area = d1.x * d2.y - d1.y * d2.x;
        area = (float)t.area;
    } else {
        float2 e1 = float2(p1 - p0), e2 = float2(p2 - p0);
        area = e1.x * e2.y - e1.y * e2.x;
    }
    if (area == 0.0) return t;
    // area > 0: clockwise on the target (y down)
    bool front = (area < 0.0) == (v.mode.y != 0);
    if ((v.mode.x == 3 && !front) || (v.mode.x == 2 && front)) return t;
    t.flipped = area < 0.0;
    if (t.flipped) {
        int2 s = p1; p1 = p2; p2 = s;
        z = z.xzy;
        t.area = -t.area;
        area = -area;
        invW = invW.xzy;
    }
    t.p0 = p0;
    t.d1 = p1 - p0;
    t.d2 = p2 - p0;
    t.bias = int3(TopLeft(p1 - p0) ? 0 : 1, TopLeft(p2 - p1) ? 0 : 1, TopLeft(p0 - p2) ? 0 : 1);
    t.f0 = float2(0.0, 0.0);
    t.f1 = float2(t.d1);
    t.f2 = float2(t.d2);
    t.z = z;
    t.invW = invW;
    int4 box = CentreBox(v, lo, hi);
    if (any(box.zw < box.xy)) return t;
    t.box = box;
    t.ok = true;
    return t;
}

Tri SetupTri(HsView v, float4 c0, float4 c1, float4 c2) { return SetupTriC(v, Project(v, c0), Project(v, c1), Project(v, c2)); }

// Only exceptional triangles use this path. Preserve weights in the original triangle
// so resolve interpolates the same attributes after clipping (including texture gradients).
#define HS_CLIP_VERTICES 10
struct ClipVertex { float4 clip; float3 original; };

float ClipDistance(float4 c, uint plane) {
    if (plane == 0) return c.w - 1e-6;
    if (plane == 1) return c.z;
    if (plane == 2) return c.w - c.z;
    if (plane == 3) return c.w + c.x;
    if (plane == 4) return c.w - c.x;
    if (plane == 5) return c.w + c.y;
    return c.w - c.y;
}

uint ClipTriangle(float4 c0, float4 c1, float4 c2, out ClipVertex polygon[HS_CLIP_VERTICES]) {
    ClipVertex work[HS_CLIP_VERTICES];
    [unroll] for (uint zero = 0; zero < HS_CLIP_VERTICES; zero++) polygon[zero] = (ClipVertex)0;
    polygon[0].clip = c0; polygon[0].original = float3(1, 0, 0);
    polygon[1].clip = c1; polygon[1].original = float3(0, 1, 0);
    polygon[2].clip = c2; polygon[2].original = float3(0, 0, 1);
    uint count = 3;
    [loop] for (uint plane = 0; plane < 7; plane++) {
        if (count < 3) return 0;
        uint written = 0;
        ClipVertex a = polygon[count - 1];
        float da = ClipDistance(a.clip, plane);
        [loop] for (uint i = 0; i < count; i++) {
            ClipVertex b = polygon[i];
            float db = ClipDistance(b.clip, plane);
            bool ia = da >= 0.0, ib = db >= 0.0;
            if (ia != ib) {
                float t = da / (da - db);
                work[written].clip = a.clip + t * (b.clip - a.clip);
                work[written].original = a.original + t * (b.original - a.original);
                written++;
            }
            if (ib) work[written++] = b;
            a = b; da = db;
        }
        count = written;
        [loop] for (uint j = 0; j < count; j++) polygon[j] = work[j];
    }
    return count;
}

// Edge functions at a pixel centre: x = edge 1-2 (weight of v0), y = edge 2-0 (v1), z = edge 0-1 (v2); inside when all
// >= bias. Exact for small triangles.
bool Cover(Tri t, int2 px, out float3 bary) {
    int2 c = int2(px.x * 256 + 128, px.y * 256 + 128) - t.p0;
    bool inside;
    float3 e;
    if (t.exact) {
        int e01 = t.d1.x * c.y - t.d1.y * c.x;
        int2 q = c - t.d1, d12 = t.d2 - t.d1;
        int e12 = d12.x * q.y - d12.y * q.x;
        int e20 = -t.d2.x * (c.y - t.d2.y) + t.d2.y * (c.x - t.d2.x);
        inside = e12 >= t.bias.y && e20 >= t.bias.z && e01 >= t.bias.x;
        e = float3(e12, e20, e01);
    } else {
        float2 cf = float2(c);
        float e01 = t.f1.x * cf.y - t.f1.y * cf.x;
        float2 q = cf - t.f1, d12 = t.f2 - t.f1;
        float e12 = d12.x * q.y - d12.y * q.x;
        float e20 = -t.f2.x * (cf.y - t.f2.y) + t.f2.y * (cf.x - t.f2.x);
        inside = e12 >= (float)t.bias.y && e20 >= (float)t.bias.z && e01 >= (float)t.bias.x;
        e = float3(e12, e20, e01);
    }
    float area = t.exact ? (float)t.area : (t.f1.x * t.f2.y - t.f1.y * t.f2.x);
    bary = e / area;
    return inside;
}

// Depth at a pixel (screen-linear z, viewport range); false where depth clipping removes it.
bool Depth(HsView v, Tri t, float3 bary, out float depth) {
    float z = bary.x * t.z.x + bary.y * t.z.y + bary.z * t.z.z;
    depth = v.depth.x + z * (v.depth.y - v.depth.x);
    return z >= 0.0 && z <= 1.0;
}

// All four corners beyond one clip plane (behind the eye, say): ClipTriangle leaves nothing of either triangle.
bool Outside(Quad q) {
    [unroll] for (uint plane = 0; plane < 7; plane++)
        if (ClipDistance(q.clip[0], plane) < 0.0 && ClipDistance(q.clip[1], plane) < 0.0 && ClipDistance(q.clip[2], plane) < 0.0 &&
            ClipDistance(q.clip[3], plane) < 0.0)
            return true;
    return false;
}

// A quad too large for one thread (close to the eye): about this many pixel tests or more go to the group list, where a
// thread group shares its lines. Below it the group's 64 set-ups would cost more than they save.
#define HS_GROUP_PIXELS 1024.0
bool Large(Corner k0, Corner k1, Corner k2, Corner k3, int4 box) {
    float2 a = float2(k1.p - k0.p), b = float2(k2.p - k0.p), c = float2(k3.p - k2.p), d = float2(k1.p - k2.p);
    float area = (abs(a.x * b.y - a.y * b.x) + abs(d.x * c.y - d.y * c.x)) * (0.5 / 65536.0);
    float boxArea = (float)(box.z - box.x + 1) * (float)(box.w - box.y + 1);  // the part on the target
    return min(area, boxArea) + (float)min(box.z - box.x, box.w - box.y) >= HS_GROUP_PIXELS;
}

// ---- raster ---------------------------------------------------------------------------------------------------------

#ifdef HS_RASTER
// The depth pass lists the segments that were nearest at some pixel when they got there (x 4: segment << 2 | eyes);
// the id pass runs over that list alone. Eyes that are clipped at the camera or too large for one thread go to the
// group list instead (x 4: segment << 2 | those eyes), one thread group per entry (HS_CLIPPED); its depth pass leaves
// in each entry the eyes that were nearest somewhere. Args: {groups x,y,z,count} for the normal / group lists at
// 0 / 16; {vertexCount,instanceCount,startVertex,startInstance} for the exceptional resolve at 32.
RWTexture2D<uint> gVisDepth : register(u0);
#if HS_PASS_ID
RWTexture2D<uint> gVisId : register(u1);
ByteAddressBuffer gLive : register(t14);
ByteAddressBuffer gLiveArgs : register(t15);
#elif HS_CLIPPED
RWByteAddressBuffer gGroupList : register(u4);
ByteAddressBuffer gLiveArgs : register(t15);
#else
RWByteAddressBuffer gLiveOut : register(u2);
RWByteAddressBuffer gLiveArgsOut : register(u3);
RWByteAddressBuffer gGroupOut : register(u4);
#endif
#ifdef HS_STATS
// pixel tests, pixels covered, triangles set up, largest pixel tests of one thread, rows, triangles in the float path
RWByteAddressBuffer gStats : register(u5);
static uint gTests = 0;
#endif

// true: some pixel had no nearer hair yet (depth pass), always false in the id pass. Lines lane, lane + stride, ...
// (one thread: 0, 1).
bool RasterTri(HsView v, Tri t, uint id, uint lane, uint stride) {
    bool live = false;
    if (!t.ok) return live;
#ifdef HS_NO_RASTER
    return t.box.x == 12345;
#endif
#ifdef HS_STATS
    if (lane == 0) gStats.InterlockedAdd(8, 1);
    if (lane == 0 && !t.exact) gStats.InterlockedAdd(20, 1);
#endif
    bool greater = v.mode.z == 5 || v.mode.z == 7;
    // Lines of pixel centres along the box's longer side (rows when it is wider than tall, else columns: a hair's quad
    // is thin, so few lines), or across it when a thread group shares them (many short lines). In this frame .x runs
    // along a line, .y across; in each line the span between the edges at the line's centre, a fiftieth of a pixel
    // wider each side (Cover decides).
    bool cols = ((t.box.w - t.box.y) > (t.box.z - t.box.x)) == (stride == 1);
    float2 a1 = float2(t.d1), a2 = float2(t.d2), o = float2(t.p0);
    int4 box = t.box;
    if (cols) {
        a1 = a1.yx;
        a2 = a2.yx;
        o = o.yx;
        box = box.yxwz;
    }
    float3 ex = float3(0.0, a1.x, a2.x), ey = float3(0.0, a1.y, a2.y);  // edge e from vertex e to e + 1
    float3 fx = float3(a1.x, a2.x, 0.0), fy = float3(a1.y, a2.y, 0.0);
    float3 slope = (fx - ex) / (fy - ey);  // along per across (infinite on an edge along the lines: not used)
    for (int ln = box.y + (int)lane; ln <= box.w; ln += (int)stride) {
        float c = (float)(ln * 256 + 128) - o.y;
        float lo = 1e9, hi = -1e9;
        [unroll] for (int e = 0; e < 3; e++) {
            if ((ey[e] - c) * (fy[e] - c) <= 0.0) {
                bool flat = ey[e] == fy[e];
                float x = flat ? min(ex[e], fx[e]) : ex[e] + (c - ey[e]) * slope[e];
                float x2 = flat ? max(ex[e], fx[e]) : x;
                lo = min(lo, x);
                hi = max(hi, x2);
            }
        }
        if (lo > hi) continue;
        int s0 = max(box.x, (int)ceil((lo + o.x - 128.0) * (1.0 / 256.0) - 0.02));
        int s1 = min(box.z, (int)floor((hi + o.x - 128.0) * (1.0 / 256.0) + 0.02));
#ifdef HS_STATS
        gTests += (uint)max(s1 - s0 + 1, 0);
        gStats.InterlockedAdd(16, 1);
#endif
        for (int s = s0; s <= s1; s++) {
            int px = cols ? ln : s, py = cols ? s : ln;
            float3 bary;
            if (!Cover(t, int2(px, py), bary)) continue;
#ifdef HS_STATS
            gStats.InterlockedAdd(4, 1);
#endif
            float d;
            if (!Depth(v, t, bary, d)) continue;
            uint key = asuint(d);
#if HS_PASS_ID
            if (gVisDepth[uint2(px, py)] == key) InterlockedMax(gVisId[uint2(px, py)], id);
#else
            // a plain read first: behind hair already there (most of a deep hairstyle's layers), no atomic
            uint there = gVisDepth[uint2(px, py)];
            if (greater ? there > key : there < key) continue;
            live = true;
            if (greater) InterlockedMax(gVisDepth[uint2(px, py)], key);
            else InterlockedMin(gVisDepth[uint2(px, py)], key);
#endif
        }
    }
    return live;
}

bool RasterClipped(HsView v, float4 a, float4 b, float4 c, uint id, uint lane, uint stride) {
    ClipVertex polygon[HS_CLIP_VERTICES];
    uint count = ClipTriangle(a, b, c, polygon);
    bool live = false;
    [loop] for (uint i = 1; i + 1 < count; i++)
        live = RasterTri(v, SetupTri(v, polygon[0].clip, polygon[i].clip, polygon[i + 1].clip), id, lane, stride) || live;
    return live;
}

// Segment g: its item, end points and visibility id. Item-local segment: (patch * strands + strand) * segments + k.
struct Segment {
    HsItem it;
    HairStrandPoint a, b;
    float ua, ub;
    uint id;
};

Segment LoadSegment(uint g) {
    Segment s;
    uint item = FindItem(g);
    s.it = gItems[item];
    uint i = g - SegStart(item);
    uint S = s.it.counts.z;
    uint strand = i / S, k = i - strand * S;
    uint p = s.it.counts.w + strand * (S + 1) + k;
    s.a = LoadPoint(p);
    s.b = LoadPoint(p + 1);
    s.ua = gDomain[DomainU(S, k)];
    s.ub = gDomain[DomainU(S, k + 1)];
    s.id = (item << HS_ITEM_SHIFT) | (i + 1);
    return s;
}

#if !HS_CLIPPED
// One segment of the frame per thread (depth pass: g = thread, segments numbered across items; id pass: the list),
// both eyes.
[numthreads(64, 1, 1)]
void HsRaster(uint3 tid : SV_DispatchThreadID) {
#if HS_PASS_ID
    uint liveIndex = HsLiveIndex(tid);
    if (liveIndex >= gLiveArgs.Load(12)) return;
    uint entry = gLive.Load(liveIndex * 4);
    uint g = entry >> 2, eyes = entry & 3;
    Segment s = LoadSegment(g);
#else
    uint g = tid.x + gDispatch.z;
    if (g >= gFrame.y) return;
    Segment s = LoadSegment(g);
    uint eyes = (s.it.flags.y >> 8) & 3;
#endif
    uint live = 0;
    uint group = 0;  // eyes for the group list
    uint clipped = 0;
    [unroll] for (uint eye = 0; eye < 2; eye++) {
        [branch] if (eyes & (1u << eye)) {
            HsView v = gViews[eye];
            Quad q = MakeQuad(s.it, eye, s.a, s.b, s.ua, s.ub);
            Corner k0 = Project(v, q.clip[0]), k1 = Project(v, q.clip[1]), k2 = Project(v, q.clip[2]), k3 = Project(v, q.clip[3]);
            [branch] if (k0.ok && k1.ok && k2.ok && k3.ok) {
                // Most quads are thinner than a pixel and between pixel centres.
                int4 box = CentreBox(v, min(min(k0.p, k1.p), min(k2.p, k3.p)), max(max(k0.p, k1.p), max(k2.p, k3.p)));
                [branch] if (all(box.zw >= box.xy)) {
#if !HS_PASS_ID
                    [branch] if (Large(k0, k1, k2, k3, box)) {
                        group |= 1u << eye;
                        continue;
                    }
#endif
                    bool l0 = RasterTri(v, SetupTriC(v, k0, k1, k2), s.id, 0, 1);
                    bool l1 = RasterTri(v, SetupTriC(v, k2, k1, k3), s.id, 0, 1);
                    live |= (l0 || l1) ? 1u << eye : 0u;
                }
            }
#if !HS_PASS_ID
            // Clipped at the camera: the group kernels clip it (their scratch arrays kept out of this kernel). Hair behind
            // the eye goes nowhere.
            else if (!Outside(q)) {
                group |= 1u << eye;
                clipped = 1;
            }
#endif
        }
    }
#ifdef HS_STATS
    gStats.InterlockedAdd(0, gTests);
    gStats.InterlockedMax(12, gTests);
#endif
#if !HS_PASS_ID
    uint slot;
    [branch] if (live) {
        gLiveArgsOut.InterlockedAdd(12, 1, slot);
        if ((slot & 63) == 0) {
            uint2 groups = HsLiveGroups(slot);
            gLiveArgsOut.InterlockedMax(0, groups.x);
            gLiveArgsOut.InterlockedMax(4, groups.y);
        }
        gLiveOut.Store(slot * 4, (g << 2) | live);
    }
    [branch] if (group) {
        gLiveArgsOut.InterlockedAdd(28, 1, slot);
        uint2 groups = HsGroupGroups(slot);
        if (slot < HS_ID_GROUPS_PER_ROW) gLiveArgsOut.InterlockedMax(16, groups.x);
        if (slot % HS_ID_GROUPS_PER_ROW == 0) gLiveArgsOut.InterlockedMax(20, groups.y);
        gGroupOut.Store(slot * 4, (g << 2) | group);
        // Enable the exceptional resolve only when this frame has a clipped segment.
        if (clipped) gLiveArgsOut.InterlockedMax(36, 1u);
    }
#endif
}
#else
// One entry of the group list per thread group; its threads share each triangle's lines. Depth pass: the entry keeps
// the eyes that were nearest somewhere, for the id pass.
groupshared uint gsLive;

[numthreads(64, 1, 1)]
void HsRasterGroup(uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
    uint index = HsGroupIndex(group);
    bool valid = index < gLiveArgs.Load(28);  // the same for the whole group
#if HS_PASS_ID
    uint entry = valid ? gLive.Load(index * 4) : 0u;
#else
    uint entry = valid ? gGroupList.Load(index * 4) : 0u;
    if (lane == 0) gsLive = 0;
    GroupMemoryBarrierWithGroupSync();
#endif
    uint g = entry >> 2, eyes = entry & 3;
    uint live = 0;
    [branch] if (eyes) {
        Segment s = LoadSegment(g);
        [unroll] for (uint eye = 0; eye < 2; eye++) {
            [branch] if (eyes & (1u << eye)) {
                HsView v = gViews[eye];
                Quad q = MakeQuad(s.it, eye, s.a, s.b, s.ua, s.ub);
                Corner k0 = Project(v, q.clip[0]), k1 = Project(v, q.clip[1]), k2 = Project(v, q.clip[2]), k3 = Project(v, q.clip[3]);
                bool l0, l1;
                [branch] if (k0.ok && k1.ok && k2.ok && k3.ok) {
                    l0 = RasterTri(v, SetupTriC(v, k0, k1, k2), s.id, lane, 64);
                    l1 = RasterTri(v, SetupTriC(v, k2, k1, k3), s.id, lane, 64);
                } else {
                    l0 = RasterClipped(v, q.clip[0], q.clip[1], q.clip[2], s.id, lane, 64);
                    l1 = RasterClipped(v, q.clip[2], q.clip[1], q.clip[3], s.id, lane, 64);
                }
                live |= (l0 || l1) ? 1u << eye : 0u;
            }
        }
    }
#ifdef HS_STATS
    gStats.InterlockedAdd(0, gTests);
    gStats.InterlockedMax(12, gTests);
#endif
#if !HS_PASS_ID
    if (live) InterlockedOr(gsLive, live);
    GroupMemoryBarrierWithGroupSync();
    if (valid && lane == 0) gGroupList.Store(index * 4, (g << 2) | gsLive);
#endif
}
#endif  // HS_CLIPPED
#endif  // HS_RASTER

// ---- resolve --------------------------------------------------------------------------------------------------------

#ifdef HS_RESOLVE
Texture2D<uint> gVisDepth : register(t9);
Texture2D<uint> gVisId : register(t10);
Texture2D<float4> gRotation : register(t11);       // unity_RandomRotation16
Texture2D<float4> gScreenShadow : register(t12);   // the main light's screen-space shadows
#if HS_FORWARD_ADD
StructuredBuffer<HsLight> gLights : register(t13);
Texture2D<float4> gCookie[HS_MAX_LIGHTS] : register(t16);      // spot cookie (_LightTexture0)
Texture2D<float4> gAtten[HS_MAX_LIGHTS] : register(t32);       // point _LightTexture0, spot _LightTextureB0
Texture2D<float4> gShadow[HS_MAX_LIGHTS] : register(t48);      // spot shadow map / directional screen-space shadows
TextureCube<float4> gShadowCube[HS_MAX_LIGHTS] : register(t64);
#endif
SamplerState sRotation : register(s0);
SamplerState sScreenShadow : register(s1);
SamplerState sCookie : register(s2);
SamplerState sAtten : register(s3);
SamplerComparisonState sShadowCompare : register(s4);
SamplerState sShadowRaw : register(s5);
SamplerState sDirectionalScreen : register(s6);

static HsItem gIt;

// 2.1.0: light passing through the fibre (Marschner's TT lobe as in Karis 2016, "Physically Based Hair Shading in
// Unreal"), which the Kajiya-Kay lobes lack: hair lit from behind glows. Longitudinal width from the material's own
// primary sharpness n: Kajiya-Kay's cone (1 - d^2)^(n/2) is a Gaussian of sigma 1/sqrt(n) in theta_h, i.e. beta_R =
// 2/sqrt(n) on sin theta_i + sin theta_r, and beta_TT = beta_R / 2; widened by the light's size (s = sine of its angular
// radius, the skin's 0.25 m disc; 0 for directional lights, as the skin). pi * S: Unity's light units (Lambert = albedo
// * N.L). Root fade as the specular. gResolve.z = 0 leaves it out (2.0.1).
float HairFresnel(float c) {
    float x = 1.0 - c, x2 = x * x;
    return 0.0465 + 0.9535 * x2 * x2 * x;  // n 1.55
}
float3 HairTransmission(HairStrandPoint s, float3 view, float3 light, float sinAlpha, float u) {
    float3 t = normalize(s.tangent);
    float sl = clamp(dot(t, light), -1.0, 1.0), sv = clamp(dot(t, view), -1.0, 1.0);
    float3 lp = light - sl * t, vp = view - sv * t;
    float cosPhi = dot(lp, vp) * rsqrt(dot(lp, lp) * dot(vp, vp) + 1e-4);
    float cosHalfPhi = sqrt(saturate(0.5 + 0.5 * cosPhi));
    float cosThetaD = cos(0.5 * abs(asin(sv) - asin(sl)));
    float b0 = rsqrt(max(gIt.mat1.w, 1.0));
    float b2 = b0 * b0 + 0.25 * sinAlpha * sinAlpha;
    float x = sl + sv - 0.035;
    float mp = exp(-0.5 * x * x / b2) * rsqrt(6.2831853 * b2);
    float a = 1.0 / (1.19 / cosThetaD + 0.36 * cosThetaD);
    float h = cosHalfPhi * (1.0 + a * (0.6 - 0.8 * cosPhi));
    float f = HairFresnel(cosThetaD * sqrt(saturate(1.0 - h * h)));
    float3 tp = pow(max(s.color, 1e-4), 0.5 * sqrt(saturate(1.0 - h * h * a * a)) / cosThetaD);
    float np = exp(-3.65 * cosPhi - 3.98);
    return (3.1415927 * mp * np * (1.0 - f) * (1.0 - f) * saturate(u + u)) * tp;
}

// Direct light: two shifted Kajiya-Kay lobes, wrapped diffuse and fresnel (HairDirect of the one-pass shader), plus the
// transmission above.
float3 HairDirect(HairStrandPoint s, float3 toCamera, float invDistance, float3 view, float3 light, float3 lightColor, float u,
                  float sinAlpha) {
    float _Diffuse = gIt.mat0.w, _FresnelPower = gIt.mat1.x, _FresnelAtten = gIt.mat1.y, _SpecularShift = gIt.mat1.z;
    float _PrimarySpecular = gIt.mat1.w, _SecondarySpecular = gIt.mat2.x;
    float3 c = light.zxy * s.tangent - s.tangent.zxy * light;
    float3 ns = s.tangent.yzx * c.yzx - s.tangent.zxy * c;
    float3 t1 = (s.shift + _SpecularShift) * ns + s.tangent;
    float3 t2 = (s.shift - _SpecularShift) * ns + s.tangent;
    float nsl = saturate(dot(ns, light));
    t2 = normalize(t2);
    float3 h = normalize(toCamera * invDistance + light);
    float d2 = dot(t2, h);
    float specular2 = exp2(log2(sqrt(-d2 * d2 + 1.0)) * _PrimarySpecular);
    t1 = normalize(t1);
    float d1 = dot(t1, h);
    float specular1 = exp2(log2(sqrt(-d1 * d1 + 1.0)) * _SecondarySpecular);
    float3 specular = min(specular1 + specular2, 1.0) * gIt.specularColor.xyz;
    float nl = saturate(dot(s.normal, light));
    float nv = saturate(dot(s.normal, view));
    specular = specular * nl;
    specular = nv * specular;
    float fresnel = exp2(log2(1.0 - nv) * _FresnelPower);
    specular = specular * saturate(u + u);
    float wrap = 1.0 - _Diffuse;
    nsl = saturate(nsl * wrap + _Diffuse);
    float diffuse = saturate(nl * wrap + _Diffuse);
    diffuse = saturate(fresnel * _FresnelAtten + diffuse);
    float3 lit = diffuse * lightColor;
    float3 base = nsl * s.color + specular;
    float3 tt = gResolve.z != 0 ? HairTransmission(s, view, light, sinAlpha, u) : 0.0;
    return (lit * base + lightColor * tt) * gIt.exposureIBL.w;
}

// Ambient: order-2 SH in _SkyMatrix space plus the fixed-function ambient.
float3 HairAmbient(float3 normal, float3 color) {
    float3 sy = normal.y * float3(gIt.skyMatrix[0].y, gIt.skyMatrix[1].y, gIt.skyMatrix[2].y);
    float3 sz = normal.z * float3(gIt.skyMatrix[0].z, gIt.skyMatrix[1].z, gIt.skyMatrix[2].z);
    float3 sx = normal.x * float3(gIt.skyMatrix[0].x, gIt.skyMatrix[1].x, gIt.skyMatrix[2].x);
    float3 n = sy + sz;
    n = n + sx;
    float3 sh = gIt.sh[1].xyz * n.y + gIt.sh[0].xyz;
    sh = gIt.sh[2].xyz * n.z + sh;
    sh = gIt.sh[3].xyz * n.x + sh;
    float3 q = n.xzx * n.yyz;
    sh = gIt.sh[4].xyz * q.x + sh;
    sh = gIt.sh[5].xyz * q.y + sh;
    sh = gIt.sh[7].xyz * q.z + sh;
    float c8 = n.x * n.x - n.y * n.y;
    float c6 = n.z * n.z * 3.0 - 1.0;
    sh = gIt.sh[6].xyz * c6 + sh;
    sh = gIt.sh[8].xyz * c8 + sh;
    float3 fixedAmbient = (gIt.ambient.xyz + gIt.ambient.xyz) * gIt.exposureLM.x;
    sh = abs(sh) * (gIt.exposureIBL.x * gIt.mat2.w) + fixedAmbient;
    return color * sh * gIt.exposureIBL.w;
}

// The pixel's place on its segment: which triangle covers it and the perspective-correct weights of the quad's corners
// (at the pixel, and one pixel right / down for texture gradients), as the rasteriser interpolates.
struct Hit {
    bool ok;
    float w[4];     // corner weights (pixel centre)
    float wx[4];    // one pixel right
    float wy[4];    // one pixel down
};

void Weights(Tri t, float3 bary, int tri, out float w[4]) {
    // bary is in the (possibly swapped) triangle's vertex order; back to the quad's corners
    float3 b = bary * t.invW;
    b /= (b.x + b.y + b.z);
    if (t.flipped) b = b.xzy;
    w[0] = 0.0; w[1] = 0.0; w[2] = 0.0; w[3] = 0.0;
    if (tri == 0) { w[0] = b.x; w[1] = b.y; w[2] = b.z; }
    else { w[2] = b.x; w[1] = b.y; w[3] = b.z; }
}

void ClippedWeights(Tri t, float3 bary, ClipVertex a, ClipVertex b, ClipVertex c, int tri, out float w[4]) {
    float3 p = bary * t.invW;
    p /= p.x + p.y + p.z;
    if (t.flipped) p = p.xzy;
    p = a.original * p.x + b.original * p.y + c.original * p.z;
    w[0] = w[1] = w[2] = w[3] = 0.0;
    if (tri == 0) { w[0] = p.x; w[1] = p.y; w[2] = p.z; }
    else { w[2] = p.x; w[1] = p.y; w[3] = p.z; }
}

Hit LocateClipped(HsView v, float4 a, float4 b, float4 c, int tri, int2 px) {
    Hit h = (Hit)0;
    ClipVertex polygon[HS_CLIP_VERTICES];
    uint count = ClipTriangle(a, b, c, polygon);
    [loop] for (uint i = 1; i + 1 < count; i++) {
        Tri t = SetupTri(v, polygon[0].clip, polygon[i].clip, polygon[i + 1].clip);
        float3 bary; float depth;
        if (!t.ok || !Cover(t, px, bary) || !Depth(v, t, bary, depth)) continue;
        h.ok = true;
        ClippedWeights(t, bary, polygon[0], polygon[i], polygon[i + 1], tri, h.w);
        Cover(t, px + int2(1, 0), bary);
        ClippedWeights(t, bary, polygon[0], polygon[i], polygon[i + 1], tri, h.wx);
        Cover(t, px + int2(0, 1), bary);
        ClippedWeights(t, bary, polygon[0], polygon[i], polygon[i + 1], tri, h.wy);
        break;
    }
    return h;
}

Hit Locate(HsView v, Quad q, int2 px) {
    Hit h = (Hit)0;
    h.ok = false;
    Corner corners[4];
    [unroll] for (int j = 0; j < 4; j++) corners[j] = Project(v, q.clip[j]);
    bool safe = corners[0].ok && corners[1].ok && corners[2].ok && corners[3].ok;
#if HS_CLIPPED
    if (safe) return h;
#else
    if (!safe) return h;
#endif
    [unroll] for (int tri = 0; tri < 2; tri++) {
        if (h.ok) continue;
        float4 c0 = tri == 0 ? q.clip[0] : q.clip[2], c1 = q.clip[1], c2 = tri == 0 ? q.clip[2] : q.clip[3];
        Corner k0 = corners[0], k1 = corners[1], k2 = corners[2];
        if (tri != 0) { k0 = corners[2]; k2 = corners[3]; }
#if HS_CLIPPED
        [branch] if (!k0.ok || !k1.ok || !k2.ok) { h = LocateClipped(v, c0, c1, c2, tri, px); continue; }
#endif
        Tri t = SetupTriC(v, k0, k1, k2);
        float3 bary;
        if (h.ok || !t.ok || !Cover(t, px, bary)) continue;
        h.ok = true;
        Weights(t, bary, tri, h.w);
        float3 bx, by;
        Cover(t, px + int2(1, 0), bx);
        Cover(t, px + int2(0, 1), by);
        Weights(t, bx, tri, h.wx);
        Weights(t, by, tri, h.wy);
    }
    return h;
}

float3 Mix3(float w[4], float3 a[4]) { return a[0] * w[0] + a[1] * w[1] + a[2] * w[2] + a[3] * w[3]; }

#if HS_FORWARD_ADD
// Rotation of the filter's sample offsets (as UnitySampleShadowmap's noise).
float2 HopRotate(float2 t, float sn, float cs) { return float2(t.y * sn + cs * t.x, t.x * -sn + cs * t.y); }

static const float4 kHopPoisson[25] = {
    float4(0.123298101, -0.0392337516, -0.635181785, 0.217271104),
    float4(-0.56253773, -0.360242814, -0.149960607, 0.232067496),
    float4(0.640371919, 0.0682112277, -0.679780006, 0.688492417),
    float4(0.281338692, -0.588158786, -0.77586472, -0.253408998),
    float4(-0.573121786, 0.270057201, -0.473191589, -0.283272296),
    float4(0.203316599, 0.419773906, -0.333007902, 0.643005908),
    float4(0.846795797, -0.354558408, -0.138415098, -0.0983022526),
    float4(-0.423045099, -0.797441006, -0.818232715, -0.564593911),
    float4(0.719025314, 0.569357514, -0.91984719, 0.0654980168),
    float4(0.0381546803, -0.99141711, -0.142208502, -0.4872109),
    float4(-0.223626494, 0.502861381, -0.498083293, -0.588559926),
    float4(0.172225401, 0.983663023, -0.332615912, -0.849614799),
    float4(-0.291246414, 0.898051202, 0.306673586, -0.140199706),
    float4(-0.898414791, -0.0876278579, 0.114831701, 0.374455005),
    float4(-0.699508488, 0.673418522, -0.0388568006, 0.8071329),
    float4(-0.293195993, -0.0628911927, 0.410288513, 0.696029484),
    float4(0, 0, 0.556387722, 0.337537706),
    float4(0, 0, -0.0178657603, -0.887376487),
    float4(0, 0, 0.234990999, -0.455843806),
    float4(0, 0, 0.620677471, -0.155100495),
    float4(0, 0, 0.664064229, -0.569142699),
    float4(0, 0, 0.731272578, 0.583016813),
    float4(0, 0, 0.887970686, 0.0571521297),
    float4(0, 0, 0.312829614, -0.830802977),
    float4(0, 0, 0.868976414, -0.339797288),
};

float4 ShadowCoord(HsLight L, float3 w) {
    float4 sc = L.worldToShadow[1] * w.y;
    sc = L.worldToShadow[0] * w.x + sc;
    sc = L.worldToShadow[2] * w.z + sc;
    return sc + L.worldToShadow[3];
}

// Spot light shadow map: 25-tap PCF rotated per pixel (soft: radius from a 16-tap blocker search).
float ShadowSpot(HsLight L, Texture2D<float4> map, float3 w, float3 wx, float3 wy, bool soft) {
    float4 sc = ShadowCoord(L, w), scx = ShadowCoord(L, wx), scy = ShadowCoord(L, wy);
    float3 p = sc.xyz / sc.w;
    float2 g = p.xy * 10.0;
    float3 n = gRotation.SampleGrad(sRotation, g, scx.xy / scx.w * 10.0 - g, scy.xy / scy.w * 10.0 - g).xyz;
    float a = frac(sin(dot(n, float3(12.9898, 78.233, 45.5432))) * 43758.5469) * 10.0;
    float sn, cs;
    sincos(a, sn, cs);
    float radius = (1.0 - L.shadowData.x) * (soft ? 0.05 : 0.025);
    [branch] if (soft) {
        float blockers = 0.0;
        [loop] for (int i = 0; i < 16; i++)
            blockers += map.SampleLevel(sShadowRaw, HopRotate(kHopPoisson[i].xy, sn, cs) * radius + p.xy, 0).x;
        float mean = blockers * 0.0625;
        radius = radius * ((p.z - blockers * 0.0625) / mean);
    }
    float sum = 0.0;
    [loop] for (int i = 0; i < 25; i++)
        sum += map.SampleCmpLevelZero(sShadowCompare, HopRotate(kHopPoisson[i].zw, sn, cs) * radius + p.xy, p.z);
    return sum * 0.04;
}

float CubeDepth(HsLight L, float3 d, float nearest) {
    float3 a = abs(d);
    float3 m = max(-(a * rsqrt(dot(a, a))) * L.projectionParams.z + a, 0.0);
    float z = max(m.z, max(m.y, m.x));
    z = max(z - nearest, 0.0);
    z = L.projectionParams.y / z;
    z = z - L.projectionParams.x;
    return 1.0 - z;
}

// The screen position the cube filter's noise is read at (the original's, from the light-relative position).
float2 CubeNoiseUv(HsLight L, uint eye, float3 w) {
    float3 toLight = w - L.positionRange.xyz;
    float3 back = toLight + L.positionRange.xyz;
    float3 c = gIt.stereoMatrixVP[eye * 4 + 1].xyw * back.y;
    c = gIt.stereoMatrixVP[eye * 4 + 0].xyw * back.x + c;
    c = gIt.stereoMatrixVP[eye * 4 + 2].xyw * back.z + c;
    c = c + gIt.stereoMatrixVP[eye * 4 + 3].xyw;
    float2 half_ = float2(c.x * 0.5, (c.y * gIt.projectionParams.x) * 0.5);
    float2 uv = c.zz * 0.5 + half_;
    float4 so = gIt.stereoScaleOffset[eye];
    uv = uv * so.xy + c.zz * so.zw;
    return uv / c.z;
}

// Point light shadow cube: 25 taps in a plane across the light direction, rotated per pixel (soft: blocker search).
float ShadowCube(HsLight L, TextureCube<float4> cube, uint eye, float3 w, float3 wx, float3 wy, bool soft) {
    float3 toLight = w - L.positionRange.xyz;
    float2 uv = CubeNoiseUv(L, eye, w);
    float3 n = gRotation.SampleGrad(sRotation, uv, CubeNoiseUv(L, eye, wx) - uv, CubeNoiseUv(L, eye, wy) - uv).xyz;
    float3 r = frac(sin(cross(n, float3(12.9898, 78.233, 45.5432))) * 43758.5469) * 10.0 + 0.0001;
    float3 t1 = normalize(cross(toLight, r));
    float3 t2 = normalize(cross(toLight, t1));
    float radius = (1.0 - L.shadowData.x) * (soft ? 0.5 : 0.1);
    t1 = t1 * radius;
    t2 = t2 * radius;
    float nearest = max(L.projectionParams.z, 0.005);
    float scale = 1.0;
    [branch] if (soft) {
        float centre = CubeDepth(L, toLight, nearest);
        float blockers = 0.0;
        [loop] for (int i = 0; i < 16; i++)
            blockers += cube.SampleLevel(sShadowRaw, t2 * kHopPoisson[i].y + t1 * kHopPoisson[i].x + toLight, 0).x;
        float mean = blockers * 0.0625;
        scale = (centre - blockers * 0.0625) / mean;
    }
    float sum = 0.0;
    [loop] for (int i = 0; i < 25; i++) {
        float3 d = soft ? (t2 * kHopPoisson[i].w + t1 * kHopPoisson[i].z) * scale + toLight
                        : t2 * kHopPoisson[i].w + t1 * kHopPoisson[i].z + toLight;
        sum += cube.SampleCmpLevelZero(sShadowCompare, d, CubeDepth(L, d, nearest));
    }
    return sum * 0.04;
}

float3 LightCoord(HsLight L, float3 w) {
    float3 lc = L.worldToLight[1].xyz * w.y;
    lc = L.worldToLight[0].xyz * w.x + lc;
    lc = L.worldToLight[2].xyz * w.z + lc;
    return lc + L.worldToLight[3].xyz;
}
float4 LightCoord4(HsLight L, float3 w) {
    float4 lc = L.worldToLight[1] * w.y;
    lc = L.worldToLight[0] * w.x + lc;
    lc = L.worldToLight[2] * w.z + lc;
    return lc + L.worldToLight[3];
}

// Light t's attenuation and shadow at the pixel (world w; wx / wy one pixel right / down, for texture gradients).
float Attenuation(int t, HsLight L, uint eye, float3 w, float3 wx, float3 wy, float2 screenUv) {
    uint kind = L.kind.x, shadow = L.kind.y;
    float a = 1.0;
    [branch] if (kind == HS_LIGHT_POINT) {
        float3 lc = LightCoord(L, w), lx = LightCoord(L, wx), ly = LightCoord(L, wy);
        float2 g = dot(lc, lc).xx;
        a = gAtten[t].SampleGrad(sAtten, g, dot(lx, lx).xx - g, dot(ly, ly).xx - g).x;
        [branch] if (shadow == HS_SHADOW_CUBE || shadow == HS_SHADOW_CUBE_SOFT)
            a = ShadowCube(L, gShadowCube[t], eye, w, wx, wy, shadow == HS_SHADOW_CUBE_SOFT) * a;
    } else if (kind == HS_LIGHT_SPOT) {
        float4 lc = LightCoord4(L, w), lx = LightCoord4(L, wx), ly = LightCoord4(L, wy);
        float2 cu = lc.xy / lc.w + 0.5;
        float cookie = gCookie[t].SampleGrad(sCookie, cu, lx.xy / lx.w + 0.5 - cu, ly.xy / ly.w + 0.5 - cu).w;
        float front = 0.0 < lc.z ? 1.0 : 0.0;
        float2 g = dot(lc.xyz, lc.xyz).xx;
        float falloff = gAtten[t].SampleGrad(sAtten, g, dot(lx.xyz, lx.xyz).xx - g, dot(ly.xyz, ly.xyz).xx - g).x;
        a = falloff * (cookie * front);
        [branch] if (shadow == HS_SHADOW_DEPTH || shadow == HS_SHADOW_DEPTH_SOFT)
            a = ShadowSpot(L, gShadow[t], w, wx, wy, shadow == HS_SHADOW_DEPTH_SOFT) * a;
    } else {
        [branch] if (shadow == HS_SHADOW_SCREEN) a = gShadow[t].SampleLevel(sDirectionalScreen, screenUv, 0).x;
    }
    return a;
}
#endif  // HS_FORWARD_ADD

// The screen position Unity's ComputeScreenPos gives at the pixel (main light / directional screen-space shadows).
float2 ScreenUv(HsView v, float2 pixel) {
    float2 ndc = float2((pixel.x - v.viewport.x) / v.viewport.z * 2.0 - 1.0, 1.0 - (pixel.y - v.viewport.y) / v.viewport.w * 2.0);
    float2 p = float2(ndc.x * 0.5 + 0.5, ndc.y * gIt.projectionParams.x * 0.5 + 0.5);
    float4 so = gIt.stereoScaleOffset[v.mode.w];
    return p * so.xy + so.zw;
}

float4 HsResolveVS(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 HsResolvePS(float4 position : SV_Position, out float depth : SV_Depth) : SV_Target {
    int2 px = int2(position.xy);
    uint id = gVisId[px];
    if (id == 0) discard;
    depth = asfloat(gVisDepth[px]);
    uint item = id >> HS_ITEM_SHIFT, seg = (id & HS_SEG_MASK) - 1;
    gIt = gItems[item];
    uint eye = (uint)px.x >= gResolve.x ? 1 : 0;
    HsView v = gViews[eye];
    uint S = gIt.counts.z;
    uint strand = seg / S, k = seg - strand * S;
    uint p = gIt.counts.w + strand * (S + 1) + k;
    HairStrandPoint a = LoadPoint(p), b = LoadPoint(p + 1);
    float ua = gDomain[DomainU(S, k)], ub = gDomain[DomainU(S, k + 1)];
    Quad q = MakeQuad(gIt, eye, a, b, ua, ub);
    Hit h = Locate(v, q, px);
    if (!h.ok) discard;
    float we[4] = { h.w[0] + h.w[2], h.w[1] + h.w[3], 0.0, 0.0 };  // weight of point a, point b
    float3 world = Mix3(h.w, q.world);
    float3 cam = gIt.stereoCameraPos[eye].xyz;
    float3 tca = cam - a.pos, tcb = cam - b.pos;
    float ida = rsqrt(dot(tca, tca)), idb = rsqrt(dot(tcb, tcb));
#if HS_FORWARD_ADD
    float3 worldX = Mix3(h.wx, q.world), worldY = Mix3(h.wy, q.world);
    float2 screenUv = ScreenUv(v, position.xy);
    float3 c = 0.0;
    float alpha = 0.0;
    uint mask = eye ? gIt.flags.y >> 16 : gIt.flags.x;
    uint2 pu = (uint2)px, quad = pu & ~1u;
    [unroll] for (int t = 0; t < HS_MAX_LIGHTS; t++) {
        [branch] if (mask & (1u << t)) {
            HsLight L = gLights[t];
            uint2 sc = eye ? L.scissor.zw : L.scissor.xy;
            uint2 lo = sc & 0xffff, hi = sc >> 16;
            // tested per 2x2 quad first, as the one-pass shader (the original's texture reads had their derivatives)
            [branch] if (all(quad + 2 > lo) && all(quad < hi)) {
                float3 la, lb;
                if (L.kind.x == HS_LIGHT_DIRECTIONAL) {
                    la = HairDirect(a, tca, ida, ida * tca, L.pos.xyz, L.color.xyz, ua, 0.0);
                    lb = HairDirect(b, tcb, idb, idb * tcb, L.pos.xyz, L.color.xyz, ub, 0.0);
                } else {
                    float3 dla = L.pos.xyz - a.pos, dlb = L.pos.xyz - b.pos;
                    float ila = rsqrt(max(dot(dla, dla), 1e-6)), ilb = rsqrt(max(dot(dlb, dlb), 1e-6));
                    la = HairDirect(a, tca, ida, ida * tca, dla * ila, L.color.xyz, ua, saturate(HS_LIGHT_RADIUS * ila));
                    lb = HairDirect(b, tcb, idb, idb * tcb, dlb * ilb, L.color.xyz, ub, saturate(HS_LIGHT_RADIUS * ilb));
                }
                float3 l = (la * we[0] + lb * we[1]) * Attenuation(t, L, eye, world, worldX, worldY, screenUv);
                bool inside = all(pu >= lo) && all(pu < hi);
                c += inside ? l : 0.0;
                alpha = inside ? 1.0 : alpha;
            }
        }
    }
    if (alpha == 0.0) discard;
    return float4(c, alpha);
#else
    float3 da = HairDirect(a, tca, ida, ida * tca, gIt.mainLightPos.xyz, gIt.mainLightColor.xyz, ua, 0.0);
    float3 db = HairDirect(b, tcb, idb, idb * tcb, gIt.mainLightPos.xyz, gIt.mainLightColor.xyz, ub, 0.0);
    float3 direct = da * we[0] + db * we[1];
    float3 ambient = HairAmbient(a.normal, a.color) * we[0] + HairAmbient(b.normal, b.color) * we[1];
    [branch] if (gIt.flags.y & 1) {
        float m = gScreenShadow.SampleLevel(sScreenShadow, ScreenUv(v, position.xy), 0).x;
        return float4(direct * m + ambient, 1.0);
    }
    return float4(ambient + direct, 1.0);
#endif
}
#endif  // HS_RESOLVE
