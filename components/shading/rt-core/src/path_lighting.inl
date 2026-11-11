// GPU-built environment and emissive-triangle proposal distributions.
// CPU work is bounded by registered submeshes, never by vertices/triangles.
static void ClearPathLighting() {
    auto &s=g->pathLighting;
    for(int k=0;k<2;k++){BuryRes(s.cdf[k]);FreeDescriptor(s.srv[k]);FreeDescriptor(s.uav[k]);}
    s.ranges.clear();s.lookup.clear();s.triangles=0;s.frame=UINT64_MAX;
    for(Pipe p:{pPathEnvironmentCdf,pPathEmitterCdf,pPathEmitterRanges}){Bury(g->pipes[p]);g->pipes[p].Reset();}
}
static bool PreparePathLighting(uint32_t &rangeSrv) {
    auto &s=g->pathLighting;
    if(s.frame!=g->frame) {
        s.ranges.clear();s.lookup.assign(g->subRecords.size(),kNone);s.triangles=0;
        for(uint32_t i=0;i<g->geoRecords.size();i++) {
            if(g->frameUsable[i]!=1)continue;
            const auto &geo=g->geoRecords[i];
            if(geo.flags&(kShadowOnly|kCameraOnly))continue; // neither an invisible proxy nor a camera-only backdrop lights the scene
            for(uint32_t j=0;j<geo.subCount;j++) {
                const auto &sub=g->subRecords[geo.firstSub+j];
                const auto &m=g->pathMaterialRecords[sub.material];
                if(!(m.emission[0]>0||m.emission[1]>0||m.emission[2]>0))continue;
                uint64_t count=uint64_t(sub.triCount)*(sub.blasTess?sub.tessLevel*sub.tessLevel:1);
                if(!count)continue;
                if(uint64_t(s.triangles)+count>0x1fffffffull || s.ranges.size()>=65535) {
                    Error("path emissive distribution exceeds addressable range");return false;
                }
                s.lookup[geo.firstSub+j]=(uint32_t)s.ranges.size();
                s.ranges.push_back({i,j,s.triangles,(uint32_t)count});s.triangles+=(uint32_t)count;
            }
        }
        for(int k=0;k<2;k++) {
            uint64_t bytes=k?(uint64_t(s.triangles)+s.ranges.size())*4:2048*4;
            if(!bytes || s.cdf[k].bytes<bytes) {
                BuryRes(s.cdf[k]);FreeDescriptor(s.srv[k]);FreeDescriptor(s.uav[k]);
                if(!bytes)continue;
                if(!Buffer(s.cdf[k],bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS))return false;
                s.srv[k]=StaticDescriptor();s.uav[k]=StaticDescriptor();
                if(s.srv[k]==kNone || s.uav[k]==kNone)return false;
                RawSrv(s.srv[k],s.cdf[k].Get(),0,s.cdf[k].bytes);RawUav(s.uav[k],s.cdf[k].Get(),s.cdf[k].bytes);
            }
        }
    }
    for(Pipe p:{pPathEnvironmentCdf,pPathEmitterCdf,pPathEmitterRanges}) {
        if(g->pipes[p] || (p!=pPathEnvironmentCdf && s.ranges.empty()))continue;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=g->root.Get();
        pd.CS={rtc_shaders::kAll[p].code,rtc_shaders::kAll[p].size};
        if(FAILED(g->device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&g->pipes[p]))))return false;
    }
    if(!s.ranges.empty()) {
        uint64_t rangeBytes=s.ranges.size()*sizeof(Core::PathLightingState::Range),lookupBytes=s.lookup.size()*4;
        Alloc a;if(!Upload(rangeBytes+lookupBytes,256,a))return false;
        memcpy(a.cpu,s.ranges.data(),(size_t)rangeBytes);memcpy(a.cpu+rangeBytes,s.lookup.data(),(size_t)lookupBytes);
        rangeSrv=FrameDescriptor();if(rangeSrv==kNone)return false;
        RawSrv(rangeSrv,g->upload.Get(),a.offset,rangeBytes+lookupBytes);
    }
    return true;
}
static void RecordPathLighting(ID3D12GraphicsCommandList4 *l,PassCb pass) {
    auto &s=g->pathLighting;if(s.frame==g->frame)return;
    Barriers bar;bar.To(s.cdf[0],D3D12_RESOURCE_STATE_UNORDERED_ACCESS);bar.Flush(l);
    pass.c=s.uav[0];Dispatch(l,pPathEnvironmentCdf,pass,256,256);
    bar.To(s.cdf[0],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if(!s.ranges.empty()) {
        bar.To(s.cdf[1],D3D12_RESOURCE_STATE_UNORDERED_ACCESS);bar.Flush(l);
        pass.c=s.uav[1];Dispatch(l,pPathEmitterCdf,pass,(uint32_t)s.ranges.size()*256,256);
        bar.Uav();bar.Flush(l);Dispatch(l,pPathEmitterRanges,pass,256,256);
        bar.To(s.cdf[1],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    bar.Flush(l);s.frame=g->frame;
}
