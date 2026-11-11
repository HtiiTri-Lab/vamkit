// PT opacity micromaps (RtCore API 20, path_omm.inl): the same frames with them (test bit 19 set) and without must be
// byte-identical in every output, for every coverage rule and address mode, while the micromaps are really in use.
static ComPtr<ID3D12Resource> OmmAlphaTexture(unsigned w,unsigned h,const std::vector<uint8_t> &alpha) {
    auto texture=Tex(w,h,DXGI_FORMAT_R8G8B8A8_UNORM,false);auto d=texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes;
    G.device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
    auto upload=MakeBuffer(bytes,D3D12_HEAP_TYPE_UPLOAD);uint8_t *mapped;
    upload->Map(0,nullptr,(void**)&mapped);
    for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++) {
        uint8_t *p=mapped+fp.Offset+y*fp.Footprint.RowPitch+x*4;p[0]=200;p[1]=180;p[2]=160;p[3]=alpha[y*w+x];
    }
    upload->Unmap(0,nullptr);Begin();
    D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;
    dst.pResource=texture.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    G.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    Transition(texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);SubmitWait();
    return texture;
}
static bool OmmCheck(rtc::PathView view,const std::string &out) {
    // alpha: quadrants of all 0, all 1, a 0/1 texel checker, a ramp; 32 x 32
    const unsigned T=32;std::vector<uint8_t> alpha(T*T);
    for(unsigned y=0;y<T;y++)for(unsigned x=0;x<T;x++) {
        const bool right=x>=T/2,bottom=y>=T/2;
        alpha[y*T+x]=!right&&!bottom?0:right&&!bottom?255:!right?(((x^y)&1)?255:0):(uint8_t)(((x-T/2)+(y-T/2))*255/(T-2));
    }
    auto map=OmmAlphaTexture(T,T,alpha);
    // nine quads in a 3 x 3 grid at z 0.2 .. 0.6, 12 x 12 cells each, uvs rotated 30 degrees over [-0.3, 2.2]
    const unsigned N=13,kQuads=9;
    std::vector<float> pos,uv;std::vector<uint32_t> idx;
    for(unsigned q=0;q<kQuads;q++) {
        const float cx=-1.6f+1.6f*(q%3),cy=-1.6f+1.6f*(q/3),z=.2f+.05f*q;
        const uint32_t first=(uint32_t)pos.size()/3;
        for(unsigned j=0;j<N;j++)for(unsigned i=0;i<N;i++) {
            const float s=i/(N-1.f),t=j/(N-1.f);
            pos.insert(pos.end(),{cx-.75f+1.5f*s,cy-.75f+1.5f*t,z+.05f*s*t});
            const float u0=s*2.5f-.3f,v0=t*2.5f-.3f;
            uv.insert(uv.end(),{.866f*u0-.5f*v0+.5f,.5f*u0+.866f*v0});
        }
        for(unsigned j=0;j+1<N;j++)for(unsigned i=0;i+1<N;i++) {
            const uint32_t a=first+j*N+i;idx.insert(idx.end(),{a,a+N+1,a+1,a,a+N,a+N+1});
        }
    }
    // a lit back wall
    const uint32_t wall=(uint32_t)pos.size()/3;
    pos.insert(pos.end(),{-4,-4,1.5f,4,-4,1.5f,4,4,1.5f,-4,4,1.5f});uv.insert(uv.end(),{0,0,1,0,1,1,0,1});
    idx.insert(idx.end(),{wall,wall+2,wall+1,wall,wall+3,wall+2});
    auto vb=Data(pos.data(),pos.size()*4),uvb=Data(uv.data(),uv.size()*4),ib=Data(idx.data(),idx.size()*4);
    const uint32_t cells=(N-1)*(N-1)*6;
    struct Case {const char *name;rtc::MaterialKind kind;float cutoff,opacity;uint32_t flags,sampling;float base,controls2,controls3,adjust,transmission;bool textured,cosmetic;};
    const Case cases[kQuads]={
        {"cutout",rtc::MaterialKind::Cutout,.5f,1,0,0,1,0,0,0,0,true,false},
        {"translucent",rtc::MaterialKind::Translucent,.5f,1,0,0,1,0,0,0,0,true,false},
        {"post_alpha",rtc::MaterialKind::Translucent,.5f,1,32768,0,.8f,-.3f,0,.1f,0,true,false},
        {"alpha_test",rtc::MaterialKind::Translucent,.5f,1,8388608,16,1,0,.4f,0,0,true,false},
        {"glass",rtc::MaterialKind::Translucent,.5f,1,0,0,1,0,0,0,1,true,false},
        {"cosmetic",rtc::MaterialKind::Translucent,.5f,1,0,0,1,0,0,0,0,true,true},
        {"clamp",rtc::MaterialKind::Cutout,.5f,1,0,1|4,1,0,0,0,0,true,false},
        {"mirror",rtc::MaterialKind::Translucent,.5f,1,0,2|12,1,0,0,0,0,true,false},
        {"uniform_clear",rtc::MaterialKind::Translucent,.5f,1,32768,0,1,-1,0,0,0,false,false},
    };
    rtc::Submesh subs[kQuads+1];rtc::PathMaterial mats[kQuads+1];
    for(unsigned q=0;q<kQuads;q++) {
        const Case &c=cases[q];rtc::MaterialDesc old;old.kind=c.kind;old.alphaCutoff=c.cutoff;old.opacity=c.opacity;
        subs[q].start=q*cells;subs[q].count=cells;subs[q].material=rtc::CreateMaterial(old);
        rtc::PathMaterial &m=mats[q];
        m.baseColor[0]=.3f+.07f*q;m.baseColor[1]=.6f;m.baseColor[2]=.9f-.08f*q;m.baseColor[3]=c.base;
        m.flags=c.flags;m.sampling[7]=c.sampling;m.materialControls[2]=c.controls2;m.materialControls[3]=c.controls3;
        m.alphaAdjust=c.adjust;m.transmission=c.transmission;m.roughness=.4f;
        if(c.textured){m.alpha=map.Get();m.alphaVersion=1;m.alphaST[0]=m.alphaST[1]=1.3f;m.alphaST[2]=.05f;m.alphaST[3]=-.1f;}
    }
    subs[kQuads].start=kQuads*cells;subs[kQuads].count=6;subs[kQuads].material=rtc::CreateMaterial(rtc::MaterialDesc{});
    mats[kQuads].baseColor[0]=mats[kQuads].baseColor[1]=mats[kQuads].baseColor[2]=.8f;
    rtc::Geometry geo[2]{};
    for(unsigned k=0;k<2;k++) {
        auto &g=geo[k];g.id=900+k;g.flags=rtc::kRigid|(k?rtc::kNoOcclude:0);g.vertexCount=(uint32_t)pos.size()/3;
        g.position={vb.Get(),0,12,rtc::VertexFormat::Float3};g.uv={uvb.Get(),0,8,rtc::VertexFormat::Float2};
        g.indexBuffer=ib.Get();g.index32=1;
    }
    // geometry 0: every quad but the cosmetic one, and the wall; geometry 1 (kNoOcclude): the cosmetic quad
    rtc::Submesh main[kQuads],cosmetic=subs[5];uint32_t mainCount=0;
    for(unsigned q=0;q<=kQuads;q++)if(q!=5)main[mainCount++]=subs[q];
    geo[0].submeshes=main;geo[0].submeshCount=mainCount;geo[1].submeshes=&cosmetic;geo[1].submeshCount=1;
    rtc::Light light;light.id=91;light.type=rtc::LightType::Point;
    light.position[0]=.4f;light.position[1]=.3f;light.position[2]=-1.2f;light.range=8;light.radius=.05f;light.angularRadius=0;
    light.color[0]=light.color[1]=light.color[2]=3;
    rtc::Frame frame;frame.frame=5000;frame.completedFrame=4999;
    frame.geometries=geo;frame.geometryCount=2;frame.lights=&light;frame.lightCount=1;
    rtc::SetPathEnvironment(rtc::PathEnvironment{});
    view.samples=2;view.maxBounces=4;view.motion=nullptr;view.detailReference=nullptr;view.jitter[0]=view.jitter[1]=0;
    view.diffuseRadiance=view.specularRadiance=nullptr;
    ID3D12Resource *outputs[]={view.color,view.diffuse,view.normal,view.specular,view.depth,view.hitDistance};
    const unsigned elementBytes[]={8,8,8,8,2,2};
    std::vector<std::vector<uint16_t>> results[2];rtc::Status status[2];
    for(int on=1;on>=0;on--) {
        rtc::SetTestLayers(on?0xffffffffu:~524288u);
        for(unsigned k=0;k<16+4;k++) {
            for(unsigned q=0;q<=kQuads;q++)if(!rtc::SetPathMaterial(subs[q].material,mats[q]))return false;
            view.seed=k<16?0:k;
            Begin();if(!rtc::Record(G.list.Get(),frame)||!rtc::RecordPathTrace(G.list.Get(),view))return false;
            SubmitWait();Drain("PT micromaps");frame.completedFrame=frame.frame++;
            if(k<16)continue;
            if(k==16)status[on]=rtc::GetStatus();
            for(unsigned o=0;o<6;o++)results[on].push_back(ReadTexture(outputs[o],0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,elementBytes[o]/2));
        }
    }
    rtc::SetTestLayers(0xffffffffu);
    size_t differing=0,compared=0;
    for(size_t i=0;i<results[0].size();i++) {
        compared+=results[0][i].size();
        for(size_t j=0;j<results[0][i].size();j++)differing+=results[0][i][j]!=results[1][i][j];
    }
    // the cases' quads in the colour of the first frame: something drawn and something seen through
    const bool used=status[1].pathOmmBaked>=7 && status[1].pathOmmUniform>=1 && status[0].pathOmmBaked==0 && status[0].pathOmmUniform==0;
    const bool pass=used && differing==0 && compared>0 && !G.errors;
    char text[512];snprintf(text,sizeof text,
        "{\"pass\":%s,\"compared_values\":%zu,\"differing_values\":%zu,\"on\":{\"uniform\":%u,\"baked\":%u,\"triangles\":%llu},"
        "\"off\":{\"uniform\":%u,\"baked\":%u,\"triangles\":%llu},\"debug_errors\":%d}\n",pass?"true":"false",compared,differing,
        status[1].pathOmmUniform,status[1].pathOmmBaked,(unsigned long long)status[1].pathOmmTriangles,
        status[0].pathOmmUniform,status[0].pathOmmBaked,(unsigned long long)status[0].pathOmmTriangles,G.errors);
    printf("PT micromaps: %s",text);
    FILE *f=fopen((out+"/omm.json").c_str(),"wb");fputs(text,f);fclose(f);
    return pass;
}
