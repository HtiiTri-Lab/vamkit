#pragma once

#include "xrfg/dlss_motion_vectors.hpp"

#include <d3d12.h>

#include <cstdint>
#include <memory>

namespace xrfg {

// A frame generator that makes the in-between frames of a pair from the newer frame B, B's guides (the producer's
// depth, motion vectors and camera, OFXR_PublishD3D11GuidesV3) and its own history of the frame before. Two exist:
// NVIDIA DLSS Frame Generation (d3d12_dlssg.cpp) and AMD FSR frame generation (d3d12_fsrfg.cpp, on any card). Both are compiled into the OpenXR layer and into the desktop
// presenter of the d3d11-proxy component.
//
// The contract is the one DLSS-G set: every real frame is fed exactly once and in order; a gap resets the history,
// and a reset pair carries a copy of B that must be shown with B's pose. One instance per eye.

// Interpolated frames per pair this code is built for: 3, for 4X. That is where the work stopped, not what the
// generators can do: DLSS-G 310.9.16 on an RTX 50 reports 5 (6X). Raising it means this constant, the synthesizer's
// kMaxExtraSynthetics, the layer's swapchain budget, the presenter's clamp, the launcher's and the menu's 2 / 3 / 4
// (docs/knowledge/frame-generation.md, "Not built here"). A generator says what it makes through max_outputs().
inline constexpr std::uint32_t kGuidedMaxOutputs = 3;

struct GuidedRecordInput {
    // B as captured: an RGBA8/BGRA8-family texture in COMMON.
    ID3D12Resource* backbuffer{};
    // B's guides; depth and motion_vectors in COMMON.
    const DlssMotionVectorFrame* guides{};
    // 1 for 2X, 2 for 3X, 3 for 4X; no more than max_outputs().
    std::uint32_t outputs{1};
    ID3D12Resource* destinations[kGuidedMaxOutputs]{};
    // State the destinations are in before and after the copies.
    D3D12_RESOURCE_STATES destination_state{D3D12_RESOURCE_STATE_COMMON};
};

class D3D12GuidedFrameGenerator {
public:
    virtual ~D3D12GuidedFrameGenerator() = default;

    // "DLSS-G" or "FSR", for logs.
    [[nodiscard]] virtual const char* name() const noexcept = 0;
    // Interpolated frames per pair this generator makes at most (FSR: 1).
    [[nodiscard]] virtual std::uint32_t max_outputs() const noexcept = 0;
    // Whether this pair can be generated: complete guides and a supported frame count. Cheap; records nothing.
    [[nodiscard]] virtual bool can_generate(const DlssMotionVectorFrame& guides, std::uint32_t outputs) const noexcept = 0;
    // Records the generation of this pair and the copies into the destinations. *generated is the number of
    // interpolated frames written (0 after a reset or a failure, when the destinations got a copy of B instead).
    // Never leaves the list half-recorded.
    [[nodiscard]] virtual HRESULT record(ID3D12GraphicsCommandList* list, const GuidedRecordInput& input,
        std::uint32_t* generated) noexcept = 0;
    // The next record() resets the history: called when a real frame went another way (prime, optical flow, repeat).
    virtual void invalidate_history() noexcept = 0;
    [[nodiscard]] virtual bool usable() const noexcept = 0;
};

// Which generator the user chose: [Performance] FrameGenModel in the run's performance.ini (VAM_PERFORMANCE_CONFIG),
// DLSS (the default) or FSR. Followed while the game runs (read again when the file changes).
enum class FrameGenModel : std::uint32_t { dlss = 0, fsr = 1 };
[[nodiscard]] FrameGenModel frame_gen_model_wanted() noexcept;

// The generator for images of `image` on this device and queue: the one chosen when it can run here, else the other;
// null when neither can (the caller then has no guided generation). `made` receives which one it is.
[[nodiscard]] std::unique_ptr<D3D12GuidedFrameGenerator> make_guided_frame_generator(
    ID3D12Device* device, ID3D12CommandQueue* queue, const D3D12_RESOURCE_DESC& image, FrameGenModel* made) noexcept;

// The most frames per application frame the generator the user's choice leads to can deliver: 2 with FSR (one
// generated frame per pair), 4 with DLSS Frame Generation. From the choice and what can run, not from the generator
// last made, so a live change of the choice is answered at once.
[[nodiscard]] std::uint32_t guided_frames_most() noexcept;

// What can run in this process, for the in-game menu (OFXR_GetFrameGeneratorsV1): bit 0 DLSS Frame Generation,
// bit 1 FSR frame generation, bit 4 while FSR is the one generating, bit 8 once a generator was made (the bits are
// what was found then; before that they are what the files and the environment promise).
[[nodiscard]] std::uint32_t frame_generators_report() noexcept;

}  // namespace xrfg
