#pragma once

#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/provider_api.hpp"

#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace xrfg {

// One OFXR_PublishD3D11GuidesV3 call, held until the swapchain's release
// takes it. The texture pointers in `data` are cleared; the references are
// the ComPtrs.
struct D3D11GuideRecord {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> depth;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> motion_vectors;
    OFXR_D3D11GuidePublicationV3 data{};
};

// The newest publication for this application swapchain that no release
// has taken yet; each is handed out once.
[[nodiscard]] std::optional<D3D11GuideRecord> take_d3d11_guides(
    std::uint64_t swapchain) noexcept;

// Moves one swapchain's D3D11 guides into D3D12 textures owned per history
// slot. Runs at the image's release, after the history capture has proven
// the slot free: the producer's context copies into a shared pair, the
// layer's queue copies on into the slot's pair and signals a fence the
// synthesis queue waits for. Nothing here waits on the CPU; a copy list
// still in flight costs that frame its guides.
class D3D11GuideTransfer final {
public:
    D3D11GuideTransfer() = default;
    D3D11GuideTransfer(const D3D11GuideTransfer&) = delete;
    D3D11GuideTransfer& operator=(const D3D11GuideTransfer&) = delete;

    [[nodiscard]] HRESULT initialize(
        ID3D11Device* d3d11_device,
        ID3D11DeviceContext* d3d11_context,
        ID3D12Device* d3d12_device,
        ID3D12CommandQueue* d3d12_queue) noexcept;

    // The application's context thread only. On success the returned set
    // holds the slot's textures and the fence its copy signals.
    [[nodiscard]] HRESULT transfer(
        const D3D11GuideRecord& record,
        std::uint32_t history_slot,
        std::shared_ptr<const DlssMotionVectorSet>* output) noexcept;

    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    struct SharedImage {
        Microsoft::WRL::ComPtr<ID3D12Resource> d3d12;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d11;
        D3D11_TEXTURE2D_DESC description{};
    };
    static constexpr std::size_t kListCount = 4;

    [[nodiscard]] HRESULT create_resource(const D3D11_TEXTURE2D_DESC& source,
        D3D12_RESOURCE_FLAGS flags, D3D12_HEAP_FLAGS heap_flags,
        Microsoft::WRL::ComPtr<ID3D12Resource>* output) noexcept;
    [[nodiscard]] HRESULT ensure_shared(std::size_t index,
        const D3D11_TEXTURE2D_DESC& source) noexcept;

    Microsoft::WRL::ComPtr<ID3D11Device5> d3d11_device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> d3d11_context_;
    Microsoft::WRL::ComPtr<ID3D12Device> d3d12_device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> d3d12_queue_;
    Microsoft::WRL::ComPtr<ID3D12Fence> d3d12_fence_;
    Microsoft::WRL::ComPtr<ID3D11Fence> d3d11_fence_;
    std::array<SharedImage, 2> shared_{};
    std::vector<std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2>> slots_;
    std::array<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>, kListCount> allocators_{};
    std::array<std::uint64_t, kListCount> allocator_values_{};
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
    std::size_t next_list_{};
    std::uint64_t next_value_{1};
    std::uint64_t last_d3d12_read_{};
    bool failed_{};
    bool format_logged_{};
};

// The single-eye guide set the synthesizer reads, around the history slot's
// own copies of the textures.
[[nodiscard]] std::shared_ptr<const DlssMotionVectorSet> make_dlssg_guide_set(
    const D3D11GuideRecord& record,
    Microsoft::WRL::ComPtr<ID3D12Resource> depth,
    Microsoft::WRL::ComPtr<ID3D12Resource> motion_vectors,
    Microsoft::WRL::ComPtr<ID3D12Fence> fence,
    std::uint64_t fence_value) noexcept;

}  // namespace xrfg
