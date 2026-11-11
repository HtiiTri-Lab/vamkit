// Experimental full-scene integrator. Shared acceleration structures, separate
// screen-space dispatch. No atlas radiance, ambient occlusion or lighting history
// is read here. RR receives the unfiltered estimator and its own primary guides.
#include "rtc_common.hlsli"
#define NV_SHADER_EXTN_SLOT u0
#define NV_SHADER_EXTN_REGISTER_SPACE space8
#include "nvHLSLExtns.h"
// PT_RESPONSE: each light candidate carries its specular share, for the split (NRD) outputs and for the skin's screen-space
// scattering (PT_SSS, 11-10, docs\pt-skin-sss-ss-20261110), which moves the direct diffuse light alone.
#if defined(PT_SPLIT) || defined(PT_SSS)
#define PT_RESPONSE 1
#endif

static const float PT_PI=3.14159265359;
static const uint PT_MATERIAL_BYTES=448;
// PtMaterial.second.y: how the source program uses the second colour layer and coverage.
static const uint PT_MODE_DECAL=1,PT_MODE_DETAIL_ALBEDO=2,PT_MODE_COVERAGE_CLOSURE=4,PT_MODE_PREMULTIPLIED=8,
    PT_MODE_ADDITIVE=16,PT_MODE_ALBEDO_SMOOTHNESS=64,PT_MODE_REFLECTION_MASK=128,
    PT_MODE_PERSON=256,PT_MODE_HZM_WORLD_ALBEDO=512,PT_MODE_HZM_PBR=1024,PT_MODE_PANORAMA=2048,
    PT_MODE_PANORAMA_180=4096,PT_MODE_PANORAMA_MIRROR=8192,PT_MODE_PANORAMA_SBS=16384,PT_MODE_PANORAMA_OU=32768;
// Surface-only flag: authored diffuse is not partitioned by the authored specular colour.
static const uint PT_SURFACE_ADDITIVE=536870912;
// Material flag (0.23.4): the gloss slot's red channel is the perceptual roughness (Unity's Standard Roughness setup).
static const uint PT_ROUGHNESS_MAP=2147483648u;
struct PtMaterial {
    uint4 textures;
    uint4 extra;
    float4 base;
    float4 spec; // F0, perceptual roughness
    float4 emission; // emissive RGB, metallic
    float4 uv;
    float4 params; // normal scale, IOR, transmission, flags (uint bits)
    float4 gloss;
    float4 skinCtl,skinLobe,tierA,tierB,tierC,scattering;
    float4 skinFilter; // spec normal mip bias, pore variance gain, diffuse/cavity widths (m)
    float4 normalST,surfaceST,emissionST,glossST,detailST,bump;
    float4 alphaST;
    float4 materialControls;
    uint4 sampling0,sampling1;
    uint4 second; // decal/detail-albedo texture, PT_MODE bits, its sampling, reserved
    float4 secondST;
    float4 directGain; // analytic-light diffuse gain (the family's x2(1-S/2)), mode parameter
};
struct PtSurface {
    float3 p,n,dn,ng,t,b,base,f0,emission;
    float3 directExtra; // analytic-light diffuse gain - 1; ambient and bounces see the albedo itself
    float roughness,roughnessWide,sharpWeight,specGain,grazing,sheen,transmission,ior,eta;
    float3 hairCenter,hairReflection;
    float hairRadius,hairH,hairShift,sssWidth,sssStrength;
    float thinStrength; // 11-10: light through thin skin ([General] Strength, the skin material's scattering.z; PtThinCandidate)
    float3 lift,liftNormal; // smooth-surface offset and traced facet for shadow terminators
    uint flags,frontFace;
    uint diffuseStop; // no diffuse bounce left on this path: only specular lobes continue (PathEnvironment::diffuseBounces)
    uint diffuseDepth; // prior diffuse events; a local baked probe is used only before the first one
    uint link; // light-link class bit of the object's layer (GeoGpu::lightLink, PathHairItem::pad)
    uint noReceive; // API 26: Unity draws this surface without shadows (a scene material past queue 2500)
    uint mirrorSheet; // PluginIdea MirrorSurface: its reflection is added over what is behind it
    uint person; // the person's skin, clothing and mesh hair (PT_MODE_PERSON)
    uint unityProbe; // API 27: Unity's Standard family, inside the scene's reflection probe (PtProbeRadiance)
    // 0.23.4: the scale of Unity's ambient in this surface's environment light, by its shader (MatGpu env2.w,
    // rtcore.h EnvironmentShader): VaM's skin family x _ExposureLM.x, Marmoset and Standard (and the rest) x 1.
    float unityAmbient;
};

float3 PtMatrix(uint at,float4 p) {
    float4 a=asfloat(gBuf[pA].Load4(at)), b=asfloat(gBuf[pA].Load4(at+16));
    float4 c=asfloat(gBuf[pA].Load4(at+32)), d=asfloat(gBuf[pA].Load4(at+48));
    float w=dot(d,p);
    return float3(dot(a,p),dot(b,p),dot(c,p))/(abs(w)>1e-9?w:1e-9);
}
float3 PtSafeNormal(float3 n,float3 fallback) { return dot(n,n)>1e-12?normalize(n):fallback; }
float PtMax(float3 x) { return max(x.x,max(x.y,x.z)); }
float PtLuma(float3 c) { return dot(c,float3(.2126,.7152,.0722)); }
float PtAddress(float u,uint mode) {
    if(mode==2)return 1-abs(frac(u*.5)*2-1); // mirror
    if(mode==3)return saturate(abs(u));      // mirror once
    return u;
}
float4 PtRead(uint index,float2 uv,float lod,uint mode) {
    uint um=mode&3,vm=(mode>>2)&3;
    uv=float2(PtAddress(uv.x,um),PtAddress(uv.y,vm));
    if(mode&32)lod=floor(lod+.5); // Unity bilinear selects one mip
    if(mode&16) {
        if(um==0 && vm==0)return gTex[NonUniformResourceIndex(index)].SampleLevel(gPointRepeat,uv,lod);
        if(um==0)return gTex[NonUniformResourceIndex(index)].SampleLevel(gPointWrapU,uv,lod);
        if(vm==0)return gTex[NonUniformResourceIndex(index)].SampleLevel(gPointWrapV,uv,lod);
        return gTex[NonUniformResourceIndex(index)].SampleLevel(gPoint,uv,lod);
    }
    if(um==0 && vm==0)return gTex[NonUniformResourceIndex(index)].SampleLevel(gRepeat,uv,lod);
    if(um==0)return gTex[NonUniformResourceIndex(index)].SampleLevel(gWrapU,uv,lod);
    if(vm==0)return gTex[NonUniformResourceIndex(index)].SampleLevel(gWrapV,uv,lod);
    return gTex[NonUniformResourceIndex(index)].SampleLevel(gLinear,uv,lod);
}
float PtAlphaAt(uint inst,uint geo,uint prim,float2 hb,PtMaterial m) {
    if(m.extra.w==RTC_NONE)return 1;
    GeoGpu g=LoadGeo(inst);if(g.uvBuf==RTC_NONE)return 1;
    uint tri;float3 bary;HitTriangle(LoadSub(g.firstSub+geo),prim,hb,tri,bary);
    uint3 ids=LoadTriangle(g,tri);
    float2 uv=LoadFloat2(g.uvBuf,g.uvOffset,g.uvStride,g.uvFormat,ids.x)*bary.x+
        LoadFloat2(g.uvBuf,g.uvOffset,g.uvStride,g.uvFormat,ids.y)*bary.y+
        LoadFloat2(g.uvBuf,g.uvOffset,g.uvStride,g.uvFormat,ids.z)*bary.z;
    float alpha=PtRead(m.extra.w,uv*m.alphaST.xy+m.alphaST.zw,0,m.sampling1.w).a;
    return saturate(alpha+m.bump.w);
}
float PtOpacity(float alpha,MatGpu old,PtMaterial m) {
    return saturate(alpha*m.base.a+((asuint(m.params.w)&32768)?m.materialControls.z:0));
}

static const uint PT_AUTHOR_NEAR=1,PT_AUTHOR_NO_SHADOW=2,PT_AUTHOR_AMBIENT=8,PT_AUTHOR_NO_RECEIVE=16,
    PT_AUTHOR_CULL=32,PT_AUTHOR_SOLID=64,PT_AUTHOR_EMITTER=128,PT_AUTHOR_CRISP=256,PT_AUTHOR_BOUNCE=1024,
    PT_AUTHOR_SPOT=2048,PT_AUTHOR_PANORAMA=4096,PT_AUTHOR_SKY_SH=8192,PT_AUTHOR_BG_BOUNCE=16384,PT_AUTHOR_BG_ROUGH=32768,PT_AUTHOR_PROBE=65536;
uint PtAuthor() { return gBuf[pA].Load(480); }
// Stable closest-hit alpha coverage for camera and secondary paths. There is
// no distance falloff on indirect light; a hit at 2 m is still a real hit.
// fixedThreshold < 0: each candidate's own hashed threshold (the path); >= 0: coverage above it is taken (the guides).
Hit PtHitOnce(float3 o,float3 d,float tmin,uint key,float fixedThreshold,bool cameraRay,out bool variedCoverage) {
    variedCoverage=false;
    float firstCoverage=10001;
    RayDesc ray; ray.Origin=o; ray.Direction=d; ray.TMin=tmin; ray.TMax=10000;
    RayQuery<RAY_FLAG_NONE> q;
    // rtcore.cpp instance masks: 0x02 what the camera sees, 0x01 what every other ray sees (API 21: the two differ by
    // the host's camera-hidden geometry, a first-person plugin's head); shadow-only proxies are neither.
    q.TraceRayInline(gPathScene,cameraRay?RAY_FLAG_CULL_BACK_FACING_TRIANGLES:RAY_FLAG_NONE,cameraRay?0x02u:0x01u,ray);
    while(q.Proceed()) {
        uint inst=q.CandidateInstanceID(),geo=q.CandidateGeometryIndex(),prim=q.CandidatePrimitiveIndex();
        MatGpu m=LoadMat(LoadSub(LoadGeo(inst).firstSub+geo).material);
        PtMaterial pm=gBuf[pB].Load<PtMaterial>(LoadSub(LoadGeo(inst).firstSub+geo).material*PT_MATERIAL_BYTES);
        float a=PtAlphaAt(inst,geo,prim,q.CandidateTriangleBarycentrics(),pm);
        if((m.kind==1 || (asuint(pm.params.w)&8388608)) && PtOpacity(a,m,pm)<(m.kind==1?m.color.a:pm.materialControls.w))continue;
        float threshold=fixedThreshold>=0?fixedThreshold:(Hash(key^Hash(prim*2654435761u+inst*40503u+geo))>>8)*(1.0/16777216.0);
        if(m.kind==3 && !(asuint(pm.params.w)&(1048576|2097152)) && (pm.params.z<.999 || (asuint(pm.params.w)&16384))) {
            // Only coverage strictly between 0 and 1 varies with the seed: 0 is always skipped, 1 always taken (where
            // the PT's opacity micromaps decide in hardware, path_omm.hlsl, as here).
            float coverage=PtOpacity(a,m,pm);
            if(coverage>0 && coverage<1)firstCoverage=min(firstCoverage,q.CandidateTriangleRayT());
            if(coverage<=threshold)continue;
        }
        q.CommitNonOpaqueTriangleHit();
    }
    Hit h=(Hit)0; h.t=-1;
    if(q.CommittedStatus()!=COMMITTED_NOTHING && NvRtCommittedIsLss(q)) {
        // One fiber instance per light-link class; its ID is the class's first item.
        h.t=q.CommittedRayT();h.inst=RTC_NONE;h.geo=q.CommittedInstanceID()+q.CommittedGeometryIndex();h.prim=q.CommittedPrimitiveIndex();
        h.bary=float2(NvRtCommittedLssHitParameter(q),0);
    } else if(q.CommittedStatus()==COMMITTED_TRIANGLE_HIT) {
        h.t=q.CommittedRayT(); h.inst=q.CommittedInstanceID(); h.geo=q.CommittedGeometryIndex();
        h.prim=q.CommittedPrimitiveIndex(); h.bary=q.CommittedTriangleBarycentrics();
    }
    variedCoverage=firstCoverage<=(h.t<0?10000:h.t+1e-6);
    return h;
}
// Unity draws a Skybox/Panoramic sphere first and writes no depth: everything else covers it, what lies beyond it too
// (one stage pack's beach has its sea and hills past a 75 m sphere). Camera rays go on past it; only where nothing lies
// beyond is the sphere what they see.
bool PtPanoramaHit(Hit h) {
    if(h.t<0 || h.inst==RTC_NONE)return false;
    return (gBuf[pB].Load(LoadSub(LoadGeo(h.inst).firstSub+h.geo).material*PT_MATERIAL_BYTES+404)&PT_MODE_PANORAMA)!=0;
}
Hit PtHitTracked(float3 o,float3 d,float tmin,uint key,float fixedThreshold,bool cameraRay,out bool variedCoverage) {
    Hit h=PtHitOnce(o,d,tmin,key,fixedThreshold,cameraRay,variedCoverage);
    if(cameraRay && (PtAuthor()&PT_AUTHOR_PANORAMA) && PtPanoramaHit(h)) {
        bool varied;Hit beyond=PtHitOnce(o,d,h.t*1.0001+1e-3,key,fixedThreshold,cameraRay,varied);
        if(beyond.t>=0){variedCoverage=variedCoverage||varied;return beyond;}
    }
    return h;
}
Hit PtHit(float3 o,float3 d,float tmin,uint key,bool guide=false) {
    bool unused;return PtHitTracked(o,d,tmin,key,guide?.5:-1,guide,unused);
}
// The coverage of a committed hit where it varies with the seed (as PtHitTracked tests it); 1 elsewhere.
float PtHitCoverage(Hit h) {
    if(h.t<0 || h.inst==RTC_NONE)return 1;
    uint material=LoadSub(LoadGeo(h.inst).firstSub+h.geo).material;
    MatGpu m=LoadMat(material);
    PtMaterial pm=gBuf[pB].Load<PtMaterial>(material*PT_MATERIAL_BYTES);
    if(!(m.kind==3 && !(asuint(pm.params.w)&(1048576|2097152)) && (pm.params.z<.999 || (asuint(pm.params.w)&16384))))return 1;
    return saturate(PtOpacity(PtAlphaAt(h.inst,h.geo,h.prim,h.bary,pm),m,pm));
}

float4 PtTexture(uint index,float2 uv,float footprint,float4 fallback,uint sampling) {
    if(index==RTC_NONE)return fallback;
    uint w,h,levels;
    gTex[NonUniformResourceIndex(index)].GetDimensions(0,w,h,levels);
    float lod=log2(max(footprint*max(w,h),1e-6))+asfloat(gBuf[pA].Load(152));
    return PtRead(index,uv,clamp(lod,0,max(levels,1u)-1),sampling);
}
float4 PtMap(PtMaterial m,uint slot,float2 uv,float footprint,float4 st,float4 fallback) {
    uint index=slot<4?m.textures[slot]:m.extra[slot-4];
    uint sampling=slot<4?m.sampling0[slot]:m.sampling1[slot-4];
    return PtTexture(index,uv*st.xy+st.zw,footprint*max(abs(st.x),abs(st.y)),fallback,sampling);
}
float3 PtDecodeNormal(float4 nm,float scale,uint flags) {
    if(flags&4)nm.r*=nm.a;
    float2 xy=(nm.rg*2-1)*scale;
    return float3(xy,sqrt(saturate(1-dot(xy,xy))));
}
// HZM's three world-space projections. Normalize before exponentiation so
// large authored blend powers (up to 64) cannot underflow on diagonal faces.
float3 PtHzmWeights(float3 n,float power) {
    float3 a=abs(n);a/=max(max(a.x,max(a.y,a.z)),1e-20);
    float3 w=pow(max(a,1e-20),max(power,0));return w/max(w.x+w.y+w.z,1e-20);
}
void PtHzmCoords(float3 p,float3 n,float tiling,out float2 ux,out float2 uy,out float2 uz) {
    float3 q=p*tiling;ux=float2(q.z*sign(n.x),q.y);uy=float2(q.x*sign(n.y),q.z);uz=float2(-q.x*sign(n.z),q.y);
}
float4 PtHzmMap(PtMaterial m,uint slot,float3 p,float3 n,float footprint,float power,float4 fallback) {
    uint index=slot<4?m.textures[slot]:m.extra[slot-4];
    uint sampling=slot<4?m.sampling0[slot]:m.sampling1[slot-4];
    float2 x,y,z;PtHzmCoords(p,n,m.uv.x,x,y,z);float3 w=PtHzmWeights(n,power);
    return PtTexture(index,x,footprint,fallback,sampling)*w.x+
        PtTexture(index,y,footprint,fallback,sampling)*w.y+PtTexture(index,z,footprint,fallback,sampling)*w.z;
}
float3 PtHzmNormal(PtMaterial m,float3 p,float3 n,float footprint) {
    float2 x,y,z;PtHzmCoords(p,n,m.uv.x,x,y,z);
    float4 flat=float4(.5,.5,1,1);uint index=m.textures.y,sampling=m.sampling0.y;
    float3 nx=PtDecodeNormal(PtTexture(index,x,footprint,flat,sampling),1,4);
    float3 ny=PtDecodeNormal(PtTexture(index,y,footprint,flat,sampling),1,4);
    float3 nz=PtDecodeNormal(PtTexture(index,z,footprint,flat,sampling),1,4);
    float3 px=float3(nx.z*n.x,nx.y+n.y,nx.x*sign(n.x)+n.z);
    float3 py=float3(ny.x*sign(n.y)+n.x,ny.z*n.y,ny.y+n.z);
    float3 pz=float3(-nz.x*sign(n.z)+n.x,nz.y+n.y,nz.z*n.z);
    float3 w=PtHzmWeights(n,m.bump.x);
    return PtSafeNormal(px*w.x+py*w.y+pz*w.z,n);
}
float3 PtTangentNormal(PtMaterial m,float2 uv,float footprint) {
    uint flags=asuint(m.params.w);
    float3 tn=PtDecodeNormal(PtMap(m,1,uv,footprint,m.normalST,float4(.5,.5,1,1)),m.params.x,flags);
    if(m.extra.y!=RTC_NONE) {
        float3 detail=PtDecodeNormal(PtMap(m,5,uv,footprint,m.detailST,float4(.5,.5,1,1)),(flags&65536)?1:m.bump.z,flags);
        float mask=PtMap(m,6,uv,footprint,m.uv,1).a;
        tn=(flags&65536)?PtSafeNormal(tn+detail*m.bump.z,tn):normalize(lerp(tn,normalize(float3(tn.xy+detail.xy,tn.z*detail.z)),mask));
    }
    return tn;
}
// RtCore's test layers (rtcore.h SetTestLayers; every bit set by default), camera.path[1]. Cleared, a PT bit
// restores 0.22.66: 20 a light sampled over its disc alone, 21 environment / emitter samples stopped by clear
// sheets, 22 the cornea / tear film at its authored gloss (pt-eye-audit-20261103). Bits 23-24 restore 0.22.68:
// 23 VaM's Subsurface family lit directly as (2-S) N.L, 24 the eye film at its authored reflection strength
// (pt-eye-light-20261103). Bit 25 restores 0.22.69: an eyeball's lights not shut out by its lids (pt-eyelid-20261103).
static const uint PT_TEST_LOBE_LIGHT=1u<<20,PT_TEST_SHEET_NEE=1u<<21,PT_TEST_WET_FILM=1u<<22;
static const uint PT_TEST_FAMILY_DIRECT=1u<<23,PT_TEST_WET_F0=1u<<24,PT_TEST_LID=1u<<25;
// Bit 28 (0.23.4, pt-scene-elements-20261105): Unity's ambient scaled by each surface's shader family
// (PtSurface::unityAmbient); cleared: x _ExposureLM.x for every surface (0.23.3).
static const uint PT_TEST_AMBIENT_CLASS=1u<<28;
static const uint PT_EYEBALL=1073741824; // material: VaM's Sclera, Irises, Pupils (RtScene.cs)
uint PtTest() { return gBuf[pA].Load(468); }
// API 26 (docs\pt-author-intent-20261109): VaM's scenes are lit by their authors under Unity's rules, and those rules
// are part of what the author says: a fill light with no shadow, a light inside its lamp shade, a shadow only 20 %
// dark, an ambient colour everywhere, an unseen shell that stops the light. Each bit keeps one of them; 0 = physical.
static const uint PT_MODE_NO_RECEIVE=65536,PT_MODE_MIRROR_SHEET=131072,PT_MODE_UNITY_PROBE=262144;
float PtAuthorContact() { return asfloat(gBuf[pA].Load(492)); }
float PtAuthorAmbientRange() { return asfloat(gBuf[pA].Load(496)); }
float PtAuthorEmitter() { return (PtAuthor()&PT_AUTHOR_EMITTER)?asfloat(gBuf[pA].Load(500)):1; }
// The share of a surface's diffuse environment that Unity draws unoccluded: VaM's ambient colour and Skyshop's
// diffuse are everywhere, in a closed room too. Only the first surface of a path (bounces would add it up), and not
// where a scene carries its own interior probe (PtIndoorCandidate).
float PtAuthorAmbientShare(uint diffuseDepth) {
    return (PtAuthor()&PT_AUTHOR_AMBIENT) && diffuseDepth==0 && gBuf[pA].Load(188)==RTC_NONE?asfloat(gBuf[pA].Load(488)):0;
}
// VaM's eyelid occlusion on the eyeball (0.22.71, bit 26; cleared: 0.22.70; the existing lighting's E1 without its
// traced atlas, gen_spec_table.py EyeOcclusion): along each lid's contact line on the eyeball's sphere, a band darkened
// by (1 - upper u e^(-d^2/wu^2)) (1 - lower (1-u) e^(-d^2/wl^2)) - the lid's rim, which the mesh has no thickness for.
// It scales the eyeball's albedo: all its diffuse light, not its highlights nor the film's catchlight. camera.path[2]
// the eye rigs (SetPathEyeRigs, rt.cpp RecordPathEyes), path[3] how many; per character PT_EYE_RIG bytes: uint (points
// of eye 0, of eye 1), the draw offset (world = rig + offset), the occlusion (upper darkening, its width, lower
// darkening, its width; widths in eyeball radii; all 0: off for this character), then the skin's gather (each eye's
// centre and scale, its gaze and radius, its lid points on the sphere, w the upper share, + 2 where a line starts).
static const uint PT_TEST_LID_BAND=1u<<26,PT_EYE_RIG=2160;
float PtEyeBand(float3 p) {
    uint2 rigs=gBuf[pA].Load2(472);
    if(!(PtTest()&PT_TEST_LID_BAND) || rigs.x==RTC_NONE)return 1;
    float band=1,nearest=1e30;
    [loop] for(uint k=0;k<rigs.y;k++) {
        uint at=k*PT_EYE_RIG;
        uint2 n=gBuf[rigs.x].Load2(at);
        float4 occ=asfloat(gBuf[rigs.x].Load4(at+32));
        if(!(occ.x>0 || occ.z>0))continue;
        float3 lp=p-asfloat(gBuf[rigs.x].Load3(at+16));
        [loop] for(uint e=0;e<2;e++) {
            float4 c=asfloat(gBuf[rigs.x].Load4(at+48+32*e)),gz=asfloat(gBuf[rigs.x].Load4(at+64+32*e));
            float3 dc=lp-c.xyz;float d2=dot(dc,dc),R=gz.w;
            if(!(R>0) || d2>=1.69*R*R || d2>=nearest)continue;
            nearest=d2;
            float3 q=c.xyz+R*normalize(dc);
            float wu=occ.y*R,wl=occ.w*R,du=0,dl=0;
            uint first=at+48+16*(4+(e?n.x:0)),count=e?n.y:n.x;
            float4 a=asfloat(gBuf[rigs.x].Load4(first));
            [loop] for(uint i=1;i<count;i++) {
                float4 b=asfloat(gBuf[rigs.x].Load4(first+16*i));
                if(b.w<2) {
                    float3 ab=b.xyz-a.xyz;
                    float t=saturate(dot(q-a.xyz,ab)/max(dot(ab,ab),1e-20));
                    float3 dd=q-(a.xyz+t*ab);float dd2=dot(dd,dd);
                    float u=lerp(a.w>=2?a.w-2:a.w,b.w,t);
                    du=max(du,u*exp(-dd2/max(wu*wu,1e-20)));
                    dl=max(dl,(1-u)*exp(-dd2/max(wl*wl,1e-20)));
                }
                a=b;
            }
            band=(1-occ.x*du)*(1-occ.z*dl);
        }
    }
    return band;
}
PtSurface PtShading(Hit hit,float3 o,float3 d,float cone) {
    if(hit.inst==RTC_NONE) {
        uint4 hair=gBuf[pA].Load4(256);
        uint4 item=gBuf[hair.z].Load4(hit.geo*48);
        float4 material=asfloat(gBuf[hair.z].Load4(hit.geo*48+16));
        uint vertex=item.x+(hit.prim/item.z)*(item.z+1)+hit.prim%item.z;
        float4 a=asfloat(gBuf[hair.y].Load4(vertex*48)),b=asfloat(gBuf[hair.y].Load4((vertex+1)*48));
        float4 ca=asfloat(gBuf[hair.y].Load4(vertex*48+16)),cb=asfloat(gBuf[hair.y].Load4((vertex+1)*48+16));
        PtSurface s=(PtSurface)0;s.p=o+d*hit.t;s.t=PtSafeNormal(b.xyz-a.xyz,float3(0,1,0));
        s.hairCenter=lerp(a.xyz,b.xyz,saturate(hit.bary.x));s.hairRadius=lerp(a.w,b.w,saturate(hit.bary.x));
        s.ng=PtSafeNormal(s.p-s.hairCenter,-d);
        s.n=PtSafeNormal(-d+s.t*dot(d,s.t),s.ng);s.b=PtSafeNormal(cross(s.n,s.t),float3(1,0,0));
        s.dn=s.n;
        s.hairH=clamp(dot(s.ng,s.b),-.9999,.9999);
        // Kajiya-Kay's authored tangent shift locates the R highlight at twice
        // the cuticle angle. Preserve that intent, including per-strand variation.
        s.hairShift=clamp(.5*atan(material.z+lerp(ca.w,cb.w,saturate(hit.bary.x))),-.174533,.174533);
        s.base=saturate(lerp(ca.rgb,cb.rgb,saturate(hit.bary.x)));s.f0=.0465;
        s.hairReflection=saturate(asfloat(gBuf[hair.z].Load3(hit.geo*48+32)));
        s.roughness=s.roughnessWide=clamp(material.y,.05,1);s.sharpWeight=s.specGain=1;
        s.ior=1.55;s.flags=64;s.link=item.w&0x1fu; // pad: link class | 0x20 camera-hidden (API 21)
        s.unityAmbient=fAmbientLM; // VaM's hair shaders: the skin family's ambient
        return s;
    }
    GeoGpu g=LoadGeo(hit.inst); SubGpu sub=LoadSub(g.firstSub+hit.geo);
    MatGpu old=LoadMat(sub.material);
    PtMaterial m=gBuf[pB].Load<PtMaterial>(sub.material*PT_MATERIAL_BYTES);
    uint tri; float3 bary; HitTriangle(sub,hit.prim,hit.bary,tri,bary);
    uint3 ids=LoadTriangle(g,tri);
    float3 a=GeoPosition(g,ids.x),b=GeoPosition(g,ids.y),c=GeoPosition(g,ids.z);
    PtSurface s=(PtSurface)0;
    s.p=o+d*hit.t;s.link=g.lightLink;
    s.unityAmbient=(old.env2.w>.5 && old.env2.w<1.5)?fAmbientLM:1; // EnvironmentShader 1: VaM's skin family
    float detM=dot(g.m0.xyz,cross(g.m1.xyz,g.m2.xyz));
    // Preserve the object's authored outward side under a reflected instance,
    // consistently with DXR's object-space face classification and IOR crossing.
    s.ng=PtSafeNormal(cross(b-a,c-a)*(detM<0?-1:1),-d);
    // DXR intersects the Phong sub-triangle, not its untessellated parent.
    // Use the same three positions as Tessellate in object space before the
    // instance transform; the parent normal can face the other side at a
    // curved silhouette and can offset later rays into the traced surface.
    if(sub.blasTess && g.nrmBuf!=RTC_NONE) {
        float3 u,v,w;SubTriangle(sub.tessLevel,hit.prim%(sub.tessLevel*sub.tessLevel),u,v,w);
        float3 pa=LocalPosition(g,ids.x),pb=LocalPosition(g,ids.y),pc=LocalPosition(g,ids.z);
        float3 na=LocalNormal(g,ids.x),nb=LocalNormal(g,ids.y),nc=LocalNormal(g,ids.z);
        float3 ta=ToWorld(g,PhongPoint(u,pa,pb,pc,na,nb,nc,sub.tessAlpha));
        float3 tb=ToWorld(g,PhongPoint(v,pa,pb,pc,na,nb,nc,sub.tessAlpha));
        float3 tc=ToWorld(g,PhongPoint(w,pa,pb,pc,na,nb,nc,sub.tessAlpha));
        s.ng=PtSafeNormal(cross(tb-ta,tc-ta)*(detM<0?-1:1),s.ng);
    }
    s.n=s.ng;
    if(g.nrmBuf!=RTC_NONE) {
        float3 local=LocalNormal(g,ids.x)*bary.x+LocalNormal(g,ids.y)*bary.y+LocalNormal(g,ids.z)*bary.z;
        float3 transformed=float3(dot(cross(g.m1.xyz,g.m2.xyz),local),dot(cross(g.m2.xyz,g.m0.xyz),local),dot(cross(g.m0.xyz,g.m1.xyz),local));
        s.n=PtSafeNormal(transformed*(detM<0?-1:1),s.ng); // inverse transpose, including nonuniform room scales
        // The smooth normal describes a surface outside a convex flat facet.
        // Start near-tangent visibility rays on that surface, not inside it.
        float3x3 cofactor=float3x3(cross(g.m1.xyz,g.m2.xyz),cross(g.m2.xyz,g.m0.xyz),cross(g.m0.xyz,g.m1.xyz));
        float3 na=PtSafeNormal(mul(cofactor,LocalNormal(g,ids.x))*(detM<0?-1:1),s.ng);
        float3 nb=PtSafeNormal(mul(cofactor,LocalNormal(g,ids.y))*(detM<0?-1:1),s.ng);
        float3 nc=PtSafeNormal(mul(cofactor,LocalNormal(g,ids.z))*(detM<0?-1:1),s.ng);
        if(dot(na,s.ng)>0 && dot(nb,s.ng)>0 && dot(nc,s.ng)>0) {
            float3 q=a*bary.x+b*bary.y+c*bary.z,facet=s.ng;
            float3 lift=q-s.p+bary.x*max(dot(a-q,na),0)*na+bary.y*max(dot(b-q,nb),0)*nb+bary.z*max(dot(c-q,nc),0)*nc;
            if(dot(lift,facet)>0) { s.lift=lift; s.liftNormal=facet; }
        }
    }
    bool entering=dot(s.ng,-d)>=0;
    s.frontFace=entering?1:0;
    if(!entering) { s.ng=-s.ng; s.n=-s.n; }
    float2 uv=0,ua=0,ub=0,uc=0;
    if(g.uvBuf!=RTC_NONE) {
        ua=LoadFloat2(g.uvBuf,g.uvOffset,g.uvStride,g.uvFormat,ids.x);
        ub=LoadFloat2(g.uvBuf,g.uvOffset,g.uvStride,g.uvFormat,ids.y);
        uc=LoadFloat2(g.uvBuf,g.uvOffset,g.uvStride,g.uvFormat,ids.z);
        uv=ua*bary.x+ub*bary.y+uc*bary.z;
    }
    bool panoramaBlack=false;
    if(m.second.y&PT_MODE_PANORAMA) {
        float3 p=LocalPosition(g,ids.x)*bary.x+LocalPosition(g,ids.y)*bary.y+LocalPosition(g,ids.z)*bary.z;
        float sn,cs;sincos(-radians(m.directGain.w),sn,cs);
        p.xz=float2(cs*p.x-sn*p.z,sn*p.x+cs*p.z);
        p=normalize(p);
        uv=float2(.5-atan2(p.z,p.x)/(2*PT_PI),1-acos(clamp(p.y,-1,1))/PT_PI);
        if(m.second.y&PT_MODE_PANORAMA_180) {
            panoramaBlack=uv.x>.5 && !(m.second.y&PT_MODE_PANORAMA_MIRROR);
            uv.x=frac(uv.x*2);
        }
        float eye=asfloat(gBuf[pA].Load(460));
        if(m.second.y&PT_MODE_PANORAMA_SBS)uv.x=(uv.x+eye)*.5;
        else if(m.second.y&PT_MODE_PANORAMA_OU)uv.y=(uv.y+1-eye)*.5;
    }
    float2 du=ub-ua,dv=uc-ua;
    float det=du.x*dv.y-du.y*dv.x;
    Basis(s.n,s.t,s.b);
    if(g.tanBuf!=RTC_NONE) {
        float4 tangent=LoadFloat4(g.tanBuf,g.tanOffset,g.tanStride,g.tanFormat,ids.x)*bary.x+
            LoadFloat4(g.tanBuf,g.tanOffset,g.tanStride,g.tanFormat,ids.y)*bary.y+
            LoadFloat4(g.tanBuf,g.tanOffset,g.tanStride,g.tanFormat,ids.z)*bary.z;
        float3 world=float3(dot(g.m0.xyz,tangent.xyz),dot(g.m1.xyz,tangent.xyz),dot(g.m2.xyz,tangent.xyz));
        s.t=PtSafeNormal(world-s.n*dot(world,s.n),s.t);
        float transformSign=dot(g.m0.xyz,cross(g.m1.xyz,g.m2.xyz))<0?-1:1;
        s.b=cross(s.n,s.t)*(tangent.w<0?-1:1)*transformSign;
    } else if(abs(det)>1e-10) {
        float3 tangent=((b-a)*dv.y-(c-a)*du.y)/det;
        s.t=PtSafeNormal(tangent-s.n*dot(tangent,s.n),s.t);
        s.b=cross(s.n,s.t)*(det<0?-1:1);
    }
    // Ray-cone footprint: LOD follows input pixels and path spread, rather than
    // throwing away detail by hard-coding a coarse mip for all materials.
    float area=max(length(cross(b-a,c-a)),1e-10);
    float uvDensity=sqrt(abs(det)/area);
    // Project the cone onto the surface. Omitting the grazing factor aliases
    // the same texture on floors, cloth folds and angled skin.
    float lod=cone*uvDensity/max(abs(dot(s.ng,-d)),.1);
    bool hzmWorld=(m.second.y&PT_MODE_HZM_WORLD_ALBEDO)!=0;
    float worldFootprint=cone*abs(m.uv.x)/max(abs(dot(s.ng,-d)),.1);
    float4 baseSample=hzmWorld?PtHzmMap(m,0,s.p,s.n,worldFootprint,m.directGain.w,1):PtMap(m,0,uv,lod,m.uv,1);
    s.base=saturate(m.base.rgb*saturate(baseSample.rgb+m.materialControls.x));
    uint mode=m.second.y;
    s.noReceive=(mode&PT_MODE_NO_RECEIVE)?1:0;
    s.person=(mode&PT_MODE_PERSON)?1:0;
    if((mode&PT_MODE_UNITY_PROBE) && (PtAuthor()&PT_AUTHOR_PROBE) && gBuf[pA].Load(512)!=RTC_NONE) {
        float3 lo=asfloat(gBuf[pA].Load3(544)),hi=asfloat(gBuf[pA].Load3(560));
        s.unityProbe=all(s.p>=lo) && all(s.p<=hi)?1:0;
    }
    float4 layer=1;
    if(m.second.x!=RTC_NONE) {
        layer=PtTexture(m.second.x,uv*m.secondST.xy+m.secondST.zw,lod*max(abs(m.secondST.x),abs(m.secondST.y)),0,m.second.z);
        // VaM's decal is composited over the tinted albedo by its own alpha;
        // Standard's detail albedo is x2 in linear space, under _DetailMask.a.
        if(mode&PT_MODE_DECAL)s.base=lerp(s.base,saturate(layer.rgb),saturate(layer.a));
        if(mode&PT_MODE_DETAIL_ALBEDO)s.base=saturate(s.base*lerp(1,layer.rgb*4.59479380,PtMap(m,6,uv,lod,m.uv,1).a));
    }
    s.directExtra=max(m.directGain.rgb,0)-1;
    float4 packed=hzmWorld?PtHzmMap(m,2,s.p,s.n,worldFootprint,m.directGain.w,1):PtMap(m,2,uv,lod,m.surfaceST,1);
    s.flags=asuint(m.params.w);
    float metal=saturate(m.emission.w*((s.flags&1)?packed.r:1));
    s.roughness=clamp((s.flags&2)?1-packed.a*(1-m.spec.w):m.spec.w,.03,1);
    if(mode&PT_MODE_ALBEDO_SMOOTHNESS)s.roughness=clamp(1-baseSample.a*(1-m.spec.w),.03,1);
    if(s.flags&16) {
        float gloss=saturate(PtMap(m,4,uv,lod,m.glossST,0).r+m.gloss.x),u=1-gloss;
        float exponent=exp2(lerp(m.gloss.y,1,u*u));
        s.roughness=clamp(pow(2/(exponent+2),.25),.03,1);
    }
    // 0.23.4: Unity's Standard (Roughness setup) and authors' copies of it: the gloss slot holds _SpecGlossMap, whose red
    // channel is the perceptual roughness itself (UnityStandardInput.cginc MetallicRough: smoothness = 1 - map.r).
    if(s.flags&PT_ROUGHNESS_MAP)s.roughness=clamp(PtMap(m,4,uv,lod,m.glossST,1).r,.03,1);
    if(mode&PT_MODE_HZM_PBR)s.roughness=clamp(packed.g*m.spec.w,.03,1);
    float legacyPeakRatio=1;
    if(s.flags&512) {
        float u=1-saturate(packed.a+m.gloss.z);
        float exponent=exp2(lerp(m.gloss.y,0,u*u));
        if(s.flags&67108864) {
            // Skyshop's direct lobe uses N.H^(2n), but its normalizer is n+2.
            // Keep both authored width and strength when translating to GGX.
            legacyPeakRatio=(exponent+2)/(2*exponent+2);exponent*=2;
        }
        s.roughness=clamp(pow(2/(exponent+2),.25),.03,1);
    }
    s.f0=lerp(saturate(m.spec.rgb*((s.flags&1)?1:saturate(packed.rgb+m.gloss.z))),s.base,metal);
    s.roughnessWide=s.roughness;s.sharpWeight=1;s.specGain=max(m.materialControls.y,0);
    if(s.flags&268435456) {
        // Unity's actual legacy shader is UNnormalized N.H^(128*Shininess),
        // with MainTex.a as strength. It is neither Skyshop nor physical F0.
        float exponent=max(128*m.gloss.y,0);
        s.roughness=s.roughnessWide=clamp(pow(2/(exponent+2),.25),.03,1);
        s.f0=saturate(m.spec.rgb*baseSample.a*(8/(exponent+2)));s.specGain=1;
    }
    // The cornea / tear film (EyeReflection over the eyeball, Tear at the lids) is a wet interface, smooth as water:
    // a light's catchlight is its image. The authored gloss (EyeReflection's Skyshop ~.25, Tear's ~.54) is raster's
    // stand-in, where a smooth film under VaM's sizeless lights shows nothing. Strength and tint stay the author's.
    if((s.flags&262144) && (PtTest()&PT_TEST_WET_FILM))s.roughness=s.roughnessWide=.03;
    bool wetTongue=(s.flags&8)!=0 && m.scattering.w>0;
    bool authoredSheet=(s.flags&2048) && (s.flags&(33554432|67108864));
    if((s.flags&(33554432|67108864)) && (!(s.flags&8) || wetTongue) && (m.params.z<.999 || authoredSheet)) {
        // Subsurface shader's _SpecColor is a Blinn-Phong authoring strength,
        // not Fresnel F0. Match its frontal lobe amplitude before changing the
        // angular model. Standard physical specular materials skip this.
        float f=1-saturate(m.gloss.w);
        float facing=(s.flags&67108864)?pow(saturate(1.25-m.gloss.w),5):(.05+.95*f*f);
        float source=max(m.materialControls.y,0)*facing;
        float amplitude=lerp(sqrt(source),source,.9224439);
        s.f0=saturate(s.f0*(2/PT_PI)*legacyPeakRatio*amplitude);s.specGain=PtMax(s.f0)>0?1:0;
        // These programs add the Blinn lobe on top of the full albedo; the
        // albedo is never reduced by _SpecColor. Keep the authored colour.
        if(!(s.flags&(8|16777216)) && m.params.z<.999)mode|=PT_MODE_ADDITIVE;
    }
    if((mode&PT_MODE_REFLECTION_MASK) && m.second.x!=RTC_NONE)s.f0*=lerp(1,saturate(layer.rgb),saturate(m.directGain.w));
    float3 dryF0=s.f0;float dryRoughness=s.roughness,dryGain=s.specGain;
    float skinBaseA2=0;
    if((s.flags&8) && m.skinCtl.x>0) {
        float a2=pow(s.roughness,4);
        if(m.tierA.x>0 && m.tierC.w>0) {
            float gloss=PtMap(m,4,uv,lod*exp2(m.tierB.x),m.glossST,0).r;
            a2=clamp(m.tierA.x*exp2(m.tierA.y*(gloss-m.tierC.x)/max(m.tierC.y,max(m.tierB.z,.01))),m.tierA.z,m.tierA.w);
        }
        skinBaseA2=a2;
        float rough=pow(max(a2,1e-8),.25)*m.skinCtl.y;
        s.roughness=clamp(rough*m.skinCtl.z,.2,1);
        s.roughnessWide=clamp(rough*m.skinCtl.w,.2,1);
        s.sharpWeight=saturate(m.skinLobe.x);
        s.f0=saturate(m.skinCtl.x);
        // Keep the authored spatial cavity response and the installed skin's
        // intensity convention. A scalar lobe gain alone made every pore and
        // lip reflect alike, despite carrying the actual specular texture.
        float cavityFootprint=max(lod,m.skinFilter.w*uvDensity);
        float3 cavity=saturate(PtMap(m,2,uv,cavityFootprint,m.surfaceST,1).rgb+m.gloss.z);
        float divisor=m.tierC.w>0?m.tierC.z:3;
        if(m.tierC.w<=0 && m.textures.z!=RTC_NONE) {
            float3 mean=PtRead(m.textures.z,uv*m.surfaceST.xy+m.surfaceST.zw,16,m.sampling0.z).rgb;
            divisor=dot(saturate(mean+m.gloss.z),1);
        }
        s.specGain=max(m.skinLobe.y,0)*saturate(4*m.materialControls.y)*min(dot(cavity,1)/max(divisor,.15),3);
        s.grazing=m.skinLobe.z;s.sheen=m.skinLobe.w;
        s.sssWidth=max(m.scattering.x,0);s.sssStrength=saturate(m.scattering.y);
        s.thinStrength=max(m.scattering.z,0);
    }
    s.base*=1-metal;
    float4 glow=hzmWorld?PtHzmMap(m,3,s.p,s.n,0,m.directGain.w,0):PtMap(m,3,uv,0,m.emissionST,1);
    if(s.flags&131072)glow.rgb=saturate(glow.rgb+m.materialControls.x);
    s.emission=max(m.emission.rgb*((s.flags&4096)?glow.aaa:glow.rgb),0);
    if(panoramaBlack)s.emission=0;
    if(s.flags&1024){s.base=0;s.f0=0;s.specGain=0;}
    if(s.flags&8192)s.base=0;
    s.transmission=saturate(m.params.z); s.ior=max(m.params.y,1.001);
    if(s.transmission>=.999) {
        // A physical volume uses its IOR. Skyshop/legacy thin films instead
        // have authored reflection tint, intensity and gloss; replacing these
        // with water F0 and a fixed .05 roughness erased their material.
        if(!authoredSheet)s.f0=pow((s.ior-1)/(s.ior+1),2);
        if((s.flags&2048) && !(s.flags&16384))s.base=lerp(1.0,s.base,saturate(m.base.a));
        // EyeReflection/Tear are clear wet interfaces. Their raster colour is
        // a reflection tint, not pigment absorbing transmitted iris radiance.
        if(s.flags&262144)s.base=1;
        // 0.22.69 (bit 24): the film reflects as tear on a cornea, air to n 1.336: ((n-1)/(n+1))^2, 2.1 %. The authored
        // strength was matched to raster's wide lobe (one test scene's EyeReflection 1.5 %); made smooth (bit 22) the film is
        // itself. The authored tint stays; an author's zero stays zero.
        if((s.flags&262144) && (PtTest()&PT_TEST_WET_F0) && PtMax(s.f0)*s.specGain>0) {
            s.f0*=pow((s.ior-1)/(s.ior+1),2)/PtMax(s.f0);s.specGain=1;
        }
    }
    s.eta=entering || (s.flags&2048)?s.ior:1/s.ior;
    if(s.flags&1048576) {
        // An authored multiplicative darkening layer is optical attenuation,
        // not black diffuse geometry and not a material to silently remove.
        float alpha=PtOpacity(PtAlphaAt(hit.inst,hit.geo,hit.prim,hit.bary,m),old,m);
        s.base=1-alpha;s.f0=0;s.specGain=0;s.transmission=1;s.roughness=s.roughnessWide=0;
    }
    if(s.flags&2097152) {
        if(s.flags&4194304)s.emission*=PtOpacity(PtAlphaAt(hit.inst,hit.geo,hit.prim,hit.bary,m),old,m);
        s.base=1;s.f0=0;s.specGain=0;s.transmission=1;s.roughness=s.roughnessWide=0;
    }
    s.dn=s.n;
    if(hzmWorld && m.textures.y!=RTC_NONE) {
        s.n=PtHzmNormal(m,s.p,s.n,worldFootprint);s.dn=s.n;
        if(dot(s.n,s.ng)<.05)s.n=s.dn=PtSafeNormal(s.n+s.ng*(.05-dot(s.n,s.ng)),s.ng);
        s.t=PtSafeNormal(s.t-s.n*dot(s.t,s.n),cross(s.b,s.n));s.b=cross(s.n,s.t);
    } else if(!hzmWorld && (m.textures.y!=RTC_NONE || m.extra.y!=RTC_NONE)) {
        float3 tn=PtTangentNormal(m,uv,lod);
        float3 sn=tn,dn=tn;
        if(s.flags&256) {
            sn=normalize(lerp(float3(0,0,1),tn,m.bump.x));
            dn=normalize(lerp(float3(0,0,1),tn,m.bump.y));
        }
        if((s.flags&16777216) && m.skinCtl.y>0 && lod>0) {
            // Ray-cone neighbours replace raster ddx/ddy for the same cloth
            // variance control. Broaden unresolved highlights, not albedo.
            float3 nx=PtTangentNormal(m,uv+float2(lod,0),lod);
            float3 ny=PtTangentNormal(m,uv+float2(0,lod),lod);
            if(s.flags&256) {
                nx=normalize(lerp(float3(0,0,1),nx,m.bump.x));
                ny=normalize(lerp(float3(0,0,1),ny,m.bump.x));
            }
            float variance=2*m.skinCtl.y*(dot(nx-sn,nx-sn)+dot(ny-sn,ny-sn));
            s.roughness=pow(saturate(pow(s.roughness,4)+min(variance,max(m.skinCtl.z,0))),.25);
            s.roughnessWide=s.roughness;
        }
        if((s.flags&8) && m.skinCtl.x>0 && m.textures.y!=RTC_NONE) {
            // Same material filtering as the installed skin shader: the
            // unresolved pore slope broadens the lobe instead of becoming a
            // large moving highlight. Albedo and resolvable normals remain.
            if(m.skinFilter.x>0) {
                float3 filtered=PtTangentNormal(m,uv,lod*exp2(m.skinFilter.x));
                filtered=normalize(lerp(float3(0,0,1),filtered,m.bump.x));
                float3 delta=sn-filtered;sn=filtered;
                float variance=max(m.skinFilter.y,0)*dot(delta,delta);
                float rough=pow(clamp(skinBaseA2+variance,1e-8,1),.25)*m.skinCtl.y;
                s.roughness=clamp(rough*m.skinCtl.z,.2,1);
                s.roughnessWide=clamp(rough*m.skinCtl.w,.2,1);
            }
            if(m.skinFilter.z>0) {
                float footprint=max(lod,m.skinFilter.z*uvDensity);
                float3 filtered=PtTangentNormal(m,uv,footprint);
                dn=normalize(lerp(float3(0,0,1),filtered,m.bump.y));
            }
        }
        s.dn=PtSafeNormal(s.t*dn.x+s.b*dn.y+s.n*dn.z,s.n);
        s.n=PtSafeNormal(s.t*sn.x+s.b*sn.y+s.n*sn.z,s.n);
        if(dot(s.n,s.ng)<.05)s.n=normalize(s.n+s.ng*(.05-dot(s.n,s.ng)));
        if(dot(s.dn,s.ng)<.05)s.dn=normalize(s.dn+s.ng*(.05-dot(s.dn,s.ng)));
        s.t=PtSafeNormal(s.t-s.n*dot(s.t,s.n),cross(s.b,s.n)); s.b=cross(s.n,s.t);
    }
    // An interpolated or mapped normal can lean away from the eye on a triangle that faces it
    // (a smooth body's silhouette); every lobe then evaluates to zero (nv <= 0), a black seam
    // along the contour that raster never draws. Within cos .1 of that, blend it towards the
    // triangle's normal (Falcor's adjustShadingNormal). Diffuse keeps s.dn, as raster's N.L.
    float facing=dot(s.n,-d);
    if(facing<.1) {
        s.n=PtSafeNormal(lerp(s.ng,s.n,saturate(facing*10)),s.ng);
        s.t=PtSafeNormal(s.t-s.n*dot(s.t,s.n),cross(s.b,s.n)); s.b=cross(s.n,s.t);
    }
    if(wetTongue) {
        float wet=saturate(m.scattering.w);
        s.f0=lerp(dryF0,s.f0,wet);s.specGain=lerp(dryGain,s.specGain,wet);
        s.roughness=lerp(dryRoughness,s.roughness,wet);s.roughnessWide=lerp(dryRoughness,s.roughnessWide,wet);
        s.sharpWeight=lerp(1,s.sharpWeight,wet);
    }
    if(s.flags&16777216) {
        float alpha=s.roughness*s.roughness;
        float sheen=saturate((alpha-m.skinLobe.x)/max(m.skinLobe.y-m.skinLobe.x,1e-4));
        s.specGain=max(m.skinCtl.w,0)*(1-sheen);
        s.sheen=max(m.skinCtl.w*m.skinLobe.z,0)*sheen;
    }
    if((mode&PT_MODE_ADDITIVE) && s.transmission<.999) {
        // The albedo keeps its dielectric partition only. The specular yields
        // where albedo + specular would reflect more than arrives.
        s.f0=min(s.f0*s.specGain,max(1-.96*s.base,0));s.specGain=PtMax(s.f0)>0?1:0;
        s.flags|=PT_SURFACE_ADDITIVE;
    }
    if(mode&(PT_MODE_COVERAGE_CLOSURE|PT_MODE_PREMULTIPLIED)) {
        // Stochastic coverage already weights the hit by alpha. Programs that
        // also premultiply colour and specular by it (SeparateAlpha clothing)
        // weight them by alpha again; Standard's premultiplied mode keeps its
        // specular unweighted by alpha.
        float alpha=PtOpacity(PtAlphaAt(hit.inst,hit.geo,hit.prim,hit.bary,m),old,m);
        if(mode&PT_MODE_COVERAGE_CLOSURE){s.base*=alpha;s.specGain*=alpha;s.sheen*=alpha;}
        else s.specGain/=max(alpha,.05);
    }
    if(s.flags&PT_EYEBALL)s.base*=PtEyeBand(s.p);
    if(mode&PT_MODE_MIRROR_SHEET) {
        // HighPerformanceMirrorReflection's program: colour = reflection x _ReflectStrengh x (_Freshnel (1 + dir.y)^5 + .1)
        // over the background, dir from the camera to the point: nothing is taken from what is behind (alpha 0, premultiplied).
        float rise=saturate(1+normalize(d).y),rise2=rise*rise;
        s.f0=saturate(m.materialControls.y*(m.directGain.w*rise2*rise2*rise+.1));s.specGain=1;s.mirrorSheet=1;
        s.roughness=s.roughnessWide=0;
    }
    return s;
}
float3 PtDiffuseWeight(PtSurface s,float3 spec) { return (s.flags&PT_SURFACE_ADDITIVE)?.96.xxx:1-spec; }
float3 PtFresnel(float3 f0,float cosTheta) {
    float x=1-saturate(cosTheta),x2=x*x;
    return f0+(1-f0)*x2*x2*x;
}
float3 PtSpecAlbedo(PtSurface s,float3 v) {
    float nv=saturate(dot(s.n,v));
    float3 result=0;
    [unroll] for(uint i=0;i<2;i++) {
        float r=i?s.roughnessWide:s.roughness;
        float4 q=r*float4(-1,-.0275,-.572,.022)+float4(1,.0425,1.04,-.04);
        float a=min(q.x*q.x,exp2(-9.28*nv))*q.x+q.y;
        float2 ab=float2(-1.04,1.04)*a+q.zw;
        result+=(s.f0*ab.x+ab.y)*(i?1-s.sharpWeight:s.sharpWeight);
    }
    result*=s.specGain;
    if(s.flags&64)result*=s.hairReflection;
    if(s.flags&16777216)result+=s.f0*s.sheen*(.5*pow(1-nv,2));
    return saturate(result);
}
float PtG1(float alpha,float noV) {
    return 2*noV/max(noV+sqrt(alpha*alpha+(1-alpha*alpha)*noV*noV),1e-6);
}
float PtD(float alpha,float noH) {
    float a2=alpha*alpha,d=(noH*a2-noH)*noH+1;
    return a2/max(PT_PI*d*d,1e-10);
}
float PtCorrelatedVisibility(float alpha,float nv,float nl) {
    float a2=alpha*alpha;
    return .5/max(nv*sqrt(nl*nl*(1-a2)+a2)+nl*sqrt(nv*nv*(1-a2)+a2),1e-6);
}
float PtDielectric(float cosine,float eta) {
    float ci=saturate(abs(cosine));
    float st2=(1-ci*ci)/(eta*eta);
    if(st2>=1)return 1;
    float ct=sqrt(1-st2);
    float rs=(ci-eta*ct)/max(ci+eta*ct,1e-8);
    float rp=(eta*ci-ct)/max(eta*ci+ct,1e-8);
    return .5*(rs*rs+rp*rp);
}
float3 PtInterfaceFresnel(PtSurface s,float cosine) {
    if(s.mirrorSheet)return s.f0;
    if((s.flags&2048) && (s.flags&(33554432|67108864)))
        return saturate(PtFresnel(s.f0,abs(cosine))*s.specGain);
    return PtDielectric(cosine,s.eta);
}
#include "path_hair_bsdf.hlsli"
#include "path_skin_bssrdf.hlsli"
float3 PtOffset(PtSurface s,float3 direction) {
    if(s.flags&64) {
        float3 outward=PtSafeNormal(direction-s.t*dot(direction,s.t),s.ng);
        return s.hairCenter+outward*(s.hairRadius+1e-6)+direction*1e-6;
    }
    bool reflecting=s.transmission<.999 && !(s.flags&(2048|1048576|2097152));
    bool front=dot(direction,s.ng)>0 || (reflecting && max(dot(direction,s.n),dot(direction,s.dn))>0);
    float3 origin=s.p;
    if(front && dot(s.liftNormal,s.ng)>0) {
        float ngl=dot(s.liftNormal,direction),nl=dot(s.n,direction);
        origin+=s.lift*(nl<.1?saturate(2-(ngl+nl)*10):saturate(1-ngl*10));
    }
    return origin+s.ng*((front?1:-1)*max(1e-4,1e-5*length(s.p)));
}
float PtSpecChance(PtSurface s,float3 v) {
    float spec=PtLuma(saturate(PtFresnel(s.f0,dot(s.n,v))*s.specGain+s.f0*s.sheen));
    float diff=PtLuma(s.base)*(1-s.transmission);
    return clamp(spec/max(spec+diff,1e-6),.05,.95);
}
float3 PtEvalComponents(PtSurface s,float3 v,float3 l,out float pdf,out float3 diffusePart) {
    diffusePart=0;
    if((s.flags&(1048576|2097152)) || s.mirrorSheet){pdf=0;return 0;}
    if(s.flags&64)return PtFiberEval(s,v,l,pdf);
    if(s.flags&128) {
        float nl=saturate(dot(s.n,l));pdf=nl/PT_PI;
        // Normalized Schlick boundary transmission. Its cosine-weighted
        // hemispherical integral is (1-F0)*20/21, so no energy is invented.
        diffusePart=s.base*(1-PtFresnel(s.f0,nl))/max((1-s.f0)*(20.0/21),1e-6)*pdf;
        return diffusePart;
    }
    float nv=saturate(dot(s.n,v)),signedNl=dot(s.n,l),nl=abs(signedNl);
    if(s.transmission>=.999) {
        pdf=0;
        if(nv<=0||nl<=0)return 0;
        bool reflected=signedNl>0;
        if((s.flags&2048) && !reflected)return 0; // thin-sheet transmission is a delta event
        float3 h=PtSafeNormal(v+l*(reflected?1:s.eta),s.n);
        if(dot(h,s.n)<0)h=-h;
        float vh=dot(v,h),lh=dot(l,h);
        if(vh<=0 || lh*signedNl<=0)return 0;
        float a=s.roughness*s.roughness,D=PtD(a,saturate(dot(s.n,h))),g1=PtG1(a,nv);
        float3 F=PtInterfaceFresnel(s,vh);
        float probability=PtLuma(F),microPdf=D*g1*vh/max(nv,1e-8);
        if(reflected) {
            pdf=microPdf*probability/max(4*vh,1e-8);
            return F*D*g1*PtG1(a,nl)/max(4*nv,1e-8);
        }
        float den=lh+vh/s.eta;den=max(den*den,1e-12);
        pdf=microPdf*(1-probability)*abs(lh)/den;
        // Radiance transport includes the refraction solid-angle Jacobian.
        return s.base*(1-F)*D*g1*PtG1(a,nl)*abs(lh*vh)/(max(nv,1e-8)*den*s.eta*s.eta);
    }
    nl=saturate(signedNl);
    float dnl=saturate(dot(s.dn,l)),dnv=saturate(dot(s.dn,v));
    if(nv<=0||(nl<=0&&dnl<=0)) { pdf=0; return 0; }
    float3 h=normalize(v+l); float nh=saturate(dot(s.n,h)),vh=saturate(dot(v,h));
    float2 a=float2(s.roughness*s.roughness,s.roughnessWide*s.roughnessWide);
    float2 D=float2(PtD(a.x,nh),PtD(a.y,nh)),g1=float2(PtG1(a.x,nv),PtG1(a.y,nv));
    float3 F=saturate(PtFresnel(s.f0,vh)*s.specGain);
    float ps=PtSpecChance(s,v);
    float specPdf=nl>0?lerp(D.y*g1.y,D.x*g1.x,s.sharpWeight)/max(4*nv,1e-6):0;
    float specBrdf=lerp(D.y*PtCorrelatedVisibility(a.y,nv,nl),D.x*PtCorrelatedVisibility(a.x,nv,nl),s.sharpWeight);
    // With no diffuse bounce left the continuation samples the specular lobes alone:
    // that branch is the only BSDF density light samples are weighed against.
    pdf=s.diffuseStop?ps*specPdf:lerp(dnl/PT_PI,specPdf,ps);
    float fd90=.5+2*s.roughness*vh*vh;
    float diffuse=lerp(1,(1+(fd90-1)*pow(1-dnl,5))*(1+(fd90-1)*pow(1-dnv,5)),s.grazing);
    float sheen=s.sheen*(4*max(1-nh*nh,0)/(2*PT_PI))/(4*max(nl+nv-nl*nv,1e-4));
    float3 sheenColor=sheen;
    if(s.flags&16777216) {
        // Fabric is a dielectric with a rough micro-fiber sheen, not skin and
        // not generic glossy paint. The same closure feeds all PT proposals.
        float alpha=max(s.roughness*s.roughness,.02),inv=1/alpha;
        float Dsheen=(2+inv)*pow(max(1-nh*nh,0),.5*inv)/(2*PT_PI);
        sheenColor=s.f0*s.sheen*Dsheen/(4*max(nl+nv-nl*nv,1e-4));
    }
    // Lambert diffuse plus microfacet reflection. Energy is partitioned by F,
    // or by a dielectric's own F where the source program adds its specular.
    float3 partition=(s.flags&PT_SURFACE_ADDITIVE)?PtFresnel(.04,vh):F;
    diffusePart=(1-partition)*s.base*(1-s.transmission)*diffuse*dnl/PT_PI;
    return diffusePart+(F*specBrdf+sheenColor)*nl;
}
float3 PtEval(PtSurface s,float3 v,float3 l,out float pdf) {
    float3 unused;return PtEvalComponents(s,v,l,pdf,unused);
}
float3 PtVndf(float3 v,float alpha,float2 u) {
    float3 vh=normalize(float3(alpha*v.xy,v.z));
    float lensq=dot(vh.xy,vh.xy);
    float3 t1=lensq>0?float3(-vh.y,vh.x,0)*rsqrt(lensq):float3(1,0,0),t2=cross(vh,t1);
    float r=sqrt(u.x),phi=2*PT_PI*u.y,p1=r*cos(phi),p2=r*sin(phi),s=.5*(1+vh.z);
    p2=(1-s)*sqrt(max(0,1-p1*p1))+s*p2;
    float3 n=p1*t1+p2*t2+sqrt(max(0,1-p1*p1-p2*p2))*vh;
    return normalize(float3(alpha*n.xy,max(n.z,0)));
}
bool PtSample(PtSurface s,float3 v,inout uint seed,out float3 d,out float3 weight,out bool specular,out float pdf,out float3 share) {
    share=0;
    if(s.flags&(1048576|2097152)){d=-v;weight=s.base;specular=false;pdf=0;return true;}
    if(s.flags&64){specular=true;return PtFiberSample(s,v,seed,d,weight,pdf);}
    if(s.mirrorSheet) {
        // A delta choice between its added reflection and the background, which the sheet does not dim.
        float reflects=PtLuma(s.f0);specular=true;pdf=0;
        if(Rand(seed)<reflects){d=reflect(-v,s.n);weight=s.f0/max(reflects,1e-8);return dot(d,s.ng)>0;}
        d=-v;weight=s.base/max(1-reflects,1e-8);return true;
    }
    float branch=Rand(seed),ps=PtSpecChance(s,v);
    float2 u=float2(Rand(seed),Rand(seed));
    if(s.transmission>=.999) {
        float3 vl=float3(dot(v,s.t),dot(v,s.b),dot(v,s.n));
        float3 localH=PtVndf(vl,s.roughness*s.roughness,u);
        float3 h=s.t*localH.x+s.b*localH.y+s.n*localH.z;
        float3 F=PtInterfaceFresnel(s,dot(v,h));float probability=PtLuma(F);
        bool reflection=branch<probability;
        if(reflection)d=reflect(-v,h);
        else if(s.flags&2048) { d=-v;weight=s.base*(1-F)/max(1-probability,1e-8);specular=true;pdf=0;return true; }
        else d=refract(-v,h,1/s.eta);
        specular=true;
        float3 fc=PtEval(s,v,d,pdf);
        weight=pdf>1e-9?fc/pdf:0;
        return pdf>1e-9 && dot(d,s.ng)*(reflection?1:-1)>0 && all(isfinite(weight));
    }
    specular=branch<ps;
    if(!specular && s.diffuseStop){d=0;weight=0;pdf=0;return false;} // no diffuse bounce left: the path ends
    if(specular) {
        float3 vl=float3(dot(v,s.t),dot(v,s.b),dot(v,s.n));
        float rough=Rand(seed)<s.sharpWeight?s.roughness:s.roughnessWide;
        float3 h=PtVndf(vl,rough*rough,u);
        d=reflect(-v,s.t*h.x+s.b*h.y+s.n*h.z);
    } else {
        float r=sqrt(u.x),phi=2*PT_PI*u.y;
        float3 tx,ty;Basis(s.dn,tx,ty);
        d=normalize(tx*(r*cos(phi))+ty*(r*sin(phi))+s.dn*sqrt(1-u.x));
    }
    float3 diffusePart;float3 fc=PtEvalComponents(s,v,d,pdf,diffusePart);
    if(s.diffuseStop){fc-=diffusePart;diffusePart=0;} // the diffuse lobe took its light from the light samples
    weight=pdf>1e-9?fc/pdf:0;share=saturate(diffusePart/max(fc,1e-8));
    return pdf>1e-9 && dot(d,s.ng)>0 && all(isfinite(weight));
}
float PtMis(float sampled,float other) {
    // Ratio form avoids overflow for tiny, bright emitters far from a surface.
    if(sampled<=0)return 0;
    float ratio=other/sampled;return 1/(1+ratio*ratio);
}
// The specular lobes' own proposal (PtSample's specular branch without its branch choice): a direction and its
// solid-angle pdf. It finds a light disc wider than the lobe, where a point drawn over the disc rarely lands in it.
bool PtSpecLobeSample(PtSurface s,float3 v,inout uint seed,out float3 l) {
    l=0;if(dot(s.n,v)<=0)return false;
    float3 vl=float3(dot(v,s.t),dot(v,s.b),dot(v,s.n));
    float pick=Rand(seed);float rough=s.transmission>=.999 || pick<s.sharpWeight?s.roughness:s.roughnessWide;
    float3 h=PtVndf(vl,rough*rough,float2(Rand(seed),Rand(seed)));
    l=reflect(-v,s.t*h.x+s.b*h.y+s.n*h.z);
    return dot(l,s.ng)>0 && dot(l,s.n)>0;
}
float PtSpecLobePdf(PtSurface s,float3 v,float3 l) {
    float nv=dot(s.n,v),nl=dot(s.n,l);if(nv<=0||nl<=0)return 0;
    float nh=saturate(dot(s.n,normalize(v+l))),ax=s.roughness*s.roughness;
    if(s.transmission>=.999)return PtD(ax,nh)*PtG1(ax,nv)/(4*nv);
    float ay=s.roughnessWide*s.roughnessWide;
    return lerp(PtD(ay,nh)*PtG1(ay,nv),PtD(ax,nh)*PtG1(ax,nv),s.sharpWeight)/(4*nv);
}
// A light seen as a disc of radius R at distance D across c (R / D: its angular tangent). A point drawn uniformly
// over its area; that direction's solid-angle pdf is D^2 / (pi R^2 cos^3), 0 off the disc.
float3 PtDiscPoint(float3 c,float D,float R,inout uint seed) {
    float3 tx,ty; Basis(c,tx,ty);
    float r=sqrt(Rand(seed)),phi=2*PT_PI*Rand(seed);
    float3 offset=tx*(r*cos(phi))+ty*(r*sin(phi));
    return c*D+offset*R;
}
float PtDiscPdf(float3 c,float D,float R,float3 d) {
    float cosT=dot(d,c);if(!(R>0) || cosT<=0 || (1-cosT*cosT)*D*D>R*R*cosT*cosT)return 0;
    return D*D/(PT_PI*R*R*cosT*cosT*cosT);
}
// mask: 0x01 environment / emitters (every ray but the camera's); analytic lights
// 0x04 plus the light-link classes they light (0x08 << class, rtcore.cpp instance masks).
float3 PtVisibility(float3 o,float3 d,float distance,bool thinTransmission,uint mask) {
    RayDesc ray;ray.Origin=o;ray.Direction=d;ray.TMin=1e-6;ray.TMax=max(distance-1e-5,1e-6);
    // Opaque triangle/fiber hits terminate in RT hardware. Only genuine alpha
    // or transmissive materials enter the shader, where coverage is required.
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    // Analytic lights (thin) are blocked by shadow-only proxies as well; the
    // environment/emitter NEE sees exactly what its BSDF continuation sees.
    // API 26: Unity's shadow map draws the faces looking at the light: leaving a surface for the light a shadow ray
    // meets those from behind. A one-sided shell does not stop a light outside it; a closed mesh shadows the same.
    q.TraceRayInline(gPathScene,thinTransmission&&(PtAuthor()&PT_AUTHOR_CULL)?RAY_FLAG_CULL_FRONT_FACING_TRIANGLES:RAY_FLAG_NONE,mask,ray);
    float3 visibility=1;
    while(q.Proceed()) {
        uint inst=q.CandidateInstanceID(),geo=q.CandidateGeometryIndex(),prim=q.CandidatePrimitiveIndex();
        uint mat=LoadSub(LoadGeo(inst).firstSub+geo).material;
        MatGpu m=LoadMat(mat);PtMaterial pm=gBuf[pB].Load<PtMaterial>(mat*PT_MATERIAL_BYTES);
        // Cosmetic layers sit on an existing receiver: their own geometry is
        // not another occluder. Physical glass still transmits shadow rays.
        if((LoadGeo(inst).flags&16) && (pm.params.z<.999 || (asuint(pm.params.w)&1048576)))continue;
        float a=PtAlphaAt(inst,geo,prim,q.CandidateTriangleBarycentrics(),pm);
        if((m.kind==1 || (asuint(pm.params.w)&8388608)) && PtOpacity(a,m,pm)<(m.kind==1?m.color.a:pm.materialControls.w))continue;
        if(asuint(pm.params.w)&2097152)continue;
        if(asuint(pm.params.w)&1048576) {
            visibility*=1-PtOpacity(a,m,pm);
        } else if(asuint(pm.params.w)&2048) {
            // Analytic lights have no hittable surface: straight-through attenuation is their visibility. The
            // environment / emitter samples pass a sheet the same way, and a continuation through one keeps the pdf
            // of the vertex below (PtTracePixel): the two are weighed against each other. Stopped here (0.22.66), an
            // eye under its film had no sky at all with no diffuse bounce left.
            float coverage=(asuint(pm.params.w)&16384)?PtOpacity(a,m,pm):1;
            if(!thinTransmission && !(PtTest()&PT_TEST_SHEET_NEE)) {visibility*=1-coverage;if(PtMax(visibility)<1e-5)return 0;continue;}
            Hit h=(Hit)0;h.inst=inst;h.geo=geo;h.prim=prim;h.bary=q.CandidateTriangleBarycentrics();h.t=q.CandidateTriangleRayT();
            PtSurface glass=PtShading(h,o,d,0);
            visibility*=lerp(1.0,glass.base*(glass.mirrorSheet?1:1-PtInterfaceFresnel(glass,dot(glass.n,d))),coverage);
        } else if(m.kind==3 && pm.params.z<.999) {
            float opacity=PtOpacity(a,m,pm);
            // API 26: a scene object Unity's shadow map takes as a cutout: its shadow is whole, however sheer it looks.
            if(thinTransmission && (m.hasAlpha&4u) && (PtAuthor()&PT_AUTHOR_SOLID))opacity=opacity>=(m.hasAlpha>>16)/65535.0?1:0;
            visibility*=1-opacity;
        }
        else return 0;
        if(PtMax(visibility)<1e-5)return 0;
    }
    if(q.CommittedStatus()!=COMMITTED_NOTHING)return 0;
    return visibility;
}
// An eyeball sits in the fold of its lids (bit 25). VaM's point, spot and directional lights are effects, small
// stand-ins for broad light and the room's bounces; most of that light never reaches the eyeball's top under the upper
// lid, nor its corners. Their diffuse light on it is scaled by the share of its cosine hemisphere open within 12 mm:
// the lids, the lashes (by their coverage), hair; the film and the cornea let it through (kNoOcclude). Four rays a hit,
// stratified in elevation. 1 for every other surface, and where nothing is that close (an eyeball's open middle).
// Their highlights keep their own shadow ray (exact for the one reflected direction); the environment and emitters
// their own sampled occlusion.
static const float PT_LID_RANGE=.012;
float PtLidLight(PtSurface s,inout uint seed) {
    if(!(PtTest()&PT_TEST_LID) || !(s.flags&PT_EYEBALL))return 1;
    float3 tx,ty;Basis(s.ng,tx,ty);
    float open=0;
    [unroll] for(uint i=0;i<4;i++) {
        float r=sqrt((i+Rand(seed))*.25),phi=2*PT_PI*Rand(seed);
        float3 d=normalize(tx*(r*cos(phi))+ty*(r*sin(phi))+s.ng*sqrt(max(1-r*r,0)));
        open+=PtLuma(PtVisibility(PtOffset(s,d),d,PT_LID_RANGE,false,0x01u));
    }
    return open*.25;
}
// Streaming RIS across independent light-family proposals. Conditional on
// those proposals, E[C_j * V_j / p_j] = sum(C_i * V_i). A single visibility
// query replaces up to three, without deleting any light or changing its MIS.
// Per-pixel light visibility (11-07, test bit 31; user: RR's static dark speckles, absent from the accumulated PT).
// One shadow ray serves the reservoir, picked by unshadowed brightness: indoors the sun and the sky behind the walls
// won most picks and were blocked - half or more of a surface's frames were exactly 0, its light in a rare bright
// frame. The mean was right; RR, still, did not reach it. At the camera's first surface each pixel keeps, a byte a
// slot, how often a pick from each source was visible (analytic lights 0-8, the last shared; sky 9; indoor probe 10;
// emitters 11) and weighs that source's proposals by it next frame. Only the proposal weights change (any positive
// target is unbiased RIS); floor .05. Moved pixels start again at "visible" (as before).
static const uint PT_TEST_VIS_RIS=1u<<31;
static bool gVisUse=false;static uint3 gVis=0xffffffffu;static uint gVisSlot=0;
static bool gVisValid=false,gVisSeen=false;static uint gVisLast=0;
float PtVisRate(uint slot) {
    if(!gVisUse)return 1;
    uint word=slot<4?gVis.x:slot<8?gVis.y:gVis.z;
    return max(float((word>>((slot&3)*8))&255u)/255.0,.05);
}
void PtVisUpdate() {
    if(!gVisUse || !gVisValid)return;
    uint shift=(gVisLast&3)*8,word=gVisLast<4?gVis.x:gVisLast<8?gVis.y:gVis.z;
    uint b=(word>>shift)&255u;b=(b*7+(gVisSeen?255u:0u)+4)/8;
    word=(word&~(255u<<shift))|(b<<shift);
    if(gVisLast<4)gVis.x=word;else if(gVisLast<8)gVis.y=word;else gVis.z=word;
}
struct PtDirectReservoir {
    float3 normalized,origin,direction;
    float distance,total;
    uint thin,mask,slot;
    float3 unshadowed;
    float3 transport;
#ifdef PT_RESPONSE
    float3 responseSpec,specNormalized,specUnshadowed;
#endif
};
// Separately scaled diffuse and specular responses: lights' direct gain, or
// the environment's diffuse and specular radiance.
float3 PtDirectEvalLobes(PtSurface s,float3 v,float3 d,float3 diffuseScale,float3 specScale,out float pdf,inout PtDirectReservoir r,out float3 diffuseOut) {
    float3 diffuse;float3 value=PtEvalComponents(s,v,d,pdf,diffuse);
    float3 spec=(value-diffuse)*specScale;diffuse*=diffuseScale;
#ifdef PT_RESPONSE
    r.responseSpec=saturate(spec/max(diffuse+spec,1e-8));
#endif
    diffuseOut=diffuse;
    return diffuse+spec;
}
float3 PtDirectEval(PtSurface s,float3 v,float3 d,out float pdf,inout PtDirectReservoir r,out float3 diffuse) {
    float3 value=PtEvalComponents(s,v,d,pdf,diffuse);
#ifdef PT_RESPONSE
    r.responseSpec=saturate((value-diffuse)/max(value,1e-8));
#endif
    return value;
}
void PtDirectCandidate(inout PtDirectReservoir r,float3 contribution,float3 origin,float3 direction,
    float distance,bool traced,bool thin,uint mask,inout uint seed) {
    contribution*=r.transport;
#ifdef PT_RESPONSE
    float3 spec=contribution*r.responseSpec;
    if(!traced)r.specUnshadowed+=spec;
#endif
    if(!traced){r.unshadowed+=contribution;return;}
    float w=PtLuma(contribution)*PtVisRate(gVisSlot);if(!(w>0))return;
    r.total+=w;
    if(Rand(seed)*r.total<w) {
        r.normalized=contribution/w;r.origin=origin;r.direction=direction;r.distance=distance;r.thin=thin;r.mask=mask;r.slot=gVisSlot;
#ifdef PT_RESPONSE
        r.specNormalized=spec/w;
#endif
    }
}
// The author's falloff 1/(1+25(d/range)^2), windowed to zero over the last
// fifth of the range (Unity fades there too) instead of a hard cut at the
// range, which drew a bright ring on walls; the interior is unchanged.
// Unity's forward spot: its default cookie 'Soft' x its falloff, measured in the game (a white plane under a spot alone,
// angles 60-160, docs\pt-author-intent-20261109\measure-121324): 1/(1+25 q) with q = (distance/range)^2, fading from q .64.
// r = tan(angle) / tan(half cone). The cookie as read from the game (128x128, alpha along its centre row): 1 to r .63, .52
// at .82, 0 at .99: a narrower pool than the cone, the circle authors compose with. The table: the texture, one value per
// 1/24 of r .625-1.
static const float PT_SPOT_COOKIE[25]={1,.996,.99,.975,.958,.93,.895,.855,.805,.745,.68,.61,.535,.46,.385,.31,.245,.185,.135,.097,.066,.041,.016,.002,0};
float PtSpotUnity(LightGpu L,float cosine,float dist) {
    if(cosine<=1e-4)return 0;
    float c=clamp(L.spotCos,.01,.9999),r=sqrt(max(1-cosine*cosine,0))/cosine*c/sqrt(1-c*c);
    float x=dist/max(L.range,1e-4),q=x*x;
    if(q>=1 || r>=1)return 0;
    float at=saturate((r-.625)/.375)*24;uint i=min((uint)at,23u);
    return lerp(PT_SPOT_COOKIE[i],PT_SPOT_COOKIE[i+1],at-i)*smoothstep(1,.64,q)/(1+25*q);
}
float PtLightFalloff(LightGpu L,float3 p,out float3 toLight,out float dist) {
    float att=LightFalloff(L,p,toLight,dist);
    if(L.type==1 && (PtAuthor()&PT_AUTHOR_SPOT))return PtSpotUnity(L,dot(-toLight,L.direction),dist);
    if(L.type!=0) {float x=dist/max(L.range,1e-4);att*=smoothstep(1,.64,x*x);}
    return att;
}
// along: the distance to the sampled point of a point/spot disc.
void PtLightCandidate(inout PtDirectReservoir reservoir,PtSurface s,LightGpu light,float3 contribution,float3 dir,
    float along,float distance,inout uint seed) {
    float3 origin=PtOffset(s,dir);
    float3 shadowVector=s.p+dir*(light.type==0?distance:along)-origin;
    // Full PT always tests visibility. Unity's shadow-map/per-light budget
    // switches belong to the legacy renderer and must not leak light through rooms.
    // Nor do the objects it leaves out shadow it.
    uint mask=0x04u|((~light.excludedLinks&0x1fu)<<3);
    float toLight=length(shadowVector);float3 toward=shadowVector/max(toLight,1e-8);
    uint rules=PtAuthor();
    if(rules) {
        // Near plane: a light inside its fixture is not blocked by it.
        if((rules&PT_AUTHOR_NEAR) && light.type!=0)toLight=max(toLight-((light.traced>>8)&0xfffu)/1024.0,1e-4);
        // No shadow in Unity (the light, or this surface): the shape stays with contact shadows, what is farther lets
        // the light by. The range is drawn so that no edge marks where it ends.
        if(((rules&PT_AUTHOR_NO_SHADOW) && !(light.traced>>31)) || ((rules&PT_AUTHOR_NO_RECEIVE) && s.noReceive))
            toLight=min(toLight,PtAuthorContact()*(.5+.5*Rand(seed)));
    }
    PtDirectCandidate(reservoir,contribution,origin,toward,toLight,true,true,mask,seed);
}
// An effect light's size on what is not the person. Its 8 degrees (0.22.64) are for the person's own shape; on a wall 5 m
// away they are a 70 cm source, and the shadow an author throws there (one stage's giant silhouette) melts away.
// VaM reads shadowStrength as this blur on every surface and never as depth (11-09, a Standard wall at 0, .166 and 1: as
// black, only the edge widens): 25 taps over +-.025 strength of the shadow map's UV, which spans 2 tan(half cone) per
// metre from a spot; a point light's blur is (1 - strength) x .1 m. This disc gives that edge for what stands about
// 1 m from the light.
float PtAuthorShadowRadius(LightGpu L) {
    float strength=(L.traced&255u)/255.0;
    if(L.type==1){float c=clamp(L.spotCos,.01,.9999);return max(.05*strength*sqrt(1-c*c)/c,.001);}
    return max(.03*(1-strength),.001);
}
// The direct diffuse of VaM's Subsurface family (bit 23): 2(1-S/2) saturate(S/2+(1-S/2)N.L) ((N.L+1)/2)^2 per channel,
// S its _SubdermisColor, against (2-S) N.L: as bright head-on, darker as it turns from the light (.56 of it at N.L .5
// when S is 0: an eyeball's sides by its corners). The scale on PtEvalComponents' diffuse (N.L) for one light
// direction. Its skin (the skin model), fabric, sheets, fibres and layers light as their own; light from behind,
// which S wraps round in raster, stays out.
float3 PtDirectScale(PtSurface s,float3 l) {
    float3 gain=1+s.directExtra;
    if(!(PtTest()&PT_TEST_FAMILY_DIRECT) || !(s.flags&33554432) ||
        (s.flags&(8|64|128|1024|2048|16777216|1048576|2097152)) || s.transmission>=.999)return gain;
    float nl=saturate(dot(s.dn,l));if(nl<=0)return gain;
    float3 S=saturate(2-gain);float fall=(nl+1)*.5;
    return gain*saturate(S*.5+(1-S*.5)*nl)*(fall*fall/nl);
}
void PtAnalyticCandidate(PtSurface s,float3 v,float lid,inout uint seed,inout PtDirectReservoir reservoir) {
    // One light sampled by estimated incident power, including a PDF division.
    // The light count does not multiply the number of visibility rays.
    // Weighted reservoir needs constant storage even for many lights.
    float total=0,chosenWeight=0; LightGpu light=(LightGpu)0;uint chosen=0;
    uint count=pCount;
    [loop] for(uint i=0;i<count;i++) {
        LightGpu candidate=gBuf[pLight].Load<LightGpu>(i*64);
        // Unity's cullingMask: a light that leaves this object's layer out does not light it.
        if(candidate.excludedLinks&s.link)continue;
        float3 d; float dist; float fall=PtLightFalloff(candidate,s.p,d,dist);
        float weight=max(PtLuma(candidate.color),0)*fall*PtVisRate(min(i,8u));
        total+=weight;
        if(weight>0 && Rand(seed)*total<weight) { light=candidate;chosenWeight=weight;chosen=min(i,8u); }
    }
    if(total<=0)return;
    gVisSlot=chosen;
    float3 center; float distance;
    float fall=PtLightFalloff(light,s.p,center,distance),txPdf=chosenWeight/total;
    // A point/spot disc: radius metres plus tanAngular per metre of distance.
    // VaM's authors place these lights as effects (Unity's falloff follows the
    // range, not the distance); the host gives them a constant apparent size.
    float D=light.type==0?1:distance,R=light.type==0?light.tanAngular:light.radius+light.tanAngular*distance;
    if(light.type!=0 && (PtAuthor()&PT_AUTHOR_CRISP) && !s.person && !(s.flags&(8|64)))R=min(R,PtAuthorShadowRadius(light));
    float3 sampleVector=PtDiscPoint(center,D,R,seed);
    float3 dir=normalize(sampleVector);
    // VaM analytic light units are irradiance/pi. The pi restores BRDF units.
    float3 irradiance=light.color*(fall*PT_PI/max(txPdf,1e-8));
    // VaM's family factor x2(1-S/2) (and its falloff) multiplies only direct diffuse light; so does an eyeball's lids'.
    float pdf;float3 diffusePart; float3 fcos=PtDirectEvalLobes(s,v,dir,PtDirectScale(s,dir)*lid,1,pdf,reservoir,diffusePart);
    // A specular lobe narrower than the disc (a wet cornea, a mirror, glass) is hardly ever met by a point drawn
    // over it. A draw from the lobe that lands on the disc meets it: power heuristic between the two, in the same
    // reservoir (one shadow ray). Wider lobes keep the disc draw alone; fibres theirs.
    bool lobes=(PtTest()&PT_TEST_LOBE_LIGHT) && R>0 && !(s.flags&(64|128|1048576|2097152)) &&
        2*s.roughness*s.roughness<R/D && (s.transmission>=.999 || PtMax(s.f0)*s.specGain>0);
    if(!lobes) {
        if(PtMax(fcos)>0)PtLightCandidate(reservoir,s,light,irradiance*fcos,dir,length(sampleVector),distance,seed);
        return;
    }
    float3 first=diffusePart+(fcos-diffusePart)*PtMis(PtDiscPdf(center,D,R,dir),PtSpecLobePdf(s,v,dir));
    if(PtMax(first)>0) {
#ifdef PT_RESPONSE
        reservoir.responseSpec=saturate((first-diffusePart)/max(first,1e-8));
#endif
        PtLightCandidate(reservoir,s,light,irradiance*first,dir,length(sampleVector),distance,seed);
    }
    float3 d2;
    if(PtSpecLobeSample(s,v,seed,d2)) {
        float p=PtDiscPdf(center,D,R,d2),q=PtSpecLobePdf(s,v,d2);
        if(p>0 && q>0) {
            float3 spec=PtDirectEvalLobes(s,v,d2,PtDirectScale(s,d2),1,pdf,reservoir,diffusePart)-diffusePart;
            if(PtMax(spec)>0) {
#ifdef PT_RESPONSE
                reservoir.responseSpec=1;
#endif
                // The disc's radiance in d2 is irradiance x its pdf there (what a disc draw estimates).
                PtLightCandidate(reservoir,s,light,irradiance*spec*(p/q)*PtMis(q,p),d2,D/dot(d2,center),distance,seed);
            }
        }
    }
}
// Light through thin skin (11-10, on both routes): the existing lighting's
// transmission (the skin component's shaders\transmission.hlsli) at the skin the eye meets first. A light behind it
// comes through ears, nostrils and fingers: its incident light (VaM's units, as raster's lightColor x atten) x
// max(N.-L, 0) x [General] Strength x the albedo x a Gaussian per colour over the path through the body, sigma
// 2.5 / 1.45 / 0.85 mm (red widest), x raster's low-light fade. One light from behind, by incident power x facing. The
// path is one short ray from just under the surface toward the light: the first surface within 20 mm of anything
// that shadows that light (the far side of an ear; an eyeball or other fingers in the way make the path end there and
// the light beyond is then stopped by the body). No surface that close: thick, nothing. The light beyond the far side is
// one more candidate of this point's reservoir: no shadow ray of its own. Diffuse light (no specular share).
static const float3 PT_THIN_SIGMA_MM=float3(2.5,1.45,.85);
static const float PT_THIN_RANGE=.02,PT_THIN_UNDER=1e-4;
void PtThinCandidate(PtSurface s,inout uint seed,inout PtDirectReservoir reservoir) {
    if(!(s.thinStrength>0))return;
    float total=0,chosenWeight=0;LightGpu light=(LightGpu)0;uint chosen=0;
    uint count=pCount;
    [loop] for(uint i=0;i<count;i++) {
        LightGpu candidate=gBuf[pLight].Load<LightGpu>(i*64);
        if(candidate.excludedLinks&s.link)continue;
        float3 d;float dist;float fall=PtLightFalloff(candidate,s.p,d,dist);
        float weight=max(PtLuma(candidate.color),0)*fall*saturate(-dot(s.dn,d));
        total+=weight;
        if(weight>0 && Rand(seed)*total<weight){light=candidate;chosenWeight=weight;chosen=min(i,8u);}
    }
    if(total<=0)return;
    float3 center;float distance;float fall=PtLightFalloff(light,s.p,center,distance);
    float D=light.type==0?1:distance,R=light.type==0?light.tanAngular:light.radius+light.tanAngular*distance;
    float3 sampleVector=PtDiscPoint(center,D,R,seed);
    float3 dir=normalize(sampleVector);
    float facing=saturate(-dot(s.dn,dir));
    if(!(facing>0) || dot(dir,s.ng)>-1e-3)return; // the path goes into the body
    uint mask=0x04u|((~light.excludedLinks&0x1fu)<<3);
    RayDesc ray;ray.Origin=s.p-s.ng*PT_THIN_UNDER;ray.Direction=dir;ray.TMin=0;ray.TMax=PT_THIN_RANGE;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;q.TraceRayInline(gPathScene,RAY_FLAG_NONE,mask,ray);q.Proceed();
    if(q.CommittedStatus()==COMMITTED_NOTHING)return;
    float exitT=q.CommittedRayT();
    float3 x=(exitT+PT_THIN_UNDER)*1000/PT_THIN_SIGMA_MM;float3 through=exp(-.5*x*x);
    if(!(through.r>1e-3))return;
    float3 incident=light.color*fall;
    float low=smoothstep(0,.06,PtLuma(incident));
    float3 contribution=incident*(facing*s.thinStrength*low*total/chosenWeight)*through*s.base;
    float along=light.type==0?distance:length(sampleVector);
    float toLight=max(along-exitT-PT_THIN_UNDER-1e-4,1e-4);
    uint rules=PtAuthor();
    if(rules) {
        if((rules&PT_AUTHOR_NEAR) && light.type!=0)toLight=max(toLight-((light.traced>>8)&0xfffu)/1024.0,1e-4);
        if(((rules&PT_AUTHOR_NO_SHADOW) && !(light.traced>>31)) || ((rules&PT_AUTHOR_NO_RECEIVE) && s.noReceive))
            toLight=min(toLight,PtAuthorContact()*(.5+.5*Rand(seed)));
    }
    gVisSlot=chosen;
#ifdef PT_RESPONSE
    reservoir.responseSpec=0;
#endif
    PtDirectCandidate(reservoir,contribution,ray.Origin+dir*(exitT+1e-4),dir,toLight,true,true,mask,seed);
}
// Specular-lobe radiance; diffuse-lobe radiance in `diffuse`. VaM's lit
// programs read the Skyshop cube for reflections (x _ExposureIBL.y), its SH
// for diffuse (x _ExposureIBL.x), and Unity's ambient SH for diffuse only.
// unity: the reached surface's scale of Unity's ambient (PtSurface::unityAmbient; until 0.23.4 fAmbientLM for every
// surface, so Standard objects lost the author's Unity ambient wherever VaM's unityAmbientIntensity was 0, as some stage scenes save it).
float3 PtEnvironmentLobes(float3 d,bool background,float unity,out float3 diffuse) {
    uint4 cubes=gBuf[pA].Load4(176);
    uint index=background?cubes.y:cubes.x;
    float4 scales=asfloat(gBuf[pA].Load4(240));
    float3 radiance=0;diffuse=0;
    bool skySh=(PtAuthor()&PT_AUTHOR_SKY_SH)!=0;
    // The visible sky is Skyshop's own mesh, sampled in its object space
    // (rotated by skyboxYAngle independently of the lighting sky); with it
    // hidden, the host gives scale 0 and the camera's clear colour.
    uint rows=background?400:192;
    if(index!=RTC_NONE) {
        float3 sky=float3(dot(asfloat(gBuf[pA].Load4(rows)).xyz,d),
            dot(asfloat(gBuf[pA].Load4(rows+16)).xyz,d),dot(asfloat(gBuf[pA].Load4(rows+32)).xyz,d));
        float4 c=gCube[NonUniformResourceIndex(index)].SampleLevel(gLinear,sky,0);
        float decode=((cubes.z&1u) && (!background || !(cubes.z&2u)))?dot(float3(.46533632,25.012255,49.17438),float3(c.a,c.a*c.a,c.a*c.a*c.a)):1;
        radiance=max(c.rgb*decode,0);
        if(background)radiance*=scales.y;
        else {
            // API 26: Unity's diffuse is Skyshop's SH x _ExposureIBL.x; it never reads this cube, whose scale for reflections
            // (.y) the author may have set far from .x (a scene's HDR probe at .0027 under diffuse .42: tens of times too bright).
            if(!skySh)diffuse=radiance*scales.z;
            radiance*=scales.x;
        }
    }
    if(background)return radiance+max(asfloat(gBuf[pA].Load4(448)).rgb,0);
    // Unity ambient is independent of Skyshop. Use SH only where a real cube
    // is unavailable; never add the diffuse SH of the same sky a second time.
    float4 n=float4(d,1),q=n.xyzz*n.yzzx;
    // These are diffuse irradiance/pi coefficients, already cosine-convolved.
    // Recover radiance band-wise before integrating them again: inverse cosine
    // convolution is 1, 3/2, 4 for l=0,1,2. Unity packs part of l=2 into A.w.
    float3 az=float3(fSh[3].z,fSh[4].z,fSh[5].z)/3;
    float3 a0=float3(fSh[0].w,fSh[1].w,fSh[2].w)+az;
    float3 a1=float3(dot(fSh[0].xyz,d),dot(fSh[1].xyz,d),dot(fSh[2].xyz,d));
    float3 a2=float3(dot(fSh[3],q),dot(fSh[4],q),dot(fSh[5],q))+fSh[6].rgb*(d.x*d.x-d.y*d.y)-az;
    float3 a=a0+1.5*a1+4*a2;
    float4 sq=float4(d.x*d.y,d.y*d.z,3*d.z*d.z-1,d.z*d.x);
    float3 b=float3(fSky[0].w,fSky[1].w,fSky[2].w)+1.5*float3(dot(fSky[0].xyz,d),dot(fSky[1].xyz,d),dot(fSky[2].xyz,d));
    b+=4*(float3(dot(fSky[3],sq),dot(fSky[4],sq),dot(fSky[5],sq))+fSky[6].rgb*(d.x*d.x-d.y*d.y));
    diffuse+=max(a*((PtTest()&PT_TEST_AMBIENT_CLASS)?unity:fAmbientLM)+((index==RTC_NONE||skySh)?b:0),0);
    return radiance;
}
// API 27 (docs\pt-unity-probe-20261109): an author's reflection probe. Unity's Standard family takes its indirect specular
// from unity_SpecCube0 and nowhere else (not Skyshop, not the room): one club stage is lit by nothing else where its
// lights do not reach. Roughness picks the mip the probe's own convolution stored (UnityGlossyEnvironment), the decode is
// ReflectionProbe.textureHDRDecodeValues (DecodeHDR).
float3 PtProbeRadiance(float3 d,float3 p,float perceptual) {
    float4 lo=asfloat(gBuf[pA].Load4(544)),hi=asfloat(gBuf[pA].Load4(560)),decode=asfloat(gBuf[pA].Load4(528));
    if(lo.w>0) { // BoxProjectedCubemapDirection
        float3 reach=(lerp(lo.xyz,hi.xyz,step(0,d))-p)/d;
        d=(p-asfloat(gBuf[pA].Load3(576)))+d*min(reach.x,min(reach.y,reach.z));
    }
    float4 c=gCube[NonUniformResourceIndex(gBuf[pA].Load(512))].SampleLevel(gLinear,d,perceptual*(1.7-.7*perceptual)*6);
    return decode.x*pow(max(decode.w*(c.a-1)+1,1e-4),decode.y)*max(c.rgb,0);
}
// BRDF1_Unity_PBS: surfaceReduction x gi.specular x FresnelLerp (specColor, grazingTerm, nv), linear colour space
float3 PtProbeSpecular(PtSurface s,float3 v) {
    float a=s.roughness*s.roughness,t=1-saturate(dot(s.n,v));t*=t*t*t*t;
    return PtProbeRadiance(reflect(-v,s.n),s.p,s.roughness)/(a*a+1)*lerp(s.f0,saturate(1-s.roughness+PtMax(s.f0)),t)*s.specGain;
}
float3 PtEnvironment(float3 d,bool background) {
    // the proposal covers every surface's environment: Unity's ambient at the larger of its two scales
    float3 diffuse;float3 spec=PtEnvironmentLobes(d,background,(PtTest()&PT_TEST_AMBIENT_CLASS)?max(1,fAmbientLM):fAmbientLM,diffuse);
    return background?spec:spec+diffuse; // proposal density only
}
#include "path_lighting.hlsli"
float4 PtDetailReference(PtSurface s,float3 spec,float3 transmission,float3 reflected) {
    // Only material colour may be remodulated after reconstruction. An
    // unshadowed lighting reference fabricates highlights in shadow and applies
    // the normal map twice. Reflection/normal detail belongs in the estimator
    // and RR guides, never in an output-space lighting multiplier. Reflections
    // off clear layers in front (a cornea) are not this material's colour.
    float3 diff=s.base*PtDiffuseWeight(s,spec)*(1-s.transmission);
    float fraction=PtLuma(transmission*diff)/max(PtLuma(reflected+transmission*(diff+spec+s.emission)),1e-6);
    return float4(s.base,saturate(fraction));
}
float3 PtPreviousPosition(Hit hit,PtSurface s,out bool valid) {
    uint4 table=gBuf[pA].Load4(352);valid=false;
    if(table.x==RTC_NONE || hit.inst>=table.y)return s.p;
    uint at=hit.inst*96;uint4 record=gBuf[table.x].Load4(at);
    if(record.y==0)return s.p;
    GeoGpu geo=LoadGeo(hit.inst);SubGpu sub=LoadSub(geo.firstSub+hit.geo);
    uint tri;float3 bary;HitTriangle(sub,hit.prim,hit.bary,tri,bary);uint3 ids=LoadTriangle(geo,tri);
    uint format=gBuf[table.x].Load(at+64);
    float3 a=LoadFloat3(record.x,record.z,record.w,format,ids.x),b=LoadFloat3(record.x,record.z,record.w,format,ids.y),c=LoadFloat3(record.x,record.z,record.w,format,ids.z);
    float3 local=a*bary.x+b*bary.y+c*bary.z;
    uint4 normals=gBuf[table.x].Load4(at+80);
    if(sub.blasTess && normals.x!=RTC_NONE) {
        float3 na=normalize(LoadFloat3(normals.x,normals.y,normals.z,normals.w,ids.x));
        float3 nb=normalize(LoadFloat3(normals.x,normals.y,normals.z,normals.w,ids.y));
        float3 nc=normalize(LoadFloat3(normals.x,normals.y,normals.z,normals.w,ids.z));
        float3 u,v,w;SubTriangle(sub.tessLevel,hit.prim%(sub.tessLevel*sub.tessLevel),u,v,w);
        local=PhongPoint(u,a,b,c,na,nb,nc,sub.tessAlpha)*(1-hit.bary.x-hit.bary.y)+
            PhongPoint(v,a,b,c,na,nb,nc,sub.tessAlpha)*hit.bary.x+PhongPoint(w,a,b,c,na,nb,nc,sub.tessAlpha)*hit.bary.y;
    }
    float4 p=float4(local,1);
    float3 before=float3(dot(asfloat(gBuf[table.x].Load4(at+16)),p),dot(asfloat(gBuf[table.x].Load4(at+32)),p),dot(asfloat(gBuf[table.x].Load4(at+48)),p));
    valid=true;return before;
}

bool PtClearInterface(PtSurface s) {
    return (s.flags&(1048576|2097152)) || (s.transmission>=.999 && ((s.flags&2048) || s.roughness<=.1) && min(s.base.x,min(s.base.y,s.base.z))>=.95);
}
// A single contract for coverage, attenuation layers, thin sheets and clear
// volumes. No object names enter the integrator. Reuse the primary hit whenever
// coverage is deterministic; otherwise recast only those affected pixels.
// One layer of the guides: hit (traced from the camera), resolved through clear interfaces; the sky's guides where none.
void PtGuideLayer(Hit hit,PtSurface first,bool useFirst,float3 camera,float3 direction,float cone,uint width,uint height,
                  out float3 diffuseOut,out float4 normalOut,out float3 specularOut,out float depthOut,out float4 detailOut,
                  out float2 motionOut,out float instanceOut,out uint materialOut) {
    uint4 control=gBuf[pA].Load4(336);
    float3 origin=camera,ray=direction,transmission=1,reflected=0;
    float travel=0;PtSurface s=(PtSurface)0;
    [loop]for(uint layer=0;layer<16 && hit.t>=0;layer++) {
        travel+=hit.t;
        if(layer==0 && useFirst)s=first;else s=PtShading(hit,origin,ray,travel*cone);
        if(!PtClearInterface(s) || layer==15)break;
        float3 next=(s.flags&2048)?ray:refract(ray,s.n,1/s.eta);
        if(dot(next,next)<.5)break; // total internal reflection has no transmitted primary
        // Specular guide = what these layers actually reflect. Tint and
        // attenuation layers absorb; that loss is not reflectance.
        float3 fresnel=(s.flags&(1048576|2097152))?0:PtInterfaceFresnel(s,dot(s.n,-ray));
        reflected+=transmission*fresnel;transmission*=s.base*(s.mirrorSheet?1:1-fresnel);
        origin=PtOffset(s,next);ray=next;hit=PtHit(origin,ray,1e-6,0,true);
    }
    motionOut=0;
    if(hit.t<0) {
        // RR guide default for sky: specular albedo .5, never left at zero.
        diffuseOut=0;normalOut=float4(0,0,0,1);specularOut=.5;depthOut=0;detailOut=0;instanceOut=0;materialOut=0;
        if(control.x!=RTC_NONE) {
            // A direction at infinity removes camera translation from sky motion.
            float2 mv=(PtMatrix(272,float4(direction,0)).xy-PtMatrix(64,float4(direction,0)).xy)*(float2(width,height)*float2(.5,-.5));
            motionOut=all(isfinite(mv))?mv:0;
        }
        return;
    }
    // Signed kind avoids collisions between fiber IDs and large mesh scenes.
    instanceOut=hit.inst==RTC_NONE?-float(1+hit.geo):float(hit.inst+1);
    materialOut=hit.inst==RTC_NONE?0:LoadSub(LoadGeo(hit.inst).firstSub+hit.geo).material;
    float3 spec=PtSpecAlbedo(s,-ray);
    float3 diffuse=s.base*((s.flags&64)?1:PtDiffuseWeight(s,spec)*(1-s.transmission));
    diffuseOut=transmission*diffuse;
    // 11-07 (user: RR's static dark speckles on skin, absent from the accumulated PT): skin's diffuse shades with its
    // own weak, filtered normal (s.dn), its specular with the full pore normal (s.n). RR given s.n alone imprinted the
    // pores into the noisy diffuse light across a terminator. The guide follows the lobe that carries the energy.
    float specShare=PtLuma(spec)/max(PtLuma(spec)+PtLuma(diffuse),1e-6);
    float3 guideNormal=PtSafeNormal(lerp(s.dn,s.n,specShare),s.n);
    normalOut=float4(guideNormal,s.roughness);
    specularOut=saturate(reflected+transmission*spec);
    float3 virtualPoint=camera+direction*travel;
    depthOut=saturate((1-PtMatrix(64,float4(virtualPoint,1)).z)*.5);
    // No depth from a sphere Unity draws without any: later raster effects (a sea past it) are not cut off at it.
    if((PtAuthor()&PT_AUTHOR_PANORAMA) && PtPanoramaHit(hit))depthOut=0;
    detailOut=!(s.flags&64) && s.transmission<.999?PtDetailReference(s,spec,transmission,reflected):0;
    if(control.x!=RTC_NONE) {
        float3 current=s.p,before=s.p;
        if(s.flags&64) {
            uint4 hair=gBuf[pA].Load4(256),item=gBuf[hair.z].Load4(hit.geo*48);
            uint vertex=item.x+(hit.prim/item.z)*(item.z+1)+hit.prim%item.z;
            current=s.hairCenter;
            before=control.y?lerp(asfloat(gBuf[hair.y].Load3(vertex*48+32)),asfloat(gBuf[hair.y].Load3((vertex+1)*48+32)),saturate(hit.bary.x)):current;
            virtualPoint+=current-s.p;
        } else {bool valid;before=PtPreviousPosition(hit,s,valid);}
        float3 previousVirtual=virtualPoint+before-current;
        float2 mv=(PtMatrix(272,float4(previousVirtual,1)).xy-PtMatrix(64,float4(virtualPoint,1)).xy)*(float2(width,height)*float2(.5,-.5));
        motionOut=all(isfinite(mv)) && all(abs(mv)<16384)?mv:0;
    }
}
// Test bit 27 (0.23.1, docs\pt-cover-guides-20261104), cleared: the guides take the layer above coverage .5 as before.
// Where coverage varies with the seed (sheer cloth, lace, VaM's translucent alpha) the path takes each layer in the
// share of frames its coverage gives; the guides hold every layer in that share - the surface the frames average to -
// rather than only the layer above coverage .5 (a sheer layer under .5 was missing from them, and RR resolved it
// away as if the cloth were clear). Depth, motion, instance and material follow the layer with the largest share;
// the detail reference is left out where layers mix.
static const uint PT_TEST_COVER_GUIDES=1u<<27,PT_TEST_OPAQUE_MOTION=1u<<29,PT_TEST_PERSON_MOTION=1u<<30;
void PtPrimaryGuides(uint2 pixel,Hit initial,PtSurface initialSurface,bool varied,float3 camera,float3 direction,float cone,uint width,uint height) {
    uint reference=gBuf[pA].Load(360);uint4 control=gBuf[pA].Load4(336);
    bool blend=varied && (PtTest()&PT_TEST_COVER_GUIDES);
    float3 diffuse=0,specular=0,sumNormal=0;float4 normal=float4(0,0,0,1),detail=0;float2 motion=0;
    float depth=0,instance=0,roughness=0,total=0,best=-1,remaining=1,tmin=.005;uint material=0;
    bool person=false,layered=false;float keepDepth=0;float2 keepMotion=0;
    [loop]for(uint cover=0;cover<4 && remaining>1.0/64;cover++) {
        bool unused;
        Hit hit=initial;
        if(cover>0 || varied)hit=PtHitTracked(camera,direction,tmin,0,blend?0:.5,true,unused);
        float3 d,sp;float4 nr,dt;float dp,ins;float2 mv;uint mat;
        PtGuideLayer(hit,initialSurface,cover==0 && !varied,camera,direction,cone,width,height,d,nr,sp,dp,dt,mv,ins,mat);
        if(!blend){diffuse=d;normal=nr;specular=sp;depth=dp;detail=dt;motion=mv;instance=ins;material=mat;break;}
        float w=remaining*(cover<3?PtHitCoverage(hit):1);
        diffuse+=w*d;specular+=w*sp;sumNormal+=w*nr.xyz;roughness+=w*nr.w;total+=w;remaining-=w;
        if(w>best){best=w;normal=nr;depth=dp;detail=dt;motion=mv;instance=ins;material=mat;}
        // Test bit 29 (11-05): a see-through layer has no motion or depth of its
        // own in the guides. A pixel behind haze, a beam or a glitter shell shows two things that move apart; frame
        // generation moved the person's face with the floating layer and the face's contour rippled. Depth and motion
        // follow the deepest layer reached - the opaque surface (or the sky) behind every see-through layer.
        // Test bit 30 (11-05; clothing always has depth and motion): a person's own see-through layer is not such a
        // layer. Sheer clothing over skin or the room took the motion of what lies behind it and was not smooth; the
        // first layer of a person (skin, clothing, mesh hair: PT_MODE_PERSON; hair fibres) keeps its depth and motion,
        // see-through things in front of it still do not. Bit 30 cleared: the deepest layer as above.
        if(PtTest()&PT_TEST_OPAQUE_MOTION) {
            layered=true;
            if(!person){keepDepth=dp;keepMotion=mv;}
            if((PtTest()&PT_TEST_PERSON_MOTION) && hit.t>=0)
                person=person || ins<0 || (ins>0 && (gBuf[pB].Load(mat*PT_MATERIAL_BYTES+404)&PT_MODE_PERSON)!=0);
        }
        if(hit.t<0)break;
        tmin=hit.t+max(1e-4,hit.t*1e-5);
    }
    if(layered){depth=keepDepth;motion=keepMotion;}
    if(blend && total>0) {
        diffuse/=total;specular/=total;
        float3 n=sumNormal/total;
        normal=float4(dot(n,n)>1e-6?normalize(n):normal.xyz,roughness/total);
        if(best<total*.999)detail=0;
    }
    gRwTex[pD][pixel]=float4(diffuse,instance);
    gRwTex[pE][pixel]=normal;
    gRwTex[pF][pixel]=float4(specular,material);
    gRwScalar[pG][pixel]=depth;
    if(reference!=RTC_NONE)gRwTex[reference][pixel]=detail;
    if(control.x!=RTC_NONE)gRwVec2[control.x][pixel]=motion;
    // RR F responsivity (SDK guide 3.4.14): a small positive hint counters
    // persistence on static skin. It is not a history-length control. Reuse the
    // primary surface and its unjittered motion; never touch radiance or seeds.
    uint4 response=gBuf[pA].Load4(384);
    if(response.z!=RTC_NONE) {
        bool eligible=control.x!=RTC_NONE && initial.t>=0 && !varied &&
            (initialSurface.flags&8)!=0 && (initialSurface.flags&(64|2048|1048576|2097152))==0 &&
            initialSurface.transmission==0 && depth>0 && all(isfinite(motion));
        float speed=length(motion*asfloat(control.zw));
        // Authoring choice, not a vendor default: full hint below 2 output px/s,
        // smoothly off at 12 px/s. No age counter, stale image or periodic reset.
        float hint=eligible?saturate(asfloat(response.w))*(1-smoothstep(2,12,speed)):0;
        gRwScalar[response.z][pixel]=isfinite(hint)?hint:0;
    }
}

// pA camera byte buffer, pB PtMaterial[], pC colour, pD diffuse guide,
// pE normal/roughness guide, pF specular guide, pG device depth, pH spec distance.
void PtTracePixel(uint3 tid) {
    uint4 size=gBuf[pA].Load4(160); uint width=size.x,height=size.y,spp=size.z,depthLimit=size.w;
    uint diffuseLimit=gBuf[pA].Load(464);
    if(tid.x>=width||tid.y>=height)return;
    float4 camera=asfloat(gBuf[pA].Load4(128)),jitter=asfloat(gBuf[pA].Load4(144));
    float2 uv=(float2(tid.xy)+.5-jitter.xy)/float2(width,height);
    float3 farPoint=PtMatrix(0,float4(uv*float2(2,-2)+float2(-1,1),1,1));
    float3 direction=normalize(farPoint-camera.xyz),sum=0;
#ifdef PT_SPLIT
    float3 sumDiffuse=0,sumSpecular=0;float diffuseDistance=0,specularDistance=0;
#endif
    float specDistance=0;
#ifdef PT_SSS
    float3 sssLight=0;float sssDepth=0;
#endif
    float3 neighbour=PtMatrix(0,float4((uv+float2(0,1.0/height))*float2(2,-2)+float2(-1,1),1,1));
    float pixelCone=length(normalize(neighbour-camera.xyz)-direction);
    const uint visBuf=gBuf[pA].Load4(256).w,visAt=(tid.x+width*tid.y)*12;bool visLoaded=false;
    [loop] for(uint sample=0;sample<spp;sample++) {
        uint seed=Hash((tid.x+width*tid.y)*747796405u+pSeed*2891336453u+sample*277803737u);
        float3 o=camera.xyz,d=direction,throughput=1,radiance=0;
#ifdef PT_SPLIT
        float3 betaDiffuse=0,betaSpecular=0,radianceDiffuse=0,radianceSpecular=0;
        float firstDistance=10000,secondaryTravel=0;bool distanceRecorded=false;
#endif
        float traveled=0,spread=pixelCone,previousPdf=0,envUnity=fAmbientLM,envAuthor=0,envProbe=-1;float3 previousPoint=o,envShare=0,envProbeAt=0;bool previousSpec=false;
        float reflectionTravel=0;bool firstReflective=false,distanceDone=false,primarySpecial=false;
        uint bounce=0,interfaces=0,diffuseBounces=0,pathLimit=diffuseLimit;bool afterHair=false,budgetSet=false,background=false;
        float3 radiancePrimary=0;bool pastPrimary=false;
        [loop] for(uint event=0;event<depthLimit+16 && bounce<depthLimit;event++) {
            bool varied;
            Hit hit=PtHitTracked(o,d,event?1e-6:.005,seed,-1,bounce==0,varied);
#ifdef PT_RAYGEN
            // Reorder before the expensive BSDF branches, while only the
            // compact hit and path state are live; no PtSurface across SER.
            uint coherence=0;
            if(hit.t>=0) {
                if(hit.inst==RTC_NONE)coherence=33+(hit.geo&15);
                else {
                    uint material=LoadSub(LoadGeo(hit.inst).firstSub+hit.geo).material;
                    uint flags=gBuf[pB].Load(material*PT_MATERIAL_BYTES+108);
                    coherence=1+(material&15)+((flags&8)?16:(flags&2048)?48:0);
                }
            }
            NvReorderThread(coherence,7);
#endif
            if(hit.t<0) {
                if(event==0 && sample==0)PtPrimaryGuides(tid.xy,hit,(PtSurface)0,varied,camera.xyz,direction,pixelCone,width,height);
                // A traced reflection that escapes sees the environment at
                // infinity. 0 means "lobe not sampled" (NRD convention), not this.
                if(bounce==1 && firstReflective && !primarySpecial && !distanceDone){specDistance+=(reflectionTravel+10000)/spp;distanceDone=true;}
                float mis=previousPdf>0?PtMis(previousPdf,PtEnvPdf(d)):1;
                float3 environmentDiffuse;float3 environment=PtEnvironmentLobes(d,bounce==0,envUnity,environmentDiffuse);
                environmentDiffuse*=1-envAuthor; // the rest the light sample below it took (PtEnvironmentCandidate)
                if(bounce>0)environment=lerp(environment,environmentDiffuse,envShare);
                if(envProbe>=0)environment=PtProbeRadiance(d,envProbeAt,envProbe); // a Standard surface's sharp reflection that leaves the scene: Unity's probe
                environment*=mis;
                radiance+=throughput*environment;
#ifdef PT_SPLIT
                if(bounce>0){radianceDiffuse+=betaDiffuse*environment;radianceSpecular+=betaSpecular*environment;}
#endif
                break;
            }
            // What fibres pass among themselves is PtFiberEval's multiple-scattering
            // lobe: a path leaving a fibre ends at the next fibre, as its shadow rays do.
            if(afterHair && hit.inst==RTC_NONE)break;
            traveled+=hit.t;
            PtSurface s=PtShading(hit,o,d,traveled*spread);
            // 11-09 (user: the background only has to hold no error; the budget goes to the person): the first thing the
            // eye rests on, past clear layers, sets the path's budget. A person (skin, clothing, mesh hair, fibres) keeps the
            // setting; anything else takes its diffuse light from the light samples alone, as Unity does (it has no bounce),
            // and only its specular lobes continue. A path that left a person is the person's to the end.
            if(!budgetSet && !PtClearInterface(s)) {
                budgetSet=true;
                background=!s.person && !(s.flags&(8|64));
                // By distance from the camera: nearer than near it keeps the setting, between near and far a share of paths does, so no line marks the change.
                if(background && (PtAuthor()&(PT_AUTHOR_BG_BOUNCE|PT_AUTHOR_BG_ROUGH))) {
                    float2 keep=asfloat(gBuf[pA].Load2(504));
                    if(traveled<keep.x || (traveled<keep.y && Rand(seed)*(keep.y-keep.x)<keep.y-traveled))background=false;
                }
                if((PtAuthor()&PT_AUTHOR_BG_BOUNCE) && background)pathLimit=0;
            }
            s.diffuseDepth=diffuseBounces;
            s.diffuseStop=diffuseBounces>=pathLimit?1:0;
            if(event==0) {
                primarySpecial=varied || PtClearInterface(s) || (s.flags&64);
                if(sample==0)PtPrimaryGuides(tid.xy,hit,s,varied,camera.xyz,direction,pixelCone,width,height);
                if(sample==0 && visBuf!=RTC_NONE && (PtTest()&PT_TEST_VIS_RIS)) {
                    gVis=gRwBuf[visBuf].Load3(visAt);uint motionUav=gBuf[pA].Load(336);
                    float2 mv=motionUav!=RTC_NONE?gRwVec2[motionUav][tid.xy]:0;
                    if(!all(isfinite(mv)) || dot(mv,mv)>.09)gVis=0xffffffffu; // moved: start again
                    gVisUse=true;visLoaded=true;
                }
            }
            if(bounce==1 && firstReflective && !primarySpecial && !distanceDone) {
                reflectionTravel+=hit.t;
                if(!PtClearInterface(s)){specDistance+=reflectionTravel/spp;distanceDone=true;}
            }
#ifdef PT_SPLIT
            if(bounce==1) {
                secondaryTravel+=hit.t;
                if(!PtClearInterface(s) && !distanceRecorded){firstDistance=secondaryTravel;distanceRecorded=true;}
            }
#endif
            float3 v=-d;
            bool nullInterface=(s.flags&(1048576|2097152))!=0;
            float carryPdf=previousPdf,carryUnity=envUnity,carryAuthor=envAuthor;bool carrySpec=previousSpec;float3 carryPoint=previousPoint,carryShare=envShare;
            float emissionMis=previousPdf>0?PtMis(previousPdf,PtEmitterPdf(hit,previousPoint,s.p)):1;
            // API 26: seen directly or in a reflection an emitter is as bright as drawn; the light it casts is scaled.
            if(bounce>0 && !previousSpec)emissionMis*=PtAuthorEmitter();
            radiance+=throughput*s.emission*emissionMis;
#ifdef PT_SPLIT
            if(bounce>0){radianceDiffuse+=betaDiffuse*s.emission*emissionMis;radianceSpecular+=betaSpecular*s.emission*emissionMis;}
#endif
            bool clearInterface=PtClearInterface(s);
            // A hair hit is one vertex like any other: its light sample covers
            // R / TT / TRT and the fibres' multiple scattering (one analytic lobe,
            // PtFiberEval), its continuation the room's light. 0.22.55 traced up
            // to 64 fibre events per path for that brightness.
            bool continues=(bounce+1<depthLimit || clearInterface) && (!clearInterface || interfaces+1<16);
            float3 weight,next;
            bool sampleSkin=(s.flags&8) && s.sssWidth>0 && s.sssStrength>0,sheetCarry=false;
            bool thinHere=(s.flags&8) && s.thinStrength>0 && event==0 && !varied; // light through thin skin (PtThinCandidate)
#ifdef PT_SSS
            // Skin the eye meets first: its direct diffuse light is spread in screen space after the trace (path_scatter.hlsl);
            // no probe ray, exit point or second light sample. Elsewhere (in a mirror, under a sheet) the skin is plain diffuse.
            bool scatterHere=sampleSkin && event==0 && !varied;
            sampleSkin=false;
#endif
            if(sampleSkin) {
                PtSurface spec=s,exitSurface;spec.base=0;
                float ps=PtSpecChance(s,v);
                float3 spatialWeight;
                bool exits=PtSampleSkin(s,hit,v,traveled*spread,seed,exitSurface,spatialWeight);
                exitSurface.diffuseDepth=s.diffuseDepth;
                // Include the BSSRDF transport weight in the same reservoir.
                // One final visibility ray covers surface reflection and
                // transported diffuse, with all six proposals still present.
                PtDirectReservoir direct=(PtDirectReservoir)0;direct.transport=1;
                PtGatherDirect(spec,v,continues?ps:0,seed,direct);
                if(thinHere)PtThinCandidate(s,seed,direct);
                if(exits){direct.transport=spatialWeight;PtGatherDirect(exitSurface,exitSurface.n,continues&&!s.diffuseStop?1-ps:0,seed,direct);}
                float3 directSpec;float3 directValue=PtResolveDirect(direct,directSpec);
                if(gVisUse){PtVisUpdate();gVisUse=false;}
                radiance+=throughput*directValue;
#ifdef PT_SPLIT
                if(bounce==0){radianceDiffuse+=throughput*(directValue-directSpec);radianceSpecular+=throughput*directSpec;}
                else {radianceDiffuse+=betaDiffuse*directValue;radianceSpecular+=betaSpecular*directValue;}
#endif
                if(!continues)break;
                if(Rand(seed)<ps) {
                    float pdf;if(!PtSample(spec,v,seed,next,weight,previousSpec,pdf,envShare))break;
                    previousPdf=pdf*ps;
                    weight/=ps;previousSpec=true;envShare=0;envUnity=s.unityAmbient;
                } else {
                    if(!exits||s.diffuseStop)break;
                    float2 u=float2(Rand(seed),Rand(seed));float r=sqrt(u.x),phi=2*PT_PI*u.y;
                    next=normalize(exitSurface.t*(r*cos(phi))+exitSurface.b*(r*sin(phi))+exitSurface.n*sqrt(1-u.x));
                    float pdf;float3 fc=PtEval(exitSurface,exitSurface.n,next,pdf);
                    if(pdf<=0||dot(next,exitSurface.ng)<=0)break;
                    weight=spatialWeight*fc/(pdf*(1-ps));previousPdf=pdf*(1-ps);previousSpec=false;s=exitSurface;envShare=1;envUnity=exitSurface.unityAmbient;envAuthor=PtAuthorAmbientShare(exitSurface.diffuseDepth);diffuseBounces++;
                    if((PtAuthor()&PT_AUTHOR_BOUNCE) && envUnity<1) {weight*=envUnity;envUnity=1;}
                }
            } else
            {
                PtDirectReservoir direct=(PtDirectReservoir)0;direct.transport=1;
                PtGatherDirect(s,v,continues?1:0,seed,direct);
                if(thinHere)PtThinCandidate(s,seed,direct);
                float3 directSpec;float3 directValue=PtResolveDirect(direct,directSpec);
                if(gVisUse){PtVisUpdate();gVisUse=false;}
#ifdef PT_SSS
                if(scatterHere){sssLight+=throughput*max(directValue-directSpec,0)*(s.sssStrength/max(spp,1u));sssDepth=hit.t*dot(direction,asfloat(gBuf[pA].Load3(596)));}
#endif
                // API 27: Unity's probe is this broad lobe's whole estimate (no ray below, no room in it twice); a sharp lobe is traced.
                bool probed=s.unityProbe && !clearInterface && !s.mirrorSheet && s.transmission<.999;
                bool probeLobe=probed && s.roughness>=.4;
                float3 probeValue=probeLobe?PtProbeSpecular(s,v):0;
                radiance+=throughput*(directValue+probeValue);
#ifdef PT_SPLIT
                if(bounce==0){radianceDiffuse+=throughput*(directValue-directSpec);radianceSpecular+=throughput*(directSpec+probeValue);}
                else {radianceDiffuse+=betaDiffuse*(directValue+probeValue);radianceSpecular+=betaSpecular*(directValue+probeValue);}
#endif
                if(!continues)break;
                float pdf;if(!PtSample(s,v,seed,next,weight,previousSpec,pdf,envShare))break;
                if(probeLobe && previousSpec && dot(next,s.ng)>0) {
                    if(bounce==0 && !primarySpecial && !distanceDone){specDistance+=10000/spp;distanceDone=true;} // at infinity, as an escaping ray's
                    break;
                }
                envProbe=-1;if(probed && previousSpec){envProbe=s.roughness;envProbeAt=s.p;}
                // 11-09: a background's broad reflection is the sky's cube, unoccluded, as in Unity: the same estimate as a ray
                // that escapes, with no ray, no second surface and its light sample. Sharp ones (a floor's image of her) are traced.
                if((PtAuthor()&PT_AUTHOR_BG_ROUGH) && background && previousSpec && !clearInterface && !s.mirrorSheet && s.transmission<.999 && s.roughness>=.4 && dot(next,s.ng)>0) {
                    float3 unusedDiffuse;float3 reflected=throughput*weight*PtEnvironmentLobes(next,false,s.unityAmbient,unusedDiffuse)*(pdf>0?PtMis(pdf,PtEnvPdf(next)):1);
                    radiance+=reflected;
#ifdef PT_SPLIT
                    if(bounce==0)radianceSpecular+=reflected;
                    else {radianceDiffuse+=reflected*betaDiffuse/max(throughput,1e-8);radianceSpecular+=reflected*betaSpecular/max(throughput,1e-8);}
#endif
                    if(bounce==0 && !primarySpecial && !distanceDone){specDistance+=10000/spp;distanceDone=true;} // at infinity, as an escaping ray's
                    break;
                }
                if(!previousSpec && !nullInterface)diffuseBounces++;
                envUnity=s.unityAmbient; // the escaping direction's diffuse share is this surface's ambient
                // API 26: an author who takes the ambient off the person (unityAmbientIntensity) has her lit by her lights: the
                // room's bounce is ambient to her too. Scaled here, so not again where the path escapes.
                if((PtAuthor()&PT_AUTHOR_BOUNCE) && s.unityAmbient<1 && !nullInterface) {weight*=lerp(1,s.unityAmbient,(s.flags&64)?1:envShare);envUnity=1;}
                envAuthor=PtAuthorAmbientShare(s.diffuseDepth);
                const bool thinTransmission=(s.flags&2048)&&dot(next,s.ng)<0;
                // Straight through a sheet the direction is still the one drawn below it: the environment / emitter
                // it reaches is weighed against the samples taken there, which pass the sheet too (PtVisibility).
                sheetCarry=thinTransmission && (PtTest()&PT_TEST_SHEET_NEE)!=0;
                previousPdf=sheetCarry?carryPdf:thinTransmission?0:pdf;
                // A transmitted eye ray is not a reflected virtual surface.
                if(thinTransmission)previousSpec=false;
                if(nullInterface){previousPdf=carryPdf;previousSpec=carrySpec;}
                // Straight-through interfaces keep the lobe that reached them.
                if(nullInterface || (clearInterface && dot(next,s.ng)<0)){envShare=carryShare;envUnity=carryUnity;envAuthor=carryAuthor;}
            }
            const bool clearTransmission=clearInterface && dot(next,s.ng)<0;
            if(bounce==0 && !clearTransmission)firstReflective=previousSpec && dot(next,s.ng)>0;
#ifdef PT_SPLIT
            if(bounce==0 && !clearTransmission) {
                float3 fraction=0;
                if(sampleSkin)fraction=(s.flags&128)?1:0;
                else {float pdf;float3 diffuse;float3 value=PtEvalComponents(s,v,next,pdf,diffuse);fraction=s.diffuseStop?0:saturate(diffuse/max(value,1e-8));}
                betaDiffuse=throughput*weight*fraction;betaSpecular=throughput*weight*(1-fraction);
            } else if(bounce>0){betaDiffuse*=weight;betaSpecular*=weight;}
#endif
            throughput*=weight;
            // The first two vertices always contribute. Low-energy later
            // transport uses unbiased roulette instead of a guaranteed third
            // vertex; reflective/transmissive paths retain high survival.
            if(bounce>=1 && !clearTransmission) {
                float survive=clamp(PtMax(throughput),.05,.95);
                if(Rand(seed)>survive)break;
                throughput/=survive;
#ifdef PT_SPLIT
                betaDiffuse/=survive;betaSpecular/=survive;
#endif
            }
            if(PtMax(throughput)<1e-7)break;
            previousPoint=nullInterface||sheetCarry?carryPoint:s.p;o=PtOffset(s,next);d=next;afterHair=(s.flags&64)!=0;
            if(clearTransmission)interfaces++;
            else {if(!pastPrimary){radiancePrimary=radiance;pastPrimary=true;}spread=max(spread,s.roughness*s.roughness*.25);bounce++;}
        }
        // 11-07 (bit 31): a sample's light gathered after a diffuse bounce is capped at luminance 4 on the output's scale.
        // About one sample in 100000 there was 200-300x a surface's mean (a sunlit spot met by a bounce); RR, still,
        // pressed each down and left a dark patch round it. Under .1 % of the energy in the measured room.
        if(pastPrimary && diffuseBounces>0 && (PtTest()&PT_TEST_VIS_RIS)) {
            float3 indirect=radiance-radiancePrimary;float l=PtLuma(indirect),cap=4/max(camera.w,1e-8);
            if(l>cap)radiance=radiancePrimary+indirect*(cap/l);
        }
        if(all(isfinite(radiance)))sum+=radiance/max(spp,1u);
#ifdef PT_SPLIT
        if(all(isfinite(radianceDiffuse)) && all(isfinite(radianceSpecular))) {
            sumDiffuse+=radianceDiffuse/max(spp,1u);sumSpecular+=radianceSpecular/max(spp,1u);
            diffuseDistance+=firstDistance*PtLuma(radianceDiffuse)/max(spp,1u);
            specularDistance+=firstDistance*PtLuma(radianceSpecular)/max(spp,1u);
        }
#endif
    }
    if(visLoaded)gRwBuf[visBuf].Store3(visAt,gVis);
    gRwTex[pC][tid.xy]=float4(clamp(sum*camera.w,0,65000),1);
    gRwScalar[pH][tid.xy]=specDistance;
#ifdef PT_SSS
    // For the scattering passes: strength x the skin's direct diffuse light over the RR albedo guide (the output's scale), and
    // its depth along the camera's axis (m); 0 where the eye meets no such skin. The composite multiplies by the same guide.
    float3 sssAlbedo=max(gRwTex[pD][tid.xy].rgb,1e-3),sssE=sssLight*camera.w/sssAlbedo;
    gRwTex[gBuf[pA].Load(592)][tid.xy]=sssDepth>0 && all(isfinite(sssE))?float4(clamp(sssE,0,65000),sssDepth):0;
#endif
#ifdef PT_SPLIT
    uint2 signal=gBuf[pA].Load2(384);
    float3 diffuseAlbedo=max(gRwTex[pD][tid.xy].rgb,1e-3),specAlbedo=max(gRwTex[pF][tid.xy].rgb,1e-3);
    gRwTex[signal.x][tid.xy]=float4(clamp(sumDiffuse*camera.w/diffuseAlbedo,0,65000),diffuseDistance/max(PtLuma(sumDiffuse),1e-8));
    gRwTex[signal.y][tid.xy]=float4(clamp(sumSpecular*camera.w/specAlbedo,0,65000),specularDistance/max(PtLuma(sumSpecular),1e-8));
#endif
}
#ifdef PT_RAYGEN
[shader("raygeneration")] void PathTraceRaygen() { PtTracePixel(DispatchRaysIndex()); }
#else
[RootSignature(RTC_ROOT)] [numthreads(8,8,1)] void PathTrace(uint3 tid:SV_DispatchThreadID) { PtTracePixel(tid); }
#endif
