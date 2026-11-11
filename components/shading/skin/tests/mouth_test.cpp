#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define CINTERFACE
#define COBJMACROS
#define D3D11_NO_HELPERS
#include "../native/mouth_gpu.h"
#include <cstdio>
#include <cmath>
int main(){
 ID3D11Device*d=nullptr;ID3D11DeviceContext*c=nullptr;D3D_FEATURE_LEVEL fl;
 if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,&fl,&c)))return 1;
 std::vector<float>pos,nrm,uv;std::vector<int>ix;
 for(int z=0;z<3;z++)for(int x=0;x<3;x++){pos.insert(pos.end(),{(x-1)*.01f,0,(z-1)*.01f});nrm.insert(nrm.end(),{0,1,0});uv.insert(uv.end(),{x*.5f,z*.5f});}
 for(int z=0;z<2;z++)for(int x=0;x<2;x++){int a=z*3+x;ix.insert(ix.end(),{a,a+3,a+1,a+1,a+3,a+4});}
 pos.insert(pos.end(),{-.05f,.004f,-.05f,.05f,.004f,-.05f,-.05f,.004f,.05f,.05f,.004f,.05f});
 nrm.resize(39);uv.resize(26);ix.insert(ix.end(),{9,10,11,10,12,11});
 BakeSub subs[2]={{0,24,BAKE_TONGUE|BAKE_OCCLUDER},{24,6,BAKE_LIPS|BAKE_OCCLUDER}};
 BakeInput in;in.vertexCount=13;in.pos=pos.data();in.nrm=nrm.data();in.uv=uv.data();in.indexCount=(int)ix.size();in.indices=ix.data();in.subCount=2;in.subs=subs;
 MouthData data;BuildMouth(in,data);auto rig=CreateMouthGpu(d,data);if(!rig)return 2;
 D3D11_BUFFER_DESC bd={};bd.ByteWidth=(UINT)pos.size()*4;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_SHADER_RESOURCE;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.StructureByteStride=12;
 D3D11_SUBRESOURCE_DATA init={pos.data(),0,0};ID3D11Buffer*b=nullptr;ID3D11ShaderResourceView*srv=nullptr;
 ID3D11Device_CreateBuffer(d,&bd,&init,&b);ID3D11Device_CreateShaderResourceView(d,(ID3D11Resource*)b,nullptr,&srv);ID3D11DeviceContext_VSSetShaderResources(c,0,1,&srv);
 auto read=[&](int frame){if(!rig->Gather(c,frame,0))return -1.f;ID3D11Resource*r=nullptr;ID3D11ShaderResourceView_GetResource(rig->srv,&r);D3D11_BUFFER_DESC sb={};ID3D11Buffer_GetDesc((ID3D11Buffer*)r,&sb);sb.Usage=D3D11_USAGE_STAGING;sb.BindFlags=0;sb.MiscFlags=0;sb.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ID3D11Buffer*stage=nullptr;ID3D11Device_CreateBuffer(d,&sb,nullptr,&stage);ID3D11DeviceContext_CopyResource(c,(ID3D11Resource*)stage,r);D3D11_MAPPED_SUBRESOURCE map={};ID3D11DeviceContext_Map(c,(ID3D11Resource*)stage,0,D3D11_MAP_READ,0,&map);float result=-1;for(size_t i=0;i<data.receivers.size();i++)if(data.receivers[i][0]==4)result=((float*)map.pData)[i];ID3D11DeviceContext_Unmap(c,(ID3D11Resource*)stage,0);ID3D11Buffer_Release(stage);ID3D11Resource_Release(r);return result;};
 float nearValue=read(1);for(int i=9;i<13;i++)pos[3*i+1]=.5f;ID3D11DeviceContext_UpdateSubresource(c,(ID3D11Resource*)b,0,nullptr,pos.data(),0,0);float farValue=read(2);
 printf("posed triangle AO: nearValue=%f farValue=%f nodes=%zu receivers=%zu maps=%zu\n",nearValue,farValue,data.nodes.size(),data.receivers.size(),data.maps.size());
 bool ok=nearValue>=0&&nearValue<.4f&&farValue>.999f;printf("%s: geometry moves without rebaking; farValue surface gives full visibility\n",ok?"PASS":"FAIL");return ok?0:3;
}
