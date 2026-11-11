// NVIDIA RTXCR analytic far-field hair integrates the subpixel cylinder.
// MIT-licensed sources and pinned revision are in third_party/rtxcr.
#include "../third_party/rtxcr/shaders/include/rtxcr/HairFarFieldBCSDF.hlsli"
RTXCR_HairInteractionSurface PtFiberSurface(PtSurface s,float3 v) {
    RTXCR_HairInteractionSurface f;
    f.incidentRayDirection=-v;f.shadingNormal=s.n;f.tangent=s.t;
    return f; // all directions in world space, including the tangent
}
RTXCR_HairMaterialInteractionBcsdf PtFiberMaterial(PtSurface s) {
    RTXCR_HairMaterialInteractionBcsdf m;
    m.reflectionTint=s.hairReflection;
    m.diffuseReflectionTint=0;m.diffuseReflectionWeight=0;
    m.roughness=clamp(s.roughness,.05,1);
    m.absorptionCoefficient=RTXCR_AbsorptionCoefficientFromColor(s.base,m.roughness);
    m.ior=1.55;m.cuticleAngle=s.hairShift;
    return m;
}
// 0.22.63: no fibre-to-fibre rays (a path leaving a fibre ends at the next one,
// path_trace.hlsl). The light the fibres scatter among themselves returns as
// RTXCR's diffuse cylinder lobe in the author's colour, lit by the same light
// sample and shadow ray, added to R / TT / TRT at full strength. A gain of 1.739
// fits the 64-event transport of 0.22.55 on a dense hair block (tools\path_tracing
// contract_probe.inl, docs\pt-hair-terminal-20261102\fit.py); in the game that
// was brighter than 0.22.55, and the user asked for darker hair (11-02): 1, the
// lobe at RTXCR's own weight.
static const float PT_HAIR_MULTIPLE=1;
float3 PtFiberEval(PtSurface s,float3 v,float3 l,out float pdf) {
    float3 spec,diff;
    RTXCR_HairFarFieldBcsdfEval(PtFiberSurface(s,v),PtFiberMaterial(s),l,v,spec,diff,pdf);
    pdf=.5*pdf+.5/(4*PT_PI); // PtFiberSample's mixture
    float3 t=normalize(s.t),pl=l-t*dot(l,t),pv=v-t*dot(v,t);
    float cosPhi=clamp(dot(pl,pv)*rsqrt(max(dot(pl,pl)*dot(pv,pv),1e-14)),-1,1);
    float multiple=sqrt(saturate(1-dot(l,t)*dot(l,t)))*(.25/PT_PI)*max((PT_PI-acos(cosPhi))*cosPhi+sqrt(1-cosPhi*cosPhi),0);
    return spec+diff+PT_HAIR_MULTIPLE*multiple*s.base; // CRCF already includes the hair integration measure
}
bool PtFiberSample(PtSurface s,float3 v,inout uint seed,out float3 d,out float3 weight,out float pdf) {
    // Half the directions follow RTXCR's R / TT / TRT, half the whole sphere
    // (the multiple-scattering lobe is broad).
    if(Rand(seed)<.5) {
        float h=2*Rand(seed)-1;
        float3 random[2]={float3(Rand(seed),Rand(seed),Rand(seed)),float3(Rand(seed),Rand(seed),0)};
        float3 spec,diff;
        if(!RTXCR_SampleFarFieldBcsdf(PtFiberSurface(s,v),PtFiberMaterial(s),v,h,random,d,spec,diff,pdf)){weight=0;return false;}
    } else {
        float z=2*Rand(seed)-1,phi=2*PT_PI*Rand(seed),r=sqrt(saturate(1-z*z));
        d=float3(r*cos(phi),r*sin(phi),z);
    }
    weight=PtFiberEval(s,v,d,pdf);
    weight=pdf>1e-9?weight/pdf:0;
    return pdf>1e-9 && all(isfinite(weight));
}
float3 PtFiberAlbedo(PtSurface s,float3 v) {
    // Same R / TT / TRT directional attenuation as the BSDF. Deterministic
    // four-point Gauss-Legendre integration over |h| for R and TT; TRT uses
    // RTXCR's h=0 attenuation. No extra rays, texture, or frame allocation.
    RTXCR_HairMaterialInteractionBcsdf m=PtFiberMaterial(s);
    float si=clamp(dot(v,s.t),-1,1),co=sqrt(saturate(1-si*si));
    float ct=sqrt(max(1-si*si/(m.ior*m.ior),1e-7));
    float ep=co/sqrt(max(m.ior*m.ior-si*si,1e-7));
    float f0=RTXCR_CalculateBaseReflectivity(1,m.ior);
    const float4 h=float4(.0694318442,.3300094782,.6699905218,.9305681558);
    const float4 w=float4(.1739274226,.3260725774,.3260725774,.1739274226);
    float3 energy=0;
    [unroll] for(uint i=0;i<4;i++) {
        float f=RTXCR_EvalFresnelSchlick(f0,co*sqrt(1-h[i]*h[i]));
        float3 t=exp(-2*m.absorptionCoefficient*sqrt(1-h[i]*h[i]*ep*ep)/ct);
        energy+=w[i]*(f*m.reflectionTint+(1-f)*(1-f)*t);
    }
    float f=RTXCR_EvalFresnelSchlick(f0,co);
    energy+=(1-f)*(1-f)*f*exp(-4*m.absorptionCoefficient/ct);
    return saturate(energy);
}
