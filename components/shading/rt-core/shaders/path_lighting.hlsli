// Next-event estimation and power-heuristic MIS. Distributions are built on
// the GPU once per scene frame and shared by both eyes; no triangle readback.
static const uint PT_ENV_W=64,PT_ENV_H=32,PT_ENV_N=2048;
#ifndef PT_RAYGEN
groupshared float PtPrefix[256];
float PtGroupPrefix(uint lane,float value) {
    PtPrefix[lane]=value;GroupMemoryBarrierWithGroupSync();
    [unroll] for(uint step=1;step<256;step*=2) {
        float before=lane>=step?PtPrefix[lane-step]:0;
        GroupMemoryBarrierWithGroupSync();PtPrefix[lane]+=before;
        GroupMemoryBarrierWithGroupSync();
    }
    return lane?PtPrefix[lane-1]:0;
}
#endif
float3 PtEnvDirection(uint cell,float2 u) {
    uint x=cell%PT_ENV_W,y=cell/PT_ENV_W;
    float c=lerp(cos(PT_PI*y/PT_ENV_H),cos(PT_PI*(y+1)/PT_ENV_H),u.y);
    float phi=2*PT_PI*(x+u.x)/PT_ENV_W,r=sqrt(saturate(1-c*c));
    return float3(r*cos(phi),c,r*sin(phi));
}
float PtEnvSolidAngle(uint cell) {
    uint y=cell/PT_ENV_W;
    return (2*PT_PI/PT_ENV_W)*(cos(PT_PI*y/PT_ENV_H)-cos(PT_PI*(y+1)/PT_ENV_H));
}
#ifndef PT_RAYGEN
[RootSignature(RTC_ROOT)] [numthreads(256,1,1)] void PathEnvironmentCdf(uint lane:SV_GroupIndex) {
    float sum=0;
    [unroll] for(uint k=0;k<8;k++) {
        uint cell=lane*8+k;
        sum+=max(PtLuma(PtEnvironment(PtEnvDirection(cell,.5),false)),0)*PtEnvSolidAngle(cell);
        gRwBuf[pC].Store(cell*4,asuint(sum));
    }
    float before=PtGroupPrefix(lane,sum);
    [unroll] for(uint j=0;j<8;j++) {
        uint at=(lane*8+j)*4;
        gRwBuf[pC].Store(at,asuint(asfloat(gRwBuf[pC].Load(at))+before));
    }
}
#endif
uint PtCdfSample(uint srv,uint first,uint count,float value) {
    uint lo=0,hi=count-1;
    [loop] while(lo<hi) {
        uint mid=(lo+hi)/2;
        if(asfloat(gBuf[srv].Load((first+mid)*4))<=value)lo=mid+1;else hi=mid;
    }
    return lo;
}
float PtCdfWeight(uint srv,uint first,uint index) {
    float hi=asfloat(gBuf[srv].Load((first+index)*4));
    float lo=index?asfloat(gBuf[srv].Load((first+index-1)*4)):0;
    return max(hi-lo,0);
}
float PtEnvPdf(float3 d) {
    uint srv=gBuf[pA].Load(364);
    float total=asfloat(gBuf[srv].Load((PT_ENV_N-1)*4));
    if(total<=0)return 1/(4*PT_PI);
    float phi=atan2(d.z,d.x);if(phi<0)phi+=2*PT_PI;
    uint x=min((uint)(phi*(PT_ENV_W/(2*PT_PI))),PT_ENV_W-1);
    uint y=min((uint)(acos(clamp(d.y,-1,1))*(PT_ENV_H/PT_PI)),PT_ENV_H-1),cell=y*PT_ENV_W+x;
    return .95*PtCdfWeight(srv,0,cell)/(total*PtEnvSolidAngle(cell))+.05/(4*PT_PI);
}
// A light sample's contribution over its pdf, MIS-weighted against BSDF sampling for the lobes a continuation
// samples: with no diffuse bounce left that is the specular branch alone, so the diffuse share keeps weight 1.
float3 PtMisLobes(PtSurface s,float3 value,float3 diffuse,float lightPdf,float bsdfPdf) {
    float w=PtMis(lightPdf,bsdfPdf)/lightPdf;
    return s.diffuseStop?diffuse/lightPdf+(value-diffuse)*w:value*w;
}
void PtEnvironmentCandidate(PtSurface s,float3 v,float continuation,inout uint seed,inout PtDirectReservoir reservoir) {
    uint srv=gBuf[pA].Load(364);
    float total=asfloat(gBuf[srv].Load((PT_ENV_N-1)*4));float3 d;
    // Uniform mixture preserves support for bright texels smaller than a cell.
    if(total>0 && Rand(seed)<.95) {
        uint cell=PtCdfSample(srv,0,PT_ENV_N,min(Rand(seed)*total,total*(1-1e-7)));
        d=PtEnvDirection(cell,float2(Rand(seed),Rand(seed)));
    } else {
        float c=1-2*Rand(seed),phi=2*PT_PI*Rand(seed),r=sqrt(saturate(1-c*c));d=float3(r*cos(phi),c,r*sin(phi));
    }
    float pdf=PtEnvPdf(d);float3 diffuse;float3 radiance=PtEnvironmentLobes(d,false,s.unityAmbient,diffuse);if(PtMax(radiance+diffuse)<=0)return;
    float bsdf;float3 diffusePart;float3 fc=PtDirectEvalLobes(s,v,d,diffuse,radiance,bsdf,reservoir,diffusePart);if(PtMax(fc)<=0)return;
    gVisSlot=9;
    // API 26: Unity draws the diffuse environment unoccluded. This share is seen past a short range (the shape's own
    // occlusion stays), drawn so that no edge marks it, its value whole (no continuation adds to it: PtTracePixel
    // takes the same share off an escaping diffuse ray); reflections and the rest stay traced to the sky.
    float share=PtAuthorAmbientShare(s.diffuseDepth);
    if(share>0 && PtMax(diffusePart)>0) {
#ifdef PT_RESPONSE
        reservoir.responseSpec=0;
#endif
        PtDirectCandidate(reservoir,diffusePart*share/pdf,PtOffset(s,d),d,PtAuthorAmbientRange()*(.5+.5*Rand(seed)),true,false,0x01u,seed);
        fc-=diffusePart*share;diffusePart*=1-share;
#ifdef PT_RESPONSE
        reservoir.responseSpec=saturate((fc-diffusePart)/max(fc,1e-8));
#endif
    }
    PtDirectCandidate(reservoir,PtMisLobes(s,fc,diffusePart,pdf,bsdf*continuation),PtOffset(s,d),d,10000,true,false,0x01u,seed);
}
// A baked local cubemap contains the room surfaces' incoming radiance, not an
// infinitely distant sky. Sample it for diffuse response before the first
// diffuse bounce; only nearby geometry may shadow this approximate local IBL.
// Real sky and analytic lights retain their normal path-traced visibility.
void PtIndoorCandidate(PtSurface s,float3 v,inout uint seed,inout PtDirectReservoir reservoir) {
    uint cube=gBuf[pA].Load(188);float scale=asfloat(gBuf[pA].Load(252));
    if(cube==RTC_NONE || scale<=0 || s.diffuseDepth!=0)return;
    float2 u=float2(Rand(seed),Rand(seed));float phi=2*PT_PI*u.y;
    float3 d;float proposal;
    if(s.flags&64) {
        float c=1-2*u.x,r=sqrt(saturate(1-c*c));d=float3(r*cos(phi),c,r*sin(phi));proposal=1/(4*PT_PI);
    } else {
        float r=sqrt(u.x);float3 t,b;Basis(s.dn,t,b);
        d=normalize(t*(r*cos(phi))+b*(r*sin(phi))+s.dn*sqrt(1-u.x));proposal=saturate(dot(s.dn,d))/PT_PI;
    }
    if(proposal<=1e-8)return;
    float pdf;float3 diffuse;float3 value=PtEvalComponents(s,v,d,pdf,diffuse);
    if(s.flags&64)diffuse=value; // hair's full scattering lobe responds to the local incident field
    float3 incoming=max(gCube[NonUniformResourceIndex(cube)].SampleLevel(gLinear,d,5).rgb,0)*scale;
#ifdef PT_RESPONSE
    reservoir.responseSpec=(s.flags&64)?1:0;
#endif
    gVisSlot=10;PtDirectCandidate(reservoir,diffuse*incoming/proposal,PtOffset(s,d),d,.35,true,false,0x01u,seed);
}
void PtEmitterTriangle(uint inst,uint geo,uint prim,out float3 a,out float3 b,out float3 c) {
    GeoGpu g=LoadGeo(inst);SubGpu sub=LoadSub(g.firstSub+geo);
    uint n=sub.blasTess?sub.tessLevel*sub.tessLevel:1;
    uint3 ids=LoadTriangle(g,sub.triStart+prim/n);
    if(sub.blasTess) {
        float3 p0=LocalPosition(g,ids.x),p1=LocalPosition(g,ids.y),p2=LocalPosition(g,ids.z);
        float3 n0=LocalNormal(g,ids.x),n1=LocalNormal(g,ids.y),n2=LocalNormal(g,ids.z);
        float3 u,v,w;SubTriangle(sub.tessLevel,prim%n,u,v,w);
        a=ToWorld(g,PhongPoint(u,p0,p1,p2,n0,n1,n2,sub.tessAlpha));
        b=ToWorld(g,PhongPoint(v,p0,p1,p2,n0,n1,n2,sub.tessAlpha));
        c=ToWorld(g,PhongPoint(w,p0,p1,p2,n0,n1,n2,sub.tessAlpha));
    } else {a=GeoPosition(g,ids.x);b=GeoPosition(g,ids.y);c=GeoPosition(g,ids.z);}
}
float3 PtEmitterRadiance(Hit hit,bool coverage) {
    GeoGpu g=LoadGeo(hit.inst);SubGpu sub=LoadSub(g.firstSub+hit.geo);
    PtMaterial m=gBuf[pB].Load<PtMaterial>(sub.material*PT_MATERIAL_BYTES);
    uint tri;float3 bary;HitTriangle(sub,hit.prim,hit.bary,tri,bary);
    uint3 ids=LoadTriangle(g,tri);float2 uv=0;
    if(g.uvBuf!=RTC_NONE)uv=LoadFloat2(g.uvBuf,g.uvOffset,g.uvStride,g.uvFormat,ids.x)*bary.x+
        LoadFloat2(g.uvBuf,g.uvOffset,g.uvStride,g.uvFormat,ids.y)*bary.y+
        LoadFloat2(g.uvBuf,g.uvOffset,g.uvStride,g.uvFormat,ids.z)*bary.z;
    float4 glow;
    if(m.second.y&PT_MODE_HZM_WORLD_ALBEDO) {
        // Match the same world position and interpolated world normal used by
        // PtShading, including the inverse transpose for scaled instances.
        float3 a,b,c;PtEmitterTriangle(hit.inst,hit.geo,hit.prim,a,b,c);
        float3 p=a*(1-hit.bary.x-hit.bary.y)+b*hit.bary.x+c*hit.bary.y;
        float detM=dot(g.m0.xyz,cross(g.m1.xyz,g.m2.xyz));
        float3 n=PtSafeNormal(cross(b-a,c-a)*(detM<0?-1:1),float3(0,1,0));
        if(g.nrmBuf!=RTC_NONE) {
            float3 local=LocalNormal(g,ids.x)*bary.x+LocalNormal(g,ids.y)*bary.y+LocalNormal(g,ids.z)*bary.z;
            float3 world=float3(dot(cross(g.m1.xyz,g.m2.xyz),local),dot(cross(g.m2.xyz,g.m0.xyz),local),dot(cross(g.m0.xyz,g.m1.xyz),local));
            n=PtSafeNormal(world*(detM<0?-1:1),n);
        }
        glow=PtHzmMap(m,3,p,n,0,m.directGain.w,0);
    } else glow=PtMap(m,3,uv,0,m.emissionST,1);
    if(asuint(m.params.w)&131072)glow.rgb=saturate(glow.rgb+m.materialControls.x);
    float3 value=max(m.emission.rgb*((asuint(m.params.w)&4096)?glow.aaa:glow.rgb),0);
    if(coverage) {
        MatGpu old=LoadMat(sub.material);float alpha=PtAlphaAt(hit.inst,hit.geo,hit.prim,hit.bary,m);
        uint flags=asuint(m.params.w);
        if((old.kind==1 || (flags&8388608)) && PtOpacity(alpha,old,m)<(old.kind==1?old.color.a:m.materialControls.w))return 0;
        if((old.kind==3 && m.params.z<.999) || (flags&4194304))value*=PtOpacity(alpha,old,m);
    }
    return value;
}
#ifndef PT_RAYGEN
[RootSignature(RTC_ROOT)] [numthreads(256,1,1)] void PathEmitterCdf(uint lane:SV_GroupIndex,uint3 group:SV_GroupID) {
    uint4 lighting=gBuf[pA].Load4(368),range=gBuf[lighting.x].Load4(group.x*16);
    uint chunk=(range.w+255)/256,first=min(lane*chunk,range.w),last=min(first+chunk,range.w);
    float sum=0;
    [loop] for(uint prim=first;prim<last;prim++) {
        float3 a,b,c;PtEmitterTriangle(range.x,range.y,prim,a,b,c);
        Hit hit=(Hit)0;hit.inst=range.x;hit.geo=range.y;hit.prim=prim;hit.bary=1.0/3;
        uint mat=LoadSub(LoadGeo(range.x).firstSub+range.y).material;
        PtMaterial m=gBuf[pB].Load<PtMaterial>(mat*PT_MATERIAL_BYTES);
        // A positive floor retains support for small emissive texels missed by
        // the centroid estimate. It changes the proposal, never the radiance.
        float power=max(PtLuma(PtEmitterRadiance(hit,false)),max(PtLuma(m.emission.rgb),0)*.001);
        sum+=length(cross(b-a,c-a))*.5*power;
        gRwBuf[pC].Store((range.z+prim)*4,asuint(sum));
    }
    float before=PtGroupPrefix(lane,sum);
    [loop] for(uint j=first;j<last;j++) {
        uint at=(range.z+j)*4;gRwBuf[pC].Store(at,asuint(asfloat(gRwBuf[pC].Load(at))+before));
    }
    if(lane==255)gRwBuf[pC].Store((lighting.w+group.x)*4,asuint(PtPrefix[255]));
}
[RootSignature(RTC_ROOT)] [numthreads(256,1,1)] void PathEmitterRanges(uint lane:SV_GroupIndex) {
    uint4 lighting=gBuf[pA].Load4(368);
    uint chunk=(lighting.y+255)/256,first=min(lane*chunk,lighting.y),last=min(first+chunk,lighting.y);
    float sum=0;
    [loop] for(uint k=first;k<last;k++) {uint at=(lighting.w+k)*4;sum+=asfloat(gRwBuf[pC].Load(at));gRwBuf[pC].Store(at,asuint(sum));}
    float before=PtGroupPrefix(lane,sum);
    [loop] for(uint j=first;j<last;j++) {uint at=(lighting.w+j)*4;gRwBuf[pC].Store(at,asuint(asfloat(gRwBuf[pC].Load(at))+before));}
}
#endif
float PtEmitterPdf(Hit hit,float3 from,float3 hitPoint) {
    uint4 lighting=gBuf[pA].Load4(368);if(!lighting.y||hit.inst==RTC_NONE)return 0;
    uint sub=LoadGeo(hit.inst).firstSub+hit.geo;
    uint index=gBuf[lighting.x].Load(lighting.y*16+sub*4);if(index==RTC_NONE)return 0;
    uint4 range=gBuf[lighting.x].Load4(index*16);
    float total=asfloat(gBuf[lighting.z].Load((lighting.w+lighting.y-1)*4));if(total<=0)return 0;
    float3 a,b,c;PtEmitterTriangle(hit.inst,hit.geo,hit.prim,a,b,c);
    float3 crossN=cross(b-a,c-a),d=hitPoint-from;float distance=length(d),area=length(crossN)*.5;
    if(area<=0||distance<=0)return 0;
    float cosine=abs(dot(crossN/(2*area),d/distance));if(cosine<=1e-8)return 0;
    return PtCdfWeight(lighting.z,range.z,hit.prim)*distance*distance/(total*area*cosine);
}
void PtEmitterCandidate(PtSurface s,float3 v,float continuation,inout uint seed,inout PtDirectReservoir reservoir) {
    uint4 lighting=gBuf[pA].Load4(368);if(!lighting.y)return;
    float total=asfloat(gBuf[lighting.z].Load((lighting.w+lighting.y-1)*4));if(total<=0)return;
    uint ri=PtCdfSample(lighting.z,lighting.w,lighting.y,min(Rand(seed)*total,total*(1-1e-7)));
    uint4 range=gBuf[lighting.x].Load4(ri*16);
    float rangeTotal=asfloat(gBuf[lighting.z].Load((range.z+range.w-1)*4));if(rangeTotal<=0)return;
    uint prim=PtCdfSample(lighting.z,range.z,range.w,min(Rand(seed)*rangeTotal,rangeTotal*(1-1e-7)));
    float r=sqrt(Rand(seed)),u=Rand(seed);float3 bary=float3(1-r,r*(1-u),r*u);
    float3 a,b,c;PtEmitterTriangle(range.x,range.y,prim,a,b,c);
    float3 hitPoint=a*bary.x+b*bary.y+c*bary.z,d=hitPoint-s.p;float distance=length(d);if(distance<=1e-5)return;
    d/=distance;
    Hit hit=(Hit)0;hit.inst=range.x;hit.geo=range.y;hit.prim=prim;hit.bary=bary.yz;
    float pdf=PtEmitterPdf(hit,s.p,hitPoint),bsdf;if(pdf<=0)return;
    float3 diffusePart;float3 fc=PtDirectEval(s,v,d,bsdf,reservoir,diffusePart);if(PtMax(fc)<=0)return;
    float3 origin=PtOffset(s,d),shadow=hitPoint-origin;float shadowLength=length(shadow);
    // End before the sampled triangle; the offset must not leave it in TMax.
    float3 emitted=PtEmitterRadiance(hit,true);
    float scale=PtAuthorEmitter(); // API 26: only the diffuse light it casts
    gVisSlot=11;PtDirectCandidate(reservoir,PtMisLobes(s,(fc-diffusePart*(1-scale))*emitted,diffusePart*scale*emitted,pdf,bsdf*continuation),origin,
        shadow/shadowLength,max(shadowLength-max(1e-4,shadowLength*1e-5),1e-6),true,false,0x01u,seed);
}
void PtGatherDirect(PtSurface s,float3 v,float continuation,inout uint seed,inout PtDirectReservoir r) {
    PtAnalyticCandidate(s,v,PtLidLight(s,seed),seed,r); // an eyeball's diffuse under its lids (bit 25)
    PtEnvironmentCandidate(s,v,continuation,seed,r);PtIndoorCandidate(s,v,seed,r);PtEmitterCandidate(s,v,continuation,seed,r);
}
float3 PtResolveDirect(PtDirectReservoir r,out float3 specular) {
    float3 visibility=r.total>0?PtVisibility(r.origin,r.direction,r.distance,r.thin!=0,r.mask):0;
    gVisValid=r.total>0;gVisSeen=PtMax(visibility)>0;gVisLast=r.slot;
    specular=0;
#ifdef PT_RESPONSE
    specular=r.specUnshadowed+r.specNormalized*r.total*visibility;
#endif
    return r.unshadowed+r.normalized*r.total*visibility;
}
float3 PtAllDirect(PtSurface s,float3 v,float continuation,inout uint seed,out float3 specular) {
    PtDirectReservoir r=(PtDirectReservoir)0;r.transport=1;
    PtGatherDirect(s,v,continuation,seed,r);return PtResolveDirect(r,specular);
}
