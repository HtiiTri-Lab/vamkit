// Included inside rtc, after shared allocation/dispatch helpers.
static void ClearPathHair() {
    auto &h=g->pathHair;
    BuryRes(h.vertices);BuryRes(h.indices);BuryRes(h.tlas);BuryRes(h.scratch);
    for(auto &b:h.blas)BuryRes(b);
    FreeDescriptor(h.vertexSrv);FreeDescriptor(h.vertexUav);FreeDescriptor(h.indexUav);
    h=Core::PathHairState{};
    Bury(g->pipes[pPathHairPrepare]);g->pipes[pPathHairPrepare].Reset();
}
static bool PathLssCaps() {
    if(!g->pathLssChecked) {
        g->pathLssChecked=true;
        NVAPI_D3D12_RAYTRACING_LINEAR_SWEPT_SPHERES_CAPS caps=NVAPI_D3D12_RAYTRACING_LINEAR_SWEPT_SPHERES_CAP_NONE;
        g->pathLss=NvAPI_D3D12_GetRaytracingCaps(g->device.Get(),NVAPI_D3D12_RAYTRACING_CAPS_TYPE_LINEAR_SWEPT_SPHERES,&caps,sizeof caps)==NVAPI_OK &&
            (caps&NVAPI_D3D12_RAYTRACING_LINEAR_SWEPT_SPHERES_CAP_STANDARD)!=0;
    }
    return g->pathLss;
}
bool RecordPathHair(ID3D12GraphicsCommandList4 *l,ID3D12Resource *source,const PathHairItem *items,uint32_t count,uint64_t serial) {
    if(!g || !g->ok || !l)return false;
    auto &h=g->pathHair;
    if(!count || !source) { if(h.active)ClearPathHair();return true; }
    if(!items || count>64 || !PathLssCaps())return false;
    if(h.active && h.serial==serial && h.frame==g->frame)return true; // both eyes share the build
    if(!g->pipes[pPathHairPrepare]) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=g->root.Get();
        pd.CS={rtc_shaders::kAll[pPathHairPrepare].code,rtc_shaders::kAll[pPathHairPrepare].size};
        if(FAILED(g->device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&g->pipes[pPathHairPrepare]))))return false;
    }
    uint64_t points=0,segments=0,signature=1469598103934665603ull;
    // Runs of one light-link class and camera visibility (the host keeps each
    // contiguous) become one BLAS each: an instance mask can then leave a class
    // out of a light's shadow, or a camera-hidden hair out of the camera rays.
    uint32_t groups=0,groupFirst[Core::PathHairState::kGroups],groupLink[Core::PathHairState::kGroups];
    for(uint32_t i=0;i<count;i++) {
        const auto &it=items[i];
        if(!it.strands || !it.segments || it.segments>64 || !std::isfinite(it.radius) || it.radius<0)return false;
        points=std::max(points,uint64_t(it.firstPoint)+uint64_t(it.strands)*(it.segments+1));
        segments+=uint64_t(it.strands)*it.segments;
        for(auto v:{it.firstPoint,it.strands,it.segments,it.pad})signature=(signature^v)*1099511628211ull;
        const uint32_t link=it.pad&(0x1fu|kPathHairCameraHidden);
        if(!groups || groupLink[groups-1]!=link) {
            if(groups==Core::PathHairState::kGroups)return false;
            groupFirst[groups]=i;groupLink[groups++]=link;
        }
    }
    // D3D12 raw views and dispatch indexing are 32-bit; reject invalid input
    // rather than overflow or silently omit any part of the hairstyle.
    if(points>UINT32_MAX/48 || segments>UINT32_MAX/4 || source->GetDesc().Width<((points+31)&~31ull)*36)return false;
    h.previousValid=h.active && h.signature==signature && h.frame+1==g->frame;
    // Refit indefinitely preserves a poor BVH after large animated shape
    // changes. Rebuild the same LSS geometry at most every 16 rendered frames;
    // preserve previous vertices and RR history (this is not a geometry change).
    bool rebuild=!h.built || signature!=h.signature || h.groups!=groups || g->frame-h.lastBuild>=16;
    auto ensure=[&](Res &r,uint64_t bytes,D3D12_RESOURCE_STATES state) {
        if(r.r && r.bytes>=bytes)return true;
        rebuild=true;BuryRes(r);
        return Buffer(r,bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,state);
    };
    if(h.vertices.bytes<points*48) {
        h.previousValid=false;
        FreeDescriptor(h.vertexSrv);FreeDescriptor(h.vertexUav);h.vertexSrv=h.vertexUav=kNone;
    }
    if(h.indices.bytes<segments*4){FreeDescriptor(h.indexUav);h.indexUav=kNone;}
    if(!ensure(h.vertices,points*48,D3D12_RESOURCE_STATE_UNORDERED_ACCESS) || !ensure(h.indices,segments*4,D3D12_RESOURCE_STATE_UNORDERED_ACCESS))return false;
    if(h.vertexSrv==kNone)h.vertexSrv=StaticDescriptor();
    if(h.vertexUav==kNone)h.vertexUav=StaticDescriptor();
    if(h.indexUav==kNone)h.indexUav=StaticDescriptor();
    uint32_t input=FrameDescriptor();h.recordSrv=FrameDescriptor();
    if(h.vertexSrv==kNone || h.vertexUav==kNone || h.indexUav==kNone || input==kNone || h.recordSrv==kNone)return false;
    RawSrv(h.vertexSrv,h.vertices.Get(),0,h.vertices.bytes);RawUav(h.vertexUav,h.vertices.Get(),h.vertices.bytes);RawUav(h.indexUav,h.indices.Get(),h.indices.bytes);
    RawSrv(input,source,0,source->GetDesc().Width);
    Alloc records;if(!Upload(uint64_t(count)*sizeof(PathHairItem),256,records))return false;
    memcpy(records.cpu,items,count*sizeof(PathHairItem));RawSrv(h.recordSrv,g->upload.Get(),records.offset,uint64_t(count)*sizeof(PathHairItem));
    ID3D12DescriptorHeap *heaps[]={g->heap.Get()};l->SetDescriptorHeaps(1,heaps);l->SetComputeRootSignature(g->root.Get());
    auto table=g->heap->GetGPUDescriptorHandleForHeapStart();l->SetComputeRootDescriptorTable(3,table);l->SetComputeRootDescriptorTable(4,table);
    Barriers bar;bar.To(h.vertices,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);bar.To(h.indices,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);bar.Flush(l);
    auto scratch=[&](uint64_t bytes,D3D12_GPU_VIRTUAL_ADDRESS &address) {
        if(h.scratch.bytes<bytes) {
            BuryRes(h.scratch);
            if(!Buffer(h.scratch,bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS))return false;
        }
        bar.To(h.scratch,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);bar.Flush(l);address=h.scratch.Va();return true;
    };
    uint32_t firstSegment=0;
    std::vector<NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX> geometries(count);
    for(uint32_t i=0;i<count;i++) {
        const auto &it=items[i];uint32_t n=it.strands*(it.segments+1),ns=it.strands*it.segments;
        PassCb p{};p.receiver=it.firstPoint;p.seed=it.segments;p.count=n;memcpy(&p.light,&it.radius,4);
        p.a=input;p.b=h.vertexUav;p.c=h.indexUav;p.d=firstSegment;p.e=h.previousValid?1u:0u;
        constexpr uint32_t chunk=65535*256;
        for(uint32_t at=0;at<n;at+=chunk){p.f=at;Dispatch(l,pPathHairPrepare,p,std::min(n-at,chunk),256);}
        auto &e=geometries[i];e.type=NVAPI_D3D12_RAYTRACING_GEOMETRY_TYPE_LSS_EX;e.flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        auto &ls=e.lss;ls.vertexCount=n;ls.indexCount=ns;ls.primitiveCount=ns;
        ls.vertexPositionBuffer={h.vertices.Va()+uint64_t(it.firstPoint)*48,48};ls.vertexPositionFormat=DXGI_FORMAT_R32G32B32_FLOAT;
        ls.vertexRadiusBuffer={h.vertices.Va()+uint64_t(it.firstPoint)*48+12,48};ls.vertexRadiusFormat=DXGI_FORMAT_R32_FLOAT;
        ls.indexBuffer={h.indices.Va()+uint64_t(firstSegment)*4,4};ls.indexFormat=DXGI_FORMAT_R32_UINT;
        ls.endcapMode=NVAPI_D3D12_RAYTRACING_LSS_ENDCAP_MODE_CHAINED;ls.primitiveFormat=NVAPI_D3D12_RAYTRACING_LSS_PRIMITIVE_FORMAT_SUCCESSIVE_IMPLICIT;
        firstSegment+=ns;
    }
    bar.To(h.vertices,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);bar.To(h.indices,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);bar.Flush(l);
    for(uint32_t k=0;k<groups;k++) {
        const uint32_t first=groupFirst[k],n=(k+1<groups?groupFirst[k+1]:count)-first;
        NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS_EX in{};
        in.type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.flags=(NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS_EX)(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE|D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE);
        in.numDescs=n;in.descsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;in.geometryDescStrideInBytes=sizeof(geometries[0]);in.pGeometryDescs=geometries.data()+first;
        if(rebuild) {
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pi{};
            NVAPI_GET_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_EX_PARAMS p={NVAPI_GET_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_EX_PARAMS_VER,&in,&pi};
            if(NvAPI_D3D12_GetRaytracingAccelerationStructurePrebuildInfoEx(g->device.Get(),&p)!=NVAPI_OK || !pi.ResultDataMaxSizeInBytes)return false;
            if(!ensure(h.blas[k],pi.ResultDataMaxSizeInBytes,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE))return false;
            h.scratchBytes[k]=pi.ScratchDataSizeInBytes;h.updateBytes[k]=pi.UpdateScratchDataSizeInBytes;
        }
        NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC_EX build{};build.inputs=in;build.destAccelerationStructureData=h.blas[k].Va();
        if(!rebuild){build.inputs.flags=(NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS_EX)(in.flags|D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE);build.sourceAccelerationStructureData=h.blas[k].Va();}
        if(!scratch(rebuild?h.scratchBytes[k]:h.updateBytes[k],build.scratchAccelerationStructureData))return false;
        NVAPI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_EX_PARAMS bp={NVAPI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_EX_PARAMS_VER,&build,0,nullptr};
        if(NvAPI_D3D12_BuildRaytracingAccelerationStructureEx(l,&bp)!=NVAPI_OK)return false;
        bar.Uav();bar.Flush(l); // the next class reuses the scratch
    }
    if(rebuild)h.lastBuild=g->frame;
    // One traversal for the camera/continuation/visibility rays. Keep the mesh
    // TLAS at t0 for the BSSRDF's surface-only projection, and append the fiber
    // BLAS to the same mesh instances at t1. No geometry copy or second refit.
    // InstanceID: the class's first item (a fiber hit's item = it + geometry index).
    // An unlinked class blocks analytic lights with 0x04, a linked one with its own bit.
    // Camera rays see 0x02, every other ray 0x01 (rtcore.cpp); a camera-hidden group has no 0x02.
    D3D12_RAYTRACING_INSTANCE_DESC fibers[Core::PathHairState::kGroups]{};
    for(uint32_t k=0;k<groups;k++) {
        auto &instance=fibers[k];instance.Transform[0][0]=instance.Transform[1][1]=instance.Transform[2][2]=1;
        const uint32_t link=groupLink[k]&0x1fu,seen=(groupLink[k]&kPathHairCameraHidden)?0x01u:0x03u;
        instance.InstanceID=groupFirst[k];instance.InstanceMask=seen|(link?link<<3:0x04u);instance.AccelerationStructure=h.blas[k].Va();
    }
    const uint64_t meshBytes=g->instances.size()*sizeof fibers[0];
    Alloc ia;if(!Upload(meshBytes+groups*sizeof fibers[0],16,ia))return false;
    if(meshBytes)memcpy(ia.cpu,g->instances.data(),(size_t)meshBytes);
    memcpy(ia.cpu+meshBytes,fibers,groups*sizeof fibers[0]);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC t{};t.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    t.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;t.Inputs.NumDescs=(UINT)g->instances.size()+groups;t.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;t.Inputs.InstanceDescs=ia.gpu;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO ti{};g->device->GetRaytracingAccelerationStructurePrebuildInfo(&t.Inputs,&ti);
    if(!ensure(h.tlas,ti.ResultDataMaxSizeInBytes,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) || !scratch(ti.ScratchDataSizeInBytes,t.ScratchAccelerationStructureData))return false;
    t.DestAccelerationStructureData=h.tlas.Va();l->BuildRaytracingAccelerationStructure(&t,0,nullptr);bar.Uav();bar.Flush(l);
    h.active=h.built=true;h.count=count;h.groups=groups;h.segments=(uint32_t)segments;h.frame=g->frame;h.serial=serial;h.signature=signature;
    return true;
}
void GetPathHairStatus(uint32_t &segments,uint64_t &bytes) {
    segments=0;bytes=0;if(!g)return;
    const auto &h=g->pathHair;
    segments=h.active?h.segments:0;
    bytes=h.vertices.bytes+h.indices.bytes+h.tlas.bytes+h.scratch.bytes;
    for(const auto &b:h.blas)bytes+=b.bytes;
}
