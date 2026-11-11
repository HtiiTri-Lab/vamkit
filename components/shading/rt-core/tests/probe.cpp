// A bounded GPU integration check for the new integrator, not a game-quality
// benchmark. Uses the RtCore code under test and the D3D12 debug layer.
#include "probe_gpu.h"
#include "path_tracing.h"
#include <cmath>

static float Half(uint16_t h) {
    unsigned sign=h>>15,ex=(h>>10)&31,m=h&1023;
    float v=ex==0?std::ldexp((float)m,-24):ex==31?(m?NAN:INFINITY):std::ldexp(1.f+m/1024.f,(int)ex-15);
    return sign?-v:v;
}
static ComPtr<ID3D12Resource> Tex(unsigned width,unsigned height,DXGI_FORMAT format,bool uav=true,unsigned mips=1) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;
    d.DepthOrArraySize=1;d.MipLevels=(UINT16)mips;d.SampleDesc.Count=1;d.Format=format;
    d.Flags=uav?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    if(FAILED(G.device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,uav?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r))))return nullptr;
    return r;
}
static ComPtr<ID3D12Resource> Data(const void *p,size_t bytes) {
    auto r=MakeBuffer(bytes,D3D12_HEAP_TYPE_DEFAULT),u=MakeBuffer(bytes,D3D12_HEAP_TYPE_UPLOAD);
    void *dst=nullptr;u->Map(0,nullptr,&dst);memcpy(dst,p,bytes);u->Unmap(0,nullptr);
    Begin();Transition(r.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
    G.list->CopyBufferRegion(r.Get(),0,u.Get(),0,bytes);
    Transition(r.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    SubmitWait();return r;
}
static ComPtr<ID3D12Resource> Cube() {
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=d.Height=4;
    d.DepthOrArraySize=6;d.MipLevels=1;d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    ComPtr<ID3D12Resource> cube;
    if(FAILED(G.device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&cube))))return nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[6];UINT64 bytes;
    G.device->GetCopyableFootprints(&d,0,6,0,fp,nullptr,nullptr,&bytes);
    auto upload=MakeBuffer(bytes,D3D12_HEAP_TYPE_UPLOAD);uint8_t *mapped;
    upload->Map(0,nullptr,(void**)&mapped);memset(mapped,0,(size_t)bytes);
    for(int f=0;f<6;f++)for(int y=0;y<4;y++)for(int x=0;x<4;x++) {
        float *p=(float*)(mapped+fp[f].Offset+y*fp[f].Footprint.RowPitch+x*16);
        p[0]=(float)(f+1);p[1]=.5f;p[2]=.25f;p[3]=1;
    }
    upload->Unmap(0,nullptr);Begin();
    for(int f=0;f<6;f++) {
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp[f];
        dst.pResource=cube.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=f;
        G.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    }
    Transition(cube.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);SubmitWait();
    return cube;
}
static bool Inverse(const float *in,float *out) {
    double a[4][8]{};
    for(int y=0;y<4;y++)for(int x=0;x<4;x++){a[y][x]=in[y*4+x];a[y][x+4]=x==y;}
    for(int j=0;j<4;j++) {
        int p=j;for(int i=j+1;i<4;i++)if(std::abs(a[i][j])>std::abs(a[p][j]))p=i;
        if(std::abs(a[p][j])<1e-10)return false;
        for(int x=0;x<8;x++)std::swap(a[j][x],a[p][x]);
        double v=a[j][j];for(int x=0;x<8;x++)a[j][x]/=v;
        for(int i=0;i<4;i++)if(i!=j){v=a[i][j];for(int x=0;x<8;x++)a[i][x]-=a[j][x]*v;}
    }
    for(int y=0;y<4;y++)for(int x=0;x<4;x++)out[y*4+x]=(float)a[y][x+4];
    return true;
}
#include "lighting_probe.inl"
#include "fiber_probe.inl"
#include "material_probe.inl"
#include "contract_probe.inl"
#include "split_probe.inl"
#include "motion_size_probe.inl"
#include "omm_probe.inl"
#ifndef RTC_PROBE_MAIN
#define RTC_PROBE_MAIN main
#endif
int RTC_PROBE_MAIN(int argc,char **argv) {
    if(argc!=2)return 2;
    std::string out=argv[1];CreateDirectoryA(out.c_str(),nullptr);
    ComPtr<ID3D12Debug> debug;
    if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    if(FAILED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_12_1,IID_PPV_ARGS(&G.device))))return 3;
    G.device.As(&G.info);
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    G.device->CreateCommandQueue(&q,IID_PPV_ARGS(&G.queue));
    G.device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&G.alloc));
    G.device->CreateCommandList(0,q.Type,G.alloc.Get(),nullptr,IID_PPV_ARGS(&G.list));G.list->Close();
    G.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&G.fence));
    G.event=CreateEventA(nullptr,FALSE,FALSE,nullptr);G.queue->GetTimestampFrequency(&G.frequency);
    char error[256]{};if(!rtc::Init(G.device.Get(),error,sizeof error)){puts(error);return 4;}
    float positions[]={-1,-1,-1,1,-1,-1,1,-1,1,-1,-1,1, -1,1,-1,1,1,-1,1,1,1,-1,1,1};
    uint32_t indices[]={0,1,2,0,2,3, 3,2,6,3,6,7, 0,3,7,0,7,4, 1,5,6,1,6,2, 4,7,6,4,6,5};
    auto vb=Data(positions,sizeof positions),ib=Data(indices,sizeof indices);
    rtc::Submesh subs[5];rtc::Geometry geometry{};rtc::PathMaterial boxMaterials[5];
    geometry.id=1;geometry.flags=rtc::kRigid;geometry.vertexCount=8;
    geometry.position={vb.Get(),0,12,rtc::VertexFormat::Float3};geometry.indexBuffer=ib.Get();geometry.index32=1;
    geometry.submeshes=subs;geometry.submeshCount=5;
    for(unsigned i=0;i<5;i++) {
        rtc::MaterialDesc old;
        rtc::PathMaterial mat;
        mat.baseColor[0]=i==2?.65f:i==3?.08f:.7f;
        mat.baseColor[1]=i==3?.6f:i==2?.08f:.7f;
        mat.baseColor[2]=i==2||i==3?.08f:.7f;
        if(i==1){mat.metallic=1;mat.roughness=.08f;mat.baseColor[0]=mat.baseColor[1]=mat.baseColor[2]=.9f;}
        memcpy(old.averageColor,mat.baseColor,12);
        subs[i].start=i*6;subs[i].count=6;subs[i].material=rtc::CreateMaterial(old);
        if(!rtc::SetPathMaterial(subs[i].material,mat))return 5;
        boxMaterials[i]=mat;
    }
    rtc::Light light;light.id=1;light.type=rtc::LightType::Point;
    light.position[0]=0;light.position[1]=.65f;light.position[2]=-.2f;light.range=4;light.radius=.15f;light.angularRadius=0;
    light.color[0]=light.color[1]=light.color[2]=2;
    rtc::Frame frame;frame.frame=1;frame.geometries=&geometry;frame.geometryCount=1;frame.lights=&light;frame.lightCount=1;
    Begin();bool recorded=rtc::Record(G.list.Get(),frame);SubmitWait();Drain("scene");if(!recorded)return 6;
    constexpr unsigned W=128,H=128;
    ComPtr<ID3D12Resource> targets[6];
    for(int i=0;i<6;i++)targets[i]=Tex(W,H,i<4?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R16_FLOAT);
    rtc::PathView view;view.width=W;view.height=H;view.samples=8;view.camera[2]=-3;
    view.color=targets[0].Get();view.diffuse=targets[1].Get();view.normal=targets[2].Get();view.specular=targets[3].Get();
    view.depth=targets[4].Get();view.hitDistance=targets[5].Get();
    const float a=100.01f/99.99f,b=-2.f/99.99f;
    float projection[]={1,0,0,0,0,1,0,0,0,0,a,3*a+b,0,0,1,3};
    memcpy(view.worldToClip,projection,sizeof projection);if(!Inverse(projection,view.clipToWorld))return 7;
    double means[2]{};unsigned nonfinite=0;std::vector<float> final;
    for(int mode=0;mode<2;mode++) {
        view.maxBounces=mode?4:1;
        std::vector<float> sum(W*H*4,0);
        for(unsigned k=0;k<8;k++) {
            view.seed=k;
            Begin();if(!rtc::RecordPathTrace(G.list.Get(),view))return 8;SubmitWait();Drain("path trace");
            auto pixels=ReadTexture(targets[0].Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
            for(size_t i=0;i<pixels.size();i++){float f=Half(pixels[i]);nonfinite+=!std::isfinite(f);sum[i]+=f/8;}
        }
        for(size_t i=0;i<sum.size();i+=4)means[mode]+=(sum[i]+sum[i+1]+sum[i+2])/(W*H*3);
        FILE *f=fopen((out+(mode?"/four-bounce.f32":"/direct.f32")).c_str(),"wb");fwrite(sum.data(),4,sum.size(),f);fclose(f);
        if(mode)final=std::move(sum);
    }
    auto distance=ReadTexture(targets[5].Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,1);
    unsigned reflected=0;for(auto h:distance)reflected+=Half(h)>0;
    auto cube=Cube();rtc::PathEnvironment env;env.lighting=env.background=cube.Get();
    env.rgbm=0;env.lightingScale=env.diffuseScale=.1f;env.backgroundScale=.25f;
    if(!cube || !rtc::SetPathEnvironment(env))return 10;
    Begin();if(!rtc::RecordPathTrace(G.list.Get(),view))return 11;SubmitWait();Drain("environment");
    auto sky=ReadTexture(targets[0].Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    // This ray misses the Cornell box and sees +Z away from cube filter seams.
    // Rotate 180 degrees
    // around Y: the same ray must read -Z, without changing the cube allocation.
    constexpr unsigned skyAt=((H/2)*W+110)*4;
    bool skyPass=std::abs(Half(sky[skyAt])-1.25f)<.001f && std::abs(Half(sky[skyAt+1])-.125f)<.001f;
    // API 17: the visible sky has its own transform (VaM rotates the sky mesh,
    // not the lighting sky): rotating the lighting sky alone leaves it in place.
    env.worldToSky[0]=env.worldToSky[10]=-1;rtc::SetPathEnvironment(env);
    Begin();rtc::RecordPathTrace(G.list.Get(),view);SubmitWait();Drain("rotated lighting sky");
    auto lightingRotated=ReadTexture(targets[0].Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    skyPass &= std::abs(Half(lightingRotated[skyAt])-1.25f)<.001f;
    env.backgroundToSky[0]=env.backgroundToSky[10]=-1;rtc::SetPathEnvironment(env);
    Begin();rtc::RecordPathTrace(G.list.Get(),view);SubmitWait();Drain("rotated environment");
    auto rotated=ReadTexture(targets[0].Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    skyPass &= std::abs(Half(rotated[skyAt])-1.5f)<.001f;
    // API 22: diffuse bounce limit, in the box with matte walls, lit by the point light and the sky through
    // its open side. With none left (1) the walls keep the light samples' whole diffuse (MIS weight 1, only
    // specular lobes continue) and match the direct-only render (0); the mirror still shows the lit walls;
    // the full limit (2) adds the walls' interreflection. The mirror is glossy (.3) here: the box's .08 one
    // shows the sky in the direct-only render through the environment sample alone, a heavy tail that failed
    // the mirror check with other seeds (pt-eye-audit-20261103).
    // 11-09 (3, 4): kPathAuthorBgBounce at the full limit. Scene walls and their mirror: as with no bounce left (1).
    // The walls a person's (mode 256): the full limit's (2).
    // (5) as (3) with the walls nearer than backgroundNear: they keep the setting.
    bool bouncePass;double bounceMeans[6][2]{};
    {
        for(unsigned i=0;i<5;i++)if(i!=1){rtc::PathMaterial matte=boxMaterials[i];matte.materialControls[1]=0;rtc::SetPathMaterial(subs[i].material,matte);}
        {rtc::PathMaterial glossy=boxMaterials[1];glossy.roughness=.3f;rtc::SetPathMaterial(subs[1].material,glossy);}
        rtc::PathEnvironment lit=env;lit.lightingScale=lit.diffuseScale=2;
        const auto seed=view.seed;const auto bounces=view.maxBounces;
        for(int mode=0;mode<6;mode++) {
            lit.backgroundNear=lit.backgroundFar=mode==5?1000.f:0.f;
            lit.diffuseBounces=mode==1?0:8;lit.authorRules=mode>=3?rtc::kPathAuthorBgBounce:0;rtc::SetPathEnvironment(lit);view.maxBounces=mode?4:1;
            for(unsigned i=0;i<5;i++)if(i!=1){rtc::PathMaterial matte=boxMaterials[i];matte.materialControls[1]=0;matte.mode=mode==4?256:0;rtc::SetPathMaterial(subs[i].material,matte);}
            std::vector<double> sum(W*H,0);
            for(unsigned k=0;k<32;k++) {
                view.seed=200+k;
                Begin();rtc::RecordPathTrace(G.list.Get(),view);SubmitWait();Drain("diffuse bounces");
                auto pixels=ReadTexture(targets[0].Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
                for(unsigned i=0;i<W*H;i++)for(int c=0;c<3;c++){float v=Half(pixels[i*4+c]);nonfinite+=!std::isfinite(v);sum[i]+=v/96;}
            }
            unsigned count[2]{};
            for(unsigned y=0;y<H;y++)for(unsigned x=0;x<W;x++) {
                float r=std::max(std::abs((x+.5f)*2/W-1),std::abs((y+.5f)*2/H-1));
                int region=r<.22f?1:r>.27f&&r<.47f?0:-1;if(region<0)continue;
                bounceMeans[mode][region]+=sum[y*W+x];count[region]++;
            }
            for(int k=0;k<2;k++)bounceMeans[mode][k]/=std::max(count[k],1u);
        }
        view.seed=seed;view.maxBounces=bounces;rtc::SetPathEnvironment(env);
        for(unsigned i=0;i<5;i++)rtc::SetPathMaterial(subs[i].material,boxMaterials[i]);
        double walls=bounceMeans[1][0]/bounceMeans[0][0];
        bouncePass=walls>.98&&walls<1.02&&bounceMeans[1][1]>bounceMeans[0][1]*1.05&&bounceMeans[2][0]>bounceMeans[1][0]*1.02;
        FILE* f=fopen((out+"/diffuse-bounces.json").c_str(),"wb");
        fprintf(f,"{\"pass\":%s,\"walls\":[%.6f,%.6f,%.6f],\"mirror\":[%.6f,%.6f,%.6f]}\n",bouncePass?"true":"false",
            bounceMeans[0][0],bounceMeans[1][0],bounceMeans[2][0],bounceMeans[0][1],bounceMeans[1][1],bounceMeans[2][1]);fclose(f);
        double scene=bounceMeans[3][0]/bounceMeans[1][0],sceneMirror=bounceMeans[3][1]/bounceMeans[1][1],person=bounceMeans[4][0]/bounceMeans[2][0],kept=bounceMeans[5][0]/bounceMeans[2][0];
        f=fopen((out+"/background-bounces.json").c_str(),"wb");
        fprintf(f,"{\"pass\":%s,\"scene_over_none\":%.6f,\"mirror_over_none\":%.6f,\"person_over_full\":%.6f,\"near_over_full\":%.6f}\n",
            scene>.98&&scene<1.02&&sceneMirror>.95&&sceneMirror<1.05&&person>.98&&person<1.02&&kept>.98&&kept<1.02?"true":"false",scene,sceneMirror,person,kept);fclose(f);
    }
    rtc::PathMaterial glass;glass.transmission=1;glass.roughness=.12f;glass.ior=1.5f;
    rtc::SetPathMaterial(subs[1].material,glass);
    Begin();rtc::RecordPathTrace(G.list.Get(),view);SubmitWait();Drain("rough dielectric");
    auto transmitted=ReadTexture(targets[0].Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    float glassCenter=Half(transmitted[((H/2)*W+W/2)*4]);
    for(auto h:transmitted)nonfinite+=!std::isfinite(Half(h));
    uint32_t hairData[32*9]{};
    for(unsigned i=0;i<4;i++) {
        float pos[]={0,-.6f+i*.4f,-1.5f};memcpy(hairData+i*3,pos,12);
        hairData[64*3+i*3+1]=0x34003a00;hairData[64*3+i*3+2]=0x3000;
    }
    auto hairBuffer=Data(hairData,sizeof hairData);PathHairItem hair;hair.strands=1;hair.segments=3;hair.radius=.08f;
    Begin();if(!rtc::RecordPathHair(G.list.Get(),hairBuffer.Get(),&hair,1,1))return 12;
    if(!rtc::RecordPathTrace(G.list.Get(),view))return 13;SubmitWait();Drain("LSS build and trace");
    auto hairGuide=ReadTexture(targets[1].Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    unsigned hairHits=0;for(size_t i=3;i<hairGuide.size();i+=4)hairHits+=Half(hairGuide[i])<0;
    for(unsigned i=0;i<4;i++){float x=.1f;memcpy(hairData+i*3,&x,4);}
    auto movedHair=Data(hairData,sizeof hairData);
    frame.frame=2;frame.completedFrame=1;
    Begin();if(!rtc::Record(G.list.Get(),frame))return 14;SubmitWait();Drain("next scene frame");
    auto motion=Tex(W,H,DXGI_FORMAT_R32G32_FLOAT);view.motion=motion.Get();memcpy(view.previousWorldToClip,projection,sizeof projection);
    Begin();if(!rtc::RecordPathHair(G.list.Get(),movedHair.Get(),&hair,1,2))return 14;
    if(!rtc::RecordPathTrace(G.list.Get(),view))return 15;SubmitWait();Drain("LSS refit");
    auto movedGuide=ReadTexture(targets[1].Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    auto motionData=ReadTexture(motion.Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    double motionX=0,motionY=0;unsigned motionHits=0;
    for(unsigned i=0;i<W*H;i++)if(Half(movedGuide[i*4+3])<0) {
        float xy[2];memcpy(xy,motionData.data()+i*4,8);
        nonfinite+=!std::isfinite(xy[0])||!std::isfinite(xy[1]);motionX+=xy[0];motionY+=std::abs(xy[1]);motionHits++;
    }
    if(motionHits){motionX/=motionHits;motionY/=motionHits;}
    bool motionPass=motionHits>100 && motionX< -4.1 && motionX> -4.7 && motionY<.001;
    uint32_t hairSegments;uint64_t hairBytes;rtc::GetPathHairStatus(hairSegments,hairBytes);
    Begin();rtc::RecordPathHair(G.list.Get(),nullptr,nullptr,0,3);SubmitWait();Drain("LSS release");
    uint32_t releasedSegments;uint64_t releasedBytes;rtc::GetPathHairStatus(releasedSegments,releasedBytes);
    bool hairPass=hairHits>100 && hairSegments==3 && hairBytes>0 && releasedSegments==0 && releasedBytes==0;
    // Uniform white illumination on a large planar skin surface. Moving the
    // diffuse event inside the tissue must preserve the reflected energy;
    // this catches an incorrect area PDF, projection weights or metre scale.
    float skinPositions[]={-10,-10,1,10,-10,1,10,10,1,-10,10,1};
    uint32_t skinIndices[]={0,2,1,0,3,2};
    auto skinVb=Data(skinPositions,sizeof skinPositions),skinIb=Data(skinIndices,sizeof skinIndices);
    rtc::MaterialDesc skinDesc;rtc::Submesh skinSub;skinSub.count=6;skinSub.material=rtc::CreateMaterial(skinDesc);
    rtc::Geometry skinGeometry;skinGeometry.id=2;skinGeometry.flags=rtc::kRigid;skinGeometry.vertexCount=4;
    skinGeometry.position={skinVb.Get(),0,12,rtc::VertexFormat::Float3};skinGeometry.indexBuffer=skinIb.Get();skinGeometry.index32=1;
    skinGeometry.submeshes=&skinSub;skinGeometry.submeshCount=1;
    rtc::PathMaterial skin;skin.flags=8;skin.baseColor[0]=skin.baseColor[1]=skin.baseColor[2]=.6f;
    skin.skin[0]=.028f;skin.skin[1]=skin.skin[2]=skin.skin[3]=skin.skin[4]=skin.skin[5]=1;skin.skin[20]=.001f;
    frame.frame=3;frame.completedFrame=2;frame.geometries=&skinGeometry;frame.geometryCount=1;frame.lightCount=0;
    frame.ambientSh[3]=frame.ambientSh[7]=frame.ambientSh[11]=1;
    rtc::SetPathEnvironment(rtc::PathEnvironment{});
    Begin();if(!rtc::Record(G.list.Get(),frame))return 16;SubmitWait();Drain("skin furnace scene");
    double skinMeans[2]{};view.motion=nullptr;view.maxBounces=2;
    for(int mode=0;mode<2;mode++) {
        skin.skin[21]=(float)mode;if(!rtc::SetPathMaterial(skinSub.material,skin))return 17;
        for(unsigned k=0;k<4;k++) {
            view.seed=100+k;Begin();if(!rtc::RecordPathTrace(G.list.Get(),view))return 18;SubmitWait();Drain("skin furnace");
            auto pixels=ReadTexture(targets[0].Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
            for(unsigned i=0;i<W*H;i++)for(int c=0;c<3;c++) {
                float value=Half(pixels[i*4+c]);nonfinite+=!std::isfinite(value);skinMeans[mode]+=value/(W*H*3.0*4);
            }
        }
    }
    bool skinPass=skinMeans[0]>.5 && skinMeans[1]>.5 && std::abs(skinMeans[1]/skinMeans[0]-1)<.04;
    // 11-10: a live skin material with a scattering strength selects the PT's scattering program (PT_SSS); the checks below
    // trace without it, as before.
    skin.skin[21]=0;if(!rtc::SetPathMaterial(skinSub.material,skin))return 17;
    struct Influence {float weight[4]={1,0,0,0};uint32_t bone[4]={};} influences[4];
    auto weights=Data(influences,sizeof influences);
    float normalData[]={.70710678f,0,-.70710678f,.70710678f,0,-.70710678f,.70710678f,0,-.70710678f,.70710678f,0,-.70710678f};
    auto skinNormals=Data(normalData,sizeof normalData);
    float boneRows[]={2,0,0,0,0,1,0,0,0,0,1,0};
    skinGeometry.normal={skinNormals.Get(),0,12,rtc::VertexFormat::Float3};
    skinGeometry.boneCount=1;skinGeometry.skinMatrices=boneRows;skinGeometry.skinWeights=weights.Get();
    view.motion=motion.Get();view.maxBounces=1;
    for(int k=0;k<2;k++) {
        frame.frame=4+k;frame.completedFrame=3+k;boneRows[3]=.1f*k;
        Begin();if(!rtc::Record(G.list.Get(),frame))return 19;
        if(!rtc::RecordPathTrace(G.list.Get(),view))return 20;SubmitWait();Drain("CUA skin and motion");
    }
    auto skinnedMotion=ReadTexture(motion.Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    auto skinnedNormal=ReadTexture(targets[2].Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    float skinnedXY[2];memcpy(skinnedXY,skinnedMotion.data()+((H/2)*W+W/2)*4,8);
    size_t center=((H/2)*W+W/2)*4;
    bool skinnedPass=std::abs(skinnedXY[0]+1.6f)<.002f && std::abs(skinnedXY[1])<.002f &&
        std::abs(Half(skinnedNormal[center])-.4472136f)<.002f && std::abs(Half(skinnedNormal[center+2])+.8944272f)<.002f;
    printf("CUA: motion=(%.6f,%.6f), normal=(%.6f,%.6f), pass=%s\n",skinnedXY[0],skinnedXY[1],Half(skinnedNormal[center]),Half(skinnedNormal[center+2]),skinnedPass?"true":"false");
    uint32_t sparse[4*10+4*12]={};memcpy(sparse,influences,sizeof influences);
    for(unsigned i=0;i<4;i++) {
        sparse[32+i*2]=(40+i*12)*4;sparse[33+i*2]=1;
        const float delta=.1f;memcpy(sparse+40+i*12+1,&delta,4);
    }
    auto blendData=Data(sparse,sizeof sparse);float blendRows[13]={};memcpy(blendRows,boneRows,sizeof boneRows);
    skinGeometry.skinWeights=blendData.Get();skinGeometry.skinMatrices=blendRows;skinGeometry.blendFrameCount=1;
    for(int k=0;k<2;k++) {
        frame.frame=6+k;frame.completedFrame=5+k;blendRows[12]=k*.5f;
        Begin();if(!rtc::Record(G.list.Get(),frame))return 23;
        if(!rtc::RecordPathTrace(G.list.Get(),view))return 24;SubmitWait();Drain("CUA morph target");
    }
    auto blendMotion=ReadTexture(motion.Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    float blendXY[2];memcpy(blendXY,blendMotion.data()+center,8);
    bool blendPass=std::abs(blendXY[0]+1.6f)<.002f && std::abs(blendXY[1])<.002f;
    printf("CUA morph: motion=(%.6f,%.6f), pass=%s\n",blendXY[0],blendXY[1],blendPass?"true":"false");
    auto reference=Tex(W,H,DXGI_FORMAT_R16G16B16A16_FLOAT);view.detailReference=reference.Get();
    Begin();if(!rtc::RecordPathTrace(G.list.Get(),view))return 21;SubmitWait();Drain("PT detail reference");
    auto referenceOn=ReadTexture(reference.Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    bool detailPass=std::abs(Half(referenceOn[center])-.6f)<.002f && Half(referenceOn[center+3])>.8f && Half(referenceOn[center+3])<=1;
    for(auto h:referenceOn)nonfinite+=!std::isfinite(Half(h));
    view.detailReference=nullptr;view.seed++;
    Begin();if(!rtc::RecordPathTrace(G.list.Get(),view))return 22;SubmitWait();Drain("PT detail zero");
    auto referenceOff=ReadTexture(reference.Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    detailPass&=referenceOn==referenceOff; // null reference must not touch its former output
    printf("PT detail: enabled=%g disabled_unchanged=%s\n",Half(referenceOn[center]),detailPass?"true":"false");
    // A newly visible mesh has no object history but DOES have camera motion.
    // A clear wet film must guide RR with the opaque surface behind it.
    rtc::Geometry eyeGeo[2]={skinGeometry,skinGeometry};
    for(unsigned i=0;i<2;i++) {
        eyeGeo[i].id=800+i;eyeGeo[i].flags=rtc::kRigid;
        eyeGeo[i].skinWeights=nullptr;eyeGeo[i].skinMatrices=nullptr;
        eyeGeo[i].boneCount=eyeGeo[i].blendFrameCount=0;eyeGeo[i].normal={};
        eyeGeo[i].position={skinVb.Get(),0,12,rtc::VertexFormat::Float3};
    }
    eyeGeo[1].matrix[11]=-1; // clear film at z=0, opaque surface at z=1
    rtc::MaterialDesc filmDesc;filmDesc.kind=rtc::MaterialKind::Translucent;filmDesc.opacity=1;
    rtc::Submesh filmSub=skinSub;filmSub.material=rtc::CreateMaterial(filmDesc);
    eyeGeo[1].submeshes=&filmSub;
    rtc::PathMaterial film;film.flags=2048|262144;film.transmission=1;film.ior=1.336f;film.roughness=.05f;
    rtc::SetPathMaterial(filmSub.material,film);
    skin.skin[21]=0;rtc::SetPathMaterial(skinSub.material,skin);
    frame.frame=8;frame.completedFrame=7;frame.geometries=eyeGeo;frame.geometryCount=2;
    view.maxBounces=4;view.motion=motion.Get();
    memcpy(view.previousWorldToClip,projection,sizeof projection);view.previousWorldToClip[3]+=.2f;
    Begin();if(!rtc::Record(G.list.Get(),frame))return 25;
    if(!rtc::RecordPathTrace(G.list.Get(),view))return 26;SubmitWait();Drain("clear film camera motion");
    auto eyeAlbedo=ReadTexture(targets[1].Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    auto eyeMotion=ReadTexture(motion.Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    float eyeXY[2];memcpy(eyeXY,eyeMotion.data()+center,8);
    bool eyePass=Half(eyeAlbedo[center])>.5f && Half(eyeAlbedo[center+3])==1 &&
        std::abs(eyeXY[0]-3.2f)<.002f && std::abs(eyeXY[1])<.002f;
    printf("Clear film: diffuse=%g instance=%g camera_mv=(%g,%g) expected=(3.2,0) pass=%s\n",
        Half(eyeAlbedo[center]),Half(eyeAlbedo[center+3]),eyeXY[0],eyeXY[1],eyePass?"true":"false");
    {FILE* f=fopen((out+"/eye-motion.json").c_str(),"wb");fprintf(f,"{\"pass\":%s,\"albedo\":%g,\"mv_x\":%g}\n",eyePass?"true":"false",Half(eyeAlbedo[center]),eyeXY[0]);fclose(f);}
    if(!eyePass)return 27;
    bool lightingPass=LightingCheck(view,out,nonfinite);
    bool fiberPass=FiberCheck(out);
    bool materialPass=MaterialCheck(view,out);
    bool contractPass=ContractCheck(view,out);
    bool splitPass=SplitCheck(view,out);
    // A scheduled BLAS rebuild must keep the previous fiber surface for motion.
    // Move the same strand on the exact refresh boundary; zeroed history would
    // incorrectly produce zero instead of the analytic -4.2667 pixel motion.
    rtc::Frame hairFrame;hairFrame.frame=2000;hairFrame.completedFrame=1999;
    hairFrame.geometries=&geometry;hairFrame.geometryCount=1;
    view.motion=motion.Get();view.samples=1;view.maxBounces=1;
    memcpy(view.previousWorldToClip,projection,sizeof projection);
    for(unsigned step=0;step<=16;step++) {
        Begin();if(!rtc::Record(G.list.Get(),hairFrame))return 28;
        if(!rtc::RecordPathHair(G.list.Get(),step==16?movedHair.Get():hairBuffer.Get(),&hair,1,hairFrame.frame))return 28;
        if(!rtc::RecordPathTrace(G.list.Get(),view))return 28;
        SubmitWait();Drain("LSS refresh boundary");hairFrame.completedFrame=hairFrame.frame++;
    }
    auto refreshGuide=ReadTexture(targets[1].Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    auto refreshMotion=ReadTexture(motion.Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    double refreshX=0;unsigned refreshHits=0;
    for(unsigned pixel=0;pixel<W*H;pixel++)if(Half(refreshGuide[pixel*4+3])<0) {
        float xy[2];memcpy(xy,refreshMotion.data()+pixel*4,8);refreshX+=xy[0];refreshHits++;
    }
    if(refreshHits)refreshX/=refreshHits;
    bool refreshPass=refreshHits>100&&std::abs(refreshX+4.266667)<.01;
    {FILE* f=fopen((out+"/hair-refresh.json").c_str(),"wb");fprintf(f,"{\"pass\":%s,\"pixels\":%u,\"motion_x\":%.9g}\n",refreshPass?"true":"false",refreshHits,refreshX);fclose(f);}
    const bool ser=rtc::PathUsesSer();printf("PT shader execution reordering: %s\n",ser?"active":"compute fallback");
    {FILE *f=fopen((out+"/ser.json").c_str(),"wb");fprintf(f,"{\"active\":%s}\n",ser?"true":"false");fclose(f);}
    bool exactMotionPass=MotionSizeCheck(view,out);
    bool ommPass=OmmCheck(view,out);
    bool pass=bouncePass&&ommPass&&exactMotionPass&&refreshPass&&lightingPass&&fiberPass&&materialPass&&contractPass&&splitPass&&!G.errors&&!nonfinite&&means[0]>0&&means[1]>means[0]*1.01&&reflected>100&&skyPass&&glassCenter>0&&hairPass&&motionPass&&skinPass&&skinnedPass&&blendPass&&detailPass;
    char report[1400];snprintf(report,sizeof report,"{\"pass\":%s,\"debug_errors\":%d,\"nonfinite\":%u,\"direct_mean\":%.6f,\"four_bounce_mean\":%.6f,\"specular_hit_pixels\":%u,\"sky_rotation_pass\":%s,\"glass_center\":%.6f,\"lss_hits\":%u,\"lss_pass\":%s,\"motion_pass\":%s,\"motion_x\":%.6f,\"motion_y\":%.6f,\"skin_pass\":%s,\"skin_local_mean\":%.6f,\"skin_scattered_mean\":%.6f,\"width\":%u,\"height\":%u}\n",pass?"true":"false",G.errors,nonfinite,means[0],means[1],reflected,skyPass?"true":"false",glassCenter,hairHits,hairPass?"true":"false",motionPass?"true":"false",motionX,motionY,skinPass?"true":"false",skinMeans[0],skinMeans[1],W,H);
    puts(report);FILE *f=fopen((out+"/result.json").c_str(),"wb");fputs(report,f);fclose(f);
    rtc::Shutdown();CloseHandle(G.event);return pass?0:9;
}
