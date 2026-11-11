// Production integrator: a visible receiver, offset light, mesh/fiber blocker.
// Camera and shadow rays cross different locations, so a visible fiber is not
// mistaken for evidence that its shadow query works.
#define RTC_PROBE_MAIN full_contract_main
#include "probe.cpp"
#undef RTC_PROBE_MAIN
int main(int argc,char** argv) {
    if(argc!=2)return 2;std::string out=argv[1];CreateDirectoryA(out.c_str(),nullptr);
    ComPtr<ID3D12Debug> dbg;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))dbg->EnableDebugLayer();
    if(FAILED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_12_1,IID_PPV_ARGS(&G.device))))return 3;
    G.device.As(&G.info);D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    G.device->CreateCommandQueue(&q,IID_PPV_ARGS(&G.queue));G.device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&G.alloc));
    G.device->CreateCommandList(0,q.Type,G.alloc.Get(),nullptr,IID_PPV_ARGS(&G.list));G.list->Close();
    G.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&G.fence));G.event=CreateEventA(nullptr,FALSE,FALSE,nullptr);
    G.queue->GetTimestampFrequency(&G.frequency);char error[256]{};if(!rtc::Init(G.device.Get(),error,sizeof error)){puts(error);return 4;}
    float vertices[]={-10,-10,2,10,-10,2,10,10,2,-10,10,2, .19f,-2,1,.31f,-2,1,.31f,2,1,.19f,2,1};
    uint32_t indices[]={0,2,1,0,3,2,4,6,5,4,7,6};
    auto vb=Data(vertices,sizeof vertices),ib=Data(indices,sizeof indices);
    rtc::MaterialDesc desc;rtc::Submesh subs[2];rtc::Geometry geo[2];
    rtc::PathMaterial matte;for(int k=0;k<3;k++){matte.baseColor[k]=1;matte.specular[k]=0;}matte.roughness=1;
    for(unsigned i=0;i<2;i++) {
        subs[i].material=rtc::CreateMaterial(desc);subs[i].start=i*6;subs[i].count=6;
        if(!rtc::SetPathMaterial(subs[i].material,matte))return 5;
        geo[i].id=i+1;geo[i].flags=rtc::kRigid;geo[i].vertexCount=8;geo[i].position={vb.Get(),0,12,rtc::VertexFormat::Float3};
        geo[i].indexBuffer=ib.Get();geo[i].index32=1;geo[i].submeshes=subs+i;geo[i].submeshCount=1;
    }
    rtc::Light light;light.id=1;light.type=rtc::LightType::Directional;light.direction[0]=.6f;light.direction[2]=.8f;
    light.angularRadius=0;light.radius=0;for(float &c:light.color)c=1;
    rtc::Frame frame;frame.geometries=geo;frame.geometryCount=1;frame.lights=&light;frame.lightCount=1;
    constexpr unsigned W=128,H=128;ComPtr<ID3D12Resource> target[6];
    for(unsigned i=0;i<6;i++)target[i]=Tex(W,H,i<4?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R16_FLOAT);
    rtc::PathView view;view.width=W;view.height=H;view.samples=8;view.maxBounces=1;
    view.color=target[0].Get();view.diffuse=target[1].Get();view.normal=target[2].Get();view.specular=target[3].Get();view.depth=target[4].Get();view.hitDistance=target[5].Get();
    const float a=100.01f/99.99f,b=-2.f/99.99f;
    float projection[]={1,0,0,0,0,1,0,0,0,0,a,b,0,0,1,0};
    memcpy(view.worldToClip,projection,64);if(!Inverse(projection,view.clipToWorld))return 6;
    uint32_t data[32*9]{};
    for(unsigned i=0;i<4;i++){float p[]={.25f,-2.f+i*(4.f/3),1};memcpy(data+i*3,p,12);data[64*3+i*3+1]=0x38003800;data[64*3+i*3+2]=0x3800;}
    auto hairBuffer=Data(data,sizeof data);PathHairItem hair;hair.strands=1;hair.segments=3;hair.radius=.06f;
    double means[6]{};unsigned nonfinite=0,receiverPixels[6]{};
    for(unsigned mode=0;mode<6;mode++) {
        frame.frame=mode+1;frame.completedFrame=mode;frame.geometryCount=mode==1?2:1;light.angularRadius=mode==3||mode==4?.0873f:0;
        // 4, 5 = 3, 2 lit by a point light 1000 m back up the sun's ray: an angular size softens it as it does the sun
        light.type=mode>=4?rtc::LightType::Point:rtc::LightType::Directional;light.range=1e6f;light.radius=0;
        {const float fiber[]={.25f,0,1};for(int k=0;k<3;k++)light.position[k]=fiber[k]-1000*light.direction[k];}
        Begin();if(!rtc::Record(G.list.Get(),frame))return 7;
        if(!rtc::RecordPathHair(G.list.Get(),mode>=2?hairBuffer.Get():nullptr,mode>=2?&hair:nullptr,mode>=2?1:0,mode+1))return 8;
        SubmitWait();Drain("shadow scene");
        for(unsigned k=0;k<8;k++) {
            view.seed=100+k;Begin();if(!rtc::RecordPathTrace(G.list.Get(),view))return 9;SubmitWait();Drain("shadow render");
            auto color=ReadTexture(target[0].Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4),guide=ReadTexture(target[1].Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
            for(unsigned y=48;y<80;y++)for(unsigned x=95;x<97;x++) {
                unsigned p=(y*W+x)*4;float v=Half(color[p]);nonfinite+=!std::isfinite(v);means[mode]+=v/(8*32.0*2);
                receiverPixels[mode]+=Half(guide[p+3])==1;
            }
        }
        printf("mode %u mean %.9f receiver_samples %u\n",mode,means[mode],receiverPixels[mode]);
    }
    bool pass=means[0]>.5&&means[1]<1e-5&&means[2]<1e-5&&receiverPixels[2]==512&&!G.errors&&!nonfinite;
    pass=pass&&means[3]>.01&&std::abs(means[4]/means[3]-1)<1e-3&&means[5]<1e-5;
    char report[512];snprintf(report,sizeof report,"{\"pass\":%s,\"unblocked\":%.9f,\"mesh_shadow\":%.9f,\"lss_shadow\":%.9f,\"lss_five_degree_light\":%.9f,\"lss_five_degree_point\":%.9f,\"lss_point_hard\":%.9f,\"debug_errors\":%u,\"nonfinite\":%u}\n",pass?"true":"false",means[0],means[1],means[2],means[3],means[4],means[5],G.errors,nonfinite);
    puts(report);FILE* f=fopen((out+"/shadow.json").c_str(),"wb");fputs(report,f);fclose(f);
    rtc::Shutdown();return pass?0:1;
}
