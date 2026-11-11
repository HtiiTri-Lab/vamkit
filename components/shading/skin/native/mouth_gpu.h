#pragma once
#include <d3d11.h>
#include "mouth.h"
#include <memory>
struct MouthGpu {
    ID3D11ShaderResourceView *input[4]={}, *srv=nullptr, *triangleSrv=nullptr, *boundsSrv=nullptr;
    ID3D11UnorderedAccessView *uav[4]={};
    ID3D11ComputeShader *prepare=nullptr,*refit=nullptr,*trace=nullptr;
    ID3D11Buffer *cb=nullptr;
    uint32_t nodeCount=0,receiverCount=0,vertexCount=0,depth=0,triangleCount=0;
    uint32_t spanVertex[2]={};
    float span=0, radius=.04f;
    int frame=-1; bool ok=false;
    ~MouthGpu();
    bool Gather(ID3D11DeviceContext *c,int currentFrame,int base);
};
std::shared_ptr<MouthGpu> CreateMouthGpu(ID3D11Device *d,const MouthData &m);
