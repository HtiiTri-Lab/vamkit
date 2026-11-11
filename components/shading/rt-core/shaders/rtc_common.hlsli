// rtc_common.hlsli: bindings, records and helpers shared by RtCore's passes (path_*.hlsl, cs_6_5 / lib_6_5).
// Records mirror src/rtcore.cpp's *Gpu structs byte for byte.
#ifndef RTC_COMMON_HLSLI
#define RTC_COMMON_HLSLI

#define RTC_NONE 0xffffffffu

// ---- bindings ---------------------------------------------------------------------------------------------------
// One root signature for every pass: b0 per-dispatch constants, b1 the frame, t0 the scene, one SRV and one UAV table
// over RtCore's shader-visible heap (each range aliases the whole heap with its own view type; indices are absolute).
#define RTC_ROOT \
    "RootFlags(0)," \
    "RootConstants(num32BitConstants=12, b0)," \
    "CBV(b1)," \
    "SRV(t0)," \
    "DescriptorTable(" \
    "SRV(t0, space=1, numDescriptors=unbounded, offset=0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)," \
    "SRV(t0, space=2, numDescriptors=unbounded, offset=0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)," \
    "SRV(t0, space=3, numDescriptors=unbounded, offset=0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)," \
    "SRV(t0, space=4, numDescriptors=unbounded, offset=0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)," \
    "SRV(t0, space=5, numDescriptors=unbounded, offset=0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "DescriptorTable(" \
    "UAV(u0, space=1, numDescriptors=unbounded, offset=0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)," \
    "UAV(u0, space=2, numDescriptors=unbounded, offset=0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)," \
    "UAV(u0, space=3, numDescriptors=unbounded, offset=0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)," \
    "UAV(u0, space=4, numDescriptors=unbounded, offset=0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)," \
    "UAV(u0, space=5, numDescriptors=unbounded, offset=0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)," \
    "UAV(u0, space=6, numDescriptors=unbounded, offset=0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)," \
    "UAV(u0, space=7, numDescriptors=unbounded, offset=0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))," \
    "SRV(t1)," \
    "DescriptorTable(UAV(u0, space=8, numDescriptors=1))," \
    "StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)," \
    "StaticSampler(s1, filter=FILTER_MIN_MAG_MIP_POINT, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)," \
    "StaticSampler(s2, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_WRAP, addressV=TEXTURE_ADDRESS_WRAP, addressW=TEXTURE_ADDRESS_WRAP)," \
    "StaticSampler(s3, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_WRAP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)," \
    "StaticSampler(s4, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_WRAP, addressW=TEXTURE_ADDRESS_CLAMP)," \
    "StaticSampler(s5, filter=FILTER_MIN_MAG_MIP_POINT, addressU=TEXTURE_ADDRESS_WRAP, addressV=TEXTURE_ADDRESS_WRAP, addressW=TEXTURE_ADDRESS_WRAP)," \
    "StaticSampler(s6, filter=FILTER_MIN_MAG_MIP_POINT, addressU=TEXTURE_ADDRESS_WRAP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP)," \
    "StaticSampler(s7, filter=FILTER_MIN_MAG_MIP_POINT, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_WRAP, addressW=TEXTURE_ADDRESS_CLAMP)"

cbuffer Pass : register(b0) {
    uint pReceiver;   // pass specific (see each entry)
    uint pLight;      // light slot (shadow passes)
    uint pCount;      // work items
    uint pSeed;
    uint pA, pB, pC, pD;
    uint pE, pF, pG, pH;
};

struct LightGpu {
    float3 position; uint type;      // 0 directional, 1 spot, 2 point
    float3 direction; float range;   // direction the light travels
    float3 color; float spotCos;
    float radius; float tanAngular; uint traced; uint excludedLinks;  // API 18: link classes it leaves out
};
cbuffer Frame : register(b1) {
    float4 fSh[7];                   // unity_SHAr, SHAg, SHAb, SHBr, SHBg, SHBb, SHC
    float4 fSky[7];                  // API 8: Skyshop's diffuse SH x _ExposureIBL.x in world directions (rtcore.h skySh)
    uint fGeometries, fSubmeshes, fMaterials;  // record buffers' SRV indices
    float fAmbientLM;                // API 8: _ExposureLM.x
};

RaytracingAccelerationStructure gScene : register(t0);
RaytracingAccelerationStructure gPathScene : register(t1); // shared meshes + explicit fibers
ByteAddressBuffer gBuf[] : register(t0, space1);
Texture2D<float4> gTex[] : register(t0, space2);
TextureCube<float4> gCube[] : register(t0, space5);
RWByteAddressBuffer gRwBuf[] : register(u0, space1);
RWTexture2D<float4> gRwTex[] : register(u0, space2);
RWTexture2D<float> gRwScalar[] : register(u0, space6);
RWTexture2D<float2> gRwVec2[] : register(u0, space7);
SamplerState gLinear : register(s0);
SamplerState gPoint : register(s1);
SamplerState gRepeat : register(s2);
SamplerState gWrapU : register(s3);
SamplerState gWrapV : register(s4);
SamplerState gPointRepeat : register(s5);
SamplerState gPointWrapU : register(s6);
SamplerState gPointWrapV : register(s7);

// 1D work items over a 2D grid of 64-thread groups (rows of 65535 groups): RtCore's Dispatch splits the same way.
uint Linear(uint3 t) { return t.x + t.y * 4194240u; }

// ---- records ----------------------------------------------------------------------------------------------------
struct GeoGpu {
    uint posBuf, posOffset, posStride, posFormat;
    uint nrmBuf, nrmOffset, nrmStride, nrmFormat;
    uint uvBuf, uvOffset, uvStride, uvFormat;
    uint idxBuf, idxOffset, index32, baseVertex;
    uint firstSub, subCount, receiver, receiverTriBase;  // receiver, receiverTriBase: unused (RTC_NONE, 0)
    uint flags, lightLink, pad1, pad2;  // lightLink (API 18, PT): the object's light-link class bit
    float4 m0, m1, m2;               // object -> world rows (skinned: identity or VaM's drawOffset)
    uint tanBuf, tanOffset, tanStride, tanFormat;
};
struct SubGpu {
    uint material, triStart, triCount, tessLevel;  // triStart: first triangle in the geometry's index buffer
    float tessAlpha; uint blasTess; uint tessBuf; uint pad0;  // blasTess 1: the BLAS geometry is the tessellated copy
};
struct MatGpu {
    uint kind, alphaSlice, hasAlpha, alphaFine;  // kind: 0 opaque 1 cutout 2 emissive 3 translucent; hasAlpha bit 2
                                                 // (API 26): solid shadow, its cutoff above bit 16; alphaSlice, alphaFine: 0
    float4 color;                    // linear average colour; a = alpha cutoff (cutout) / opacity (translucent)
    float4 uvST;
    float4 env1, env2;               // API 8: SurfaceAmbient's (f1 rgb, 1 - _IBLFilter), (f2 rgb, EnvironmentShader)
};
#define GEO_STRIDE 160
#define SUB_STRIDE 32
#define MAT_STRIDE 80
GeoGpu LoadGeo(uint i) { return gBuf[fGeometries].Load<GeoGpu>(i * GEO_STRIDE); }
SubGpu LoadSub(uint i) { return gBuf[fSubmeshes].Load<SubGpu>(i * SUB_STRIDE); }
MatGpu LoadMat(uint i) { return gBuf[fMaterials].Load<MatGpu>(i * MAT_STRIDE); }

// Vertex formats (rtcore.h VertexFormat)
float4 LoadFloat4(uint buf,uint offset,uint stride,uint format,uint v) {
    uint at=offset+v*stride;
    if(format==2) {
        uint2 h=gBuf[NonUniformResourceIndex(buf)].Load2(at);
        return float4(f16tof32(h.x),f16tof32(h.x>>16),f16tof32(h.y),f16tof32(h.y>>16));
    }
    return asfloat(gBuf[NonUniformResourceIndex(buf)].Load4(at));
}
float3 LoadFloat3(uint buf, uint offset, uint stride, uint format, uint v) {
    uint at = offset + v * stride;
    if (format == 2) {  // Half4
        uint2 h = gBuf[NonUniformResourceIndex(buf)].Load2(at);
        return float3(f16tof32(h.x), f16tof32(h.x >> 16), f16tof32(h.y));
    }
    return asfloat(gBuf[NonUniformResourceIndex(buf)].Load3(at));
}
float2 LoadFloat2(uint buf, uint offset, uint stride, uint format, uint v) {
    uint at = offset + v * stride;
    if (format == 4) { uint h = gBuf[NonUniformResourceIndex(buf)].Load(at); return float2(f16tof32(h), f16tof32(h >> 16)); }
    return asfloat(gBuf[NonUniformResourceIndex(buf)].Load2(at));
}
uint3 LoadTriangle(GeoGpu g, uint tri) {
    if (g.index32) return gBuf[NonUniformResourceIndex(g.idxBuf)].Load3(g.idxOffset + tri * 12) + g.baseVertex;
    uint at = g.idxOffset + tri * 6;
    uint w0 = gBuf[NonUniformResourceIndex(g.idxBuf)].Load(at & ~3u), w1 = gBuf[NonUniformResourceIndex(g.idxBuf)].Load((at & ~3u) + 4);
    uint3 i;
    if ((at & 3u) == 0) i = uint3(w0 & 0xffff, w0 >> 16, w1 & 0xffff);
    else i = uint3(w0 >> 16, w1 & 0xffff, w1 >> 16);
    return i + g.baseVertex;
}
float3 ToWorld(GeoGpu g, float3 p) {
    return float3(dot(g.m0.xyz, p) + g.m0.w, dot(g.m1.xyz, p) + g.m1.w, dot(g.m2.xyz, p) + g.m2.w);
}
float3 LocalPosition(GeoGpu g, uint v) { return LoadFloat3(g.posBuf, g.posOffset, g.posStride, g.posFormat, v); }
float3 LocalNormal(GeoGpu g, uint v) { return normalize(LoadFloat3(g.nrmBuf, g.nrmOffset, g.nrmStride, g.nrmFormat, v)); }
float3 GeoPosition(GeoGpu g, uint v) { return ToWorld(g, LocalPosition(g, v)); }

// Phong tessellation (VaM's GlossNMTessMapped* domain shader): the flat point projected onto each corner's tangent
// plane, blended by the barycentrics, then lerp(flat, phong, alpha).
float3 PhongPoint(float3 b, float3 p0, float3 p1, float3 p2, float3 n0, float3 n1, float3 n2, float alpha) {
    float3 q = b.x * p0 + b.y * p1 + b.z * p2;
    float3 c0 = q - dot(q - p0, n0) * n0, c1 = q - dot(q - p1, n1) * n1, c2 = q - dot(q - p2, n2) * n2;
    float3 ph = b.x * c0 + b.y * c1 + b.z * c2;
    return lerp(q, ph, alpha);
}

// Sub-triangle s (0..L*L-1) of a uniform L-subdivision: its corners as barycentrics of the parent triangle.
// Row j (0..L-1) holds L-j "up" triangles and L-j-1 "down" ones, alternating up, down, up, ...
void SubTriangle(uint L, uint s, out float3 b0, out float3 b1, out float3 b2) {
    uint j = 0, first = 0;
    [loop] for (; j < L; j++) {
        uint inRow = 2 * (L - j) - 1;
        if (s < first + inRow) break;
        first += inRow;
    }
    uint k = s - first, i = k >> 1;
    float inv = 1.0 / L;
    // grid point (i, j): weight of corner1 = i/L, corner2 = j/L
    float2 a, bq, cq;
    if ((k & 1u) == 0) { a = float2(i, j); bq = float2(i + 1, j); cq = float2(i, j + 1); }
    else { a = float2(i + 1, j); bq = float2(i + 1, j + 1); cq = float2(i, j + 1); }
    b0 = float3(1 - (a.x + a.y) * inv, a.x * inv, a.y * inv);
    b1 = float3(1 - (bq.x + bq.y) * inv, bq.x * inv, bq.y * inv);
    b2 = float3(1 - (cq.x + cq.y) * inv, cq.x * inv, cq.y * inv);
}

// A BLAS hit -> the geometry's triangle (index buffer order) and barycentrics on it.
void HitTriangle(SubGpu s, uint prim, float2 hb, out uint tri, out float3 b) {
    b = float3(1 - hb.x - hb.y, hb.x, hb.y);
    if (s.blasTess != 0) {
        uint L = max(s.tessLevel, 1u), n = L * L;
        float3 c0, c1, c2;
        SubTriangle(L, prim % n, c0, c1, c2);
        b = b.x * c0 + b.y * c1 + b.z * c2;
        tri = s.triStart + prim / n;
    } else {
        tri = s.triStart + prim;
    }
}

// ---- random -----------------------------------------------------------------------------------------------------
uint Hash(uint x) {  // lowbias32
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
float Rand(inout uint s) { s = Hash(s); return (s >> 8) * (1.0 / 16777216.0); }

void Basis(float3 n, out float3 t, out float3 b) {  // Duff et al. 2017
    float s = n.z >= 0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z), c = n.x * n.y * a;
    t = float3(1 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

// ---- tracing ----------------------------------------------------------------------------------------------------
// A committed hit (t < 0: none).
struct Hit { float t; uint inst, geo, prim; float2 bary; };

// Unity's light falloff as VaM uses it (attenuation 1/(1+25(d/range)^2), nothing past range) and a smooth spot edge.
float LightFalloff(LightGpu L, float3 p, out float3 toLight, out float dist) {
    if (L.type == 0) { toLight = -L.direction; dist = 1e4; return 1; }
    float3 v = L.position - p;
    dist = length(v);
    toLight = v / max(dist, 1e-6);
    float x = dist / max(L.range, 1e-4);
    if (x >= 1) return 0;
    float att = 1.0 / (1.0 + 25.0 * x * x);
    if (L.type == 1) {
        float c = dot(-toLight, L.direction);
        att *= smoothstep(L.spotCos, lerp(L.spotCos, 1.0, 0.25), c);
    }
    return att;
}

#endif
