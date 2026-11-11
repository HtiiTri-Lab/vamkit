// Host D3D11 structured buffers retain their exact element byte count. The
// history allocator rounds capacity to 256 bytes; the fixture must not do so.
static ComPtr<ID3D12Resource> ExactData(const void *data,size_t bytes) {
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;
    d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    if(FAILED(G.device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r))))return nullptr;
    auto upload=MakeBuffer(bytes,D3D12_HEAP_TYPE_UPLOAD);void *p=nullptr;
    upload->Map(0,nullptr,&p);memcpy(p,data,bytes);upload->Unmap(0,nullptr);
    Begin();G.list->CopyBufferRegion(r.Get(),0,upload.Get(),0,bytes);
    Transition(r.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);SubmitWait();
    return r;
}
static bool MotionSizeCheck(rtc::PathView view,const std::string &out) {
    float source[]={-10,-10,1,10,-10,1,10,10,1,-10,10,1};
    const float normals[]={0,0,-1,0,0,-1,0,0,-1,0,0,-1};
    const uint32_t indices[]={0,2,1,0,3,2};auto ib=Data(indices,sizeof indices);
    rtc::MaterialDesc md;rtc::PathMaterial pm;rtc::Submesh sub;sub.count=6;sub.material=rtc::CreateMaterial(md);
    if(!rtc::SetPathMaterial(sub.material,pm))return false;
    view.samples=1;view.maxBounces=1;
    double measured[2]{};bool pass=true;
    for(unsigned test=0;test<2;test++) {
        // First isolate positions; then isolate separate tessellation normals.
        auto vb=test?Data(source,sizeof source):ExactData(source,sizeof source);
        auto nb=test?ExactData(normals,sizeof normals):Data(normals,sizeof normals);
        if(!vb||!nb)return false;
        sub.tess=test?rtc::Tess::Phong:rtc::Tess::None;sub.tessLevel=test?2:1;sub.tessAlpha=test?.8f:0;
        rtc::Geometry geometry;geometry.id=9100+test;geometry.flags=rtc::kSkinned;geometry.vertexCount=4;
        geometry.position={vb.Get(),0,12,rtc::VertexFormat::Float3};geometry.normal={nb.Get(),0,12,rtc::VertexFormat::Float3};
        geometry.indexBuffer=ib.Get();geometry.index32=1;geometry.submeshes=&sub;geometry.submeshCount=1;
        rtc::Frame frame;frame.geometries=&geometry;frame.geometryCount=1;
        for(unsigned step=0;step<2;step++) {
            float moved[12];memcpy(moved,source,sizeof moved);for(unsigned i=0;i<4;i++)moved[i*3]+=.1f*step;
            auto upload=MakeBuffer(sizeof moved,D3D12_HEAP_TYPE_UPLOAD);void *p=nullptr;
            upload->Map(0,nullptr,&p);memcpy(p,moved,sizeof moved);upload->Unmap(0,nullptr);
            Begin();Transition(vb.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
            G.list->CopyBufferRegion(vb.Get(),0,upload.Get(),0,sizeof moved);
            Transition(vb.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            frame.frame=3000+test*2+step;frame.completedFrame=frame.frame-1;
            if(!rtc::Record(G.list.Get(),frame))return false;
            if(step==0 && !rtc::RecordPathHair(G.list.Get(),nullptr,nullptr,0,frame.frame))return false;
            if(!rtc::RecordPathTrace(G.list.Get(),view))return false;
            SubmitWait();Drain("exact-size deforming mesh history");
        }
        auto motion=ReadTexture(view.motion,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,4);
        float xy[2];size_t center=((view.height/2)*view.width+view.width/2)*4;memcpy(xy,motion.data()+center,8);
        measured[test]=xy[0];pass&=std::abs(xy[0]+1.6f)<.002f&&std::abs(xy[1])<.002f;
    }
    FILE *file=std::fopen((out+"/motion-size.json").c_str(),"wb");
    if(file){std::fprintf(file,"{\"pass\":%s,\"source_bytes\":48,\"history_capacity_bytes\":256,\"expected_x\":-1.6,\"position_x\":%.9g,\"tessellation_normal_x\":%.9g}\n",pass?"true":"false",measured[0],measured[1]);std::fclose(file);}
    printf("Exact-size motion: position %.6f, tessellation normals %.6f, pass=%s\n",measured[0],measured[1],pass?"true":"false");
    return pass;
}
