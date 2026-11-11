// Screen-space subsurface scattering for VaM's skin (0.10.0; native\sss.cpp, compiled by tools\gen_sss_shaders.py).
//
// The skin draws of the eye camera's opaque pass also wrote (gen_spec_table.py): the direct diffuse light without the
// albedo and the distance to the camera (E), the albedo (A) and SV_Position.z (Z). When the opaque pass ends:
//   SssPrep   E where the pixel is still that skin (Z = the depth buffer: nothing drawn over it since), else 0
//   SssBlur   twice (across, then down): d'Eon & Luebke's six-Gaussian skin profile (GPU Gems 3 ch. 14) along a line of
//             21 taps placed in millimetres on the skin (more near the centre), each tap the profile's share of its
//             interval; the taps' pixel steps from the distance (pitch = radians per pixel); a tap on other skin, off
//             the eye or on no skin counts nothing, and a tap at another depth counts less by the same Gaussians
//             (exp(-dz^2 / 2 var): the profile at the 3-D distance); weights renormalised per colour
//   SssComp   colour + albedo * strength * (blurred E - E), where the pixel is skin (full-screen triangle, others
//             discarded): only the direct diffuse light moves, the specular and the ambient stay
cbuffer SssCb : register(b0) {
    uint4 sizeEyes;        // x width, y height, z eyes side by side (1 or 2)
    float4 view;           // x radians per pixel, y depth tolerance, z strength, w the translucency's strength (0.20.0)
    float4 dir;            // xy blur direction in pixels
    float4 gInvTwoVar[2];  // the six Gaussians' 1 / (2 variance), mm^-2
    float4 gWr[2];         // their weights in red, green, blue
    float4 gWg[2];
    float4 gWb[2];
    float4 tapO[21];       // x tap offset, mm
    float4 tapI[42];       // each Gaussian's share of the tap's interval: [2i] Gaussians 0..3, [2i + 1].xy 4, 5
};

Texture2D<float4> tIn : register(t0);
Texture2D<float> tZ : register(t1);
Texture2D<float> tDepth : register(t2);
RWTexture2D<float4> uOut : register(u0);

[numthreads(8, 8, 1)]
void SssPrep(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= sizeEyes.xy)) return;
    float4 e = tIn[id.xy];
    bool skin = e.w > 0.0 && abs(tZ[id.xy] - tDepth[id.xy]) <= view.y;
    uOut[id.xy] = skin ? e : 0.0;
}

Texture2D<float4> tRegion : register(t1); // albedo alpha holds regional response
[numthreads(8, 8, 1)]
void SssBlur(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= sizeEyes.xy)) return;
    float4 c = tIn[id.xy];
    if (c.w <= 0.0) {
        uOut[id.xy] = 0.0;
        return;
    }
    uint eyeW = sizeEyes.x / sizeEyes.z;
    int x0 = (int)((id.x / eyeW) * eyeW), x1 = x0 + (int)eyeW - 1, y1 = (int)sizeEyes.y - 1;
    float regional = clamp(tRegion[id.xy].w, .5, 1.08);
    float radius = sqrt(regional); // bounded profile variation, not body-ray thickness
    float pxPerMM = radius / (c.w * 1000.0 * view.x);
    float2 at = float2(id.xy) + 0.5;
    float3 acc = 0.0, ws = 0.0;
    [unroll] for (int i = 0; i < 21; i++) {
        int2 q = (int2)floor(at + dir.xy * (tapO[i].x * pxPerMM));
        if (q.x < x0 || q.x > x1 || q.y < 0 || q.y > y1) continue;
        float4 s = tIn[q];
        if (s.w <= 0.0) continue;
        float dz = (s.w - c.w) * 1000.0 / radius;
        float dz2 = dz * dz;
        float4 f0 = tapI[2 * i] * exp(-dz2 * gInvTwoVar[0]);
        float2 f1 = tapI[2 * i + 1].xy * exp(-dz2 * gInvTwoVar[1].xy);
        float3 w = float3(dot(f0, gWr[0]) + dot(f1, gWr[1].xy), dot(f0, gWg[0]) + dot(f1, gWg[1].xy),
                          dot(f0, gWb[0]) + dot(f1, gWb[1].xy));
        acc += w * s.xyz;
        ws += w;
    }
    uOut[id.xy] = float4(acc / max(ws, 1e-12), c.w);
}

float4 SssVS(uint i : SV_VertexID) : SV_Position {
    return float4(i == 2 ? 3.0 : -1.0, i == 1 ? -3.0 : 1.0, 0.5, 1.0);
}

Texture2D<float4> tColor : register(t0);
Texture2D<float4> tE : register(t1);
Texture2D<float4> tAlbedo : register(t2);
Texture2D<float4> tBlur : register(t3);
Texture2D<float4> tBlurT : register(t4);  // 0.20.0: the translucency, blurred over centimetres (view.w 0 = none)

float4 SssComp(float4 pos : SV_Position) : SV_Target {
    uint2 p = uint2(pos.xy);
    float4 b = view.z > 0.0 ? tBlur[p] : 0.0;
    float4 bt = view.w > 0.0 ? tBlurT[p] : 0.0;
    if ((view.z > 0.0 ? b.w : bt.w) <= 0.0) discard;
    float4 c = tColor[p];
    float3 t = view.w * bt.xyz;
    float3 e = tE[p].xyz;
    // Adapt only the extra skin-light transport. Both sides of (blur - E) fade
    // together, so reducing scattering restores the original direct diffuse.
    // Include the arriving diffuse/translucency light: a shadow next to a strong
    // source can still scatter. Albedo and highlights do not drive the adaptation.
    float3 transportLight = max(e, b.xyz) + t;
    float lowLight = smoothstep(0.0, 0.06, dot(max(transportLight, 0.0), float3(0.2126, 0.7152, 0.0722)));
    c.xyz = max(c.xyz + tAlbedo[p].xyz * lowLight * (view.z * saturate(tAlbedo[p].w) * (b.xyz - e) + t), 0.0);
    return c;
}
