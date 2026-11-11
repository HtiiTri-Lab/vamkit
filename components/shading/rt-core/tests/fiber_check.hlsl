#include "../shaders/path_trace.hlsl"
// Two fixed materials: lossless white fiber and absorbing brown fiber.
// Independent uniform-sphere integration checks the importance sampler's PDF.
[RootSignature(RTC_ROOT)] [numthreads(8,8,1)] void FiberCheck(uint3 tid:SV_DispatchThreadID) {
    uint which=tid.y/256,id=tid.x+256*(tid.y%256),seed=Hash(id+150347*which+1731);
    PtSurface s=(PtSurface)0;s.flags=64;s.hairReflection=1;s.t=float3(1,0,0);s.b=float3(0,1,0);s.n=float3(0,0,1);
    s.roughness=.3;s.hairShift=.034906585;s.hairH=.4;s.base=which?.35:1;
    float3 v=normalize(float3(.3,0,1)),d,weight;
    float sampledPdf;bool valid=PtFiberSample(s,v,seed,d,weight,sampledPdf);
    float z=1-2*Rand(seed),phi=2*PT_PI*Rand(seed),r=sqrt(saturate(1-z*z)),pdf;
    float3 value=PtFiberEval(s,v,float3(r*cos(phi),r*sin(phi),z),pdf);
    gRwTex[pC][tid.xy]=float4(valid?weight.x:asfloat(0x7fc00000),value.x*(4*PT_PI),pdf*(4*PT_PI),PtFiberAlbedo(s,v).x);
}
