#pragma once
#include <d3d11.h>

// Texture mip bias for DLSS SR (DLSS programming guide 3.5): while DLSS SR
// upscales, the game's pixel-shader samplers are swapped for copies with
// MipLODBias = log2(render / output) - 1, so textures are sampled for the
// output resolution rather than the render.
class VamMipBias {
public:
    // Hooks PSSetSamplers on the device's immediate context, once per process
    // (the hook must never outlive the module, which is pinned).
    static void Install(ID3D11Device* device);
    // The bias for what the game draws from now on; 0 draws with its own
    // samplers. Called on the thread that uses the immediate context.
    static void Set(float bias);
};
