// Included inside VamDlss::Impl after rrShader. Immutable same-frame RR inputs
// for both eyes: no feedback into NGX, extra image, copy, ray or temporal lag.
// RR_STEREO builds a separate variant; disabled frames use the original shader.
static constexpr const char* rrStereoShader=R"(
cbuffer StereoCb:register(b1) {
    row_major float4x4 otherWorldToClip;
    row_major float4x4 otherClipToWorld;
    float2 otherJitter;float stereoStrength;float stereoPad;
}
Texture2D<float4> OtherColor:register(t18);
Texture2D<float4> SelfNormal:register(t19);
Texture2D<float4> OtherNormal:register(t20);
Texture2D<float> OtherDepth:register(t21);
Texture2D<float4> SelfAlbedo:register(t22);
Texture2D<float4> OtherAlbedo:register(t23);
Texture2D<float4> OtherDetail:register(t24);
Texture2D<float4> SelfSpec:register(t25);
Texture2D<float4> OtherSpec:register(t26);
Texture2D<float4> SelfReference:register(t27);
Texture2D<float4> OtherReference:register(t28);
float3 StereoWorld(float2 uv,float z,row_major float4x4 inverseClip) {
    float4 p=mul(inverseClip,float4(uv*float2(2,-2)+float2(-1,1),1-2*z,1));
    return p.xyz/p.w;
}
float StereoLuma(float3 x){return dot(x,float3(.2126,.7152,.0722));}
float3 StereoResolve(float3 color,uint2 pixel) {
    float2 size=float2(w,h),uv=(float2(pixel)+.5)/float2(ow,oh);
    float2 samplePos=uv*size+jitter-.5;
    int2 p=int2(floor(samplePos+.5));
    if(any(p<1)||any(p>=int2(w-1,h-1)))return color;
    float z=Z.Load(int3(p,0));
    float4 a=SelfAlbedo.Load(int3(p,0)),n=SelfNormal.Load(int3(p,0)),s=SelfSpec.Load(int3(p,0));
    float fraction=SelfReference.Load(int3(p,0)).a;
    // Fibres have negative instance IDs, sky zero. A mixed-coverage guide has
    // no reference. Near mirrors, clear eyes and specular-dominated surfaces
    // retain their physically view-dependent appearance.
    if(z<=0 || a.a<=0 || dot(n.xyz,n.xyz)<.5 || fraction<.75 || n.w<.3)return color;
    float diffuse=StereoLuma(a.rgb),spec=StereoLuma(s.rgb);
    float trust=smoothstep(.3,.65,n.w)*smoothstep(.75,.95,fraction)*saturate((diffuse-2*spec)/max(diffuse,.001));
    if(trust<=0)return color;
    float2 centerUv=(float2(p)+.5-jitter)/size;
    float3 center=StereoWorld(centerUv,z,clipToWorld);
    // Plane-adjust the subpixel ray using the local guide normal. This keeps
    // smooth sloping surfaces aligned instead of treating device depth as flat.
    float3 ray0=StereoWorld(uv,1,clipToWorld),ray1=StereoWorld(uv,.5,clipToWorld);
    float3 ray=ray1-ray0;float denom=dot(n.xyz,ray);
    if(abs(denom)<1e-8)return color;
    float3 world=ray0+ray*(dot(n.xyz,center-ray0)/denom);
    float4 other=mul(otherWorldToClip,float4(world,1));
    if(other.w<=0 || !all(isfinite(other)))return color;
    float2 otherUv=other.xy/other.w*float2(.5,-.5)+.5;
    float2 otherPos=otherUv*size+otherJitter-.5;
    int2 q=int2(floor(otherPos+.5));
    if(any(q<1)||any(q>=int2(w-1,h-1)))return color;
    float oz=OtherDepth.Load(int3(q,0));
    float4 oa=OtherAlbedo.Load(int3(q,0)),on=OtherNormal.Load(int3(q,0)),os=OtherSpec.Load(int3(q,0));
    float ofraction=OtherReference.Load(int3(q,0)).a;
    if(oz<=0 || a.a!=oa.a || s.a!=os.a || dot(n.xyz,on.xyz)<.9 || abs(n.w-on.w)>.1 || ofraction<.75)return color;
    float otherDiffuse=StereoLuma(oa.rgb),otherSpec=StereoLuma(os.rgb);
    trust=min(trust,smoothstep(.3,.65,on.w)*smoothstep(.75,.95,ofraction)*saturate((otherDiffuse-2*otherSpec)/max(otherDiffuse,.001)));
    // Validate the round-trip position using geometry, never image matching.
    float3 otherWorld=StereoWorld((float2(q)+.5-otherJitter)/size,oz,otherClipToWorld);
    float expectedZ=(1-other.z/other.w)*.5;
    if(abs(oz-expectedZ)>max(expectedZ*.005,1e-7))return color;
    // A world-space plane residual rejects disocclusion and virtual refracted
    // points. The footprint tolerance scales with the actual input pixel size.
    float3 neighbor=StereoWorld(centerUv+float2(1.0/w,0),z,clipToWorld);
    float footprint=length(neighbor-center);
    if(abs(dot(n.xyz,otherWorld-world))>max(.0005,footprint*.35))return color;
    // Do not filter across either eye's depth discontinuity. Four nearest
    // guide taps on each side also guard the bilinear colour footprint.
    int2 pbase=int2(floor(samplePos)),qbase=int2(floor(otherPos));
    [unroll]for(int y=0;y<2;y++)[unroll]for(int x=0;x<2;x++) {
        int2 pp=pbase+int2(x,y),qq=qbase+int2(x,y);
        float dz=Z.Load(int3(pp,0)),od=OtherDepth.Load(int3(qq,0));
        if(abs(dz-z)>max(z*.005,1e-7) || abs(od-oz)>max(oz*.005,1e-7))return color;
    }
    // Large material-colour changes are likely a texture/coverage boundary.
    trust*=1-smoothstep(.03,.15,length(a.rgb-oa.rgb)/max(length(a.rgb),.05));
    float3 peer=OtherColor.SampleLevel(linearClamp,otherUv,0).rgb;
    if(detail>0)peer*=lerp(1.0,clamp(OtherDetail.SampleLevel(linearClamp,otherUv,0).rgb,.5,2),detail);
    if(!all(isfinite(peer)) || !all(isfinite(color)))return color;
    // Bound the edit; an unusually strong inter-eye lighting difference cannot
    // pull a surface into a bright highlight from the other viewing direction.
    float limit=.15*max(StereoLuma(max(color,0)),0)+.002;
    return max(color+stereoStrength*trust*clamp(peer-color,-limit,limit),0);
}
)";
Ptr<ID3D11ComputeShader> rrStereoComposite;
Ptr<ID3D11Buffer> rrStereoConstants;
bool rrStereoFrame=false;
int rrStereoLogged=-1;
void DumpRrStereo(const RrConstants* constants) {
    wchar_t folder[1024],file[1200];
    if(viewCount!=2 || !rrEyes[0].pathTracing || !GetEnvironmentVariableW(L"VAM_PT_DUMP",folder,1024))return;
    swprintf_s(file,L"%s/stereo-request",folder);if(!DeleteFileW(file))return;
    const unsigned serial=unsigned(rrEyes[0].serial);
    for(unsigned i=0;i<2;i++) {
        auto& e=rrEyes[i];const unsigned id=serial*2+i;
        rtc::PathView p{};p.width=e.w;p.height=e.h;p.jitter[0]=constants[i].jitterX;p.jitter[1]=constants[i].jitterY;
        memcpy(p.clipToWorld,constants[i].clipToWorld,64);Invert(p.clipToWorld,p.worldToClip);
        DumpPathCamera(p,id,false);
        DumpPathTexture(e.output.tex.Get(),L"stereo-color",id);
        DumpPathTexture(e.albedo.tex.Get(),L"stereo-albedo",id);
        DumpPathTexture(e.normal.tex.Get(),L"stereo-normal",id);
        DumpPathTexture(e.spec.tex.Get(),L"stereo-spec",id);
        DumpPathTexture(e.depth.tex.Get(),L"stereo-depth",id);
        DumpPathTexture(e.pathReference.tex.Get(),L"stereo-reference",id);
        DumpPathTexture(e.detailHistory[e.detailIndex].tex.Get(),L"stereo-detail",id);
    }
    DumpPathTexture(rrScene.Get(),L"stereo-scene",serial);
    swprintf_s(file,L"%s/stereo-constants.bin",folder);FILE* f=_wfopen(file,L"wb");
    if(f){fwrite(constants,sizeof(RrConstants),2,f);fclose(f);}
    OOVR_LOGF("VAM RR stereo diagnostic captured serial=%u strength=%d",serial,ReadVamPerformanceSettings().rrStereo);
}
void BindRrStereo(unsigned i,const RrConstants* constants) {
    const int requested=ReadVamPerformanceSettings().rrStereo;
    if(!rrStereoFrame) {
        if(i==0 && rrStereoLogged>0){rrStereoLogged=0;OOVR_LOG("VAM RR stereo off: original composite");}
        return;
    }
    auto& self=rrEyes[i];auto& peer=rrEyes[i^1];
    if(self.serial!=peer.serial || !self.pathReference.srv || !peer.pathReference.srv)return;
    if(!rrStereoComposite) {
        std::string source=std::string(rrShader)+rrStereoShader;
        const D3D_SHADER_MACRO macros[]={{"RR_STEREO","1"},{nullptr,nullptr}};
        auto code=CompileShader(source.c_str(),"vam_rr_stereo","Composite","cs_5_0",macros);
        Check(device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&rrStereoComposite),"RR stereo composite");
        D3D11_BUFFER_DESC d{};d.ByteWidth=144;d.Usage=D3D11_USAGE_DYNAMIC;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;d.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        Check(device->CreateBuffer(&d,nullptr,&rrStereoConstants),"RR stereo constants");
    }
    struct Cb{float worldToClip[16],clipToWorld[16],jitter[2],strength,pad;} c{};
    memcpy(c.clipToWorld,constants[i^1].clipToWorld,64);
    if(!Invert(c.clipToWorld,c.worldToClip))return;
    c.jitter[0]=constants[i^1].jitterX;c.jitter[1]=constants[i^1].jitterY;
    const int strength=requested;c.strength=strength*.005f;
    VamWriteConstants(context,rrStereoConstants.Get(),&c,sizeof c);
    context->CSSetShader(rrStereoComposite.Get(),nullptr,0);context->CSSetConstantBuffers(1,1,rrStereoConstants.GetAddressOf());
    ID3D11ShaderResourceView* src[]={peer.output.srv.Get(),self.normal.srv.Get(),peer.normal.srv.Get(),peer.depth.srv.Get(),
        self.albedo.srv.Get(),peer.albedo.srv.Get(),peer.detailHistory[peer.detailIndex].srv.Get(),self.spec.srv.Get(),peer.spec.srv.Get(),
        self.pathReference.srv.Get(),peer.pathReference.srv.Get()};
    context->CSSetShaderResources(18,11,src);
    if(rrStereoLogged!=strength){rrStereoLogged=strength;OOVR_LOGF("VAM RR stereo strength=%d: same-frame two-eye resolve fused into composite, no extra images/copies/rays",strength);}
}
