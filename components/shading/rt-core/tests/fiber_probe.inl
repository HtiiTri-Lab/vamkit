#include "fiber_check.h"  // build.cmd: generated into <repo>/out/rt-core-tests
static bool FiberCheck(const std::string &out) {
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipeline;
    if(FAILED(G.device->CreateRootSignature(0,kFiberCheck,sizeof kFiberCheck,IID_PPV_ARGS(&root))))return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={kFiberCheck,sizeof kFiberCheck};
    if(FAILED(G.device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline))))return false;
    auto target=Tex(256,512,DXGI_FORMAT_R32G32B32A32_FLOAT);
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=1;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if(FAILED(G.device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))))return false;
    D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    G.device->CreateUnorderedAccessView(target.Get(),nullptr,&u,heap->GetCPUDescriptorHandleForHeapStart());
    Begin();G.list->SetDescriptorHeaps(1,heap.GetAddressOf());G.list->SetComputeRootSignature(root.Get());
    uint32_t constants[12]={};G.list->SetComputeRoot32BitConstants(0,12,constants,0);
    G.list->SetComputeRootDescriptorTable(4,heap->GetGPUDescriptorHandleForHeapStart());G.list->SetPipelineState(pipeline.Get());G.list->Dispatch(32,64,1);
    SubmitWait();Drain("fiber PDF and furnace");
    auto pixels=ReadTexture(target.Get(),0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,8);
    double sums[2][4]={};bool pass=true;
    for(unsigned which=0;which<2;which++)for(unsigned i=0;i<65536;i++) {
        float value[4];memcpy(value,pixels.data()+(which*65536+i)*8,16);
        for(int c=0;c<4;c++){pass&=std::isfinite(value[c]);sums[which][c]+=value[c]/65536.0;}
    }
    for(auto &m:sums)pass&=std::abs(m[0]-m[1])<.025 && std::abs(m[2]-1)<.025;
    // Far-field RTXCR retains R/TT/TRT (the guide), not the lossless residual
    // orders of near-field Chiang. White must be conservative, rather than exactly 1.
    pass&=sums[0][3]>.8 && sums[0][3]<=1.002 && sums[1][3]>0 && sums[1][3]<1;
    // 0.22.63: on top, the fibres' multiple scattering returns PT_HAIR_MULTIPLE
    // (path_hair_bsdf.hlsli) x the fibre colour; its cylinder lobe integrates to 1.
    const double multiple=1,colour[2]={1,.35};
    for(int w=0;w<2;w++)pass&=std::abs(sums[w][0]-(sums[w][3]+multiple*colour[w]))<.025;
    char report[1024];snprintf(report,sizeof report,
        "{\"pass\":%s,\"white_importance\":%.8f,\"white_uniform\":%.8f,\"white_pdf_integral\":%.8f,\"white_guide\":%.8f,\"brown_importance\":%.8f,\"brown_uniform\":%.8f,\"brown_pdf_integral\":%.8f,\"brown_guide\":%.8f}\n",
        pass?"true":"false",sums[0][0],sums[0][1],sums[0][2],sums[0][3],sums[1][0],sums[1][1],sums[1][2],sums[1][3]);
    puts(report);FILE *f=fopen((out+"/fiber.json").c_str(),"wb");fputs(report,f);fclose(f);return pass;
}
