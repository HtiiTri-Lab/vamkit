#pragma once
#include <d3d11.h>
#include <cstdint>
#include <memory>

// Render-size, upright, per-eye guide images built from the VaM BepInEx bridge
// capture: the scene depth bound at CameraEvent.AfterForwardAlpha plus the
// camera matrices and projection jitter Unity rendered that frame with.
struct VamFrameGuides {
    ID3D11Texture2D* depth = nullptr;          // R32_FLOAT hardware depth, reversed Z (1 = near)
    ID3D11Texture2D* motion = nullptr;         // RG32_FLOAT pixels, current -> previous, jitter removed
    ID3D11Texture2D* motionJittered = nullptr; // RG32_FLOAT, same plus the jitter change between frames
    ID3D11Texture2D* motionHalf = nullptr;     // RG16_FLOAT copy of motion: D3D11 cannot share RG32_FLOAT
    ID3D11Texture2D* motionJitteredHalf = nullptr; // RG16_FLOAT copy of motionJittered, for NR (shared to D3D12)
    ID3D11Texture2D* hairMask = nullptr;       // R8_UNORM DLSS bias-current-colour mask on hair; null while DLSSHairBias=0
    // R8_UNORM, 1 where VaM's world-space UI was drawn in front of the scene
    // (same jittered raster as the colour); null on frames without UI, or
    // with DLSSUIBypass=0 (the default). DLSS SR's output there is replaced (UI bypass).
    ID3D11Texture2D* uiMask = nullptr;
    // R16G16B16A16_FLOAT render size: the nasolabial-fold band of female
    // faces ([NR] NasolabialReduction) as (1, keep, keep, 1), keep = 1 - cut
    // * band, 1 elsewhere. NR's composite blends its result toward its input
    // by 1 - keep (never a DLSSNR.ControlMask). Null while that is 0. NR after
    // the upscaler gets this render-size mask as well.
    ID3D11Texture2D* nrControl = nullptr;
    float jitterX = 0.f, jitterY = 0.f;        // image content shift of this frame, render pixels, y down
    float frameDeltaMs = 16.6667f;
    uint64_t serial = 0;
    bool reset = false;
    // Native frame (bridge flag): rendered for the output size without
    // jitter, at render scale 100% or while VaM's menus are open (menuResized
    // when that is not the usual render size); no DLSS SR for it.
    bool nativeFrame = false;
    bool menuResized = false;
    // This eye's camera for frame generation: Unity/OpenGL clip convention
    // (y up, z -1 at near), row-major for column vectors, without jitter.
    float viewToClip[16] = {};
    float clipToPrevClip[16] = {};
    float viewToWorld[16] = {};
    float nearPlane = 0.1f, farPlane = 1000.f;
};

// Real NVIDIA DLSS Super Resolution (NGX D3D11) for the VaM performance path.
// Process lifetime: Unity's render-event callback can reach it at any time.
class VamDlss {
public:
    static VamDlss* GetIfEnabled(ID3D11Device* device);
    // False when no fresh bridge capture matches this submission. ahead: made
    // before that eye's own submission ([NR] PackEyes, eye 1 during eye 0's);
    // its submission then gets the same guides.
    bool PrepareGuides(ID3D11Texture2D* submitted, const float* bounds, unsigned eye,
        UINT width, UINT height, VamFrameGuides& guides, bool ahead = false);
    // color: render-size eye image after optional NR (RGBA8/BGRA8/RGBA16F family).
    // output: output-size texture in the same 8-bit or FP16 family (typeless,
    // typed or sRGB); the result is copied into its mip 0.
    bool Upscale(unsigned eye, ID3D11Texture2D* color, const VamFrameGuides& guides,
        ID3D11Texture2D* output, UINT outputWidth, UINT outputHeight);
    // RR only (DLSSPreset=RR): the image Upscale will give `eye` this frame
    // (`serial`), into `output` (as Upscale's), before that eye's own
    // submission ([NR] PackEyes after the upscaler: eye 1 during eye 0's).
    // Nothing else changes; that eye's Upscale still runs. False otherwise.
    bool UpscaledAhead(unsigned eye, uint64_t serial, ID3D11Texture2D* output, UINT outputWidth, UINT outputHeight);
    // NR after the upscaler (NRAfterUpscale): `guides` (this
    // eye's, from PrepareGuides this frame) resampled to outputWidth x
    // outputHeight into `out`: depth (R32_FLOAT) and motionHalf =
    // motionJitteredHalf (R16G16_FLOAT, output pixels, no jitter: the
    // upscaled image is not jittered), plus reset. Valid until the next call.
    bool PrepareOutputGuides(unsigned eye, const VamFrameGuides& guides, UINT outputWidth, UINT outputHeight, VamFrameGuides& out);
    // The upscaler reports whether DLSS is the selected upscaler right now;
    // the bridge only jitters the projection while this is true.
    void SetSelected(bool selected);
    // Hands this eye's guides to OFXR's DLSS Frame Generation
    // (OFXR_PublishD3D11GuidesV3) just before xrReleaseSwapchainImage(swapchain)
    // on the same context. Does nothing unless that OFXR build asks for them,
    // or for an eye left out by DLSSFGEyes (that eye then uses optical flow).
    void PublishFrameGeneration(uint64_t swapchain, unsigned eye, const VamFrameGuides& guides);
    // OFXR DLSS Frame Generation needs NGX D3D12 set up by its own layer
    // before anything else in the process initializes NGX D3D12 (the NR
    // prepass) or creates a DLSS SR feature. Once per process; true when
    // DLSS-G is up, and NGX D3D12 is then OFXR's.
    static bool PrepareFrameGenerationNgx();
    struct Impl;
private:
    explicit VamDlss(ID3D11Device* device);
    std::unique_ptr<Impl> impl;
};
