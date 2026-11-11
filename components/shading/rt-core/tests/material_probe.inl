static ComPtr<ID3D12Resource> FixtureTexture(unsigned width,const float *values,unsigned levels=1) {
    auto texture=Tex(width,1,DXGI_FORMAT_R32G32B32A32_FLOAT,false,levels);auto d=texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[3]{};UINT64 bytes;
    G.device->GetCopyableFootprints(&d,0,levels,0,fp,nullptr,nullptr,&bytes);
    auto upload=MakeBuffer(bytes,D3D12_HEAP_TYPE_UPLOAD);uint8_t *mapped;
    upload->Map(0,nullptr,(void**)&mapped);memcpy(mapped+fp[0].Offset,values,width*16);
    for(unsigned level=1;level<levels;level++)for(unsigned x=0;x<fp[level].Footprint.Width;x++) {
        const float flat[]={.5f,.5f,1,1};memcpy(mapped+fp[level].Offset+x*16,flat,16);
    }
    upload->Unmap(0,nullptr);Begin();
    for(unsigned level=0;level<levels;level++) {
        D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp[level];
        dst.pResource=texture.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=level;
        G.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    }
    Transition(texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);SubmitWait();return texture;
}
static bool MaterialCheck(rtc::PathView view,const std::string &out) {
    float p[]={-10,-10,1,10,-10,1,10,10,1,-10,10,1},uv[]={0,0,1,0,1,1,0,1};uint32_t ix[]={0,2,1,0,3,2};
    auto vb=Data(p,sizeof p),ib=Data(ix,sizeof ix),uvb=Data(uv,sizeof uv);
    float alphaPixels[]={1,1,1,0,1,1,1,0,1,1,1,1,1,1,1,1},detailPixels[]={.8f,.5f,1,1};
    auto alpha=FixtureTexture(4,alphaPixels),detail=FixtureTexture(1,detailPixels);
    rtc::MaterialDesc old;old.kind=rtc::MaterialKind::Cutout;old.alphaCutoff=.5f;
    rtc::Submesh sub;sub.count=6;sub.material=rtc::CreateMaterial(old);
    rtc::Geometry geo;geo.id=200;geo.flags=rtc::kRigid;geo.vertexCount=4;
    geo.position={vb.Get(),0,12,rtc::VertexFormat::Float3};geo.uv={uvb.Get(),0,8,rtc::VertexFormat::Float2};
    geo.indexBuffer=ib.Get();geo.index32=1;geo.submeshes=&sub;geo.submeshCount=1;
    rtc::Frame frame;frame.frame=200;frame.completedFrame=199;
    frame.geometries=&geo;frame.geometryCount=1;
    rtc::SetPathEnvironment(rtc::PathEnvironment{});
    Begin();if(!rtc::Record(G.list.Get(),frame))return false;SubmitWait();Drain("full-resolution material scene");
    rtc::PathMaterial m;m.flags=1024;m.alpha=alpha.Get();for(int c=0;c<3;c++)m.emissionColor[c]=1;
    view.maxBounces=1;view.samples=1;view.motion=nullptr;view.detailReference=nullptr;
    auto sample=[&](float &left,float &right) {
        if(!rtc::SetPathMaterial(sub.material,m))return false;
        Begin();if(!rtc::RecordPathTrace(G.list.Get(),view))return false;SubmitWait();Drain("material coverage");
        auto color=ReadTexture(view.color,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
        left=Half(color[(64*128+32)*4]);right=Half(color[(64*128+96)*4]);return true;
    };
    float values[4][2];if(!sample(values[0][0],values[0][1]))return false;
    m.alphaST[2]=.5f;if(!sample(values[1][0],values[1][1]))return false;
    m.alphaST[2]=0;m.alphaAdjust=.55f;if(!sample(values[2][0],values[2][1]))return false;
    m.alphaAdjust=0;m.baseColor[3]=.2f;m.materialControls[2]=.8f;m.flags|=32768;
    if(!sample(values[3][0],values[3][1]))return false;
    bool alphaPass=values[0][0]==0 && values[0][1]==1 && values[1][0]==1 && values[1][1]==0 &&
        values[2][0]==1 && values[2][1]==1 && values[3][0]==1 && values[3][1]==1;
    m=rtc::PathMaterial{};m.flags=65536|4;m.detailNormal=detail.Get();m.bump[2]=.5f;
    m.materialControls[0]=-.25f;m.materialControls[1]=0;for(int c=0;c<3;c++)m.baseColor[c]=.8f;
    float unused;if(!sample(unused,unused))return false;
    auto normals=ReadTexture(view.normal,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    auto albedo=ReadTexture(view.diffuse,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    unsigned center=(64*128+64)*4;
    float nx=Half(normals[center]),nz=Half(normals[center+2]),diffuse=Half(albedo[center]);
    bool detailPass=std::abs(nx-.3f/std::sqrt(.3f*.3f+1.4f*1.4f))<.002f &&
        std::abs(nz+1.4f/std::sqrt(.3f*.3f+1.4f*1.4f))<.002f && std::abs(diffuse-.6f)<.002f;
    const float porePixels[]={.8f,.5f,1,1,.8f,.5f,1,1,.8f,.5f,1,1,.8f,.5f,1,1};
    auto pores=FixtureTexture(4,porePixels,3);
    m=rtc::PathMaterial{};m.flags=8|4|256;m.normal=pores.Get();
    m.skin[0]=.028f;m.skin[1]=1;m.skin[2]=.75f;m.skin[3]=1.3f;m.skin[4]=.85f;m.skin[5]=1;
    // A pore map seen from far away: its tilt leaves the specular normal and comes back as roughness. The normal read
    // here is the guide for ray reconstruction, which follows the lobe that carries the energy (PtPrimaryGuides: the
    // diffuse normal mixed towards the specular one by the specular share). Without a diffuse colour the guide is the
    // specular normal, the one this filter works on.
    for(int c=0;c<3;c++)m.baseColor[c]=0;
    if(!sample(unused,unused))return false;
    auto unfiltered=ReadTexture(view.normal,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    m.skin[24]=8;m.skin[25]=1;
    if(!sample(unused,unused))return false;
    auto filtered=ReadTexture(view.normal,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
    // With its diffuse colour back the same filtered skin gives ray reconstruction the diffuse normal, which keeps the
    // pores at this distance (given the specular normal alone it printed the pores into the noisy diffuse light).
    for(int c=0;c<3;c++)m.baseColor[c]=1;
    if(!sample(unused,unused))return false;
    const float guideLit=Half(ReadTexture(view.normal,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4)[center]);
    bool filterPass=Half(unfiltered[center])>.5f && std::abs(Half(filtered[center]))<.001f &&
        Half(filtered[center+3])>Half(unfiltered[center+3])+.1f && guideLit>.5f;
    char report[800];snprintf(report,sizeof report,"{\"pass\":%s,\"alpha_original_st_adjust\":%s,\"vam_detail_normal\":[%.6f,%.6f],\"diffuse_offset\":%.6f,\"pore_to_roughness\":%s,\"specular_normal_before_after\":[%.6f,%.6f],\"guide_with_diffuse\":%.6f,\"roughness_before_after\":[%.6f,%.6f]}\n",
        alphaPass&&detailPass&&filterPass?"true":"false",alphaPass?"true":"false",nx,nz,diffuse,filterPass?"true":"false",
        Half(unfiltered[center]),Half(filtered[center]),guideLit,Half(unfiltered[center+3]),Half(filtered[center+3]));
    puts(report);FILE *f=fopen((out+"/materials.json").c_str(),"wb");fputs(report,f);fclose(f);
    // 0.23.4: Unity's Standard (Roughness setup): with PT_ROUGHNESS_MAP the gloss slot's red channel is the perceptual
    // roughness (the RR guide's .w); without the flag the same material keeps its scalar roughness.
    const float roughPixels[]={.7f,.2f,.2f,1};
    auto rough=FixtureTexture(1,roughPixels);
    m=rtc::PathMaterial{};m.flags=4;m.gloss=rough.Get();m.roughness=.5f;for(int c=0;c<3;c++)m.baseColor[c]=.5f;
    if(!sample(unused,unused))return false;
    float roughOff=Half(ReadTexture(view.normal,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4)[center+3]);
    m.flags=4|2147483648u;
    if(!sample(unused,unused))return false;
    float roughOn=Half(ReadTexture(view.normal,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4)[center+3]);
    const bool roughPass=std::abs(roughOff-.5f)<.01f && std::abs(roughOn-.7f)<.01f;
    char roughReport[200];snprintf(roughReport,sizeof roughReport,"{\"pass\":%s,\"scalar_roughness\":%.6f,\"roughness_map\":%.6f,\"expected\":[0.5,0.7]}\n",
        roughPass?"true":"false",roughOff,roughOn);
    puts(roughReport);f=fopen((out+"/roughness-map.json").c_str(),"wb");fputs(roughReport,f);fclose(f);
    return alphaPass&&detailPass&&filterPass&&roughPass;
}
