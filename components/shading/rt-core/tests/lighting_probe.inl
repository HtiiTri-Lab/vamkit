// Fixed energy/visibility fixtures, not a parameter sweep or a quality score.
static bool LightingCheck(rtc::PathView view,const std::string &out,unsigned &nonfinite) {
    constexpr unsigned W=128,H=128;
    view.detailReference=nullptr;view.motion=nullptr;view.samples=8;
    float positions[]={-10,-10,1,10,-10,1,10,10,1,-10,10,1,
        1,-.5f,-1,2,-.5f,-1,2,.5f,-1,1,.5f,-1,
        .25f,-1,0,1.25f,-1,0,1.25f,1,0,.25f,1,0};
    uint32_t indices[]={0,2,1,0,3,2,4,5,6,4,6,7,8,9,10,8,10,11};
    float normals[36]={};for(int i=0;i<12;i++)normals[i*3+2]=i<4?-1.f:1.f;
    auto vb=Data(positions,sizeof positions),ib=Data(indices,sizeof indices),nb=Data(normals,sizeof normals);
    rtc::Submesh sub[4];rtc::MaterialDesc desc;
    for(int i=0;i<4;i++){sub[i].material=rtc::CreateMaterial(desc);sub[i].count=i==1||i==2?3:6;}
    sub[1].start=6;sub[2].start=9;sub[3].start=12;
    for(int i=1;i<=2;i++){sub[i].tess=rtc::Tess::Phong;sub[i].tessLevel=2;sub[i].tessAlpha=1;}
    rtc::PathMaterial matte;matte.flags=8;matte.skin[0]=.0001f;
    matte.skin[1]=matte.skin[2]=matte.skin[3]=matte.skin[4]=1;matte.skin[5]=0;
    for(int c=0;c<3;c++)matte.baseColor[c]=.6f;
    if(!rtc::SetPathMaterial(sub[0].material,matte))return false;
    rtc::PathMaterial emitter;emitter.flags=1024;
    for(int c=0;c<3;c++)emitter.emissionColor[c]=5;
    if(!rtc::SetPathMaterial(sub[1].material,emitter)||!rtc::SetPathMaterial(sub[2].material,emitter))return false;
    auto black=matte;for(int c=0;c<3;c++)black.baseColor[c]=0;
    if(!rtc::SetPathMaterial(sub[3].material,black))return false;
    rtc::Geometry geo[3];
    for(int i=0;i<3;i++) {
        geo[i].id=100+i;geo[i].flags=rtc::kRigid;geo[i].vertexCount=12;
        geo[i].position={vb.Get(),0,12,rtc::VertexFormat::Float3};geo[i].normal={nb.Get(),0,12,rtc::VertexFormat::Float3};
        geo[i].indexBuffer=ib.Get();geo[i].index32=1;
        geo[i].submeshes=sub+(i==0?0:i==1?1:3);geo[i].submeshCount=i==1?2:1;
    }
    rtc::Frame frame;frame.frame=100;frame.completedFrame=99;frame.geometries=geo;frame.geometryCount=1;
    frame.ambientSh[3]=frame.ambientSh[7]=frame.ambientSh[11]=1;
    rtc::SetPathEnvironment(rtc::PathEnvironment{});
    auto record=[&]() {
        Begin();bool ok=rtc::Record(G.list.Get(),frame);SubmitWait();Drain("lighting scene");
        frame.completedFrame=frame.frame++;return ok;
    };
    auto measure=[&](int bounces,int repeats,bool center,double &mean) {
        mean=0;view.maxBounces=bounces;
        for(int k=0;k<repeats;k++) {
            view.seed=1500+k;Begin();if(!rtc::RecordPathTrace(G.list.Get(),view))return false;
            SubmitWait();Drain("lighting estimator");
            auto pixels=ReadTexture(view.color,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
            for(auto h:pixels)nonfinite+=!std::isfinite(Half(h));
            unsigned lo=center?60:0,hi=center?68:128;
            for(unsigned y=lo;y<hi;y++)for(unsigned x=lo;x<hi;x++)
                mean+=Half(pixels[(y*W+x)*4])/(double(repeats)*(hi-lo)*(hi-lo));
        }
        return true;
    };
    double uniform[2],mesh[2],blocked=0;
    if(!record()||!measure(1,4,false,uniform[0])||!measure(2,4,false,uniform[1]))return false;
    // SH is irradiance/pi, not incident radiance. A normal facing -Z with
    // E/pi = 1 + .3*z must reflect .6*(1-.3), after one cosine integration.
    frame.ambientSh[2]=frame.ambientSh[6]=frame.ambientSh[10]=.3f;double directional=0;
    if(!record()||!measure(2,4,false,directional))return false;
    frame.ambientSh[2]=frame.ambientSh[6]=frame.ambientSh[10]=0;
    frame.ambientSh[3]=frame.ambientSh[7]=frame.ambientSh[11]=0;frame.geometryCount=2;
    if(!record()||!measure(1,16,true,mesh[0])||!measure(2,16,true,mesh[1]))return false;
    // Independent area quadrature of L * albedo/pi * cos(receiver)*cos(light)/r².
    double reference=0;
    constexpr int N=128;
    for(unsigned y=60;y<68;y++)for(unsigned x=60;x<68;x++) {
        double px=((x+.5)/W*2-1)*4,py=(1-(y+.5)/H*2)*4;
        for(int j=0;j<N;j++)for(int i=0;i<N;i++) {
            double dx=1+(i+.5)/N-px,dy=-.5+(j+.5)/N-py,r2=dx*dx+dy*dy+4;
            reference+=(5*.6/3.141592653589793)*(4/(r2*r2))/(64*N*N);
        }
    }
    // Both proposals together exercise the RIS normalization. The emitter
    // replaces the unit environment over its solid angle (5 - 1 radiance).
    frame.ambientSh[3]=frame.ambientSh[7]=frame.ambientSh[11]=1;
    double mixed=0,mixedReference=.6+reference*.8;
    if(!record()||!measure(2,16,true,mixed))return false;
    frame.ambientSh[3]=frame.ambientSh[7]=frame.ambientSh[11]=0;
    frame.geometryCount=3;
    if(!record()||!measure(1,2,true,blocked))return false;
    // Legacy zero-opacity is still a physical interface when the PT material
    // is a dielectric. Neither old hidden-geometry culling nor OMM may erase it.
    rtc::MaterialDesc transparent;transparent.kind=rtc::MaterialKind::Translucent;transparent.opacity=0;
    sub[0].material=rtc::CreateMaterial(transparent);
    rtc::PathMaterial glass;glass.flags=2048;glass.transmission=1;glass.ior=1.336f;glass.baseColor[3]=0;
    if(!rtc::SetPathMaterial(sub[0].material,glass))return false;
    // Reflections read the Skyshop lighting cube, as in VaM's programs; Unity's
    // ambient SH is diffuse-only and would not show the sheet at all.
    auto cube=Cube();rtc::PathEnvironment env;env.lighting=cube.Get();env.rgbm=0;env.lightingScale=1;
    if(!cube || !rtc::SetPathEnvironment(env))return false;
    frame.geometryCount=1;
    if(!record())return false;double glassValue;
    if(!measure(2,1,true,glassValue))return false;
    rtc::SetPathEnvironment(rtc::PathEnvironment{});
    auto guide=ReadTexture(view.diffuse,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    // The clear sheet now replaces its primary guide with the sky (instance 0).
    // The lighting cube is bright but the visible background is black:
    // a real interface reflects positive energy; erasing the sheet yields zero.
    bool interfacePass=Half(guide[((H/2)*W+W/2)*4+3])==0 && glassValue>.005 && glassValue<.15;
    bool pass=std::abs(uniform[0]-.6)<.012 && std::abs(uniform[1]-.6)<.012 && std::abs(directional-.42)<.012 &&
        std::abs(mesh[0]/reference-1)<.04 && std::abs(mesh[1]/reference-1)<.04 && std::abs(mixed/mixedReference-1)<.04 && blocked<1e-5 && interfacePass;
    char report[1024];snprintf(report,sizeof report,
        "{\"pass\":%s,\"uniform_direct\":%.8f,\"uniform_mis\":%.8f,\"directional_sh\":%.8f,\"directional_expected\":0.42,\"emitter_reference\":%.8f,\"emitter_direct\":%.8f,\"emitter_mis\":%.8f,\"mixed\":%.8f,\"mixed_reference\":%.8f,\"blocked\":%.8f,\"zero_alpha_interface\":%s,\"interface_energy\":%.8f}\n",
        pass?"true":"false",uniform[0],uniform[1],directional,reference,mesh[0],mesh[1],mixed,mixedReference,blocked,interfacePass?"true":"false",glassValue);
    puts(report);FILE *f=fopen((out+"/lighting.json").c_str(),"wb");fputs(report,f);fclose(f);
    // 0.23.4 (test bit 28): Unity's ambient SH by the surface's shader family. VaM scales it by _ExposureLM.x
    // (Frame::ambientLM) only in its skin family (Custom/Subsurface, Custom/Hair); Standard and Marmoset take it
    // as is. Cleared bit = 0.23.3: x ambientLM on every surface (a stage scene: ambientLM 0, its Standard set unlit).
    double ambient[6]={};
    {
        rtc::MaterialDesc unityDesc,skinDesc;skinDesc.environment=rtc::EnvironmentShader::VamSkin;
        uint32_t unityMat=rtc::CreateMaterial(unityDesc),skinMat=rtc::CreateMaterial(skinDesc);
        if(!unityMat||!skinMat||!rtc::SetPathMaterial(unityMat,matte)||!rtc::SetPathMaterial(skinMat,matte))return false;
        frame.ambientSh[3]=frame.ambientSh[7]=frame.ambientSh[11]=1;frame.geometryCount=1;
        const uint32_t all=0xffffffffu,before=all&~(1u<<28);
        struct Case{uint32_t material;float lm;uint32_t layers;};
        const Case cases[6]={{unityMat,0,all},{skinMat,0,all},{unityMat,2,all},{skinMat,2,all},{unityMat,0,before},{skinMat,2,before}};
        for(int k=0;k<6;k++) {
            sub[0].material=cases[k].material;frame.ambientLM=cases[k].lm;rtc::SetTestLayers(cases[k].layers);
            if(!record()||!measure(1,4,false,ambient[k])){rtc::SetTestLayers(all);return false;}
        }
        rtc::SetTestLayers(all);frame.ambientLM=1;
    }
    const double expected[6]={.6,0,.6,1.2,0,1.2};
    bool ambientPass=true;
    for(int k=0;k<6;k++)ambientPass&=std::abs(ambient[k]-expected[k])<(expected[k]>0?.02*expected[k]:1e-5);
    snprintf(report,sizeof report,
        "{\"pass\":%s,\"unity_lm0\":%.8f,\"skin_lm0\":%.8f,\"unity_lm2\":%.8f,\"skin_lm2\":%.8f,\"before_unity_lm0\":%.8f,\"before_skin_lm2\":%.8f,"
        "\"expected\":[0.6,0,0.6,1.2,0,1.2]}\n",ambientPass?"true":"false",ambient[0],ambient[1],ambient[2],ambient[3],ambient[4],ambient[5]);
    puts(report);f=fopen((out+"/ambient-class.json").c_str(),"wb");fputs(report,f);fclose(f);
    return pass&&ambientPass;
}
