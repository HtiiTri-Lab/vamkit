cbuffer Control:register(b0){uint nodeCount,receiverCount,level,baseVertex;float radius;uint spanA,spanB;float bindSpan;};
StructuredBuffer<float3> vertices:register(t0);
StructuredBuffer<uint4> nodes:register(t1);
StructuredBuffer<uint4> triangles:register(t2);
StructuredBuffer<uint4> receivers:register(t3);
Buffer<uint> adjacency:register(t4);
globallycoherent RWStructuredBuffer<float4> bounds:register(u0);
StructuredBuffer<float4> boundData:register(t6);
RWBuffer<float> visibility:register(u1);
globallycoherent RWBuffer<uint> arrived:register(u3);
struct Triangle { float4 a,e1,e2; };
StructuredBuffer<Triangle> triangleData:register(t5);
RWStructuredBuffer<Triangle> triangleOutput:register(u2);
float3 P(uint i){return vertices[i+baseVertex];}
[numthreads(64,1,1)]
void Prepare(uint id:SV_DispatchThreadID){
    uint count,stride;triangles.GetDimensions(count,stride);if(id>=count)return;
    uint3 t=triangles[id].xyz;float3 a=P(t.x);Triangle data;
    data.a=float4(a,0);data.e1=float4(P(t.y)-a,0);data.e2=float4(P(t.z)-a,0);triangleOutput[id]=data;
}
[numthreads(64,1,1)]
void Refit(uint id:SV_DispatchThreadID){
    if(id>=nodeCount)return;uint4 n=nodes[id];if(!(n.y&0x80000000u))return;
    float3 lo=1e20,hi=-1e20;
    if(n.y&0x80000000u){for(uint i=0;i<(n.y&0x7fffffffu);i++){uint3 t=triangles[n.x+i].xyz;float3 a=P(t.x),b=P(t.y),c=P(t.z);lo=min(lo,min(a,min(b,c)));hi=max(hi,max(a,max(b,c)));}}
    else{lo=min(bounds[n.x*2].xyz,bounds[n.y*2].xyz);hi=max(bounds[n.x*2+1].xyz,bounds[n.y*2+1].xyz);}
    uint tag=n.z|((n.y&0x80000000u)?(0x80000000u|((n.y&0x7fu)<<24)):0);
    bounds[id*2]=float4(lo,asfloat(n.x));bounds[id*2+1]=float4(hi,asfloat(tag));
    [loop]while(n.w!=0xffffffffu){
        // Publish both child boxes before claiming the parent. Globally coherent
        // UAV barriers cover children completed by other thread groups.
        DeviceMemoryBarrier();
        uint parent=n.w,previous;InterlockedAdd(arrived[parent],1,previous);
        if(previous==0)return;
        DeviceMemoryBarrier();
        id=parent;n=nodes[id];
        lo=min(bounds[n.x*2].xyz,bounds[n.y*2].xyz);
        hi=max(bounds[n.x*2+1].xyz,bounds[n.y*2+1].xyz);
        bounds[id*2]=float4(lo,asfloat(n.x));bounds[id*2+1]=float4(hi,asfloat(n.z));
    }
}
bool Box(float3 o,float3 inv,uint id,float limit){
    float3 a=(boundData[2*id].xyz-o)*inv,b=(boundData[2*id+1].xyz-o)*inv;
    float3 mn=min(a,b),mx=max(a,b);return max(0,max(mn.x,max(mn.y,mn.z)))<=min(limit,min(mx.x,min(mx.y,mx.z)));
}
float RayFallback(float3 o,float3 d,float reach){
    float best=reach;float3 inv=1.0/(sign(d)*max(abs(d),1e-8));uint id=0;
    [loop]while(id<nodeCount){
        float4 lo=boundData[2*id],hi=boundData[2*id+1];uint tag=asuint(hi.w),skip=tag&0x00ffffffu;
        float3 v0=(lo.xyz-o)*inv,v1=(hi.xyz-o)*inv,mn=min(v0,v1),mx=max(v0,v1);
        if(max(0,max(mn.x,max(mn.y,mn.z)))>min(best,min(mx.x,min(mx.y,mx.z)))){id=skip;continue;}
        if(tag&0x80000000u){for(uint k=0;k<((tag>>24)&0x7fu);k++){
            Triangle t=triangleData[asuint(lo.w)+k];float3 a=t.a.xyz,e1=t.e1.xyz,e2=t.e2.xyz,h=cross(d,e2);float det=dot(e1,h);
            if(abs(det)<1e-12)continue;float f=1/det;float3 q=o-a;float u=f*dot(q,h);if(u<0||u>1)continue;
            float3 r=cross(q,e1);float v=f*dot(d,r);if(v<0||u+v>1)continue;float dist=f*dot(e2,r);if(dist>.00012&&dist<best)best=dist;
        }id=skip;}else id++;
    }return best;
}
float BoxNear(float3 o,float3 inv,uint id,float best){
    float3 a=(boundData[2*id].xyz-o)*inv,b=(boundData[2*id+1].xyz-o)*inv;
    float3 mn=min(a,b),mx=max(a,b);float near=max(0,max(mn.x,max(mn.y,mn.z)));
    return near<=min(best,min(mx.x,min(mx.y,mx.z)))?near:1e30;
}
float Ray(float3 o,float3 d,float reach){
    float best=reach;float3 inv=1.0/(sign(d)*max(abs(d),1e-8));uint id=0;
    uint stack[32];float nearStack[32];uint top=0;float currentNear=BoxNear(o,inv,0,best);
    [loop]for(;;){
        float4 lo=boundData[2*id],hi=boundData[2*id+1];uint tag=asuint(hi.w);
        if(currentNear<=best){
            if(tag&0x80000000u){
                for(uint k=0;k<((tag>>24)&0x7fu);k++){
                    Triangle t=triangleData[asuint(lo.w)+k];float3 a=t.a.xyz,e1=t.e1.xyz,e2=t.e2.xyz,h=cross(d,e2);float det=dot(e1,h);
                    if(abs(det)<1e-12)continue;float f=1/det;float3 q=o-a;float u=f*dot(q,h);if(u<0||u>1)continue;
                    float3 r=cross(q,e1);float v=f*dot(d,r);if(v<0||u+v>1)continue;float dist=f*dot(e2,r);if(dist>.00012&&dist<best)best=dist;
                }
            }else{
                uint left=id+1,right=asuint(boundData[2*left+1].w)&0x00ffffffu;
                float a=BoxNear(o,inv,left,best),b=BoxNear(o,inv,right,best);
                if(a<=best||b<=best){
                    if(a<=best&&b<=best){
                        if(top==32)return RayFallback(o,d,reach);
                        stack[top]=a<b?right:left;nearStack[top++]=max(a,b);
                    }
                    id=a<b?left:right;currentNear=min(a,b);continue;
                }
            }
        }
        if(top==0)break;id=stack[--top];currentNear=nearStack[top];
    }
    return best;
}
// One group per receiver. All 96 original directions are evaluated concurrently.
// Keep the original ordered sum: this changes scheduling, not the AO estimator.
groupshared float rayVisibility[96];
groupshared float3 sharedP,sharedN,sharedTx,sharedTy;
groupshared float sharedScale,sharedReach;
groupshared uint validNormal;
[numthreads(96,1,1)]
void Trace(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    uint id=group.x;
    if(id>=receiverCount)return;
    if(lane==0){
    uint4 r=receivers[id];float3 p=P(r.x),n=0;
    for(uint k=0;k<r.z;k++){uint3 t=triangles[adjacency[r.y+k]].xyz;n+=cross(P(t.y)-P(t.x),P(t.z)-P(t.x));}
    validNormal=dot(n,n)>=1e-20;
    n=validNormal?normalize(n)*(r.w!=0?-1:1):float3(0,0,1);
    float3 tx=normalize(cross(abs(n.z)<.9?float3(0,0,1):float3(1,0,0),n)),ty=cross(n,tx);
    float scale=bindSpan>1e-6?length(P(spanA)-P(spanB))/bindSpan:1;
    float reach=radius*scale;
    sharedP=p;sharedN=n;sharedTx=tx;sharedTy=ty;sharedScale=scale;sharedReach=reach;
    }
    GroupMemoryBarrierWithGroupSync();
    if(!validNormal){if(lane==0)visibility[id]=1;return;}
    float3 p=sharedP,n=sharedN,tx=sharedTx,ty=sharedTy;
    float scale=sharedScale,reach=sharedReach;
    const uint samples=96;
    static const uint rayOrder[96]={0,55,21,76,42,8,63,29,84,50,16,71,37,92,3,58,24,79,45,11,66,32,87,53,19,74,40,95,6,61,27,82,48,14,69,35,90,1,56,22,77,43,9,64,30,85,51,17,72,38,93,4,59,25,80,46,12,67,33,88,54,20,75,41,7,62,28,83,49,15,70,36,91,2,57,23,78,44,10,65,31,86,52,18,73,39,94,5,60,26,81,47,13,68,34,89};
    uint i=rayOrder[lane];
        float u=(i+.5)/samples,phi=i*2.39996323;float3 d=tx*(sqrt(u)*cos(phi))+ty*(sqrt(u)*sin(phi))+n*sqrt(1-u);
        float t=Ray(p+n*.00008*scale,d,reach)/reach;rayVisibility[i]=t*t*(3-2*t);
    GroupMemoryBarrierWithGroupSync();
    if(lane==0){
        float sum=0;
        [loop]for(uint j=0;j<samples;j++)sum+=rayVisibility[j];
        visibility[id]=sum/samples;
    }
}
