// The PT's diffuse / specular split (PathView::diffuseRadiance / specularRadiance) on one analytic, flat field: colour
// = diffuse radiance x diffuse albedo + specular radiance x specular albedo. The same frames and seeds as the
// workspace's NRD probe ran (frames 1000-1008), without the denoiser.
static bool SplitCheck(rtc::PathView view,const std::string& out) {
    const float positions[]={-10,-10,1,10,-10,1,10,10,1,-10,10,1};
    const uint32_t indices[]={0,2,1,0,3,2};
    auto vb=Data(positions,sizeof positions),ib=Data(indices,sizeof indices);
    rtc::MaterialDesc desc;rtc::Submesh sub;sub.material=rtc::CreateMaterial(desc);sub.count=6;
    rtc::PathMaterial matte;matte.flags=8;matte.skin[0]=.0001f;
    matte.skin[1]=matte.skin[2]=matte.skin[3]=matte.skin[4]=1;matte.skin[5]=0;
    for(int c=0;c<3;c++)matte.baseColor[c]=.6f;
    if(!rtc::SetPathMaterial(sub.material,matte))return false;
    rtc::Geometry geo;geo.id=900;geo.flags=rtc::kRigid;geo.vertexCount=4;
    geo.position={vb.Get(),0,12,rtc::VertexFormat::Float3};geo.indexBuffer=ib.Get();geo.index32=1;geo.submeshes=&sub;geo.submeshCount=1;
    rtc::Frame frame;frame.frame=1000;frame.completedFrame=999;
    frame.geometries=&geo;frame.geometryCount=1;
    frame.ambientSh[3]=frame.ambientSh[7]=frame.ambientSh[11]=1;
    rtc::SetPathEnvironment(rtc::PathEnvironment{});
    auto d=Tex(128,128,DXGI_FORMAT_R16G16B16A16_FLOAT),s=Tex(128,128,DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto motion=Tex(128,128,DXGI_FORMAT_R32G32_FLOAT);
    view.diffuseRadiance=d.Get();view.specularRadiance=s.Get();view.motion=motion.Get();view.detailReference=nullptr;
    view.samples=1;view.maxBounces=2;view.jitter[0]=view.jitter[1]=0;
    memcpy(view.previousWorldToClip,view.worldToClip,64);
    double maxSplitError=0;bool finite=true;
    for(unsigned f=1;f<=9;f++) {
        view.seed=f;
        Begin();if(!rtc::Record(G.list.Get(),frame)||!rtc::RecordPathTrace(G.list.Get(),view))return false;
        SubmitWait();Drain("PT split");frame.completedFrame=frame.frame++;
        if(f==1) {
            auto col=ReadTexture(view.color,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
            auto diff=ReadTexture(d.Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4),spec=ReadTexture(s.Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
            auto da=ReadTexture(view.diffuse,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4),sa=ReadTexture(view.specular,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
            for(size_t i=0;i<col.size();i++)if(i%4!=3) {
                const float c=Half(col[i]),split=Half(diff[i])*std::max(Half(da[i]),.001f)+Half(spec[i])*std::max(Half(sa[i]),.001f);
                finite&=std::isfinite(c)&&std::isfinite(split);
                maxSplitError=std::max(maxSplitError,double(std::abs(c-split)));
            }
        }
    }
    bool pass=finite && maxSplitError<.003;
    char report[256];snprintf(report,sizeof report,"{\"pass\":%s,\"max_split_error\":%.8g,\"finite\":%s}\n",pass?"true":"false",maxSplitError,finite?"true":"false");
    puts(report);FILE* f=fopen((out+"/split.json").c_str(),"wb");fputs(report,f);fclose(f);return pass;
}
