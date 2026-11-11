#pragma once
#include "bake.h"
#include <array>
// Immutable connectivity only. Positions and visibility are evaluated from VaM's skinned vertex buffer each frame.
struct MouthMap {
    int sub=0, width=0, height=0;
    float lo[2]={}, hi[2]={};
    std::vector<std::array<uint32_t,4>> texels; // receiver ids and two UNORM16 barycentric weights
};
struct MouthData {
    int vertexCount=0, depth=0;
    uint32_t spanVertex[2]={};
    float span=0, radius=.04f;
    std::vector<std::array<uint32_t,4>> nodes, triangles, receivers;
    std::vector<uint32_t> adjacency;
    std::vector<MouthMap> maps;
};
void BuildMouth(const BakeInput &in, MouthData &out);
