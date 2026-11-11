// path_scene.hlsl: RtCore's scene passes for the PT (cs_6_5; tools/gen_shaders.py compiles them): buffer clears and the
// Phong-tessellated copies the bottom levels are built on.
#include "rtc_common.hlsli"

// ---- utilities ----------------------------------------------------------------------------------------------------
// pA UAV, pB value, pC first dword, pCount dwords
[RootSignature(RTC_ROOT)] [numthreads(64, 1, 1)] void ClearBuffer(uint3 dtid : SV_DispatchThreadID) {
    uint id = Linear(dtid);
    if (id < pCount) gRwBuf[pA].Store((pC + id) * 4, pB);
}

// ---- Tessellate: one submesh's uniform L-subdivision on the Phong surface (the occluder copy for the BLAS) --------
// In object space, as the vertex buffer the untessellated BLAS reads: the instance's transform (the geometry's matrix)
// places both. (0.22.0 wrote world positions here and the instance moved them a second time: with a matrix other than
// identity - a skin's drawOffset - the tessellated occluders sat off the drawn surface.)
// pA geometry slot, pB submesh record, pC output UAV (float3 x 3 per sub-triangle), pCount sub-triangles
[RootSignature(RTC_ROOT)] [numthreads(64, 1, 1)] void Tessellate(uint3 dtid : SV_DispatchThreadID) {
    uint id = Linear(dtid);
    if (id >= pCount) return;
    GeoGpu g = LoadGeo(pA);
    SubGpu s = LoadSub(pB);
    uint L = max(s.tessLevel, 1u), n = L * L;
    uint3 v = LoadTriangle(g, s.triStart + id / n);
    float3 p0 = LocalPosition(g, v.x), p1 = LocalPosition(g, v.y), p2 = LocalPosition(g, v.z);
    float3 n0 = LocalNormal(g, v.x), n1 = LocalNormal(g, v.y), n2 = LocalNormal(g, v.z);
    float3 c[3];
    SubTriangle(L, id % n, c[0], c[1], c[2]);
    [unroll] for (uint k = 0; k < 3; k++) {
        float3 p = PhongPoint(c[k], p0, p1, p2, n0, n1, n2, s.tessAlpha);
        gRwBuf[pC].Store3((id * 3 + k) * 12, asuint(p));
    }
}
