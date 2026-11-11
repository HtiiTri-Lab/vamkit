// VaMWrapBatchNative: the render-thread side of the wrap batch plugin (..\VaMWrapBatch.cs). One op: Flush Unity's D3D11
// immediate context between the phases of a batch. On D3D11On12 that ends the translation layer's command list, so the
// next phase starts a new one with every buffer back in COMMON and its dispatches need no barriers (the wraps run side
// by side); the same as the cloth and hair solvers' phase flushes (ClothPhysics\plugin\native, HairSolver\plugin\native).
#include <d3d11.h>
#include <d3d11on12.h>
#include <atomic>

#define WB_API extern "C" __declspec(dllexport)

static const int kApiVersion = 1;
static ID3D11DeviceContext* gCtx = nullptr;       // Unity's immediate context (referenced while we use it)
static int gOn12 = -1;                            // -1 no device yet, 0 plain D3D11, 1 D3D11On12
static std::atomic<int> gFlushes{0};

WB_API int WB_ApiVersion() { return kApiVersion; }

// Unity's device from one of its textures (Texture.GetNativeTexturePtr), once. Returns 1 on D3D11On12, 0 on plain D3D11,
// -1 without a device.
WB_API int WB_Init(void* texture) {
    if (gCtx) return gOn12;
    if (!texture) return -1;
    ID3D11Device* dev = nullptr;
    ((ID3D11Resource*)texture)->GetDevice(&dev);
    if (!dev) return -1;
    ID3D11On12Device* on12 = nullptr;
    int is = SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11On12Device), (void**)&on12)) ? 1 : 0;
    if (on12) on12->Release();
    dev->GetImmediateContext(&gCtx);              // the context keeps the device
    dev->Release();
    gOn12 = is;
    return is;
}

static void __stdcall OnRenderEvent(int) {
    if (!gCtx) return;
    gCtx->Flush();
    gFlushes.fetch_add(1);
}

WB_API void* WB_RenderEventFunc() { return (void*)&OnRenderEvent; }

// Flushes run so far (diagnostics).
WB_API int WB_Flushes() { return gFlushes.load(); }
