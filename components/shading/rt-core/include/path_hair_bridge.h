#pragma once
#include <stdint.h>
struct ID3D11Resource;
// Render-thread-only, borrowed until the current camera has finished. The point
// buffer is HairStrands' existing 32-point tiled data, not CPU guide particles.
struct PathHairItem {
    // pad (RtCore API 18): the hair's light-link class (one bit of 0x1f); API 21: | kPathHairCameraHidden
    uint32_t firstPoint=0,strands=0,segments=0,pad=0;
    float radius=0,roughness=.3f,shift=0,diffuse=1;
    float reflection[4]={1,1,1,0}; // artist R-lobe energy/tint; does not alter TT/TRT absorption
};
static_assert(sizeof(PathHairItem)==48,"path hair material layout");
// API 21: the host's eye camera draws this hair at strand width 0 (a first-person plugin): unseen by camera rays only
constexpr uint32_t kPathHairCameraHidden=0x20;
struct PathHairSource {
    ID3D11Resource *points=nullptr;
    uint64_t serial=0;
    uint64_t allocationBytes=0;
    uint32_t count=0,capacity=0;
    PathHairItem *items=nullptr;
};
// Scene registry input, independent of camera culling and raster shader type.
// Native pointers are ID3D11Resource*, borrowed on the render thread.
struct PathHairBuildItem {
    uint64_t id=0,particles=0,barycentrics=0,indices=0,noise=0,color=0;
    uint32_t indexCount=0,index32=0,indexStart=0,baseVertex=0;
    float tess[4]={},length[4]={1,1,1,1},mat0[4]={},mat1[4]={},mat2[4]={};
    float specularColor[4]={1,1,1,1}; // actual linear shader colour, not a physical Fresnel F0
};
static_assert(sizeof(PathHairBuildItem)==160,"path hair registry layout");
