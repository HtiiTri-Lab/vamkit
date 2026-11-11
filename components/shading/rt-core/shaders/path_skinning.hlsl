#include "rtc_common.hlsli"
[RootSignature(RTC_ROOT)] [numthreads(64,1,1)] void PathSkin(uint3 tid:SV_DispatchThreadID) {
    uint i=Linear(tid);if(i>=pCount)return;
    uint4 tangentData=gBuf[pD].Load4(0),formats=gBuf[pD].Load4(16);
    float3 local=LoadFloat3(pA,pReceiver,pF,formats.x,i);
    float3 normal=pB!=RTC_NONE?LoadFloat3(pB,pLight,pG,formats.y,i):float3(0,1,0);
    float4 tangent=tangentData.x!=RTC_NONE?LoadFloat4(tangentData.x,tangentData.y,tangentData.z,tangentData.w,i):float4(1,0,0,1);
    if(pSeed) {
        uint2 range=gBuf[pC].Load2(pCount*32+i*8);
        [loop]for(uint j=0;j<range.y;j++) {
            uint at=range.x+j*48,frame=gBuf[pC].Load(at);
            if(frame>=pSeed)continue;
            float weight=asfloat(gBuf[pD].Load(32+pH*48+frame*4));
            if(weight!=0) {
                local+=weight*asfloat(gBuf[pC].Load3(at+4));
                normal+=weight*asfloat(gBuf[pC].Load3(at+16));
                tangent.xyz+=weight*asfloat(gBuf[pC].Load3(at+32));
            }
        }
    }
    float4 weights=asfloat(gBuf[pC].Load4(i*32));uint4 bones=gBuf[pC].Load4(i*32+16);
    float total=dot(weights,1);
    if(total<=1e-8){weights=float4(1,0,0,0);bones=0;}else weights/=total;
    float4 rows[3];rows[0]=rows[1]=rows[2]=0;
    [unroll]for(uint k=0;k<4;k++) {
        uint at=32+min(bones[k],pH-1)*48;
        [unroll]for(uint r=0;r<3;r++)rows[r]+=asfloat(gBuf[pD].Load4(at+r*16))*weights[k];
    }
    float3 world=float3(dot(rows[0],float4(local,1)),dot(rows[1],float4(local,1)),dot(rows[2],float4(local,1)));
    float det=dot(rows[0].xyz,cross(rows[1].xyz,rows[2].xyz));
    float3 n=float3(dot(cross(rows[1].xyz,rows[2].xyz),normal),dot(cross(rows[2].xyz,rows[0].xyz),normal),dot(cross(rows[0].xyz,rows[1].xyz),normal));
    n=dot(n,n)>1e-16?normalize(n)*(det<0?-1:1):normal;
    float3 t=float3(dot(rows[0].xyz,tangent.xyz),dot(rows[1].xyz,tangent.xyz),dot(rows[2].xyz,tangent.xyz));
    t-=n*dot(t,n);t=dot(t,t)>1e-16?normalize(t):tangent.xyz;
    gRwBuf[pE].Store4(i*48,asuint(float4(world,1)));gRwBuf[pE].Store4(i*48+16,asuint(float4(n,0)));
    gRwBuf[pE].Store4(i*48+32,asuint(float4(t,tangent.w*(det<0?-1:1))));
}
