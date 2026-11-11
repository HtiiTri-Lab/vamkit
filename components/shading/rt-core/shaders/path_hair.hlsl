#include "rtc_common.hlsli"
// One pass converts the already-expanded strands to LSS vertices and an implicit
// segment index. No CPU vertex traversal and no camera-facing triangle tubes.
[RootSignature(RTC_ROOT)] [numthreads(256,1,1)] void PathHairPrepare(uint3 tid:SV_DispatchThreadID) {
    uint local=tid.x+pF;if(local>=pCount)return;
    uint vertex=pReceiver+local,along=local%(pSeed+1);
    uint address=(vertex>>5)*(32*36)+(vertex&31)*12;
    float3 pos=asfloat(gBuf[pA].Load3(address));
    float radius=asfloat(pLight)*(1-saturate(float(along)/pSeed-.9)*10);
    float3 previous=pE!=0?asfloat(gRwBuf[pB].Load3(vertex*48)):pos;
    gRwBuf[pB].Store4(vertex*48,asuint(float4(pos,max(radius,0))));
    gRwBuf[pB].Store3(vertex*48+32,asuint(previous));
    uint2 packed=gBuf[pA].Load2(address+64*12+4);
    float4 color=float4(f16tof32(packed.x),f16tof32(packed.x>>16),f16tof32(packed.y),f16tof32(packed.y>>16));
    gRwBuf[pB].Store4(vertex*48+16,asuint(color));
    if(along<pSeed)gRwBuf[pC].Store((pD+local/(pSeed+1)*pSeed+along)*4,local);
}
