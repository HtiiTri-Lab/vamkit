// The path tracer's skin scattering in screen space, before RR (11-10, docs\pt-skin-sss-ss-20261110).
// The raster route's blur (the skin component's shaders\sss.hlsl, the look approved there) on the PT's input. The
// trace's PT_SSS variant wrote, where the eye meets skin with a scattering strength first, strength x its direct diffuse
// light over the RR albedo guide (the output's scale) and its depth along the camera's axis; 0 elsewhere.
//   PathScatterX  across: d'Eon & Luebke's six-Gaussian skin profile (GPU Gems 3 ch. 14) along 21 taps placed in profile
//                 millimetres (denser near the centre), each the profile's share of its interval; the taps' pixel steps
//                 from the depth; a tap off skin or off the image counts nothing, a tap at another depth counts less by
//                 the same Gaussians (the profile at the 3-D distance); weights renormalised per colour
//   PathScatterY  down, then colour + albedo guide x (blurred - own): only the direct diffuse light moves; the specular,
//                 the environment's and the bounced light stay
// No rays. pA E, pB the across blur, pC colour, pD the RR diffuse albedo guide (all UAVs); pE the profile (SRV, rtcore.cpp
// PathScatterTable: 1 / 2 var per Gaussian, their weights in red, green, blue, the taps' offsets, each Gaussian's share of
// each tap); pF metres per profile millimetre, pG metres per pixel per metre of depth (asfloat); pH width | height << 16.
#include "rtc_common.hlsli"

#define SCATTER_TAPS 21
static const uint kScatterInvTwoVar=0,kScatterWeights=6,kScatterOffsets=24,kScatterShares=45;

float ScatterTable(uint i) { return asfloat(gBuf[pE].Load(4*i)); }

float4 ScatterAlong(uint source,uint2 pixel,float2 dir) {
    float4 c=gRwTex[source][pixel];
    if(!(c.w>0))return 0;
    int width=(int)(pH&0xffffu),height=(int)(pH>>16);
    float unit=asfloat(pF),pxPerUnit=unit/(c.w*asfloat(pG));
    float invTwoVar[6];float3 weight[6];
    [unroll] for(uint k=0;k<6;k++) {
        invTwoVar[k]=ScatterTable(kScatterInvTwoVar+k);
        weight[k]=float3(ScatterTable(kScatterWeights+k),ScatterTable(kScatterWeights+6+k),ScatterTable(kScatterWeights+12+k));
    }
    float2 at=float2(pixel)+.5;
    float3 acc=0,ws=0;
    [unroll] for(uint i=0;i<SCATTER_TAPS;i++) {
        int2 q=(int2)floor(at+dir*(ScatterTable(kScatterOffsets+i)*pxPerUnit));
        if(q.x<0 || q.y<0 || q.x>=width || q.y>=height)continue;
        float4 s=gRwTex[source][q];
        if(!(s.w>0))continue;
        float dz=(s.w-c.w)/unit,dz2=dz*dz;
        float3 w=0;
        [unroll] for(uint k=0;k<6;k++)w+=weight[k]*(ScatterTable(kScatterShares+6*i+k)*exp(-dz2*invTwoVar[k]));
        acc+=w*s.xyz;ws+=w;
    }
    return float4(acc/max(ws,1e-12),c.w);
}

[RootSignature(RTC_ROOT)] [numthreads(8,8,1)] void PathScatterX(uint3 tid:SV_DispatchThreadID) {
    if(tid.x>=(pH&0xffffu) || tid.y>=(pH>>16))return;
    gRwTex[pB][tid.xy]=ScatterAlong(pA,tid.xy,float2(1,0));
}

[RootSignature(RTC_ROOT)] [numthreads(8,8,1)] void PathScatterY(uint3 tid:SV_DispatchThreadID) {
    if(tid.x>=(pH&0xffffu) || tid.y>=(pH>>16))return;
    float4 e=gRwTex[pA][tid.xy];
    if(!(e.w>0))return;
    float4 b=ScatterAlong(pB,tid.xy,float2(0,1));
    float4 c=gRwTex[pC][tid.xy];
    c.rgb=clamp(c.rgb+max(gRwTex[pD][tid.xy].rgb,1e-3)*(b.rgb-e.rgb),0,65000);
    gRwTex[pC][tid.xy]=c;
}
