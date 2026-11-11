// Surface BSSRDF sampling following the three projection axes in PBRT 11.4.
// The six Gaussian profile is the measured skin fit already used by sss.cpp
// (d'Eon/Luebke, GPU Gems 3 ch.14). Width is in metres, not pixels.
// Only diffuse transport moves between surface points; specular reflection
// and RR's primary material guides remain at the original camera hit.
static const float PtSkinVariance[6]={.0064,.0484,.187,.567,1.99,7.41};
static const float3 PtSkinWeight[6]={float3(.233,.455,.649),float3(.100,.336,.344),
    float3(.118,.198,0),float3(.113,.007,.007),float3(.358,.004,0),float3(.078,0,0)};
float3 PtSkinProfile(float r2,float width) {
    float3 value=0;
    [unroll] for(uint i=0;i<6;i++) {
        float v=PtSkinVariance[i]*width*width;
        value+=PtSkinWeight[i]*exp(-r2/(2*v))/(2*PT_PI*v);
    }
    return value;
}
bool PtSampleSkin(PtSurface s,Hit original,float3 view,float cone,inout uint seed,
    out PtSurface exitSurface,out float3 weight) {
    exitSurface=s;weight=s.base;
    if(Rand(seed)<s.sssStrength) {
        float3 tx,ty;Basis(s.ng,tx,ty);
        float axisU=Rand(seed);float3 axis=axisU<.5?s.ng:axisU<.75?tx:ty;
        float3 dx=axisU<.5?tx:axisU<.75?ty:s.ng,dy=cross(axis,dx);
        uint channel=min((uint)(Rand(seed)*3),2),component=5;float choose=Rand(seed);
        [unroll] for(uint k=0;k<5;k++) {
            if(choose<PtSkinWeight[k][channel]){component=k;break;}choose-=PtSkinWeight[k][channel];
        }
        float r=s.sssWidth*sqrt(-2*PtSkinVariance[component]*log(max(1-Rand(seed),1e-7)));
        // Drop at most 1e-6 of the broadest Gaussian, rather than an unbounded
        // chord through other characters or rooms. Same body instance only.
        float radius=s.sssWidth*sqrt(2*7.41*13.81551056);
        if(r>=radius)return false;
        float phi=2*PT_PI*Rand(seed),height=sqrt(radius*radius-r*r);
        RayDesc ray;ray.Origin=s.p+(dx*cos(phi)+dy*sin(phi))*r+axis*height;
        ray.Direction=-axis;ray.TMin=0;ray.TMax=2*height;
        RayQuery<RAY_FLAG_FORCE_NON_OPAQUE|RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        q.TraceRayInline(gScene,RAY_FLAG_NONE,3,ray);
        Hit selected=(Hit)0;uint count=0;
        while(q.Proceed()) {
            if(q.CandidateInstanceID()!=original.inst)continue;
            uint geo=q.CandidateGeometryIndex();
            PtMaterial m=gBuf[pB].Load<PtMaterial>(LoadSub(LoadGeo(original.inst).firstSub+geo).material*PT_MATERIAL_BYTES);
            // Exit only through scattering skin. The wet tongue carries the skin
            // flag for its lobes alone; light does not diffuse to it across air.
            if(!(asuint(m.params.w)&8) || m.scattering.y<=0)continue;
            count++;
            if(Rand(seed)*count<1) {
                selected.inst=original.inst;selected.geo=geo;selected.prim=q.CandidatePrimitiveIndex();
                selected.t=q.CandidateTriangleRayT();selected.bary=q.CandidateTriangleBarycentrics();
            }
        }
        if(!count)return false;
        exitSurface=PtShading(selected,ray.Origin,ray.Direction,cone);
        if(!exitSurface.frontFace){exitSurface.ng=-exitSurface.ng;exitSurface.n=-exitSurface.n;exitSurface.dn=-exitSurface.dn;exitSurface.b=-exitSurface.b;}
        float3 delta=exitSurface.p-s.p;
        float3 distances=float3(dot(delta,tx),dot(delta,ty),dot(delta,s.ng));
        float3 projectedR2=float3(dot(distances.yz,distances.yz),dot(distances.xz,distances.xz),dot(distances.xy,distances.xy));
        float3 normals=abs(float3(dot(exitSurface.ng,tx),dot(exitSurface.ng,ty),dot(exitSurface.ng,s.ng)));
        float pdf=0;
        [unroll] for(uint a=0;a<3;a++)pdf+=dot(PtSkinProfile(projectedR2[a],s.sssWidth),1.0/3)*normals[a]*(a==2?.5:.25);
        if(pdf<=0)return false;
        // Split texture modulation between entry and exit, preserving the
        // epidermal detail while allowing colour transport through the tissue.
        weight=sqrt(max(s.base*exitSurface.base,0))*PtSkinProfile(dot(delta,delta),s.sssWidth)*(count/pdf);
    }
    weight*=1-PtFresnel(s.f0,saturate(dot(s.n,view)));
    exitSurface.n=exitSurface.dn;Basis(exitSurface.n,exitSurface.t,exitSurface.b);
    exitSurface.flags=128;exitSurface.base=1;exitSurface.sheen=exitSurface.grazing=0;
    exitSurface.transmission=0;exitSurface.sssStrength=0;
    return all(isfinite(weight));
}
