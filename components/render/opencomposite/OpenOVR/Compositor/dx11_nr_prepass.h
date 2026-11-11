#pragma once
#include <d3d11.h>
#include <memory>

// Opt-in, shared by the two eye compositors. Runs after crop/flip and before
// xrReleaseSwapchainImage, so OFXR records NR-treated REAL frames in its history.
class VamNrPrepass {
public:
    static std::shared_ptr<VamNrPrepass> GetIfEnabled(ID3D11Device* device);
    ~VamNrPrepass();
    // guides: optional bridge depth + jittered motion for this render-size eye.
    void Process(ID3D11Texture2D* image, ID3D11RenderTargetView* target, DXGI_FORMAT viewFormat, unsigned eye,
                 const struct VamFrameGuides* guides = nullptr);
    // [NR] PackEyes: both eyes in one NR evaluation. PackEyes() = on; ProcessPair
    // = Process for eye 0 and 1 at once (eye 1's image cropped ahead), false
    // with nothing done when the regions cannot be packed.
    bool PackEyes();
    bool ProcessPair(ID3D11Texture2D* const images[2], ID3D11RenderTargetView* const targets[2], DXGI_FORMAT viewFormat,
                     const struct VamFrameGuides* const guides[2]);
private:
    struct Impl;
    explicit VamNrPrepass(ID3D11Device* device);
    std::unique_ptr<Impl> impl;
};
