#pragma once
#include <d3d11.h>
#include <memory>

class VamFfr {
public:
    // A process-lifetime instance: patched D3D11 methods must never outlive it.
    static VamFfr* GetIfEnabled(ID3D11Device* device);
    void Observe(ID3D11Texture2D* submitted, const float* bounds, unsigned eye, float centerX, float centerY);
    void Suspend();
    void Resume();
    struct Impl;
private:
    explicit VamFfr(ID3D11Device* device);
    std::unique_ptr<Impl> impl;
};

struct VamFfrScope {
    VamFfr* ffr;
    explicit VamFfrScope(VamFfr* f) : ffr(f) { if (ffr) ffr->Suspend(); }
    ~VamFfrScope() { if (ffr) ffr->Resume(); }
};
