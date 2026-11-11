// AMD FSR 1 integration. The algorithm lives in the unmodified MIT SDK headers.
// Each dispatch operates on one already cropped eye, after NR composition.
// VAM_UI_BYPASS (with VAM_FSR_EASU): DLSS SR's menu pass (dx11_vam_dlss.cpp).
// Where the UI mask marks VaM's world-space menus, DLSS's result is replaced
// by EASU of this frame alone, at this frame's jitter: no history to smear.
#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"

#ifndef VAM_UI_BYPASS
#define VAM_UI_BYPASS 0
#endif

#if VAM_UI_BYPASS
cbuffer Parameters : register(b0) {
    uint4 Easu0, Easu1, Easu2, Easu3;
    float4 MaskMap;   // output pixel centre -> UI mask uv: (p + 0.5) * xy + zw
    uint2 OutputSize;
    uint LinearInput;
    uint Pad;
};
Texture2D<float4> Upscaled : register(t1); // DLSS SR's output
Texture2D<float> UiMask : register(t2);    // render size, 1 on menus in front of the scene
#else
cbuffer Parameters : register(b0) {
    uint4 Easu0, Easu1, Easu2, Easu3;
    uint4 Rcas;
    uint2 OutputSize;
    uint LinearInput;
    uint Sharpen;
};
#endif
Texture2D<float4> Input : register(t0);
RWTexture2D<float4> Output : register(u0);
SamplerState LinearClamp : register(s0);

#if VAM_FSR_EASU
#define FSR_EASU_F 1
void GatherColor(float2 uv, out float4 r, out float4 g, out float4 b) {
    r = Input.GatherRed(LinearClamp, uv);
    g = Input.GatherGreen(LinearClamp, uv);
    b = Input.GatherBlue(LinearClamp, uv);
    if (LinearInput != 0) {
        // SDK SRTM (RGB-ratio preserving), then gamma 2.0. Convert each
        // gathered texel before filtering; inverse is performed after RCAS.
        r = max(r, 0); g = max(g, 0); b = max(b, 0);
        float4 divisor = 1 + max(r, max(g, b));
        r = sqrt(r / divisor); g = sqrt(g / divisor); b = sqrt(b / divisor);
    }
}
AF4 FsrEasuRF(AF2 p) { float4 r, g, b; GatherColor(p, r, g, b); return r; }
AF4 FsrEasuGF(AF2 p) { float4 r, g, b; GatherColor(p, r, g, b); return g; }
AF4 FsrEasuBF(AF2 p) { float4 r, g, b; GatherColor(p, r, g, b); return b; }
#else
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) {
    // Clamp integer neighbors as well as the EASU sampler at every eye edge.
    return Input.Load(int3(clamp(p, int2(0, 0), int2(OutputSize) - 1), 0));
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#endif
#include "ffx_fsr1.h"

#if VAM_UI_BYPASS
void FilterPixel(uint2 p) {
    if (any(p >= OutputSize)) return;
    float4 upscaled = Upscaled.Load(int3(p, 0));
    // Bilinear: a one-render-pixel blend at the menu's edge.
    float weight = UiMask.SampleLevel(LinearClamp, (float2(p) + 0.5) * MaskMap.xy + MaskMap.zw, 0);
    if (weight <= 0.0) {
        Output[p] = upscaled;
        return;
    }
    float3 c;
    FsrEasuF(c, p, Easu0, Easu1, Easu2, Easu3);
    if (LinearInput != 0) {
        c = saturate(c);
        c *= c;
        FsrSrtmInvF(c);
    }
    Output[p] = float4(lerp(upscaled.rgb, c, weight), upscaled.a);
}
#else
void FilterPixel(uint2 p) {
    if (any(p >= OutputSize)) return;
    float3 c;
    float alpha;
#if VAM_FSR_EASU
    FsrEasuF(c, p, Easu0, Easu1, Easu2, Easu3);
    alpha = Input.SampleLevel(LinearClamp, (float2(p) + 0.5) / float2(OutputSize), 0).a;
#else
    float4 original = Input.Load(int3(p, 0));
    if (Sharpen != 0) FsrRcasF(c.r, c.g, c.b, p, Rcas);
    else c = original.rgb;
    alpha = original.a;
    if (LinearInput != 0) {
        c = saturate(c);
        c *= c;
        FsrSrtmInvF(c);
    }
#endif
    Output[p] = float4(c, alpha);
}
#endif

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
    uint2 p = ARmp8x8(thread.x) + uint2(group.x << 4, group.y << 4);
    FilterPixel(p);
    FilterPixel(p + uint2(8, 0));
    FilterPixel(p + uint2(8, 8));
    FilterPixel(p + uint2(0, 8));
}
