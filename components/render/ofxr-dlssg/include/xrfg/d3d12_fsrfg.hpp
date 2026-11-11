#pragma once

#include "xrfg/d3d12_frame_generator.hpp"

#include <d3d12.h>

#include <cstdint>
#include <memory>

namespace xrfg {

// AMD FSR frame generation through the FSR API (amd_fidelityfx_loader_dx12.dll and the frame generation DLL beside
// it, in the folder VAM_FSR_RUNTIME_PATH names; loaded at run time, nothing is linked). The context is dispatched by
// hand: no FSR swap chain exists, the generated frame is written into a texture of the caller's. That is what lets it
// serve an OpenXR layer (per-eye images handed to a runtime) and a presenter that paces its own presents.
//
// One generated frame per pair (2X) as built: the dispatch has room for four outputs, and the one provider this was
// run on (FSR 3.1.6, an NVIDIA card) filled only the first when asked for two (VAMKIT_FSR_FRAMES=2 repeats that
// probe on any card; AMD's ML generator on a Radeon was not run). The API picks the implementation: the analytical FSR 3.1 frame generation on any
// card, AMD's ML generator where the card and driver have it. It needs what DLSS-G needs (B, B's depth, motion
// vectors and camera) and keeps the frame before B itself.

// Whether the FSR API is present and lists a frame generation version for this device. Cheap after the first call.
[[nodiscard]] bool fsrfg_available(ID3D12Device* device) noexcept;

class D3D12FsrFrameGenerator final : public D3D12GuidedFrameGenerator {
public:
    D3D12FsrFrameGenerator() noexcept;
    ~D3D12FsrFrameGenerator() override;

    D3D12FsrFrameGenerator(const D3D12FsrFrameGenerator&) = delete;
    D3D12FsrFrameGenerator& operator=(const D3D12FsrFrameGenerator&) = delete;

    // Loads the API and makes the generator's own input and output images for images of `image`. The FSR context is
    // made with the first pair, when the size of the guides is known. Fails without side effects on the caller.
    [[nodiscard]] HRESULT initialize(ID3D12Device* device, ID3D12CommandQueue* queue, const D3D12_RESOURCE_DESC& image) noexcept;

    [[nodiscard]] const char* name() const noexcept override { return "FSR"; }
    [[nodiscard]] std::uint32_t max_outputs() const noexcept override { return 1; }
    [[nodiscard]] bool can_generate(const DlssMotionVectorFrame& guides, std::uint32_t outputs) const noexcept override;
    [[nodiscard]] HRESULT record(ID3D12GraphicsCommandList* list, const GuidedRecordInput& input,
        std::uint32_t* generated) noexcept override;
    void invalidate_history() noexcept override;
    [[nodiscard]] bool usable() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xrfg
