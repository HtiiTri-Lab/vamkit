static void ClearPathSkinning() {
    for(auto &kv:g->pathSkins){BuryRes(kv.second.vertices);FreeDescriptor(kv.second.uav);}
    g->pathSkins.clear();g->pathSkinGeometries.clear();
    Bury(g->pipes[pPathSkin]);g->pipes[pPathSkin].Reset();
}
static bool PreparePathSkinning(ID3D12GraphicsCommandList4 *l,Frame &f) {
    bool any=false;for(uint32_t i=0;i<f.geometryCount;i++)any|=f.geometries[i].boneCount!=0;
    if(!any){if(!g->pathSkins.empty())ClearPathSkinning();return true;}
    if(!g->pipes[pPathSkin]) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=g->root.Get();
        pd.CS={rtc_shaders::kAll[pPathSkin].code,rtc_shaders::kAll[pPathSkin].size};
        if(FAILED(g->device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&g->pipes[pPathSkin]))))return false;
    }
    g->pathSkinGeometries.assign(f.geometries,f.geometries+f.geometryCount);
    ID3D12DescriptorHeap *heaps[]={g->heap.Get()};l->SetDescriptorHeaps(1,heaps);l->SetComputeRootSignature(g->root.Get());
    auto table=g->heap->GetGPUDescriptorHandleForHeapStart();l->SetComputeRootDescriptorTable(3,table);l->SetComputeRootDescriptorTable(4,table);
    Barriers bar;
    for(auto &x:g->pathSkinGeometries) {
        if(!x.boneCount)continue;
        if(!x.skinMatrices||!x.skinWeights||!x.vertexCount||x.position.format!=VertexFormat::Float3 ||
            x.skinWeights->GetDesc().Width<uint64_t(x.vertexCount)*32)return false;
        auto &s=g->pathSkins[x.id];s.lastSeen=f.frame;
        if(s.vertices.bytes<uint64_t(x.vertexCount)*48) {
            BuryRes(s.vertices);FreeDescriptor(s.uav);s.uav=kNone;
            if(!Buffer(s.vertices,uint64_t(x.vertexCount)*48,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS))return false;
            s.uav=StaticDescriptor();if(s.uav==kNone)return false;RawUav(s.uav,s.vertices.Get(),s.vertices.bytes);
        }
        const uint64_t rowBytes=32+uint64_t(x.boneCount)*48+uint64_t(x.blendFrameCount)*4;
        Alloc bones;if(!Upload(rowBytes,256,bones))return false;
        uint32_t metadata[8]={HostSrv(x.tangent.buffer),(uint32_t)x.tangent.offset,x.tangent.stride,(uint32_t)x.tangent.format,
            (uint32_t)x.position.format,(uint32_t)x.normal.format,0,0};
        memcpy(bones.cpu,metadata,32);memcpy(bones.cpu+32,x.skinMatrices,(size_t)rowBytes-32);
        uint32_t boneSrv=FrameDescriptor();if(boneSrv==kNone)return false;
        RawSrv(boneSrv,g->upload.Get(),bones.offset,rowBytes);
        PassCb p;p.count=x.vertexCount;p.a=HostSrv(x.position.buffer);p.b=HostSrv(x.normal.buffer);p.c=HostSrv(x.skinWeights);
        p.d=boneSrv;p.e=s.uav;p.f=x.position.stride;p.g=x.normal.stride;p.h=x.boneCount;
        p.receiver=(uint32_t)x.position.offset;p.light=(uint32_t)x.normal.offset;
        p.seed=x.blendFrameCount;
        bar.To(s.vertices,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);bar.Flush(l);Dispatch(l,pPathSkin,p,x.vertexCount);
        bar.To(s.vertices,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);bar.Flush(l);
        x.position={s.vertices.Get(),0,48,VertexFormat::Float3};
        if(x.normal.buffer)x.normal={s.vertices.Get(),16,48,VertexFormat::Float3};
        if(x.tangent.buffer)x.tangent={s.vertices.Get(),32,48,VertexFormat::Float4};
        const float identity[12]={1,0,0,0,0,1,0,0,0,0,1,0};memcpy(x.matrix,identity,48);
        x.flags=(x.flags&~kRigid)|kSkinned;
    }
    for(auto it=g->pathSkins.begin();it!=g->pathSkins.end();) {
        if(it->second.lastSeen!=f.frame){BuryRes(it->second.vertices);FreeDescriptor(it->second.uav);it=g->pathSkins.erase(it);}else ++it;
    }
    f.geometries=g->pathSkinGeometries.data();return true;
}
