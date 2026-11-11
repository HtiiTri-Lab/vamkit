#pragma once

#include "xrfg/dlss_motion_vectors.hpp"

#include <d3d12.h>

#include <cstdint>
#include <memory>


namespace xrfg {

// NVIDIA DLSS Frame Generation (NGX D3D12, SDK 310.x) for one swapchain.
//
// Opt-in per process: OFXR_DLSSG=1 in the environment, with the directory
// that holds nvngx_dlssg.dll in OFXR_DLSSG_RUNTIME_PATH. It augments the
// NVIDIA optical-flow backend rather than replacing it: a pair goes through
// DLSS-G only when the capture carries the producer's depth, motion vectors
// and camera (see OFXR_PublishD3D11GuidesV3), and through the optical-flow
// path otherwise, frame by frame.
//
// DLSS-G keeps its own previous frame, so every real frame is fed exactly
// once and in order; a gap resets it, and a reset pair carries a copy of B
// that must be shown with B's pose. One instance per eye, as the SDK
// documents for stereo.
[[nodiscard]] bool dlssg_requested() noexcept;

// One line to OFXR_DLSSG_LOG\dlssg.log (when set). Thread safe.
void dlssg_log(const char* format, ...) noexcept;

// The D3D12 device of the latest session that can run DLSS-G, and NGX D3D12
// initialization on it ahead of first use. The D3D11 producer calls
// OFXR_PrepareFrameGenerationV3 before it brings up its own NGX (DLSS SR):
// left to the first pair, NGX D3D12 came up between the producer's two
// per-eye SR features, so the eyes did not start from the same NGX state.
void dlssg_set_session_device(ID3D12Device* device) noexcept;
[[nodiscard]] bool dlssg_prepare_session_device() noexcept;

// Interpolated frames per pair the generator can make: 3, for 4X.
inline constexpr std::uint32_t kDlssgMaxOutputs = 3;

struct DlssgRecordInput {
    // B as captured: an RGBA8/BGRA8-family texture in COMMON.
    ID3D12Resource* backbuffer{};
    // B's guides; depth and motion_vectors in COMMON.
    const DlssMotionVectorFrame* guides{};
    // 1 for 2X, 2 for 3X, 3 for 4X.
    std::uint32_t outputs{1};
    ID3D12Resource* destinations[kDlssgMaxOutputs]{};
    // State the destinations are in before and after the copies.
    D3D12_RESOURCE_STATES destination_state{D3D12_RESOURCE_STATE_COMMON};
};

class D3D12DlssgFrameGenerator final {
public:
    D3D12DlssgFrameGenerator() noexcept;
    ~D3D12DlssgFrameGenerator();

    D3D12DlssgFrameGenerator(const D3D12DlssgFrameGenerator&) = delete;
    D3D12DlssgFrameGenerator& operator=(const D3D12DlssgFrameGenerator&) = delete;

    // NGX initialization (once per device), capability check and the
    // interpolation targets for images of `image`. Fails without side
    // effects on the caller, which then simply has no DLSS-G.
    [[nodiscard]] HRESULT initialize(
        ID3D12Device* device,
        ID3D12CommandQueue* queue,
        const D3D12_RESOURCE_DESC& image) noexcept;

    // Whether this pair can be generated: complete guides and a supported
    // frame count. Cheap; records nothing.
    [[nodiscard]] bool can_generate(
        const DlssMotionVectorFrame& guides,
        std::uint32_t outputs) const noexcept;

    // Records B's copy, the DLSS-G evaluations and the copies into the
    // destinations. *generated is the number of interpolated frames written
    // (0 after a reset or an evaluation failure, when the destinations got a
    // copy of B instead). Never leaves the list half-recorded: on any error
    // after the first command it falls back to those copies and returns S_OK.
    [[nodiscard]] HRESULT record(
        ID3D12GraphicsCommandList* list,
        const DlssgRecordInput& input,
        std::uint32_t* generated) noexcept;

    // The next record() resets DLSS-G's history: called when a real frame
    // went another way (prime, optical flow, repeat).
    void invalidate_history() noexcept;

    [[nodiscard]] bool usable() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xrfg
