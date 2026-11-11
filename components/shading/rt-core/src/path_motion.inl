static void DropPathMotion(Core::PathMotionState &s) {
    for(int k=0;k<2;k++) {
        BuryRes(s.positions[k]);FreeDescriptor(s.srv[k]);s.srv[k]=kNone;
        BuryRes(s.normals[k]);FreeDescriptor(s.normalSrv[k]);s.normalSrv[k]=kNone;
    }
}
static void ClearPathMotion() {
    for(auto &kv:g->pathMotion)DropPathMotion(kv.second);
    g->pathMotion.clear();g->pathMotionRecords.clear();
}
static bool PreparePathMotion(ID3D12GraphicsCommandList4 *l,const Frame &f) {
    g->pathMotionRecords.assign(f.geometryCount,Core::PathMotionGpu{});
    for(uint32_t i=0;i<f.geometryCount;i++) {
        const auto &x=f.geometries[i];if(!x.position.buffer||!x.vertexCount)continue;
        auto &s=g->pathMotion[x.id];auto &r=g->pathMotionRecords[i];
        uint64_t signature=Mix((uint64_t)(uintptr_t)x.position.buffer,(uint64_t)(uintptr_t)x.indexBuffer);
        signature=Mix(Mix(Mix(signature,x.vertexCount),x.position.stride),x.indexOffset);
        signature=Mix(signature,x.position.offset);signature=Mix(signature,x.submeshCount);
        signature=Mix(signature,(uint32_t)x.position.format);signature=Mix(signature,(uint32_t)x.baseVertex);signature=Mix(signature,x.index32);
        bool tess=false;
        for(uint32_t k=0;k<x.submeshCount;k++) {
            const auto &sub=x.submeshes[k];tess|=sub.tess==Tess::Phong && sub.tessLevel>1;
            signature=Mix(Mix(signature,sub.start),sub.count);
            signature=Mix(signature,sub.tessLevel);signature=Mix(signature,&sub.tessAlpha,1);
        }
        if(tess)signature=Mix(signature,(uint64_t)(uintptr_t)x.normal.buffer);
        const bool valid=s.lastSeen+1==f.frame && s.signature==signature;
        r.valid=valid?1:0;r.offset=(uint32_t)x.position.offset;r.stride=x.position.stride;r.format=(uint32_t)x.position.format;
        memcpy(r.matrix,valid?s.matrix:x.matrix,48);
        uint32_t before=s.current;
        if(x.flags&kSkinned) {
            uint64_t bytes=x.position.buffer->GetDesc().Width;
            // Buffer() rounds allocation capacity to 256 bytes; the host's
            // structured source keeps its exact element byte count. Equality
            // would reallocate and invalidate non-aligned history every frame.
            if(!s.positions[0].r || !s.positions[1].r || s.positions[0].bytes<bytes || s.positions[1].bytes<bytes) {
                DropPathMotion(s);r.valid=0;
                for(int k=0;k<2;k++) {
                    if(!Buffer(s.positions[k],bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST))return false;
                    s.srv[k]=StaticDescriptor();if(s.srv[k]==kNone)return false;RawSrv(s.srv[k],s.positions[k].Get(),0,bytes);
                }
            }
            s.current^=1;auto &current=s.positions[s.current];
            Barriers bar;bar.To(current,D3D12_RESOURCE_STATE_COPY_DEST);bar.Flush(l);
            D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=x.position.buffer;
            b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
            l->ResourceBarrier(1,&b);l->CopyBufferRegion(current.Get(),0,x.position.buffer,0,bytes);
            std::swap(b.Transition.StateBefore,b.Transition.StateAfter);l->ResourceBarrier(1,&b);
            bar.To(current,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if(r.valid)bar.To(s.positions[before],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            bar.Flush(l);r.position=s.srv[r.valid?before:s.current];
        } else r.position=HostSrv(x.position.buffer);
        if(tess && x.normal.buffer) {
            r.normalOffset=(uint32_t)x.normal.offset;r.normalStride=x.normal.stride;r.normalFormat=(uint32_t)x.normal.format;
            if(x.normal.buffer==x.position.buffer)r.normal=r.position;
            else if(!(x.flags&kSkinned))r.normal=HostSrv(x.normal.buffer);
            else {
                const uint64_t bytes=x.normal.buffer->GetDesc().Width;
                if(!s.normals[0].r || !s.normals[1].r || s.normals[0].bytes<bytes || s.normals[1].bytes<bytes) {
                    r.valid=0;
                    for(int k=0;k<2;k++) {
                        BuryRes(s.normals[k]);FreeDescriptor(s.normalSrv[k]);s.normalSrv[k]=kNone;
                        if(!Buffer(s.normals[k],bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST))return false;
                        s.normalSrv[k]=StaticDescriptor();if(s.normalSrv[k]==kNone)return false;RawSrv(s.normalSrv[k],s.normals[k].Get(),0,bytes);
                    }
                }
                auto &current=s.normals[s.current];Barriers bar;bar.To(current,D3D12_RESOURCE_STATE_COPY_DEST);bar.Flush(l);
                D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=x.normal.buffer;
                b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
                l->ResourceBarrier(1,&b);l->CopyBufferRegion(current.Get(),0,x.normal.buffer,0,bytes);
                std::swap(b.Transition.StateBefore,b.Transition.StateAfter);l->ResourceBarrier(1,&b);
                bar.To(current,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if(r.valid)bar.To(s.normals[before],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                bar.Flush(l);r.normal=s.normalSrv[r.valid?before:s.current];
            }
        } else for(int k=0;k<2;k++) {
            BuryRes(s.normals[k]);FreeDescriptor(s.normalSrv[k]);s.normalSrv[k]=kNone;
        }
        memcpy(s.matrix,x.matrix,48);s.lastSeen=f.frame;s.signature=signature;
    }
    for(auto it=g->pathMotion.begin();it!=g->pathMotion.end();) {
        if(it->second.lastSeen!=f.frame){DropPathMotion(it->second);it=g->pathMotion.erase(it);}else ++it;
    }
    return true;
}
