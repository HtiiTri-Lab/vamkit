// All registered GPUTools styles use their live tessellated guide particles.
// Reuse the verified DS point builder and the device's isoline domain. No
// camera visibility, old lighting, software raster, or CPU vertex readback.
struct PathHairRegistry {
    ID3D11Buffer *points=nullptr,*indices=nullptr,*items=nullptr;
    ID3D11ShaderResourceView *pointsSrv=nullptr,*indicesSrv=nullptr,*itemsSrv=nullptr;
    ID3D11UnorderedAccessView *pointsUav=nullptr;
    ID3D11SamplerState *sampler=nullptr;
    ID3D11Resource *sources[64][4]={};
    ID3D11ShaderResourceView *views[64][4]={};
    UINT pointBytes=0,indexBytes=0;
    uint64_t serial=UINT64_MAX;
    uint32_t count=0;
    PathHairItem output[64];
} gPathHairRegistry;

static void ReleasePathHairRegistry() {
    auto &r=gPathHairRegistry;
    REL(r.pointsSrv);REL(r.indicesSrv);REL(r.itemsSrv);REL(r.pointsUav);
    REL(r.points);REL(r.indices);REL(r.items);REL(r.sampler);
    for(int i=0;i<64;i++)for(int k=0;k<4;k++){REL(r.views[i][k]);REL(r.sources[i][k]);}
    r.pointBytes=r.indexBytes=r.count=0;r.serial=UINT64_MAX;
}
static bool PathHairView(unsigned i,unsigned k,uint64_t pointer) {
    auto &r=gPathHairRegistry;auto *source=(ID3D11Resource*)pointer;
    if(r.sources[i][k]==source && r.views[i][k])return true;
    REL(r.views[i][k]);REL(r.sources[i][k]);if(!source)return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC v={};
    if(k>=2) {
        D3D11_BUFFER_DESC d;ID3D11Buffer_GetDesc((ID3D11Buffer*)source,&d);
        if(!d.StructureByteStride || !(d.MiscFlags&D3D11_RESOURCE_MISC_BUFFER_STRUCTURED))return false;
        v.Format=DXGI_FORMAT_UNKNOWN;v.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;v.Buffer.NumElements=d.ByteWidth/d.StructureByteStride;
    } else {
        D3D11_TEXTURE2D_DESC d;ID3D11Texture2D_GetDesc((ID3D11Texture2D*)source,&d);
        v.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;v.Texture2D.MipLevels=d.MipLevels;v.Format=d.Format;
        switch(v.Format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:v.Format=DXGI_FORMAT_R8G8B8A8_UNORM;break;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:v.Format=DXGI_FORMAT_B8G8R8A8_UNORM;break;
        case DXGI_FORMAT_BC1_TYPELESS:v.Format=DXGI_FORMAT_BC1_UNORM;break;
        case DXGI_FORMAT_BC2_TYPELESS:v.Format=DXGI_FORMAT_BC2_UNORM;break;
        case DXGI_FORMAT_BC3_TYPELESS:v.Format=DXGI_FORMAT_BC3_UNORM;break;
        case DXGI_FORMAT_BC7_TYPELESS:v.Format=DXGI_FORMAT_BC7_UNORM;break;
        default:break;
        }
    }
    if(FAILED(ID3D11Device_CreateShaderResourceView(gDevice,source,&v,&r.views[i][k])))return false;
    r.sources[i][k]=source;ID3D11Resource_AddRef(source);return true;
}
EXPORT void HOP_PathModeV1(int enabled) {
    if(gPathLighting==(enabled!=0))return;
    gPathLighting=enabled!=0;
    if(!gPathLighting)ReleasePathHairRegistry();
    else {
        // Keep only the shared domain/build shader. Retire unused raster work
        // through D3D11's normal resource lifetime rules.
        HsClearCs();gFbOpen=gFbDone=gFaOpen=gFbPending=false;gHsCount=0;
        REL(gHs.pointsSrv);REL(gHs.pointsUav);REL(gHs.points);
        REL(gHs.liveSrv);REL(gHs.liveUav);REL(gHs.live);
        REL(gHs.indicesSrv);REL(gHs.indices);
        REL(gHs.clippedSrv);REL(gHs.clippedUav);REL(gHs.clipped);
        REL(gHs.visDepthSrv);REL(gHs.visDepthUav);REL(gHs.visDepth);
        REL(gHs.visIdSrv);REL(gHs.visIdUav);REL(gHs.visId);
        gHs.pointsBytes=gHs.liveBytes=gHs.indicesBytes=gHs.clippedBytes=gHs.visW=gHs.visH=0;
    }
}
EXPORT int HOP_BuildPathHairV2(const PathHairBuildItem *input,uint32_t count,uint64_t serial,PathHairSource *out) {
    if(!out || !out->items || count>64 || count>out->capacity)return -1;
    auto &r=gPathHairRegistry;out->points=nullptr;out->count=0;out->serial=serial;
    if(!count){ReleasePathHairRegistry();return 1;}
    if(!input || !gStrandsReady || !gHs.domainSrv || !gHs.build)return -1;
    if(r.serial==serial && r.count==count) {
        memcpy(out->items,r.output,count*sizeof(PathHairItem));out->points=(ID3D11Resource*)r.points;out->count=count;
        out->allocationBytes=r.pointBytes+r.indexBytes+64*sizeof(HsItem);return 1;
    }
    HsItem items[64]={};uint64_t points=0,indexBytes=0;
    for(uint32_t i=0;i<count;i++) {
        const auto &in=input[i];auto &h=items[i];
        if(!in.indices || in.baseVertex || !in.indexCount || in.indexCount%3)return -1;
        memcpy(h.tessFactor.v,in.tess,16);memcpy(h.length.v,in.length,16);
        memcpy(h.mat0.v,in.mat0,16);memcpy(h.mat1.v,in.mat1,16);memcpy(h.mat2.v,in.mat2,16);
        uint32_t L=TessCount(in.tess[0]),S=TessCount(in.tess[1]);if(!L||!S)return -1;
        h.counts[0]=in.indexCount/3;h.counts[1]=L;h.counts[2]=S;h.counts[3]=(uint32_t)points;
        h.flags[2]=(uint32_t)(indexBytes/(in.index32?4:2));h.flags[3]=in.index32?0:1;
        auto &p=r.output[i];p=PathHairItem{};p.firstPoint=(uint32_t)points;p.strands=h.counts[0]*L;p.segments=S;
        p.pad=(uint32_t)in.id;p.radius=std::max(in.mat0[2],0.f);p.shift=in.mat1[2];p.diffuse=in.mat0[3];
        // KK cone sigma is 1/sqrt(n) in half-angle, hence 2/sqrt(n)
        // in theta_i+theta_o. RTXCR beta_R at the lobe centre is sqrt(2)*r.
        p.roughness=std::min(1.f,std::max(.05f,sqrtf(2.f/std::max(in.mat1[3],1.f))));
        // The source controls the PEAK of an unnormalized cone. Calibrate
        // that authoring control to the normalized R lobe at normal incidence;
        // retain TT/TRT and pigmentation instead of darkening the whole fiber.
        const float beta=1.41421356237f*p.roughness;
        const float peak=.04652057f*.3133285343f/std::max(beta,.01f);
        for(int k=0;k<3;k++)p.reflection[k]=std::min(1.f,std::max(0.f,in.specularColor[k]/peak));
        points+=uint64_t(p.strands)*(S+1);indexBytes+=(uint64_t(in.indexCount)*(in.index32?4:2)+3)&~3ull;
        if(HsPointBufferBytes(points)>UINT32_MAX-16 || indexBytes>UINT32_MAX-16)return -1;
        if(!PathHairView(i,0,in.noise)||!PathHairView(i,1,in.color)||!PathHairView(i,2,in.particles)||!PathHairView(i,3,in.barycentrics))return -1;
    }
    for(uint32_t i=count;i<64;i++)for(int k=0;k<4;k++){REL(r.views[i][k]);REL(r.sources[i][k]);}
    if(!HsEnsureRaw(&r.points,&r.pointsSrv,&r.pointsUav,&r.pointBytes,(UINT)HsPointBufferBytes(points)) ||
       !HsEnsureRaw(&r.indices,&r.indicesSrv,nullptr,&r.indexBytes,(UINT)indexBytes))return -1;
    if(!r.items && !HsStructured(64,sizeof(HsItem),&r.items,&r.itemsSrv))return -1;
    if(!r.sampler) {
        D3D11_SAMPLER_DESC d={};d.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;d.AddressU=d.AddressV=d.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;
        d.MaxLOD=D3D11_FLOAT32_MAX;d.MaxAnisotropy=1;
        if(FAILED(ID3D11Device_CreateSamplerState(gDevice,&d,&r.sampler)))return -1;
    }
    if(!HsUpload(r.items,items,count*sizeof(HsItem)))return -1;
    SavedState saved;SaveState(saved);
    for(uint32_t i=0;i<count;i++) {
        const auto &in=input[i];UINT isz=in.index32?4:2;
        D3D11_BOX box={in.indexStart*isz,0,0,(in.indexStart+in.indexCount)*isz,1,1};
        ID3D11DeviceContext_CopySubresourceRegion(gCtx,(ID3D11Resource*)r.indices,0,items[i].flags[2]*isz,0,0,(ID3D11Resource*)in.indices,0,&box);
    }
    HsClearCs();ID3D11DeviceContext_CSSetConstantBuffers(gCtx,0,1,&gHs.dispatchCb);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(gCtx,0,1,&r.pointsUav,nullptr);
    ID3D11SamplerState *samplers[]={r.sampler,r.sampler};ID3D11DeviceContext_CSSetSamplers(gCtx,0,2,samplers);
    for(uint32_t i=0;i<count;i++) {
        ID3D11ShaderResourceView *views[]={r.views[i][0],r.views[i][1],r.views[i][2],r.views[i][3],r.indicesSrv,gHs.domainSrv,r.itemsSrv};
        ID3D11DeviceContext_CSSetShaderResources(gCtx,0,7,views);
        HsDispatchChunks(gHs.build,i,items[i].counts[0]*items[i].counts[1]*(items[i].counts[2]+1));
    }
    HsClearCs();RestoreState(saved,false);
    r.serial=serial;r.count=count;
    memcpy(out->items,r.output,count*sizeof(PathHairItem));out->points=(ID3D11Resource*)r.points;out->count=count;
    out->allocationBytes=r.pointBytes+r.indexBytes+64*sizeof(HsItem);return 1;
}
