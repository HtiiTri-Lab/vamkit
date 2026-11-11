#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define CINTERFACE
#define COBJMACROS
#define D3D11_NO_HELPERS
#include "mouth_gpu.h"
#include "mouth_shaders.h"

MouthGpu::~MouthGpu(){
    for(auto p:input)if(p)ID3D11ShaderResourceView_Release(p);
    if(srv)ID3D11ShaderResourceView_Release(srv);
    if(triangleSrv)ID3D11ShaderResourceView_Release(triangleSrv);
    if(boundsSrv)ID3D11ShaderResourceView_Release(boundsSrv);
    if(prepare)ID3D11ComputeShader_Release(prepare);
    for(auto p:uav)if(p)ID3D11UnorderedAccessView_Release(p);
    if(refit)ID3D11ComputeShader_Release(refit);if(trace)ID3D11ComputeShader_Release(trace);if(cb)ID3D11Buffer_Release(cb);
}
static bool Buffer(ID3D11Device*d,UINT stride,UINT count,const void*data,DXGI_FORMAT fmt,ID3D11ShaderResourceView**srv,ID3D11UnorderedAccessView**uav){
    D3D11_BUFFER_DESC bd={};bd.ByteWidth=stride*count;bd.Usage=data?D3D11_USAGE_IMMUTABLE:D3D11_USAGE_DEFAULT;
    bd.BindFlags=D3D11_BIND_SHADER_RESOURCE|(uav?D3D11_BIND_UNORDERED_ACCESS:0);
    if(fmt==DXGI_FORMAT_UNKNOWN)bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,bd.StructureByteStride=stride;
    D3D11_SUBRESOURCE_DATA init={data,0,0};ID3D11Buffer*b=nullptr;
    if(FAILED(ID3D11Device_CreateBuffer(d,&bd,data?&init:nullptr,&b)))return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd={};sd.Format=fmt;sd.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;sd.Buffer.NumElements=count;
    HRESULT hr=S_OK;if(srv)hr=ID3D11Device_CreateShaderResourceView(d,(ID3D11Resource*)b,&sd,srv);
    if(SUCCEEDED(hr)&&uav){D3D11_UNORDERED_ACCESS_VIEW_DESC ud={};ud.Format=fmt;ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=count;hr=ID3D11Device_CreateUnorderedAccessView(d,(ID3D11Resource*)b,&ud,uav);}
    ID3D11Buffer_Release(b);return SUCCEEDED(hr);
}
std::shared_ptr<MouthGpu> CreateMouthGpu(ID3D11Device*d,const MouthData&m){
    if(m.nodes.empty()||m.receivers.empty())return {};
    std::shared_ptr<MouthGpu> r(new MouthGpu);r->nodeCount=(uint32_t)m.nodes.size();r->receiverCount=(uint32_t)m.receivers.size();r->vertexCount=m.vertexCount;r->depth=m.depth;
    r->span=m.span;r->radius=m.radius;r->spanVertex[0]=m.spanVertex[0];r->spanVertex[1]=m.spanVertex[1];
    r->triangleCount=(UINT)m.triangles.size();
    if(!Buffer(d,16,r->nodeCount,m.nodes.data(),DXGI_FORMAT_UNKNOWN,&r->input[0],nullptr)||
       !Buffer(d,16,(UINT)m.triangles.size(),m.triangles.data(),DXGI_FORMAT_UNKNOWN,&r->input[1],nullptr)||
       !Buffer(d,16,r->receiverCount,m.receivers.data(),DXGI_FORMAT_UNKNOWN,&r->input[2],nullptr)||
       !Buffer(d,4,(UINT)m.adjacency.size(),m.adjacency.data(),DXGI_FORMAT_R32_UINT,&r->input[3],nullptr)||
       !Buffer(d,16,r->nodeCount*2,nullptr,DXGI_FORMAT_UNKNOWN,&r->boundsSrv,&r->uav[0])||
       !Buffer(d,4,r->receiverCount,nullptr,DXGI_FORMAT_R32_FLOAT,&r->srv,&r->uav[1])||
       !Buffer(d,48,r->triangleCount,nullptr,DXGI_FORMAT_UNKNOWN,&r->triangleSrv,&r->uav[2])||
       !Buffer(d,4,r->nodeCount,nullptr,DXGI_FORMAT_R32_UINT,nullptr,&r->uav[3]))return {};
    D3D11_BUFFER_DESC bd={};bd.ByteWidth=32;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if(FAILED(ID3D11Device_CreateBuffer(d,&bd,nullptr,&r->cb))||FAILED(ID3D11Device_CreateComputeShader(d,kMouthPrepare,sizeof kMouthPrepare,nullptr,&r->prepare))||FAILED(ID3D11Device_CreateComputeShader(d,kMouthRefit,sizeof kMouthRefit,nullptr,&r->refit))||FAILED(ID3D11Device_CreateComputeShader(d,kMouthTrace,sizeof kMouthTrace,nullptr,&r->trace)))return {};
    return r;
}
bool MouthGpu::Gather(ID3D11DeviceContext*c,int currentFrame,int base){
    if(frame==currentFrame)return ok;frame=currentFrame;ok=false;
    ID3D11ShaderResourceView*verts=nullptr;ID3D11DeviceContext_VSGetShaderResources(c,0,1,&verts);if(!verts)return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;ID3D11ShaderResourceView_GetDesc(verts,&vd);
    bool valid=vd.ViewDimension==D3D11_SRV_DIMENSION_BUFFER&&vd.Format==DXGI_FORMAT_UNKNOWN&&vd.Buffer.FirstElement==0&&base>=0&&vd.Buffer.NumElements>=vertexCount+(UINT)base;
    ID3D11Resource*res=nullptr;ID3D11ShaderResourceView_GetResource(verts,&res);
    D3D11_RESOURCE_DIMENSION dim;ID3D11Resource_GetType(res,&dim);
    if(dim==D3D11_RESOURCE_DIMENSION_BUFFER){D3D11_BUFFER_DESC bd;ID3D11Buffer_GetDesc((ID3D11Buffer*)res,&bd);valid&=bd.StructureByteStride==12;}else valid=false;
    ID3D11Resource_Release(res);if(!valid){ID3D11ShaderResourceView_Release(verts);return false;}
    ID3D11ComputeShader*oldCS=nullptr;ID3D11Buffer*oldCb=nullptr;ID3D11ShaderResourceView*oldSrv[7]={};ID3D11UnorderedAccessView*oldUav[4]={};
    ID3D11DeviceContext_CSGetShader(c,&oldCS,nullptr,nullptr);ID3D11DeviceContext_CSGetConstantBuffers(c,0,1,&oldCb);
    ID3D11DeviceContext_CSGetShaderResources(c,0,7,oldSrv);ID3D11DeviceContext_CSGetUnorderedAccessViews(c,0,4,oldUav);
    ID3D11ShaderResourceView*resources[7]={verts,input[0],input[1],input[2],input[3],nullptr,nullptr};UINT keep[4]={~0u,~0u,~0u,~0u};
    ID3D11DeviceContext_CSSetShaderResources(c,0,7,resources);ID3D11DeviceContext_CSSetUnorderedAccessViews(c,0,4,uav,keep);ID3D11DeviceContext_CSSetConstantBuffers(c,0,1,&cb);
    struct Control {uint32_t nodes,receivers,level,base;float radius;uint32_t a,b;float span;}v={nodeCount,receiverCount,0,(uint32_t)base,radius,spanVertex[0],spanVertex[1],span};
    ID3D11DeviceContext_UpdateSubresource(c,(ID3D11Resource*)cb,0,nullptr,&v,0,0);
    ID3D11DeviceContext_CSSetShader(c,prepare,nullptr,0);ID3D11DeviceContext_Dispatch(c,(triangleCount+63)/64,1,1);
    ID3D11UnorderedAccessView*none=nullptr;ID3D11DeviceContext_CSSetUnorderedAccessViews(c,2,1,&none,keep);
    ID3D11DeviceContext_CSSetShaderResources(c,5,1,&triangleSrv);
    const UINT zero[4]={};ID3D11DeviceContext_ClearUnorderedAccessViewUint(c,uav[3],zero);
    ID3D11DeviceContext_CSSetShader(c,refit,nullptr,0);
    ID3D11DeviceContext_Dispatch(c,(nodeCount+63)/64,1,1);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c,0,1,&none,keep);
    ID3D11DeviceContext_CSSetShaderResources(c,6,1,&boundsSrv);
    ID3D11DeviceContext_CSSetShader(c,trace,nullptr,0);ID3D11DeviceContext_Dispatch(c,receiverCount,1,1);
    ID3D11UnorderedAccessView*nullUav[4]={};ID3D11ShaderResourceView*nullSrv[7]={};
    ID3D11DeviceContext_CSSetUnorderedAccessViews(c,0,4,nullUav,keep);ID3D11DeviceContext_CSSetShaderResources(c,0,7,nullSrv);
    ID3D11DeviceContext_CSSetShader(c,oldCS,nullptr,0);ID3D11DeviceContext_CSSetConstantBuffers(c,0,1,&oldCb);ID3D11DeviceContext_CSSetShaderResources(c,0,7,oldSrv);ID3D11DeviceContext_CSSetUnorderedAccessViews(c,0,4,oldUav,keep);
    if(oldCS)ID3D11ComputeShader_Release(oldCS);if(oldCb)ID3D11Buffer_Release(oldCb);for(auto p:oldSrv)if(p)ID3D11ShaderResourceView_Release(p);for(auto p:oldUav)if(p)ID3D11UnorderedAccessView_Release(p);
    ID3D11ShaderResourceView_Release(verts);ok=true;return true;
}
