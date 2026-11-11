#pragma once
#include <d3d11.h>
#include <cstdint>
#include <memory>
class VamNrPrepass;
class VamDlss;

// Per-eye crop -> NR at the input size -> DLSS SR / FSR1 / NIS -> runtime-sized eye texture.
class VamNis {
public:
    explicit VamNis(ID3D11Device* device);
    ~VamNis();
    bool Process(ID3D11Texture2D* source, UINT slice, const float* bounds,
        DXGI_FORMAT format, UINT inputWidth, UINT inputHeight,
        ID3D11RenderTargetView* destination, UINT outputWidth, UINT outputHeight,
        VamNrPrepass* nr, unsigned eye, VamDlss* dlss = nullptr,
        uint64_t frameGenerationSwapchain = 0);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
