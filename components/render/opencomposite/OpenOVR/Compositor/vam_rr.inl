// Included inside VamDlss::Impl. RR F replaces K/L/M: HDR after alpha ->
// full output -> existing tone mapper -> NR/FG. No additional SR feature.
struct RrTexture {
    Ptr<ID3D11Texture2D> tex;
    Ptr<ID3D11ShaderResourceView> srv;
    Ptr<ID3D11UnorderedAccessView> uav;
    Ptr<ID3D12Resource> r12;  // set when made in D3D12 and wrapped (MakeRrTexture): handed over without an unwrap
    VamOn12Texture On12() const { return {tex.Get(),r12.Get()}; }
};
struct RrEye {
    UINT w = 0, h = 0, ow = 0, oh = 0;
    RrTexture color, albedo, normal, spec, depth, output, specDistance, opaque, detailHistory[2], pathReference;
    unsigned detailIndex=0;
    NVSDK_NGX_Handle* feature = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    uint64_t serial = 0, calls = 0, detailSerial = 0;
    uint64_t sceneRevision=0;
    bool pathTracing = false;
    RrTexture refAcc;float refPose[16]={};uint64_t refSerial=0;unsigned refFrames=0;
} rrEyes[2];
// PT reference view (2026-11-07, diagnostic for the static skin spots): while the camera holds still, the running mean
// of PT's raw per-frame input replaces RR's output (RR still runs). Spots in it are in PT's own estimate; none = RR.
Ptr<ID3D11ComputeShader> rrRefAccumulate,rrRefResolve;Ptr<ID3D11Buffer> rrRefConstants;
bool rrRefOn=false,rrRefKey=false,rrRefIni=false,rrRefIniSeen=false;ULONGLONG rrRefLog=0;
static constexpr const char* rrRefShader=R"(
cbuffer Cb:register(b0){uint w,h,ow,oh,reset,pad0;float n,pad1;}
Texture2D<float4> Color:register(t0);Texture2D<float4> Mean:register(t1);SamplerState linearClamp:register(s0);
RWTexture2D<float4> Acc:register(u0);RWTexture2D<float4> Out:register(u1);
[numthreads(8,8,1)] void Accumulate(uint3 id:SV_DispatchThreadID){
    if(id.x>=w||id.y>=h)return;
    float3 x=Color.Load(int3(id.xy,0)).rgb;if(!all(isfinite(x)))x=0;
    float3 a=reset?x:Acc[id.xy].rgb+(x-Acc[id.xy].rgb)/n;
    Acc[id.xy]=float4(a,1);
}
[numthreads(8,8,1)] void Resolve(uint3 id:SV_DispatchThreadID){
    if(id.x>=ow||id.y>=oh)return;
    Out[id.xy]=float4(Mean.SampleLevel(linearClamp,(float2(id.xy)+.5)/float2(ow,oh),0).rgb,1);
}
)";
bool PathReferenceActive() {
    const bool ini=ReadVamPerformanceSettings().ptReference;
    if(!rrRefIniSeen||ini!=rrRefIni){rrRefOn=ini;rrRefIni=ini;rrRefIniSeen=true;}
    DWORD pid=0;HWND fg=GetForegroundWindow();if(fg)GetWindowThreadProcessId(fg,&pid);
    const bool down=pid==GetCurrentProcessId() && (GetAsyncKeyState(VK_CONTROL)&0x8000) && (GetAsyncKeyState(VK_MENU)&0x8000) && (GetAsyncKeyState('R')&0x8000);
    if(down&&!rrRefKey){rrRefOn=!rrRefOn;OOVR_LOGF("VAM PT reference view %s (Ctrl+Alt+R)",rrRefOn?"on":"off");}
    rrRefKey=down;return rrRefOn;
}
void PathReference(unsigned i,RrEye& e,const VamFrameGuides& guides) {
    if(!rrRefAccumulate) {
        auto a=CompileShader(rrRefShader,"vam_pt_reference","Accumulate","cs_5_0",nullptr);
        Check(device->CreateComputeShader(a->GetBufferPointer(),a->GetBufferSize(),nullptr,&rrRefAccumulate),"PT reference accumulate");
        auto r=CompileShader(rrRefShader,"vam_pt_reference","Resolve","cs_5_0",nullptr);
        Check(device->CreateComputeShader(r->GetBufferPointer(),r->GetBufferSize(),nullptr,&rrRefResolve),"PT reference resolve");
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=32;bd.Usage=D3D11_USAGE_DYNAMIC;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        Check(device->CreateBuffer(&bd,nullptr,&rrRefConstants),"PT reference constants");
    }
    bool reset=!e.refAcc.tex || guides.reset || guides.serial!=e.refSerial+1;
    if(e.refAcc.tex){D3D11_TEXTURE2D_DESC d{};e.refAcc.tex->GetDesc(&d);if(d.Width!=e.w||d.Height!=e.h){e.refAcc=RrTexture{};reset=true;}}
    if(!e.refAcc.tex)MakeRrTexture(e.refAcc,e.w,e.h,DXGI_FORMAT_R32G32B32A32_FLOAT);
    // The camera pose only: jitter and the projection stay out. A person moving in a still camera is not detected.
    for(int k=0;k<16;k++)if(std::fabs(guides.viewToWorld[k]-e.refPose[k])>1e-5f)reset=true;
    memcpy(e.refPose,guides.viewToWorld,64);e.refSerial=guides.serial;
    e.refFrames=reset?1:e.refFrames+1;
    struct Cb{UINT w,h,ow,oh,reset,pad0;float n,pad1;} c={e.w,e.h,e.ow,e.oh,reset?1u:0u,0,float(e.refFrames),0};
    ContextScope scope(context.Get(),state.Get());context->ClearState();VamWriteConstants(context,rrRefConstants.Get(),&c,sizeof c);
    context->CSSetConstantBuffers(0,1,rrRefConstants.GetAddressOf());context->CSSetSamplers(0,1,rrSampler.GetAddressOf());
    context->CSSetShader(rrRefAccumulate.Get(),nullptr,0);
    ID3D11ShaderResourceView* in[]={e.color.srv.Get(),nullptr};context->CSSetShaderResources(0,2,in);
    ID3D11UnorderedAccessView* outs[]={e.refAcc.uav.Get(),nullptr};context->CSSetUnorderedAccessViews(0,2,outs,nullptr);
    context->Dispatch((e.w+7)/8,(e.h+7)/8,1);
    ID3D11UnorderedAccessView* none[2]={};context->CSSetUnorderedAccessViews(0,2,none,nullptr);
    context->CSSetShader(rrRefResolve.Get(),nullptr,0);
    ID3D11ShaderResourceView* in2[]={nullptr,e.refAcc.srv.Get()};context->CSSetShaderResources(0,2,in2);
    ID3D11UnorderedAccessView* outs2[]={nullptr,e.output.uav.Get()};context->CSSetUnorderedAccessViews(0,2,outs2,nullptr);
    context->Dispatch((e.ow+7)/8,(e.oh+7)/8,1);
    context->CSSetUnorderedAccessViews(0,2,none,nullptr);
    if(i==0 && GetTickCount64()>=rrRefLog){rrRefLog=GetTickCount64()+5000;OOVR_LOGF("VAM PT reference eye=0 frames=%u",e.refFrames);}
}
Ptr<ID3D11ComputeShader> rrPrep, rrComposite, rrDetailPass;
bool rrDetailPath=false;
Ptr<ID3D11ComputeShader> rrPathGuidePass;
Ptr<ID3D11Buffer> rrPathGuideConstants;
Ptr<ID3D11Buffer> rrConstants;
Ptr<ID3D11SamplerState> rrSampler;
RrTexture rrExposure;
bool rrFailed = false;
ULONGLONG rrRetryAt = 0;
bool rrWaitingForHdr = false;
uint64_t rrFrames = 0;
unsigned rrPathFrames = 0;
unsigned rrDiagnosticFrames=0;
bool PathDiagnosticRequested() {
    static std::wstring request=[](){wchar_t p[1024];return GetEnvironmentVariableW(L"VAM_PT_DUMP",p,1024)?std::wstring(p)+L"/request":std::wstring();}();
    if(request.empty())return false;
    if(DeleteFileW(request.c_str()))rrDiagnosticFrames=2;
    if(!rrDiagnosticFrames)return false;
    --rrDiagnosticFrames;return true;
}
void DumpPathCamera(const rtc::PathView& p,uint64_t serial,bool reset) {
    wchar_t folder[1024],path[1200];
    if(!GetEnvironmentVariableW(L"VAM_PT_DUMP",folder,1024))return;
    swprintf_s(path,L"%s/camera-%u.json",folder,unsigned(serial));FILE* f=_wfopen(path,L"wb");if(!f)return;
    fprintf(f,"{\"serial\":%llu,\"reset\":%d,\"width\":%u,\"height\":%u,\"jitter\":[%g,%g]",serial,reset?1:0,p.width,p.height,p.jitter[0],p.jitter[1]);
    const float* matrices[]={p.clipToWorld,p.worldToClip,p.previousWorldToClip};
    const char* names[]={"clipToWorld","worldToClip","previousWorldToClip"};
    for(int m=0;m<3;m++){fprintf(f,",\"%s\":[",names[m]);for(int i=0;i<16;i++)fprintf(f,"%s%.9g",i?",":"",matrices[m][i]);fprintf(f,"]");}
    fprintf(f,"}\n");fclose(f);
}
// Explicit development capture: two adjacent input/guide/output sets, after
// warmup. No staging textures or readbacks unless VAM_PT_DUMP is present.
void DumpPathTexture(ID3D11Texture2D* source,const wchar_t* name,unsigned frame) {
    wchar_t folder[1024];
    if(!source || !GetEnvironmentVariableW(L"VAM_PT_DUMP",folder,1024))return;
    D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);
    unsigned bpp=d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT||d.Format==DXGI_FORMAT_R32G32_FLOAT?8:
        d.Format==DXGI_FORMAT_R32G32B32A32_FLOAT?16:
        d.Format==DXGI_FORMAT_R32_FLOAT||d.Format==DXGI_FORMAT_R16G16_FLOAT?4:
        d.Format==DXGI_FORMAT_R16_FLOAT?2:0;
    if(!bpp)return;
    d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Ptr<ID3D11Texture2D> staging;
    Check(device->CreateTexture2D(&d,nullptr,&staging),"PT diagnostic staging");
    context->CopyResource(staging.Get(),source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"PT diagnostic map");
    wchar_t path[1200];swprintf_s(path,L"%s/%s-%u.bin",folder,name,frame);
    FILE* file=_wfopen(path,L"wb");
    if(file) {
        unsigned header[]={d.Width,d.Height,(unsigned)d.Format,bpp};fwrite(header,4,4,file);
        for(unsigned y=0;y<d.Height;y++)fwrite((const char*)mapped.pData+y*mapped.RowPitch,bpp,d.Width,file);
        fclose(file);
    }
    context->Unmap(staging.Get(),0);
}
// source = the camera copy; left / right = each eye's guides, D3D12 work (PT and RR) and detail; composite = both eyes' composites.
inline static const char* const rrStageNames[4] = {"source", "left", "right", "composite"};
GpuStageTimer<4> rrTimer{"RR F HDR upscaling, both eyes", rrStageNames};
RrPathStageTimer rrPathTimer;
struct RrConstants {
    UINT w,h,ow,oh,origin,hasOpaque,pad[2];
    float jitterX,jitterY,tolerance,detail;
    float clipToWorld[16];
    UINT views=VamViewCount(), layoutPad[3]{};
};
RrConstants rrPendingConstants[2]{};
uint64_t rrPendingSerial=0;
Ptr<ID3D11PixelShader> rrHandoffPs;
Ptr<ID3D11DepthStencilState> rrHandoffDepth;
Ptr<ID3D11RasterizerState> rrHandoffRaster;
static constexpr GUID kPathOwnedProgram={0xe32d8c57,0xd7c9,0x4a21,{0x91,0x65,0x43,0x2e,0xaa,0x57,0x19,0x31}};
static constexpr const char* rrShader = R"(
cbuffer RrCb : register(b0) {
    uint w,h,ow,oh,origin,hasOpaque; uint2 pad;
    float2 jitter; float tolerance,detail;
    row_major float4x4 clipToWorld;
    uint views;uint3 layoutPad;
}
Texture2D<float4> C : register(t0);
Texture2D<float4> A : register(t1);
Texture2D<float4> N : register(t2);
Texture2D<float4> S : register(t3);
Texture2D<float> Z : register(t4);
Texture2D<float> D : register(t5);
Texture2D<float4> R : register(t6);
Texture2D<float4> Reference : register(t7);
Texture2D<float4> Opaque : register(t8);
Texture2D<float4> DetailHistory : register(t9);
Texture2D<float2> Motion : register(t10);
Texture2D<float> UiMask : register(t11);
Texture2D<float4> UiBlack : register(t12);
Texture2D<float4> UiWhite : register(t13);
Texture2D<float> OverlayDepth : register(t15);
Texture2D<float4> FxBlack : register(t16);
Texture2D<float4> FxWhite : register(t17);
RWTexture2D<float4> O0 : register(u0);
RWTexture2D<float4> O1 : register(u1);
RWTexture2D<float4> O2 : register(u2);
RWTexture2D<float4> O3 : register(u3);
RWTexture2D<float> O4 : register(u4);
RWTexture2D<float4> O5 : register(u5);
SamplerState linearClamp : register(s0);
float2 LayerUv(float2 uv,float eyeOrigin) {
    // Packed stereo layers must never interpolate into the other eye.
    return clamp(uv,float2((eyeOrigin+.5)/(views*w),.5/h),float2((eyeOrigin+w-.5)/(views*w),1-.5/h));
}
int2 Raw(int2 p) { return int2(p.x+origin,h-1-p.y); }
bool Valid(int2 p) {
    p=clamp(p,int2(0,0),int2(w-1,h-1)); int2 q=Raw(p);
    float z=Z.Load(int3(q,0)),d=D.Load(int3(p,0));
    float3 n=N.Load(int3(q,0)).xyz;
    return z>=0 && abs(z-d)<=tolerance && dot(n,n)>.25;
}
float3 Position(int2 p) {
    p=clamp(p,int2(0,0),int2(w-1,h-1));
    float2 uv=(float2(p)+.5-jitter)/float2(w,h);
    float4 v=mul(clipToWorld,float4(uv*float2(2,-2)+float2(-1,1),1-2*D.Load(int3(p,0)),1));
    return v.xyz/(abs(v.w)>1e-8?v.w:1e-8);
}
// Transparents drawn over the skin after the opaque pass (sheer stockings, lace, see-through clothing: they write no
// depth, so the skin's guides pass the depth test under them) change what the pixel shows. 1 = the opaque result
// shows unchanged; 0 = changed by 1/8 of its brightness or more (the Detail pass below uses the same measure).
// pad.y bit 128 (RRTransparentGuides, default on): the skin guides fade to the raster fallback by it.
float SkinShows(int2 q) {
    if(hasOpaque==0 || (pad.y&128u)==0)return 1;
    float3 before=max(Opaque.Load(int3(q,0)).rgb,0),after=max(C.Load(int3(q,0)).rgb,0);
    return saturate(1-8*length(after-before)/max(length(before),.05));
}
[numthreads(8,8,1)] void Prepare(uint3 id:SV_DispatchThreadID) {
    if(id.x>=w||id.y>=h)return;
    int2 p=id.xy,q=Raw(p); bool skin=Valid(p);
    float shows=skin?SkinShows(q):0;
    float4 color=float4(max(C.Load(int3(q,0)).rgb,0),1);
    float depth=D.Load(int3(p,0));
    float4 normal;
    [branch] if(skin && shows>=1)normal=N.Load(int3(q,0));
    else {
        // Depth edges must not turn the foreground/background gap into a
        // material normal. Use the nearest-depth neighbour on each axis and
        // orient the resulting world normal toward this camera ray.
        int2 l=max(p-int2(1,0),int2(0,0)),r=min(p+int2(1,0),int2(w-1,h-1));
        int2 u=max(p-int2(0,1),int2(0,0)),b=min(p+int2(0,1),int2(w-1,h-1));
        bool left=p.x>0 && (abs(D.Load(int3(l,0))-depth)<abs(D.Load(int3(r,0))-depth) || p.x==int(w)-1);
        bool up=p.y>0 && (abs(D.Load(int3(u,0))-depth)<abs(D.Load(int3(b,0))-depth) || p.y==int(h)-1);
        float3 v=Position(p),dx=left?v-Position(l):Position(r)-v,dy=up?v-Position(u):Position(b)-v;
        float2 uv=(float2(p)+.5-jitter)/float2(w,h);
        float4 nearPoint=mul(clipToWorld,float4(uv*float2(2,-2)+float2(-1,1),-1,1));
        float3 toward=nearPoint.xyz/(abs(nearPoint.w)>1e-8?nearPoint.w:1e-8)-v;
        float3 n=cross(dy,dx);n=dot(n,n)>1e-15?normalize(n):normalize(toward+float3(0,0,1e-15));
        if(dot(n,toward)<0)n=-n;
        normal=float4(depth>0?n:0,1);
        if(skin) {
            float4 sn=N.Load(int3(q,0));float3 m=lerp(normal.xyz,sn.xyz,shows);
            normal=float4(dot(m,m)>1e-12?normalize(m):sn.xyz,sn.w);
        }
    }
    // Raster-only materials have no stochastic secondary specular signal.
    // Their fallback is a neutral diffuse guide with a geometric depth normal;
    // skin uses the exact texture albedo, shading normal and reflectance (faded by what covers it).
    float4 neutral=float4(depth>0?1:0,depth>0?1:0,depth>0?1:0,1);
    O0[p]=color;
    O1[p]=skin?lerp(neutral,float4(saturate(A.Load(int3(q,0)).rgb),1),shows):neutral;
    O2[p]=normal;
    O3[p]=skin?lerp(float4(0,0,0,1),S.Load(int3(q,0)),shows):float4(0,0,0,1);
    O4[p]=depth;
    O5[p]=hasOpaque?float4(max(Opaque.Load(int3(q,0)).rgb,0),1):color;
}
// Restore only missing unlit material colour, after the current RR result.
// Legacy guides are packed stereo; their alpha stores regional SSS response,
// not a PT instance id or confidence. Keep that contract separate.
float MissingMaterialDetail(float referenceRatio,float reconstructedRatio);
#ifdef RR_STEREO
float3 StereoResolve(float3 color,uint2 pixel);
#endif
bool LegacyDetailValid(int2 p,int2 center) {
    p=clamp(p,int2(0,0),int2(w-1,h-1));
    float z=D.Load(int3(p,0)),cz=D.Load(int3(center,0));
    return Valid(p) && abs(z-cz)<max(cz*.01,1e-6) &&
        dot(N.Load(int3(Raw(p),0)).xyz,N.Load(int3(Raw(center),0)).xyz)>.8;
}
[numthreads(8,8,1)] void Detail(uint3 id:SV_DispatchThreadID) {
    if(id.x>=w||id.y>=h)return;
    float2 pos=float2(id.xy)+jitter;
    int2 p=clamp(int2(floor(pos+.5)),int2(0,0),int2(w-1,h-1)),base=int2(floor(pos));
    bool valid=Valid(p);float depth=D.Load(int3(p,0));
    [unroll] for(int by=0;by<2;by++) [unroll] for(int bx=0;bx<2;bx++)
        valid=valid && LegacyDetailValid(base+int2(bx,by),p);
    float3 ratio=1;
    if(valid) {
        float2 uv=float2(pos.x+origin+.5,h-pos.y-.5)/float2(views*w,h);
        float2 resolvedUv=(float2(id.xy)+.5)/float2(w,h);
        float3 fine=Reference.SampleLevel(linearClamp,LayerUv(uv,origin),0).rgb,sum=0;
        float3 resolved=R.SampleLevel(linearClamp,resolvedUv,0).rgb,resolvedSum=0;float weight=0;
        [unroll] for(int dy=-1;dy<=1;dy++) [unroll] for(int dx=-1;dx<=1;dx++) {
            int2 q=clamp(p+int2(dx,dy),int2(0,0),int2(w-1,h-1));
            float k=LegacyDetailValid(q,p)?(dx==0?2:1)*(dy==0?2:1):0;
            sum+=Reference.SampleLevel(linearClamp,LayerUv(uv+float2(dx,-dy)/float2(views*w,h),origin),0).rgb*k;
            resolvedSum+=R.SampleLevel(linearClamp,resolvedUv+float2(dx,dy)/float2(w,h),0).rgb*k;weight+=k;
        }
        ratio=clamp((fine+.003)/(sum/max(weight,1)+.003),.5,2);
        float3 present=(max(resolved,0)+.003)/(max(resolvedSum/max(weight,1),0)+.003);
        ratio=float3(MissingMaterialDetail(ratio.r,present.r),MissingMaterialDetail(ratio.g,present.g),MissingMaterialDetail(ratio.b,present.b));
        float3 before=Opaque.Load(int3(Raw(p),0)).rgb,after=C.Load(int3(Raw(p),0)).rgb;
        float cover=hasOpaque?saturate(1-8*length(after-before)/max(length(before),.05)):1;
        ratio=lerp(1.0,ratio,cover);
        float2 prev=(float2(id.xy)+.5+Motion.Load(int3(p,0)))/float2(w,h);
        float4 old=DetailHistory.SampleLevel(linearClamp,prev,0);
        bool reuse=pad.x==0 && all(prev>0) && all(prev<1) && old.a>0 && abs(old.a-depth)<max(depth*.03,1e-6);
        float trust=saturate(1-4*length(old.rgb-ratio));
        if(reuse)ratio=lerp(ratio,old.rgb,.75*trust);
    }
    O0[id.xy]=float4(ratio,valid?depth:0);
}
bool PathDetailValid(int2 p,int2 center) {
    float4 a=A.Load(int3(p,0)),b=A.Load(int3(center,0));
    float z=D.Load(int3(p,0)),cz=D.Load(int3(center,0));
    return Reference.Load(int3(p,0)).a>0 && a.a==b.a &&
        S.Load(int3(p,0)).a==S.Load(int3(center,0)).a &&
        abs(z-cz)<max(cz*.01,1e-6) && dot(N.Load(int3(p,0)).xyz,N.Load(int3(center,0)).xyz)>.8;
}
float MissingMaterialDetail(float referenceRatio,float reconstructedRatio) {
    float wanted=referenceRatio-1, present=reconstructedRatio-1;
    // RR already resolves part (or all) of this band. Multiplying its complete
    // reference band again double-counts those normals/highlights. Keep an
    // opposing shadow edge or stronger existing contrast unchanged.
    if(wanted*present<0)return 1;
    float missing=max(abs(wanted)-abs(present),0);
    return 1+sign(wanted)*missing/max(reconstructedRatio,.003);
}
[numthreads(8,8,1)] void DetailPt(uint3 id:SV_DispatchThreadID) {
    if(id.x>=w||id.y>=h)return;
    float2 pos=float2(id.xy)+jitter;
    int2 p=clamp(int2(floor(pos+.5)),int2(0,0),int2(w-1,h-1));
    int2 base=int2(floor(pos));
    bool valid=Reference.Load(int3(p,0)).a>0 && D.Load(int3(p,0))>0;
    // Reference is jittered; resolve its subpixel phase before forming the
    // detail residual. Nearest loads turned the jitter cycle into block noise.
    [unroll] for(int by=0;by<2;by++) [unroll] for(int bx=0;bx<2;bx++)
        valid=valid && PathDetailValid(clamp(base+int2(bx,by),int2(0,0),int2(w-1,h-1)),p);
    float3 ratio=1;float depth=D.Load(int3(p,0));
    if(valid) {
        float2 uv=(pos+.5)/float2(w,h);
        float4 material=Reference.SampleLevel(linearClamp,uv,0);
        float3 fine=material.rgb,sum=0;float weight=0;
        float2 resolvedUv=(float2(id.xy)+.5)/float2(w,h);
        float3 resolved=R.SampleLevel(linearClamp,resolvedUv,0).rgb,resolvedSum=0;
        [unroll] for(int dy=-1;dy<=1;dy++) [unroll] for(int dx=-1;dx<=1;dx++) {
            int2 q=clamp(p+int2(dx,dy),int2(0,0),int2(w-1,h-1));
            float k=PathDetailValid(q,p)?(dx==0?2:1)*(dy==0?2:1):0;
            float2 delta=float2(dx,dy)/float2(w,h);
            sum+=Reference.SampleLevel(linearClamp,uv+delta,0).rgb*k;
            resolvedSum+=R.SampleLevel(linearClamp,resolvedUv+delta,0).rgb*k;weight+=k;
        }
        ratio=clamp((fine+.003)/(sum/max(weight,1)+.003),.5,2.0);
        float3 present=(max(resolved,0)+.003)/(max(resolvedSum/max(weight,1),0)+.003);
        ratio=float3(MissingMaterialDetail(ratio.r,present.r),MissingMaterialDetail(ratio.g,present.g),MissingMaterialDetail(ratio.b,present.b));
        ratio=lerp(1.0,ratio,saturate(material.a));
        float2 prev=(float2(id.xy)+.5+Motion.Load(int3(p,0)))/float2(w,h);
        float4 old=DetailHistory.SampleLevel(linearClamp,prev,0);
        bool reuse=pad.x==0 && all(prev>0) && all(prev<1) && old.a>0 && abs(old.a-depth)<max(depth*.03,1e-6);
        float trust=saturate(1-4*length(old.rgb-ratio));
        if(reuse)ratio=lerp(ratio,old.rgb,.75*trust);
    }
    O0[id.xy]=float4(ratio,valid?depth:0);
}
[numthreads(8,8,1)] void Composite(uint3 id:SV_DispatchThreadID) {
    if(id.x>=ow||id.y>=oh)return;
    int2 o=id.xy;
    float3 reconstructed=R.Load(int3(o,0)).rgb;
    if(detail>0) {
        float4 material=DetailHistory.SampleLevel(linearClamp,(float2(o)+.5)/float2(ow,oh),0);
        reconstructed*=lerp(1.0,clamp(material.rgb,.5,2.0),detail);
    }
#ifdef RR_STEREO
    reconstructed=StereoResolve(reconstructed,id.xy);
#endif
    if((pad.y&32)!=0) {
        float2 uv=(float2(o)+.5)/float2(ow,oh)+jitter/float2(w,h);
        float2 raw=LayerUv(float2((uv.x*w+origin)/(views*w),1-uv.y),origin);
        float3 foreground=FxBlack.SampleLevel(linearClamp,raw,0).rgb;
        float3 transmission=FxWhite.SampleLevel(linearClamp,raw,0).rgb-foreground;
        reconstructed=reconstructed*transmission+foreground;
    }
    if((pad.y&8)!=0) {
        float2 uv=(float2(o)+.5)/float2(ow,oh)+jitter/float2(w,h);
        float2 raw=LayerUv(float2((uv.x*w+origin)/(views*w),1-uv.y),origin);
        float3 foreground=UiBlack.SampleLevel(linearClamp,raw,0).rgb;
        float3 transmission=UiWhite.SampleLevel(linearClamp,raw,0).rgb-foreground;
        int2 p=clamp(int2(uv*float2(w,h)),int2(0,0),int2(w-1,h-1));
        float sceneDepth=Z.Load(int3(p,0));float overlayDepth=OverlayDepth.Load(int3(int2(raw*float2(views*w,h)),0));
        if(overlayDepth>0 && overlayDepth+max(sceneDepth*.005,1e-6)<sceneDepth){foreground=0;transmission=1;}
        reconstructed=reconstructed*transmission+foreground;
    } else if((pad.y&64)!=0) {
        // Legacy UI is already in C. Bypass only pixels that the actual UI
        // shader changed, never an invisible full-screen RectTransform.
        float2 uv=(float2(o)+.5)/float2(ow,oh)+jitter/float2(w,h);
        float2 raw=LayerUv(float2((uv.x*w+origin)/(views*w),1-uv.y),origin);
        float3 f=UiBlack.SampleLevel(linearClamp,raw,0).rgb;
        float3 t=UiWhite.SampleLevel(linearClamp,raw,0).rgb-f;
        if(any(abs(f)>1e-6)||any(abs(t-1)>1e-5))reconstructed=C.SampleLevel(linearClamp,raw,0).rgb;
    } else if((pad.y&1)!=0) {
        float2 uv=(float2(o)+.5)/float2(ow,oh)+jitter/float2(w,h);
        float mask=UiMask.SampleLevel(linearClamp,uv,0);
        float2 raw=float2((uv.x*w+origin)/(views*w),1-uv.y);
        reconstructed=lerp(reconstructed,C.SampleLevel(linearClamp,raw,0).rgb,mask);
    }
    if((pad.y&2)!=0) {
        int2 p=clamp(int2((float2(o)+.5)*float2(w,h)/float2(ow,oh)+jitter),int2(0,0),int2(w-1,h-1));
        int2 at=(pad.y&16)!=0?int2(origin+p.x,h-1-p.y):p;
        if(views==2 && D.Load(int3(at,0))>=.9999)reconstructed=0;
    }
    O0[int2(o.x+(origin/w)*ow,oh-1-o.y)]=float4(all(isfinite(reconstructed))?max(reconstructed,0):0,1);
}
struct OverlayVertex {float4 position:SV_Position;float2 uv:TEXCOORD0;};
OverlayVertex OverlayVs(uint id:SV_VertexID) {
    float2 p=float2((id<<1)&2,id&2);OverlayVertex v;
    v.position=float4(p*float2(2,-2)+float2(-1,1),0,1);v.uv=p;return v;
}
float4 OverlayPs(OverlayVertex v):SV_Target {
    float2 uv=float2((v.uv.x+origin/float(ow))/views,1-v.uv.y);
    float3 color=C.SampleLevel(linearClamp,uv,0).rgb;
    uv=LayerUv(uv,(origin/ow)*w);
    if((pad.y&32)!=0) {
        float3 foreground=FxBlack.SampleLevel(linearClamp,uv,0).rgb;
        color=color*(FxWhite.SampleLevel(linearClamp,uv,0).rgb-foreground)+foreground;
    }
    if((pad.y&8)==0)return float4(max(color,0),1);
    float3 foreground=UiBlack.SampleLevel(linearClamp,uv,0).rgb;
    float3 transmission=UiWhite.SampleLevel(linearClamp,uv,0).rgb-foreground;
    int2 p=clamp(int2(v.uv*float2(w,h)),int2(0,0),int2(w-1,h-1));
    float sceneDepth=Z.Load(int3(p,0));
    float overlayDepth=OverlayDepth.Load(int3(int2(uv*float2(views*w,h)),0));
    if(overlayDepth>0 && overlayDepth+max(sceneDepth*.005,1e-6)<sceneDepth){foreground=0;transmission=1;}
    return float4(max(color*transmission+foreground,0),1);
}
struct HandoffPixel {float4 color:SV_Target0;float depth:SV_Depth;};
HandoffPixel HandoffPs(OverlayVertex v) {
    int2 raw=int2(v.position.xy),p=int2(raw.x-origin,h-1-raw.y);
    float2 uv=(float2(p)+.5-jitter)/float2(w,h);
    float3 color=R.SampleLevel(linearClamp,uv,0).rgb;
    if(detail>0)color*=lerp(1.0,clamp(DetailHistory.SampleLevel(linearClamp,(float2(p)+.5)/float2(w,h),0).rgb,.5,2),detail);
    bool hidden=views==2 && D.Load(int3(raw,0))>=.9999;
    HandoffPixel o;o.color=float4(hidden?0:max(color,0),1);o.depth=hidden?1:Z.Load(int3(p,0));return o;
}

)";
#include "vam_rr_stereo.inl"
// Update all downstream guide formats after the PT dispatch. RW typed loads
// avoid a second full-resolution motion texture; RG32F loads are checked on
// the actual device before the shader is enabled.
static constexpr const char* rrPathGuideShader=R"(
cbuffer Cb:register(b0){uint w,h,origin,uiOn;float2 jitterDelta;float2 pad;}
Texture2D<float> ptDepth:register(t0);
Texture2D<float2> objectMotion:register(t2);
Texture2D<float> uiDepth:register(t3);
Texture2D<float4> uiBlack:register(t4);
Texture2D<float4> uiWhite:register(t5);
Texture2D<float4> hdrBlack:register(t6);
Texture2D<float4> hdrWhite:register(t7);
RWTexture2D<float2> motion:register(u0);
RWTexture2D<float> depth:register(u1);
RWTexture2D<float2> jittered:register(u2);
RWTexture2D<float2> halfMotion:register(u3);
RWTexture2D<float2> halfJittered:register(u4);
RWTexture2D<float> visibleUi:register(u5);
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
    if(id.x>=w||id.y>=h)return;int2 p=id.xy,raw=int2(origin+p.x,h-1-p.y);
    bool hidden=pad.x==2 && depth[p]>=.9999;float z=(uiOn&2)!=0?depth[p]:ptDepth.Load(int3(p,0));float2 m=motion[p];
    float3 foreground=uiBlack.Load(int3(raw,0)).rgb;
    float3 transmission=uiWhite.Load(int3(raw,0)).rgb-foreground;
    bool covered=any(abs(foreground)>1e-6) || any(abs(transmission-1)>1e-5);
    covered=covered && (uiOn&1)!=0;
    if((uiOn&4)!=0) {
        float3 f=hdrBlack.Load(int3(raw,0)).rgb,t=hdrWhite.Load(int3(raw,0)).rgb-f;
        covered=covered || any(abs(f)>1e-6) || any(abs(t-1)>1e-5);
    }
    float uz=uiDepth.Load(int3(raw,0));
    bool uiVisible=covered && uz>0 && uz+max(z*.005,1e-6)>=z && !hidden;
    if(uiVisible) {
        float2 ndc=objectMotion.Load(int3(raw,0));
        if(all(isfinite(ndc)) && all(abs(ndc)<100))m=ndc*(float2(w,h)*float2(.5,-.5));
        z=max(z,uz);
    }
    if(hidden){m=0;z=0;}
    depth[p]=z;motion[p]=m;halfMotion[p]=m;
    float2 j=hidden?0:m+jitterDelta;jittered[p]=j;halfJittered[p]=j;
    // Rectangles alone also cover hidden/transparent UI. Only the pixels
    // actually composited may bypass SR; forward effects never enter this mask.
    if((uiOn&2)!=0)visibleUi[p]=uiVisible?1:0;
}
)";
void UpdatePathGuides(unsigned i,RrEye& e,const VamFrameGuides& guides,ID3D11ShaderResourceView* black=nullptr,ID3D11ShaderResourceView* white=nullptr,bool late=false,bool afterTone=false) {
    if(!rrPathGuidePass) {
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support={DXGI_FORMAT_R32G32_FLOAT,0};
        Check(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2,&support,sizeof support),"PT guide typed load support");
        if(!(support.OutFormatSupport2&D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD))throw std::runtime_error("PT motion typed loads unavailable");
        auto cs=CompileShader(rrPathGuideShader,"vam_pt_guides","main","cs_5_0",nullptr);
        Check(device->CreateComputeShader(cs->GetBufferPointer(),cs->GetBufferSize(),nullptr,&rrPathGuidePass),"PT guide shader");
        D3D11_BUFFER_DESC bd{};bd.ByteWidth=32;bd.Usage=D3D11_USAGE_DYNAMIC;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        Check(device->CreateBuffer(&bd,nullptr,&rrPathGuideConstants),"PT guide constants");
    }
    Ptr<ID3D11ShaderResourceView> hdrB,hdrW;
    if(afterTone)GetPathOverlay(0,e.w*viewCount,e.h,hdrB,hdrW);
    struct Cb{UINT w,h,origin,ui;float delta[2],pad[2];} c={e.w,e.h,i*e.w,(guides.uiMask&&black?1u:0u)|(late?2u:0u)|(guides.uiMask&&hdrB?4u:0u),{info.prevJitterX-guides.jitterX,info.prevJitterY-guides.jitterY},{float(viewCount),0}};
    auto& eye=eyes[i];
    ContextScope scope(context.Get(),state.Get());context->ClearState();VamWriteConstants(context,rrPathGuideConstants.Get(),&c,sizeof c);
    context->CSSetShader(rrPathGuidePass.Get(),nullptr,0);context->CSSetConstantBuffers(0,1,rrPathGuideConstants.GetAddressOf());
    ID3D11ShaderResourceView* src[]={e.depth.srv.Get(),nullptr,objects.motionSrv.Get(),objects.uiDepthSrv.Get(),black,white,hdrB.Get(),hdrW.Get()};context->CSSetShaderResources(0,8,src);
    ID3D11UnorderedAccessView* dst[]={eye.motionUav.Get(),eye.depthUav.Get(),eye.motionJitteredUav.Get(),eye.motionHalfUav.Get(),eye.motionJitteredHalfUav.Get(),eye.uiMaskUav.Get()};
    context->CSSetUnorderedAccessViews(0,6,dst,nullptr);context->Dispatch((e.w+7)/8,(e.h+7)/8,1);
    eye.pathGuides=guides;eye.pathGuideSerial=guides.serial;
}
// toD3d12: the texture goes to the D3D12 list every eye. Made in D3D12 and wrapped (COMMON in and out, acquired except
// while that list has it), so the hand-over is a release and an acquire instead of an unwrap's queue syncs (vam_on12.h).
// Freeing one the GPU may still use: DropRrTexture.
void MakeRrTexture(RrTexture& t, UINT w, UINT h, DXGI_FORMAT format, bool toD3d12=false) {
    t.r12.Reset();
    if(toD3d12) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=w; d.Height=h;
        d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1; d.Format=format; d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Check(on12.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&t.r12)),"RR D3D12 texture");
        D3D11_RESOURCE_FLAGS f{}; f.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        Check(on12.on12->CreateWrappedResource(t.r12.Get(),&f,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COMMON,IID_PPV_ARGS(&t.tex)),"RR wrapped texture");
        ID3D11Resource* take=t.tex.Get(); on12.on12->AcquireWrappedResources(&take,1);
    } else {
        D3D11_TEXTURE2D_DESC d{};
        d.Width=w; d.Height=h; d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
        d.Format=format; d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        Check(device->CreateTexture2D(&d,nullptr,&t.tex),"RR texture");
    }
    Check(device->CreateShaderResourceView(t.tex.Get(),nullptr,&t.srv),"RR SRV");
    Check(device->CreateUnorderedAccessView(t.tex.Get(),nullptr,&t.uav),"RR UAV");
}
// D3D11 keeps an unwrapped texture alive until its queue is past it; it does not know about D3D12 use of a wrapped one.
void DropRrTexture(RrTexture& t) {
    if(t.r12)on12Commands.Drain();
    t=RrTexture{};
}
void ReleaseRrEye(RrEye& e) {
    if(e.feature) { on12Commands.Drain(); NVSDK_NGX_D3D12_ReleaseFeature(e.feature); }
    if(e.color.r12)on12Commands.Drain();  // wrapped textures: DropRrTexture
    if(e.params) NVSDK_NGX_D3D12_DestroyParameters(e.params);
    e=RrEye{};
}
void ReleaseRr() {
    if(auto skin=GetModuleHandleW(L"VaMSkinTransmissionNative.bin")) {
        auto ready=reinterpret_cast<void(__cdecl*)(int)>(GetProcAddress(skin,"ST_SetPathConsumerReadyV1"));if(ready)ready(0);
    }
    for(auto& e:rrEyes) ReleaseRrEye(e);
    rrPrep.Reset(); rrComposite.Reset(); rrRefAccumulate.Reset(); rrRefResolve.Reset(); rrRefConstants.Reset(); rrDetailPass.Reset(); rrConstants.Reset(); rrSampler.Reset(); DropRrTexture(rrExposure);
    rrPathGuidePass.Reset();rrPathGuideConstants.Reset();for(auto& e:eyes)e.pathGuideSerial=0;
    rrScene.Reset(); rrSceneRtv.Reset();rrSceneUav.Reset(); rrSceneSerial=0; rrCamera.Reset(); rrBlitter.reset();
    rrSceneSrv.Reset();rrOverlayVs.Reset();rrOverlayPs.Reset();rrFallbackSerial=rrCompletedSerial=rrToneSerial=0;
    rrSource.Reset();rrSourceSrv.Reset();rrSourceW=rrSourceH=0;
    rrPendingSerial=0;rrHandoffPs.Reset();rrHandoffDepth.Reset();rrHandoffRaster.Reset();
    rrStereoComposite.Reset();rrStereoConstants.Reset();
    // The reusable context-state object otherwise retains the last RR SRVs.
    { ContextScope scope(context.Get(),state.Get()); context->ClearState(); }
}
void EnsureRrScene(UINT ow,UINT oh) {
    if(rrScene && rrSceneW==ow && rrSceneH==oh)return;
    rrScene.Reset();rrSceneRtv.Reset();rrSceneUav.Reset();rrSceneSrv.Reset();
    rrSceneSerial=rrToneSerial=rrFallbackSerial=rrCompletedSerial=0;
    D3D11_TEXTURE2D_DESC full{};full.Width=ow*viewCount;full.Height=oh;full.MipLevels=full.ArraySize=full.SampleDesc.Count=1;
    full.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;full.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS;
    Check(device->CreateTexture2D(&full,nullptr,&rrScene),"RR full output");
    Check(device->CreateRenderTargetView(rrScene.Get(),nullptr,&rrSceneRtv),"RR full output RTV");
    Check(device->CreateUnorderedAccessView(rrScene.Get(),nullptr,&rrSceneUav),"RR full output UAV");
    Check(device->CreateShaderResourceView(rrScene.Get(),nullptr,&rrSceneSrv),"RR full output SRV");
    rrSceneW=ow;rrSceneH=oh;
    rrPostSize[0].store(ow*viewCount);rrPostSize[1].store(oh);
}
void EnsureRr(UINT w,UINT h,UINT ow,UINT oh) {
    if(!on12) throw std::runtime_error("RR requires the 11on12 queue");
    // Queue resources belong to reconstruction, not to an earlier SR/NGX
    // evaluation. Direct PT startup must not call Begin on an uninitialized ring.
    on12Commands.Init(on12.device.Get(),on12.queue.Get());
    InitializeNgx();
    // A D3D12 fence alone cannot cover the previous frame's D3D11 composite:
    // 11on12 may still have it buffered on the CPU. Submit it before waiting,
    // releasing the old wrapped textures and creating the replacement feature.
    // This boundary is needed only when an existing consumer changes size/type.
    bool replacing=false;
    for(unsigned i=0;i<viewCount;i++) {
        const auto& e=rrEyes[i];
        replacing|=e.color.tex && (e.w!=w || e.h!=h || e.ow!=ow || e.oh!=oh);
    }
    if(replacing) {
        context->Flush();
        on12Commands.Drain();
        OOVR_LOGF("VAM RR resize synchronized: %ux%u -> %ux%u; PT scene retained",w,h,ow,oh);
    }
    // RR owns the upscaler; release the previous K/L/M features and staging.
    for(auto& eye:eyes) if(eye.feature) { ReleaseFeature(eye); eye.color.Reset(); eye.output.Reset();
        eye.composite.Reset(); eye.compositeUav.Reset(); eye.colorSrv.Reset(); eye.outputSrv.Reset(); }
    if(!rrPrep) {
        auto compile=[&](const char* entry,Ptr<ID3D11ComputeShader>& dst) {
            Ptr<ID3DBlob> b,err;
            HRESULT hr=D3DCompile(rrShader,strlen(rrShader),"vam_rr",nullptr,nullptr,entry,"cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&b,&err);
            if(FAILED(hr)) throw std::runtime_error(err?static_cast<const char*>(err->GetBufferPointer()):"RR shader compile");
            Check(device->CreateComputeShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&dst),"RR compute");
        };
        compile("Prepare",rrPrep); compile("Composite",rrComposite);
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth=sizeof(RrConstants); bd.Usage=D3D11_USAGE_DYNAMIC;
        bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        Check(device->CreateBuffer(&bd,nullptr,&rrConstants),"RR constants");
        D3D11_SAMPLER_DESC sd{}; sd.Filter=D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD=D3D11_FLOAT32_MAX;
        Check(device->CreateSamplerState(&sd,&rrSampler),"RR sampler");
        MakeRrTexture(rrExposure,1,1,DXGI_FORMAT_R32_FLOAT,true);
        const float one[4]={1,1,1,1}; context->ClearUnorderedAccessViewFloat(rrExposure.uav.Get(),one);
    }
    for(unsigned i=0;i<viewCount;i++) {
        auto& e=rrEyes[i];
        if(e.color.tex&&e.w==w&&e.h==h&&e.ow==ow&&e.oh==oh)continue;
        ReleaseRrEye(e);
        CheckNgx(NVSDK_NGX_D3D12_GetCapabilityParameters(&e.params),"RR parameters");
        for(auto key:{NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA,
            NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,
            NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced,
            NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,
            NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance}) NVSDK_NGX_Parameter_SetUI(e.params,key,6);
        NVSDK_NGX_DLSSD_Create_Params c{}; c.InWidth=w; c.InTargetWidth=ow; c.InHeight=h; c.InTargetHeight=oh;
        c.InPerfQualityValue=QualityFor(w,h,ow,oh);
        unsigned recommendedW=0,recommendedH=0,maxW=0,maxH=0,minW=0,minH=0;float ignoredSharpness=0;
        auto settings=NGX_DLSSD_GET_OPTIMAL_SETTINGS(e.params,ow,oh,c.InPerfQualityValue,
            &recommendedW,&recommendedH,&maxW,&maxH,&minW,&minH,&ignoredSharpness);
        OOVR_LOGF("VAM RR F settings result=%x mode=%u input=%ux%u recommended=%ux%u range=%ux%u..%ux%u",
            unsigned(settings),unsigned(c.InPerfQualityValue),w,h,recommendedW,recommendedH,minW,minH,maxW,maxH);
        c.InFeatureCreateFlags=NVSDK_NGX_DLSS_Feature_Flags_IsHDR|NVSDK_NGX_DLSS_Feature_Flags_MVLowRes|NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
        c.InRoughnessMode=NVSDK_NGX_DLSS_Roughness_Mode_Packed; c.InUseHWDepth=NVSDK_NGX_DLSS_Depth_Type_HW;
        auto* list=on12Commands.Begin();
        auto result=NGX_D3D12_CREATE_DLSSD_EXT(list,1,1,&e.feature,e.params,&c);
        on12Commands.Submit(); CheckNgx(result,"create RR F upscaler");
        for(auto* t:{&e.color,&e.albedo,&e.normal,&e.spec}) MakeRrTexture(*t,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,true);
        MakeRrTexture(e.output,ow,oh,DXGI_FORMAT_R16G16B16A16_FLOAT,true);
        MakeRrTexture(e.depth,w,h,DXGI_FORMAT_R32_FLOAT,true); e.w=w; e.h=h; e.ow=ow; e.oh=oh;
        // The raster route has no specular secondary rays. PT overwrites this
        // initialization with actual primary specular hit distance each frame.
        MakeRrTexture(e.specDistance,w,h,DXGI_FORMAT_R16_FLOAT,true);
        const float zero[4]={}; context->ClearUnorderedAccessViewFloat(e.specDistance.uav.Get(),zero);
        OOVR_LOGF("VAM RR created eye=%u %ux%u -> %ux%u HDR F; no SR feature",unsigned(&e-rrEyes),w,h,ow,oh);
    }
}
void SetRrDetailEnabled(bool enabled,bool path) {
    if(rrDetailPath!=path) { rrDetailPass.Reset();for(auto& e:rrEyes)e.detailSerial=0;rrDetailPath=path; }
    if(!enabled) {
        bool allocated=rrDetailPass || rrEyes[0].detailHistory[0].tex || rrEyes[1].detailHistory[0].tex;
        if(!allocated)return;
        // Drop the reusable context state's SRV/UAV references too, including
        // the skin reference that its owner has just released at frame start.
        { ContextScope scope(context.Get(),state.Get()); context->ClearState(); }
        for(auto& e:rrEyes) {
            for(auto& t:e.detailHistory)t=RrTexture{};
            DropRrTexture(e.pathReference);
            e.detailIndex=0; e.detailSerial=0;
        }
        rrDetailPass.Reset();
        OOVR_LOG("VAM RR detail disabled: reference=0 history_textures=0 detail_shader=0; RR stays active");
        return;
    }
    if(!rrDetailPass) {
        Ptr<ID3DBlob> b,err;
        HRESULT hr=D3DCompile(rrShader,strlen(rrShader),"vam_rr",nullptr,nullptr,path?"DetailPt":"Detail","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&b,&err);
        if(FAILED(hr))throw std::runtime_error(err?static_cast<const char*>(err->GetBufferPointer()):"RR detail compile");
        Check(device->CreateComputeShader(b->GetBufferPointer(),b->GetBufferSize(),nullptr,&rrDetailPass),"RR detail compute");
    }
    for(unsigned i=0;i<viewCount;i++) {
        auto& e=rrEyes[i];
        if(path && !e.pathReference.tex)MakeRrTexture(e.pathReference,e.w,e.h,DXGI_FORMAT_R16G16B16A16_FLOAT,true);
        if(!path)DropRrTexture(e.pathReference);
        if(e.detailHistory[0].tex)continue;
        for(auto& t:e.detailHistory)MakeRrTexture(t,e.w,e.h,DXGI_FORMAT_R16G16B16A16_FLOAT);
        const float neutral[]={1,1,1,0};
        for(auto& t:e.detailHistory)context->ClearUnorderedAccessViewFloat(t.uav.Get(),neutral);
        e.detailIndex=0; e.detailSerial=0;
        OOVR_LOGF("VAM RR detail enabled eye=%u: history_textures=2 size=%ux%u reset=1",i,e.w,e.h);
    }
}
bool Reconstruct(ID3D11Resource* color,ID3D11Resource* albedo,ID3D11Resource* normal,ID3D11Resource* spec,ID3D11Resource* skinDepth,ID3D11Resource* reference,ID3D11Resource* opaque,float tolerance,float detail) {
    if(rrFailed||!rrCamera||!haveCapture)return false;
    // RR runs only while PT is switched on (vam_performance_config.h); K/L/M take the frame otherwise.
    if(ReadVamPerformanceSettings().dlssPreset!=6)return false;
    bool pathDispatchFailed=false;
    color=rrCamera.Get();
    try {
        Ptr<ID3D11Texture2D> target; Check(color->QueryInterface(IID_PPV_ARGS(&target)),"RR target");
        D3D11_TEXTURE2D_DESC td{}; target->GetDesc(&td);
        if(TypedColor(td.Format)!=DXGI_FORMAT_R16G16B16A16_FLOAT || td.Width%viewCount || td.Width<viewCount || td.SampleDesc.Count!=1) {
            // A launch directly into RR can capture one LDR camera target before
            // the managed HDR owner has updated Unity. This is not an NGX failure.
            if(!rrWaitingForHdr)OOVR_LOG("VAM RR waiting for the double-wide HDR eye target; will retry next capture");
            rrWaitingForHdr=true;
            return false;
        }
        if(rrWaitingForHdr)OOVR_LOG("VAM RR HDR eye target ready; reconstruction resumed");
        rrWaitingForHdr=false;
        const UINT w=td.Width/viewCount,h=td.Height;
        const auto& rec=VamRecommendedSize(); const int percent=VamDesktopDisplay()?100:VamLiveRenderSettings().outputPercent;
        const UINT finalW=VamOutputDimension(rec.fullWidth.load(),percent),finalH=VamOutputDimension(rec.fullHeight.load(),percent);
        const UINT ow=finalW,oh=finalH;
        if(w>ow||h>oh)return false;
        using PtEnabledFn=int(__cdecl*)();
        using PtRecordFn=int(__cdecl*)(ID3D12GraphicsCommandList*,const rtc::PathView*);
        using PtFinishFn=int(__cdecl*)();
        HMODULE skin=GetModuleHandleW(L"VaMSkinTransmissionNative.bin");
        auto ptEnabled=skin?(PtEnabledFn)GetProcAddress(skin,"ST_PathTracingEnabledV1"):nullptr;
        auto ptRecord=skin?(PtRecordFn)GetProcAddress(skin,"ST_RecordPathTraceV1"):nullptr;
        auto ptFinish=skin?(PtFinishFn)GetProcAddress(skin,"ST_FinishPathTraceV1"):nullptr;
        auto sceneVersion=skin?reinterpret_cast<uint64_t(__cdecl*)()>(GetProcAddress(skin,"ST_PathSceneRevisionV1")):nullptr;
        auto consumerReady=skin?reinterpret_cast<void(__cdecl*)(int)>(GetProcAddress(skin,"ST_SetPathConsumerReadyV1")):nullptr;
        const uint64_t revision=sceneVersion?sceneVersion():0;
        const bool pathTracing=ptEnabled&&ptRecord&&ptFinish&&ptEnabled()!=0;
        EnsureRr(w,h,ow,oh);
        if(consumerReady)consumerReady(1);
        pathDispatchFailed=pathTracing;
        for(unsigned i=0;i<viewCount;i++) {
            auto& e=rrEyes[i];
            if(!pathTracing && e.pathTracing) {
                // Same-size RR features survive a renderer switch. The raster route
                // has no secondary specular rays: never reuse PT's hit distances.
                const float zero[4]={};context->ClearUnorderedAccessViewFloat(e.specDistance.uav.Get(),zero);
            }
            if(pathTracing)DropRrTexture(e.opaque);
            else if(!e.opaque.tex)MakeRrTexture(e.opaque,e.w,e.h,DXGI_FORMAT_R16G16B16A16_FLOAT,true);
        }
        if(!pathTracing){rrPathGuidePass.Reset();rrPathGuideConstants.Reset();}
        rrPathFrames=pathTracing?rrPathFrames+1:0;
        detail=(pathTracing||reference)?std::clamp(detail,0.f,1.f):0.f;
        SetRrDetailEnabled(detail>0,pathTracing);
        // The existing material reference also marks mixed coverage / diffuse share.
        // Stereo must not force the user to turn detail restoration on.
        if(pathTracing && viewCount==2 && ReadVamPerformanceSettings().rrStereo>0)
            for(auto& e:rrEyes)if(!e.pathReference.tex)MakeRrTexture(e.pathReference,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,true);
        EnsureRrScene(ow,oh);
        rrPostSize[2].store(w*viewCount);rrPostSize[3].store(h);
        rrTimer.Begin(device.Get(), immediate.Get(), 0);
        // Immutable source for both eyes and the composite (avoids SRV/RTV aliasing).
        td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.BindFlags=D3D11_BIND_SHADER_RESOURCE; td.MiscFlags=td.CPUAccessFlags=0; td.Usage=D3D11_USAGE_DEFAULT;
        // Cached at size changes below; never allocate a new texture each frame.
        if(pathTracing) {rrSource.Reset();rrSourceSrv.Reset();rrSourceW=rrSourceH=0;}
        else if(!rrSource || rrSourceW!=td.Width || rrSourceH!=td.Height) {
            rrSource.Reset(); rrSourceSrv.Reset(); Check(device->CreateTexture2D(&td,nullptr,&rrSource),"RR camera copy");
            Check(device->CreateShaderResourceView(rrSource.Get(),nullptr,&rrSourceSrv),"RR camera SRV");
            rrSourceW=td.Width; rrSourceH=td.Height;
        }
        if(!pathTracing)context->CopyResource(rrSource.Get(),target.Get());
        rrTimer.Mark(0);
        Ptr<ID3D11ShaderResourceView> a,n,s,z,ref,op;
        if(albedo) Check(device->CreateShaderResourceView(albedo,nullptr,&a),"RR albedo view");
        if(normal) Check(device->CreateShaderResourceView(normal,nullptr,&n),"RR normal view");
        if(spec) Check(device->CreateShaderResourceView(spec,nullptr,&s),"RR specular view");
        if(skinDepth) Check(device->CreateShaderResourceView(skinDepth,nullptr,&z),"RR skin depth view");
        if(reference) Check(device->CreateShaderResourceView(reference,nullptr,&ref),"RR material reference view");
        if(opaque) Check(device->CreateShaderResourceView(opaque,nullptr,&op),"RR before transparency view");
        const bool diagnostic=PathDiagnosticRequested();
        Ptr<ID3D11ShaderResourceView> overlayBlack,overlayWhite;
        GetPathOverlay(0,td.Width,td.Height,overlayBlack,overlayWhite);
        RrConstants compositeConstants[2]{};
        Ptr<ID3D11ShaderResourceView> compositeDepth[2],compositeUi[2];
        for(unsigned i=0;i<viewCount;i++) {
            VamFrameGuides guides; float bounds[4]={float(i)/viewCount,1,float(i+1)/viewCount,0};
            if(!PrepareGuides(target.Get(),bounds,i,w,h,guides,false,true)) throw std::runtime_error("RR current-frame guides unavailable");
            auto& e=rrEyes[i];
            if(e.sceneRevision!=revision){guides.reset=true;eyes[i].sceneResetSerial=guides.serial;}
            Ptr<ID3D11ShaderResourceView> d,m,ui;
            Check(device->CreateShaderResourceView(guides.depth,nullptr,&d),"RR scene depth view");
            Check(device->CreateShaderResourceView(guides.motion,nullptr,&m),"RR material motion view");
            if(pathTracing && guides.uiMask)Check(device->CreateShaderResourceView(guides.uiMask,nullptr,&ui),"PT UI mask view");
            RrConstants c{}; c.w=w; c.h=h; c.ow=ow; c.oh=oh; c.origin=i*w; c.hasOpaque=opaque?1u:0u;
            c.pad[0]=guides.reset || !e.detailSerial || guides.serial!=e.detailSerial+1 || e.pathTracing!=pathTracing ? 1u : 0u;
            c.pad[1]=(pathTracing?(16u|(viewCount==2?2u:0u)):0u)|(overlayBlack?(pathTracing?8u:64u):0u)
                |(!pathTracing && ReadVamPerformanceSettings().rrTransparentGuides?128u:0u);
            c.jitterX=guides.jitterX; c.jitterY=guides.jitterY; c.tolerance=tolerance; c.detail=std::clamp(detail,0.f,1.f);
            float invProjection[16]; if(!Invert(guides.viewToClip,invProjection))throw std::runtime_error("RR inverse projection");
            Multiply(guides.viewToWorld,invProjection,c.clipToWorld);
            {
                ContextScope scope(context.Get(),state.Get());
                VamWriteConstants(context,rrConstants.Get(),&c,sizeof(c));
                if(!pathTracing) {
                context->CSSetShader(rrPrep.Get(),nullptr,0); context->CSSetConstantBuffers(0,1,rrConstants.GetAddressOf());
                ID3D11ShaderResourceView* views[]={rrSourceSrv.Get(),a.Get(),n.Get(),s.Get(),z.Get(),d.Get(),nullptr,ref.Get(),op.Get()}; context->CSSetShaderResources(0,9,views);
                ID3D11UnorderedAccessView* uavs[]={e.color.uav.Get(),e.albedo.uav.Get(),e.normal.uav.Get(),e.spec.uav.Get(),e.depth.uav.Get(),e.opaque.uav.Get()};
                context->CSSetUnorderedAccessViews(0,6,uavs,nullptr); context->Dispatch((w+7)/8,(h+7)/8,1);
                }
            }
            {
                VamOn12Unwrapped r(on12,{e.color.On12(),e.albedo.On12(),e.normal.On12(),e.spec.On12(),e.depth.On12(),guides.motion,rrExposure.On12(),e.output.On12(),e.specDistance.On12(),e.opaque.On12(),e.pathReference.On12()});
                context->Flush(); auto* list=on12Commands.Begin(); constexpr auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                constexpr auto write=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                rrPathTimer.Begin(on12.device.Get(),on12.queue.Get(),list,on12Commands.CompletedValue(),i,guides.serial);
                if(pathTracing) {
                    r.Transition(list,{write,write,write,write,write,write,read,write,write,read,write});
                    rtc::PathView p{};p.width=w;p.height=h;p.samples=1;p.maxBounces=4;p.seed=(UINT)guides.serial;p.eye=i;
                    p.jitter[0]=guides.jitterX;p.jitter[1]=guides.jitterY;
                    p.textureLodBias=(info.flags&4)?0:(std::log2(float(w)/finalW)-1)*ReadVamPerformanceSettings().dlssMipBias/100.f;
                    memcpy(p.clipToWorld,c.clipToWorld,64);
                    if(!Invert(c.clipToWorld,p.worldToClip))throw std::runtime_error("PT camera matrix");
                    Multiply(guides.clipToPrevClip,p.worldToClip,p.previousWorldToClip);
                    p.camera[0]=guides.viewToWorld[3];p.camera[1]=guides.viewToWorld[7];p.camera[2]=guides.viewToWorld[11];
                    p.color=r[0];p.diffuse=r[1];p.normal=r[2];p.specular=r[3];p.depth=r[4];p.hitDistance=r[8];
                    p.motion=r[5];
                    p.detailReference=r[10];
                    if(diagnostic && i==0)DumpPathCamera(p,guides.serial,guides.reset);
                    const int result=ptRecord(list,&p);
                    if(result!=1) {
                        r.Restore(list,{write,write,write,write,write,write,read,write,write,read,write});
                        on12Commands.Submit();pathDispatchFailed=ptFinish()==1;
                        throw std::runtime_error("PT scene/material dispatch unavailable");
                    }
                    D3D12_RESOURCE_BARRIER barriers[8]{};unsigned count=0;
                    for(unsigned at:{0u,1u,2u,3u,4u,5u,8u,10u}) {
                        if(!r[at])continue;
                        barriers[count].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        barriers[count++].Transition={r[at],D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,write,read};
                    }
                    list->ResourceBarrier(count,barriers);
                } else r.Transition(list,{read,read,read,read,read,read,read,write,read,read});
                rrPathTimer.PathEnd(list);
                float worldToView[16],projection[16],view[16];
                if(!Invert(guides.viewToWorld,view)) throw std::runtime_error("RR camera inverse");
                // NGX matrices are row-vector, D3D view/clip. Normals are world-space.
                float converted[16]; Multiply(kFlipZ,view,converted); Transpose(converted,worldToView);
                Multiply(kGlToD3d,guides.viewToClip,converted); float p[16]; Multiply(converted,kFlipZ,p); Transpose(p,projection);
                NVSDK_NGX_D3D12_DLSSD_Eval_Params v{};
                v.pInColor=r[0]; v.pInDiffuseAlbedo=r[1]; v.pInNormals=r[2]; v.pInSpecularAlbedo=r[3]; v.pInDepth=r[4];
                v.pInMotionVectors=r[5]; v.pInExposureTexture=r[6]; v.pInOutput=r[7];
                v.pInSpecularHitDistance=r[8];
                v.pInColorBeforeTransparency=opaque&&!pathTracing?r[9]:nullptr;
                v.pInWorldToViewMatrix=worldToView; v.pInViewToClipMatrix=projection;
                v.InJitterOffsetX=guides.jitterX; v.InJitterOffsetY=guides.jitterY;
                v.InMVScaleX=v.InMVScaleY=1; v.InRenderSubrectDimensions={w,h};
                v.InPreExposure=v.InExposureScale=1; v.InFrameTimeDeltaInMsec=guides.frameDeltaMs;
                v.InReset=guides.reset||!e.serial||guides.serial!=e.serial+1||e.pathTracing!=pathTracing;
                if(pathTracing && (rrPathFrames==1||rrPathFrames==64||rrPathFrames==65))
                    OOVR_LOGF("VAM PT history eye=%u frame=%u serial=%llu previous=%llu reset=%d jitter=%g,%g",i,rrPathFrames,guides.serial,e.serial,v.InReset,guides.jitterX,guides.jitterY);
                const NVSDK_NGX_Result result=NGX_D3D12_EVALUATE_DLSSD_EXT(list,e.feature,e.params,&v);
                rrPathTimer.End(list);
                r.Restore(list,{read,read,read,read,read,read,read,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,read,read,read});
                on12Commands.Submit();
                rrPathTimer.Submitted(on12Commands.SubmittedValue());
                if(pathTracing&&ptFinish()!=1)throw std::runtime_error("PT texture return failed");
                if(NVSDK_NGX_FAILED(result))pathDispatchFailed=false;
                CheckNgx(result,"evaluate RR");
            }
            const bool referenceView=pathTracing && PathReferenceActive();
            if(referenceView){PathReference(i,e,guides);c.detail=0;}
            else if(e.refAcc.tex){e.refAcc=RrTexture{};e.refFrames=0;}
            if(detail>0 && !referenceView) {
                ContextScope scope(context.Get(),state.Get());
                context->CSSetShader(rrDetailPass.Get(),nullptr,0);context->CSSetConstantBuffers(0,1,rrConstants.GetAddressOf());
                context->CSSetSamplers(0,1,rrSampler.GetAddressOf());
                ID3D11ShaderResourceView* views[]={pathTracing?nullptr:rrSourceSrv.Get(),
                    pathTracing?e.albedo.srv.Get():a.Get(),pathTracing?e.normal.srv.Get():n.Get(),pathTracing?e.spec.srv.Get():s.Get(),
                    pathTracing?nullptr:z.Get(),pathTracing?e.depth.srv.Get():d.Get(),e.output.srv.Get(),
                    pathTracing?e.pathReference.srv.Get():ref.Get(),pathTracing?nullptr:op.Get(),e.detailHistory[e.detailIndex].srv.Get(),m.Get()};
                context->CSSetShaderResources(0,11,views);e.detailIndex^=1;
                context->CSSetUnorderedAccessViews(0,1,e.detailHistory[e.detailIndex].uav.GetAddressOf(),nullptr);
                context->Dispatch((w+7)/8,(h+7)/8,1);e.detailSerial=guides.serial;
            }
            if(diagnostic && GetEnvironmentVariableW(L"VAM_RR_DISTANCE_CHECK",nullptr,0)) {
                DumpPathTexture(e.specDistance.tex.Get(),L"specular-distance",unsigned(guides.serial*2+i));
            } else if(diagnostic && i==0) {
                unsigned serial=unsigned(guides.serial);
                if(!pathTracing) {
                    rtc::PathView metadata{};metadata.width=w;metadata.height=h;
                    metadata.jitter[0]=guides.jitterX;metadata.jitter[1]=guides.jitterY;
                    memcpy(metadata.clipToWorld,c.clipToWorld,64);Invert(c.clipToWorld,metadata.worldToClip);
                    Multiply(guides.clipToPrevClip,metadata.worldToClip,metadata.previousWorldToClip);
                    DumpPathCamera(metadata,guides.serial,guides.reset);
                    DumpPathTexture(albedo?static_cast<ID3D11Texture2D*>(albedo):nullptr,L"raster-albedo",serial);
                    DumpPathTexture(normal?static_cast<ID3D11Texture2D*>(normal):nullptr,L"raster-normal",serial);
                }
                auto writeScene=reinterpret_cast<int(__cdecl*)(const wchar_t*,uint64_t)>(GetProcAddress(skin,"ST_WritePathSceneV1"));
                wchar_t folder[1024],file[1200];
                if(pathTracing && writeScene && GetEnvironmentVariableW(L"VAM_PT_DUMP",folder,1024)) {
                    swprintf_s(file,L"%s/scene-%u.json",folder,serial);writeScene(file,guides.serial);
                }
                DumpPathTexture(e.color.tex.Get(),L"color",serial);
                DumpPathTexture(e.albedo.tex.Get(),L"albedo",serial);
                DumpPathTexture(e.normal.tex.Get(),L"normal",serial);
                DumpPathTexture(e.depth.tex.Get(),L"depth",serial);
                DumpPathTexture(e.spec.tex.Get(),L"specular",serial);
                DumpPathTexture(e.specDistance.tex.Get(),L"specular-distance",serial);
                DumpPathTexture(guides.motion,L"motion",serial);
                DumpPathTexture(e.output.tex.Get(),L"output",serial);
                if(detail>0)DumpPathTexture(e.detailHistory[e.detailIndex].tex.Get(),L"detail",serial);
            }
            compositeConstants[i]=c;compositeDepth[i]=d;compositeUi[i]=ui;
            if(pathTracing)UpdatePathGuides(i,e,guides,overlayBlack.Get(),overlayWhite.Get());
            e.serial=guides.serial; e.calls++;e.pathTracing=pathTracing;e.sceneRevision=revision;
            rrTimer.Mark(1+i);
        }
        const bool earlyPath=pathTracing && (info.flags&16)!=0;
        rrStereoFrame=pathTracing && viewCount==2 && !rrRefOn && ReadVamPerformanceSettings().rrStereo>0;
        if(earlyPath) {
            for(unsigned i=0;i<viewCount;i++)rrPendingConstants[i]=compositeConstants[i];
            rrPendingSerial=info.serial;
            HandoffPathTarget(target.Get());
        }
        // Publish both eyes only after both evaluations succeeded. A failed
        // right eye must not leave a new-left/old-right frame in rrScene.
        for(unsigned i=0;i<viewCount && !earlyPath;i++) {
            auto& e=rrEyes[i];auto& c=compositeConstants[i];
            ContextScope scope(context.Get(),state.Get());
            VamWriteConstants(context,rrConstants.Get(),&c,sizeof c);
            context->CSSetShader(rrComposite.Get(),nullptr,0);context->CSSetConstantBuffers(0,1,rrConstants.GetAddressOf());
            context->CSSetSamplers(0,1,rrSampler.GetAddressOf());
            ID3D11ShaderResourceView* views[]={rrSourceSrv.Get(),a.Get(),n.Get(),s.Get(),pathTracing?e.depth.srv.Get():z.Get(),pathTracing?depthSrv.Get():compositeDepth[i].Get(),e.output.srv.Get(),ref.Get(),op.Get(),e.detailHistory[e.detailIndex].srv.Get(),nullptr,compositeUi[i].Get(),overlayBlack.Get(),overlayWhite.Get(),nullptr,objects.uiDepthSrv.Get()};
            context->CSSetShaderResources(0,16,views);context->CSSetUnorderedAccessViews(0,1,rrSceneUav.GetAddressOf(),nullptr);
            BindRrStereo(i,compositeConstants);
            context->Dispatch((ow+7)/8,(oh+7)/8,1);
            ID3D11UnorderedAccessView* none=nullptr;context->CSSetUnorderedAccessViews(0,1,&none,nullptr);
        }
        // Native RR frames bypass the submission upscaler, so publish
        // directly even when tone mapping is disabled.
        if(!earlyPath) {
            if(info.flags&4)context->CopyResource(target.Get(),rrScene.Get());
            rrCompletedSerial=info.serial;rrSceneSerial=static_cast<uint64_t>(info.serial);rrFallbackSerial=0;rrToneSerial=0;
        }
        rrTimer.End();
        if(++rrFrames==1 || rrFrames%600==0) {
            OOVR_LOGF("VAM RR frames=%llu left=%llu right=%llu input=%ux%u output=%ux%u model=F pathTracing=%d; SR evaluations=0",rrFrames,rrEyes[0].calls,rrEyes[1].calls,w,h,ow,oh,pathTracing?1:0);
        }
        return true;
    } catch(const std::exception& e) {
        rrPendingSerial=0;
        on12Commands.Discard();
        rrTimer.Abandon();
        for(auto& eye:eyes){eye.pathGuideSerial=0;eye.sceneResetSerial=info.serial;}
        info.flags|=1;
        // Keep the previous coherent stereo output for this failed frame only.
        rrFallbackSerial=rrSceneSerial?info.serial:0;
        if(HMODULE skin=GetModuleHandleW(L"VaMSkinTransmissionNative.bin")) {
            auto finish=(int(__cdecl*)())GetProcAddress(skin,"ST_FinishPathTraceV1");if(finish)finish();
            auto stop=(void(__cdecl*)(int))GetProcAddress(skin,"ST_SetPathTracingV1");
            if(stop)stop(0); // PT off: the raster route with K/L/M from the next frame
        }
        for(auto& eye:rrEyes){eye.serial=eye.detailSerial=0;}
        if(pathDispatchFailed) {
            OOVR_LOGF("VAM PT switched off after a failure, the raster route resumes next frame: %s",e.what());
        } else {
            rrFailed=true;rrRetryAt=GetTickCount64()+1000;
            OOVR_LOGF(VamDesktopDisplay()?"VAM RR device/resource failure; retry in 1s: %s":"VAM RR disabled, returning to the raster route: %s",e.what());
        }
        return false;
    }
}
Ptr<ID3D11Texture2D> rrSource;
Ptr<ID3D11ShaderResourceView> rrSourceSrv;
UINT rrSourceW=0,rrSourceH=0;

Ptr<ID3D11Texture2D> rrCamera,rrScene;
Ptr<ID3D11RenderTargetView> rrSceneRtv;
Ptr<ID3D11UnorderedAccessView> rrSceneUav;
Ptr<ID3D11ShaderResourceView> rrSceneSrv;
Ptr<ID3D11VertexShader> rrOverlayVs;
Ptr<ID3D11PixelShader> rrOverlayPs;
UINT rrSceneW=0,rrSceneH=0;
uint64_t rrSceneSerial=0,rrToneSerial=0,rrFallbackSerial=0,rrCompletedSerial=0;
bool GetPathOverlay(UINT stage,UINT width,UINT height,Ptr<ID3D11ShaderResourceView>& black,Ptr<ID3D11ShaderResourceView>& white) {
    auto module=GetModuleHandleW(L"VaMSkinTransmissionNative.bin");
    using Get=int(__cdecl*)(UINT,UINT,UINT,ID3D11Resource**,ID3D11Resource**);
    auto get=module?reinterpret_cast<Get>(GetProcAddress(module,"ST_GetPathOverlayV1")):nullptr;
    ID3D11Resource *b=nullptr,*w=nullptr;
    if(!get || get(stage,width,height,&b,&w)!=1)return false;
    Check(device->CreateShaderResourceView(b,nullptr,&black),"PT UI foreground view");
    Check(device->CreateShaderResourceView(w,nullptr,&white),"PT UI transmission view");return true;
}
// Early physical resolve supplies the actual PT depth to later raster effects
// and editor UI. Their original shaders then depth-test against hair/alpha
// geometry too. The full RR output is kept unpublished until late composition.
void HandoffPathTarget(ID3D11Texture2D* target) {
    if(!rrHandoffPs) {
        auto vs=CompileShader(rrShader,"PT handoff","OverlayVs","vs_5_0",nullptr);
        auto ps=CompileShader(rrShader,"PT handoff","HandoffPs","ps_5_0",nullptr);
        Check(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&rrOverlayVs),"PT handoff VS");
        Check(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&rrHandoffPs),"PT handoff PS");
        const UINT owned=1;rrHandoffPs->SetPrivateData(kPathOwnedProgram,sizeof owned,&owned);
        D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=TRUE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;dd.DepthFunc=D3D11_COMPARISON_ALWAYS;
        Check(device->CreateDepthStencilState(&dd,&rrHandoffDepth),"PT handoff depth");
        D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
        Check(device->CreateRasterizerState(&rd,&rrHandoffRaster),"PT handoff raster");
    }
    Ptr<ID3D11DepthStencilView> depth;immediate->OMGetRenderTargets(0,nullptr,&depth);
    if(!depth)throw std::runtime_error("PT early handoff has no scene depth");
    D3D11_DEPTH_STENCIL_VIEW_DESC dd;depth->GetDesc(&dd);
    if(dd.Flags) {Ptr<ID3D11Resource> resource;depth->GetResource(&resource);dd.Flags=0;depth.Reset();Check(device->CreateDepthStencilView(resource.Get(),&dd,&depth),"PT writable depth view");}
    D3D11_RENDER_TARGET_VIEW_DESC rv{};rv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;rv.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    Ptr<ID3D11RenderTargetView> output;Check(device->CreateRenderTargetView(target,&rv,&output),"PT camera handoff RTV");
    ContextScope scope(context.Get(),state.Get());context->ClearState();
    context->OMSetRenderTargets(1,output.GetAddressOf(),depth.Get());context->OMSetDepthStencilState(rrHandoffDepth.Get(),0);
    context->RSSetState(rrHandoffRaster.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(rrOverlayVs.Get(),nullptr,0);context->PSSetShader(rrHandoffPs.Get(),nullptr,0);
    context->PSSetConstantBuffers(0,1,rrConstants.GetAddressOf());context->PSSetSamplers(0,1,rrSampler.GetAddressOf());
    for(unsigned i=0;i<viewCount;i++) {
        auto& e=rrEyes[i];auto& c=rrPendingConstants[i];VamWriteConstants(context,rrConstants.Get(),&c,sizeof c);
        D3D11_VIEWPORT viewport{float(i*e.w),0,float(e.w),float(e.h),0,1};context->RSSetViewports(1,&viewport);
        ID3D11ShaderResourceView* src[]={e.depth.srv.Get(),depthSrv.Get(),e.output.srv.Get(),nullptr,nullptr,e.detailHistory[e.detailIndex].srv.Get()};
        context->PSSetShaderResources(4,6,src);context->Draw(3,0);
    }
    Ptr<ID3D11Resource> depthResource;depth->GetResource(&depthResource);
    auto skin=GetModuleHandleW(L"VaMSkinTransmissionNative.bin");
    auto ready=skin?reinterpret_cast<int(__cdecl*)(ID3D11Resource*,ID3D11Resource*)>(GetProcAddress(skin,"ST_SetPathDepthReadyV1")):nullptr;
    if(!ready||ready(target,depthResource.Get())!=1)throw std::runtime_error("PT depth handoff target changed");
}
bool FinishPathFrame(uint64_t serial) {
    if(!rrPendingSerial || rrPendingSerial!=serial)return false;
    rrPendingSerial=0;
    auto skin=GetModuleHandleW(L"VaMSkinTransmissionNative.bin");
    auto valid=skin?reinterpret_cast<int(__cdecl*)()>(GetProcAddress(skin,"ST_PathFrameValidV1")):nullptr;
    if(!valid || !valid()) {
        rrFallbackSerial=rrSceneSerial?serial:0;info.flags|=1;
        for(auto& eye:eyes){eye.pathGuideSerial=0;eye.sceneResetSerial=serial;}
        auto stop=skin?reinterpret_cast<void(__cdecl*)(int)>(GetProcAddress(skin,"ST_SetPathTracingV1")):nullptr;if(stop)stop(0);
        OOVR_LOG("VAM PT late scene coverage rejected: preserving coherent stereo; original route resumes");return true;
    }
    Ptr<ID3D11ShaderResourceView> black,white,fxBlack,fxWhite;
    GetPathOverlay(0,rrEyes[0].w*viewCount,rrEyes[0].h,black,white);
    GetPathOverlay(2,rrEyes[0].w*viewCount,rrEyes[0].h,fxBlack,fxWhite);
    for(unsigned i=0;i<viewCount;i++) {
        auto& e=rrEyes[i];auto c=rrPendingConstants[i];c.pad[1]=(c.pad[1]&~40u)|(black?8u:0u)|(fxBlack?32u:0u);
        ContextScope scope(context.Get(),state.Get());VamWriteConstants(context,rrConstants.Get(),&c,sizeof c);
        context->CSSetShader(rrComposite.Get(),nullptr,0);context->CSSetConstantBuffers(0,1,rrConstants.GetAddressOf());context->CSSetSamplers(0,1,rrSampler.GetAddressOf());
        ID3D11ShaderResourceView* src[]={nullptr,nullptr,nullptr,nullptr,e.depth.srv.Get(),depthSrv.Get(),e.output.srv.Get(),nullptr,nullptr,e.detailHistory[e.detailIndex].srv.Get(),nullptr,nullptr,black.Get(),white.Get(),nullptr,objects.uiDepthSrv.Get(),fxBlack.Get(),fxWhite.Get()};
        context->CSSetShaderResources(0,18,src);context->CSSetUnorderedAccessViews(0,1,rrSceneUav.GetAddressOf(),nullptr);
        BindRrStereo(i,rrPendingConstants);
        context->Dispatch((e.ow+7)/8,(e.oh+7)/8,1);
    }
    DumpRrStereo(rrPendingConstants);
    rrSceneSerial=rrCompletedSerial=serial;rrFallbackSerial=rrToneSerial=0;
    if(info.flags&4)context->CopyResource(rrCamera.Get(),rrScene.Get());
    for(unsigned i=0;i<viewCount;i++) {
        VamFrameGuides guides=eyes[i].pathGuides;
        if(guides.serial==serial)UpdatePathGuides(i,rrEyes[i],guides,black.Get(),white.Get(),true);
    }
    if(rrFrames<3 || rrFrames%600==0)OOVR_LOGF("VAM PT late composition serial=%llu UI=%d effects=%d",serial,black?1:0,fxBlack?1:0);
    return true;
}
std::unique_ptr<D3D11EyeBlitter> rrBlitter;
void EnsureRrBlitter() {
    if(rrBlitter)return;
    rrBlitter=std::make_unique<D3D11EyeBlitter>();
    Check(rrBlitter->Initialize(device.Get(),immediate.Get()),"RR output blitter");
}
bool ToneRr(int id) {
    if(!rrToneMapEvent || !rrScene || !rrSceneSerial || rrSceneSerial!=static_cast<uint64_t>(info.serial) || !rrCallback.load())return false;
    Ptr<ID3D11RenderTargetView> current; immediate->OMGetRenderTargets(1,&current,nullptr);
    if(!current)return false;
    Ptr<ID3D11Resource> resource; current->GetResource(&resource);
    Ptr<ID3D11Texture2D> target; if(FAILED(resource.As(&target)))return false;
    D3D11_TEXTURE2D_DESC d{};target->GetDesc(&d);
    if(d.Width!=rrEyes[0].w*viewCount || d.Height!=rrEyes[0].h)return false;
    try {
        { ContextScope scope(context.Get(),state.Get());
          context->OMSetRenderTargets(1,rrSceneRtv.GetAddressOf(),nullptr);
          rrToneMapEvent(id); }
        rrToneSerial=rrSceneSerial;
        EnsureRrBlitter(); const float full[]={0,0,1,1};
        Check(rrBlitter->Copy(rrScene.Get(),current.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,d.Width,d.Height,0,full),"RR Unity tone handoff");
        return true;
    } catch(const std::exception& e) { OOVR_LOGF("RR tone handoff failed: %s",e.what());return false; }
}
// 11-09: the scene's Unity image effects (PostProcessing v1, PostMagic) run on RR's output as they run on the camera's image
// without RR, instead of on a camera image RR then replaces. The bridge binds an output-size target:
// 1 = target from rrScene, 2 = rrScene from target, 3 = target (input size, R32F) from this frame's scene depth, for depth of field.
void PostRr(int id) {
    if(!rrScene || !rrSceneSerial || rrSceneSerial!=static_cast<uint64_t>(info.serial) || !rrCallback.load())return;
    Ptr<ID3D11RenderTargetView> current; immediate->OMGetRenderTargets(1,&current,nullptr);
    if(!current)return;
    Ptr<ID3D11Resource> resource; current->GetResource(&resource);
    Ptr<ID3D11Texture2D> target; if(FAILED(resource.As(&target)))return;
    D3D11_TEXTURE2D_DESC d{},s{};target->GetDesc(&d);rrScene->GetDesc(&s);
    if(id==3) {
        if(d.Width!=rrEyes[0].w*viewCount || d.Height!=rrEyes[0].h)return;
        // PT's depth has its top row first, as RR takes it; Unity's images (rrScene, this target) have their bottom row first.
        try {
            EnsureRrBlitter(); const float flipped[]={0,1,1,0};
            for(unsigned i=0;i<viewCount;i++)Check(rrBlitter->Copy(rrEyes[i].depth.tex.Get(),current.Get(),DXGI_FORMAT_R32_FLOAT,rrEyes[i].w,rrEyes[i].h,0,flipped,i*rrEyes[i].w),"RR image effect depth");
        } catch(const std::exception& e) { OOVR_LOGF("RR image effect depth failed: %s",e.what()); }
        return;
    }
    if(d.Width!=s.Width || d.Height!=s.Height)return;
    if(id==1)immediate->CopyResource(target.Get(),rrScene.Get());
    else if(id==2)immediate->CopyResource(rrScene.Get(),target.Get());
}
// ahead: the same image for an eye whose own submission comes later this frame (UpscaledAhead); its guides and
// the evaluation state are left to that submission.
bool SubmitRr(unsigned eyeIndex,const VamFrameGuides& guides,ID3D11Texture2D* target,UINT ow,UINT oh,bool ahead=false) {
    const bool fallback=rrFallbackSerial==guides.serial;
    if((rrFailed&&!fallback)||!rrScene||(!fallback&&rrSceneSerial!=guides.serial)||rrSceneW!=ow||rrSceneH!=oh)return false;
    EnsureRrBlitter();D3D11_TEXTURE2D_DESC d{};target->GetDesc(&d);
    D3D11_RENDER_TARGET_VIEW_DESC rd{}; rd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
    rd.Format=TypedColor(d.Format);
    if(rd.Format==DXGI_FORMAT_R8G8B8A8_UNORM)rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if(rd.Format==DXGI_FORMAT_B8G8R8A8_UNORM)rd.Format=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    Ptr<ID3D11RenderTargetView> view;Check(device->CreateRenderTargetView(target,&rd,&view),"RR submit target");
    const float bounds[]={float(eyeIndex)/viewCount,1,float(eyeIndex+1)/viewCount,0};
    Ptr<ID3D11ShaderResourceView> black,white,fxBlack,fxWhite;
    if(!fallback) {
        GetPathOverlay(1,rrEyes[eyeIndex].w*viewCount,rrEyes[eyeIndex].h,black,white);
        if(rrEyes[eyeIndex].pathTracing)GetPathOverlay(3,rrEyes[eyeIndex].w*viewCount,rrEyes[eyeIndex].h,fxBlack,fxWhite);
    }
    const bool overlay=black || fxBlack;
    if(overlay) {
        if(!rrOverlayPs) {
            Ptr<ID3DBlob> vs,ps,error;
            Check(D3DCompile(rrShader,strlen(rrShader),"PT overlay",nullptr,nullptr,"OverlayVs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&error),"PT overlay vertex compile");
            Check(D3DCompile(rrShader,strlen(rrShader),"PT overlay",nullptr,nullptr,"OverlayPs","ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&error),"PT overlay pixel compile");
            Check(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),nullptr,&rrOverlayVs),"PT overlay vertex shader");
            Check(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,&rrOverlayPs),"PT overlay pixel shader");
        }
        ContextScope scope(context.Get(),state.Get());context->ClearState();
        RrConstants c{};c.w=rrEyes[eyeIndex].w;c.h=rrEyes[eyeIndex].h;c.ow=ow;c.oh=oh;c.origin=eyeIndex*ow;c.pad[1]=(black?8u:0u)|(fxBlack?32u:0u);VamWriteConstants(context,rrConstants.Get(),&c,sizeof c);
        D3D11_VIEWPORT vp{0,0,float(ow),float(oh),0,1};context->RSSetViewports(1,&vp);
        context->OMSetRenderTargets(1,view.GetAddressOf(),nullptr);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(rrOverlayVs.Get(),nullptr,0);context->PSSetShader(rrOverlayPs.Get(),nullptr,0);
        context->PSSetConstantBuffers(0,1,rrConstants.GetAddressOf());context->PSSetSamplers(0,1,rrSampler.GetAddressOf());
        ID3D11ShaderResourceView* ui[]={black.Get(),white.Get()};context->PSSetShaderResources(12,2,ui);
        context->PSSetShaderResources(4,1,rrEyes[eyeIndex].depth.srv.GetAddressOf());
        context->PSSetShaderResources(15,1,objects.uiDepthSrv.GetAddressOf());
        ID3D11ShaderResourceView* fx[]={fxBlack.Get(),fxWhite.Get()};context->PSSetShaderResources(16,2,fx);
        context->PSSetShaderResources(0,1,rrSceneSrv.GetAddressOf());context->Draw(3,0);
    } else Check(rrBlitter->Copy(rrScene.Get(),view.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,ow,oh,0,bounds),"RR submit");
    if(ahead)return true;
    if(black)UpdatePathGuides(eyeIndex,rrEyes[eyeIndex],guides,black.Get(),white.Get(),true,true);
    eyes[eyeIndex].evaluatedSerial=guides.serial; lastEvaluation=GetTickCount64(); UpdateState();
    if(rrEyes[eyeIndex].calls==1||rrEyes[eyeIndex].calls%600==0)
        OOVR_LOGF("VAM RR F submitted eye=%u serial=%llu tone=%d; %ux%u -> %ux%u, no SR",eyeIndex,guides.serial,rrToneSerial==guides.serial,rrEyes[eyeIndex].w,rrEyes[eyeIndex].h,ow,oh);
    return true;
}
