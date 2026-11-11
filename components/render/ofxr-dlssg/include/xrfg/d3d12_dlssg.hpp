#pragma once

#include "xrfg/d3d12_frame_generator.hpp"
#include "xrfg/dlss_motion_vectors.hpp"

#include <d3d12.h>
#include <string>

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

// NGX D3D12 for DLSS-G on this process's devices: 1 once it came up with frame generation available, 0 once it
// failed (no RTX 40 or newer, no runtime, VAMKIT_NO_DLSS=1), -1 while nothing has tried. dlssg_available tries.
[[nodiscard]] int dlssg_state() noexcept;
// Why not, in words, once it has failed (empty before).
[[nodiscard]] std::string dlssg_why_not() noexcept;
[[nodiscard]] bool dlssg_available(ID3D12Device* device) noexcept;

// Interpolated frames per pair the generator can make: 3, for 4X.
inline constexpr std::uint32_t kDlssgMaxOutputs = kGuidedMaxOutputs;

// The record input is the guided generators' common one (d3d12_frame_generator.hpp).
using DlssgRecordInput = GuidedRecordInput;

class D3D12DlssgFrameGenerator final : public D3D12GuidedFrameGenerator {
public:
    D3D12DlssgFrameGenerator() noexcept;
    ~D3D12DlssgFrameGenerator() override;

    D3D12DlssgFrameGenerator(const D3D12DlssgFrameGenerator&) = delete;
    D3D12DlssgFrameGenerator& operator=(const D3D12DlssgFrameGenerator&) = delete;

    // NGX initialization (once per device), capability check and the
    // interpolation targets for images of `image`. Fails without side
    // effects on the caller, which then simply has no DLSS-G.
    [[nodiscard]] HRESULT initialize(
        ID3D12Device* device,
        ID3D12CommandQueue* queue,
        const D3D12_RESOURCE_DESC& image) noexcept;

    [[nodiscard]] const char* name() const noexcept override { return "DLSS-G"; }
    // What this card's DLSS-G makes (DLSSG.MultiFrameCountMax: 1 on RTX 40; 310.9.16 on an RTX 50 reports 5), cut to
    // kGuidedMaxOutputs, which is where this code stops, not DLSS-G.
    [[nodiscard]] std::uint32_t max_outputs() const noexcept override;

    // Whether this pair can be generated: complete guides and a supported
    // frame count. Cheap; records nothing.
    [[nodiscard]] bool can_generate(
        const DlssMotionVectorFrame& guides,
        std::uint32_t outputs) const noexcept override;

    // Records B's copy, the DLSS-G evaluations and the copies into the
    // destinations. *generated is the number of interpolated frames written
    // (0 after a reset or an evaluation failure, when the destinations got a
    // copy of B instead). Never leaves the list half-recorded: on any error
    // after the first command it falls back to those copies and returns S_OK.
    [[nodiscard]] HRESULT record(
        ID3D12GraphicsCommandList* list,
        const DlssgRecordInput& input,
        std::uint32_t* generated) noexcept override;

    // The next record() resets DLSS-G's history: called when a real frame
    // went another way (prime, optical flow, repeat).
    void invalidate_history() noexcept override;

    [[nodiscard]] bool usable() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xrfg
