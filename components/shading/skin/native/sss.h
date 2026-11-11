// Screen-space subsurface scattering for VaM's skin (0.10.0; sss.cpp, shaders\sss.hlsl). Render thread only, except
// SssStatus. The includer defines CINTERFACE / COBJMACROS before d3d11.h, as skin_transmission.cpp does.
#pragma once
#include <d3d11.h>
#include <string>

typedef void(STDMETHODCALLTYPE *SssSetPSFn)(ID3D11DeviceContext *, ID3D11PixelShader *, ID3D11ClassInstance *const *, UINT);
typedef HRESULT(STDMETHODCALLTYPE *SssCreatePSFn)(ID3D11Device *, const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11PixelShader **);

// the passes' shaders and states; createPS bypasses the module's own CreatePixelShader hook
bool SssInit(ID3D11Device *dev, SssCreatePSFn createPS, std::string &error);
// strength 0..1 (0 = off: no targets, no passes), width = the skin profile's scale (1 = measured skin), the eye
// camera's tan(half vertical field of view)
void SssSetParams(float strength, float width, float tanHalfY);
// 0.20.0: the translucency target's blur and composite: strength 0 = no passes (its amount is in the skin programs),
// width = its blur's scale (1 = 10 + 22 mm Gaussians)
void SssSetTranslucency(float strength, float width);
// start of the eye camera's opaque pass: its skin draws may write the subsurface targets
bool SssSetRR(bool enabled, float detail, bool materialReference=true);
void SssBegin(unsigned viewCount);
bool SssArmed();
bool SssPathTarget(ID3D11DeviceContext *context);
bool SssPathDepthReady();
bool SssMarkPathDepth(ID3D11Resource* color,ID3D11Resource* depth);
void SssDepthCleared(ID3D11DepthStencilView* depth,UINT flags);

// Around one skin draw whose pixel program writes the subsurface outputs: binds the three targets next to the
// camera's colour (the first draw of the pass makes them for its size and clears the depth one) with blend states
// derived from the draw's (base pass: written; additive light passes: the light added, distance, albedo and depth
// kept); false = not bound (another target, MSAA, unordered-access views bound, off), nothing to undo.
struct SssDrawSave {
    ID3D11RenderTargetView *rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView *dsv;
    ID3D11BlendState *blend;
    FLOAT factor[4];
    UINT mask;
};
// guidesOnly supplies eye/mouth reconstruction attributes but writes zero
// scattering irradiance, so enabling RR cannot scatter those surfaces.
bool SssBeforeDraw(ID3D11DeviceContext *c, SssDrawSave &s, bool guidesOnly=false);
void SssAfterDraw(ID3D11DeviceContext *c, SssDrawSave &s);

// end of the eye camera's opaque pass: the blur and the composite into the camera's colour, every pipeline state the
// passes touch put back (pixel shaders through setPS: the module's PSSetShader hook keeps tracking the game's)
void SssEnd(ID3D11DeviceContext *c, SssSetPSFn setPS);
// once per frame: the frame's counts become the status's
void SssFrame();
// the passes' GPU time from now on (any thread)
void SssResetTiming();
// JSON object (any thread)
std::string SssStatus();
