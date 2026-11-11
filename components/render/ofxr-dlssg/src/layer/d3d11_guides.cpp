#include "xrfg/d3d11_guides.hpp"

#include "xrfg/d3d12_dlssg.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace xrfg {
namespace {

struct HeldGuides {
    D3D11GuideRecord record;
    bool taken{};
};

std::mutex& guide_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::uint64_t, HeldGuides>& held_guides() {
    static std::unordered_map<std::uint64_t, HeldGuides> map;
    return map;
}

std::uint64_t g_published{};
std::uint64_t g_rejected{};

bool finite_values(const float* values, std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
        if (!(values[i] == values[i]) || values[i] > 1.0e30F || values[i] < -1.0e30F) return false;
    }
    return true;
}

}  // namespace

std::optional<D3D11GuideRecord> take_d3d11_guides(std::uint64_t swapchain) noexcept {
    try {
        std::scoped_lock lock(guide_mutex());
        auto found = held_guides().find(swapchain);
        if (found == held_guides().end() || found->second.taken) return std::nullopt;
        found->second.taken = true;
        D3D11GuideRecord record = std::move(found->second.record);
        found->second.record = {};
        return record;
    } catch (...) {
        return std::nullopt;
    }
}

namespace {

// Typed formats only: DLSS-G creates its views from the resource. And only
// formats D3D11 can share (D3D11_FORMAT_SUPPORT2_SHAREABLE): R32G32_FLOAT is
// not one - OpenSharedResource1 refuses it with E_INVALIDARG, measured - so
// motion comes as R16G16_FLOAT.
bool guide_format(std::size_t index, DXGI_FORMAT format) noexcept {
    return index == 0
        ? (format == DXGI_FORMAT_R32_FLOAT || format == DXGI_FORMAT_R16_FLOAT)
        : format == DXGI_FORMAT_R16G16_FLOAT;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

}  // namespace

HRESULT D3D11GuideTransfer::initialize(
    ID3D11Device* d3d11_device,
    ID3D11DeviceContext* d3d11_context,
    ID3D12Device* d3d12_device,
    ID3D12CommandQueue* d3d12_queue) noexcept {
    try {
        if (d3d11_device == nullptr || d3d11_context == nullptr ||
            d3d12_device == nullptr || d3d12_queue == nullptr) {
            return E_INVALIDARG;
        }
        HRESULT result = d3d11_device->QueryInterface(
            IID_PPV_ARGS(d3d11_device_.ReleaseAndGetAddressOf()));
        if (FAILED(result)) return result;
        result = d3d11_context->QueryInterface(
            IID_PPV_ARGS(d3d11_context_.ReleaseAndGetAddressOf()));
        if (FAILED(result)) return result;
        d3d12_device_ = d3d12_device;
        d3d12_queue_ = d3d12_queue;
        result = d3d12_device_->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
            IID_PPV_ARGS(d3d12_fence_.ReleaseAndGetAddressOf()));
        if (FAILED(result)) return result;
        HANDLE handle = nullptr;
        result = d3d12_device_->CreateSharedHandle(
            d3d12_fence_.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
        if (FAILED(result)) return result;
        result = d3d11_device_->OpenSharedFence(
            handle, IID_PPV_ARGS(d3d11_fence_.ReleaseAndGetAddressOf()));
        CloseHandle(handle);
        return result;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D11GuideTransfer::create_resource(const D3D11_TEXTURE2D_DESC& source,
    D3D12_RESOURCE_FLAGS flags, D3D12_HEAP_FLAGS heap_flags,
    Microsoft::WRL::ComPtr<ID3D12Resource>* output) noexcept {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = source.Width;
    description.Height = source.Height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = source.Format;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = flags;
    return d3d12_device_->CreateCommittedResource(&heap, heap_flags, &description,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(output->ReleaseAndGetAddressOf()));
}

HRESULT D3D11GuideTransfer::ensure_shared(std::size_t index,
    const D3D11_TEXTURE2D_DESC& source) noexcept {
    SharedImage& image = shared_[index];
    if (image.d3d11 && image.description.Width == source.Width &&
        image.description.Height == source.Height &&
        image.description.Format == source.Format) {
        return S_OK;
    }
    image = {};
    // Simultaneous access: both APIs touch it, and it decays to COMMON after
    // every D3D12 submission without a barrier back. D3D11 opens a shared
    // texture only with a bind flag it can use (as the D3D11 bridge found);
    // every guide format has a render-target member.
    HRESULT result = create_resource(source,
        static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
            D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS),
        D3D12_HEAP_FLAG_SHARED, &image.d3d12);
    if (FAILED(result)) {
        dlssg_log("Shared guide texture %ux%u format %u: CreateCommittedResource 0x%08lx",
            source.Width, source.Height, static_cast<unsigned>(source.Format), static_cast<unsigned long>(result));
        return result;
    }
    HANDLE handle = nullptr;
    result = d3d12_device_->CreateSharedHandle(image.d3d12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
    if (FAILED(result)) {
        dlssg_log("Shared guide texture: CreateSharedHandle 0x%08lx", static_cast<unsigned long>(result));
        image = {};
        return result;
    }
    Microsoft::WRL::ComPtr<ID3D11Device1> device1;
    result = d3d11_device_.As(&device1);
    if (SUCCEEDED(result)) {
        result = device1->OpenSharedResource1(handle, IID_PPV_ARGS(image.d3d11.GetAddressOf()));
    }
    CloseHandle(handle);
    if (FAILED(result)) {
        dlssg_log("Shared guide texture format %u: OpenSharedResource1 0x%08lx",
            static_cast<unsigned>(source.Format), static_cast<unsigned long>(result));
        image = {};
        return result;
    }
    image.description = source;
    return S_OK;
}

HRESULT D3D11GuideTransfer::transfer(
    const D3D11GuideRecord& record,
    std::uint32_t history_slot,
    std::shared_ptr<const DlssMotionVectorSet>* output) noexcept {
    try {
        if (output == nullptr) return E_POINTER;
        output->reset();
        if (failed_ || !d3d12_fence_ || !record.depth || !record.motion_vectors ||
            history_slot >= 64U) {
            return E_INVALIDARG;
        }
        const std::array<ID3D11Texture2D*, 2> sources{record.depth.Get(), record.motion_vectors.Get()};
        std::array<D3D11_TEXTURE2D_DESC, 2> descriptions{};
        for (std::size_t index = 0; index < sources.size(); ++index) {
            sources[index]->GetDesc(&descriptions[index]);
            if (descriptions[index].SampleDesc.Count != 1 || descriptions[index].ArraySize != 1 ||
                descriptions[index].Width != record.data.width ||
                descriptions[index].Height != record.data.height ||
                !guide_format(index, descriptions[index].Format)) {
                if (!format_logged_) {
                    format_logged_ = true;
                    dlssg_log("Guides refused: %s %ux%u format %u (want depth R32/R16_FLOAT, motion R16G16_FLOAT at %ux%u)",
                        index == 0 ? "depth" : "motion", descriptions[index].Width, descriptions[index].Height,
                        static_cast<unsigned>(descriptions[index].Format), record.data.width, record.data.height);
                }
                return E_INVALIDARG;
            }
        }
        const std::uint64_t completed = d3d12_fence_->GetCompletedValue();
        if (completed == UINT64_MAX) {
            failed_ = true;
            return DXGI_ERROR_DEVICE_REMOVED;
        }
        const std::size_t list_index = next_list_;
        if (allocator_values_[list_index] > completed) return HRESULT_FROM_WIN32(ERROR_BUSY);

        HRESULT result = S_OK;
        for (std::size_t index = 0; index < sources.size(); ++index) {
            result = ensure_shared(index, descriptions[index]);
            if (FAILED(result)) {
                failed_ = true;
                dlssg_log("Guide transfer disabled: shared guide texture failed 0x%08lx",
                    static_cast<unsigned long>(result));
                return result;
            }
        }
        if (slots_.size() <= history_slot) slots_.resize(history_slot + 1U);
        auto& slot = slots_[history_slot];
        for (std::size_t index = 0; index < slot.size(); ++index) {
            if (slot[index]) {
                const D3D12_RESOURCE_DESC existing = slot[index]->GetDesc();
                if (existing.Width == descriptions[index].Width &&
                    existing.Height == descriptions[index].Height &&
                    existing.Format == descriptions[index].Format) {
                    continue;
                }
            }
            // A frame record still holding the old texture keeps it alive.
            result = create_resource(descriptions[index], D3D12_RESOURCE_FLAG_NONE,
                D3D12_HEAP_FLAG_NONE, &slot[index]);
            if (FAILED(result)) {
                failed_ = true;
                return result;
            }
        }

        auto& allocator = allocators_[list_index];
        if (!allocator) {
            result = d3d12_device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(allocator.GetAddressOf()));
        } else {
            result = allocator->Reset();
        }
        if (FAILED(result)) return result;
        if (!list_) {
            result = d3d12_device_->CreateCommandList(d3d12_queue_->GetDesc().NodeMask,
                D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                IID_PPV_ARGS(list_.GetAddressOf()));
        } else {
            result = list_->Reset(allocator.Get(), nullptr);
        }
        if (FAILED(result)) return result;
        const std::array<D3D12_RESOURCE_BARRIER, 2> before{
            transition(slot[0].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
            transition(slot[1].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)};
        list_->ResourceBarrier(static_cast<UINT>(before.size()), before.data());
        list_->CopyResource(slot[0].Get(), shared_[0].d3d12.Get());
        list_->CopyResource(slot[1].Get(), shared_[1].d3d12.Get());
        const std::array<D3D12_RESOURCE_BARRIER, 2> after{
            transition(slot[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
            transition(slot[1].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON)};
        list_->ResourceBarrier(static_cast<UINT>(after.size()), after.data());
        result = list_->Close();
        if (FAILED(result)) return result;

        // Producer side: wait until the layer's last read of the shared pair
        // is done, copy, signal.
        if (last_d3d12_read_ != 0) {
            result = d3d11_context_->Wait(d3d11_fence_.Get(), last_d3d12_read_);
            if (FAILED(result)) {
                failed_ = true;
                return result;
            }
        }
        for (std::size_t index = 0; index < sources.size(); ++index) {
            d3d11_context_->CopySubresourceRegion(shared_[index].d3d11.Get(), 0, 0, 0, 0,
                sources[index], 0, nullptr);
        }
        const std::uint64_t written = next_value_++;
        const std::uint64_t read = next_value_++;
        result = d3d11_context_->Signal(d3d11_fence_.Get(), written);
        if (FAILED(result)) {
            failed_ = true;
            return result;
        }
        d3d11_context_->Flush();
        // Layer side.
        result = d3d12_queue_->Wait(d3d12_fence_.Get(), written);
        if (FAILED(result)) {
            failed_ = true;
            return result;
        }
        ID3D12CommandList* lists[] = {list_.Get()};
        d3d12_queue_->ExecuteCommandLists(1, lists);
        result = d3d12_queue_->Signal(d3d12_fence_.Get(), read);
        if (FAILED(result)) {
            failed_ = true;
            return result;
        }
        last_d3d12_read_ = read;
        allocator_values_[list_index] = read;
        next_list_ = (list_index + 1U) % kListCount;
        *output = make_dlssg_guide_set(record, slot[0], slot[1], d3d12_fence_, read);
        return *output ? S_OK : E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

std::shared_ptr<const DlssMotionVectorSet> make_dlssg_guide_set(
    const D3D11GuideRecord& record,
    Microsoft::WRL::ComPtr<ID3D12Resource> depth,
    Microsoft::WRL::ComPtr<ID3D12Resource> motion_vectors,
    Microsoft::WRL::ComPtr<ID3D12Fence> fence,
    std::uint64_t fence_value) noexcept {
    try {
        if (!depth || !motion_vectors) return {};
        const OFXR_D3D11GuidePublicationV3& data = record.data;
        auto frame = std::make_shared<DlssMotionVectorFrame>();
        frame->stream = data.swapchain;
        frame->epoch = 1;
        frame->serial = data.serial;
        frame->previous_serial = data.serial != 0 ? data.serial - 1 : 0;
        frame->motion_vectors = std::move(motion_vectors);
        frame->motion_width = data.width;
        frame->motion_height = data.height;
        frame->scale_x = data.motion_scale_x;
        frame->scale_y = data.motion_scale_y;
        frame->jitter_x = data.jitter_x;
        frame->jitter_y = data.jitter_y;
        frame->jittered = data.jitter_x != 0.0F || data.jitter_y != 0.0F;
        frame->resource_state = D3D12_RESOURCE_STATE_COMMON;
        frame->reset = data.reset != 0;
        frame->depth = std::move(depth);
        frame->depth_width = data.width;
        frame->depth_height = data.height;
        frame->depth_resource_state = D3D12_RESOURCE_STATE_COMMON;
        frame->frame_time_delta_ms = data.frame_time_delta_ms;
        frame->camera_near = data.camera_near;
        frame->camera_far = data.camera_far;
        frame->depth_inverted = data.depth_inverted != 0;
        frame->dlssg_camera = true;
        std::copy(std::begin(data.clip_to_prev_clip), std::end(data.clip_to_prev_clip), frame->clip_to_prev_clip.begin());
        std::copy(std::begin(data.prev_clip_to_clip), std::end(data.prev_clip_to_clip), frame->prev_clip_to_clip.begin());
        std::copy(std::begin(data.view_to_clip), std::end(data.view_to_clip), frame->view_to_clip.begin());
        std::copy(std::begin(data.clip_to_view), std::end(data.clip_to_view), frame->clip_to_view.begin());
        std::copy(std::begin(data.camera_position), std::end(data.camera_position), frame->camera_position.begin());
        std::copy(std::begin(data.camera_up), std::end(data.camera_up), frame->camera_up.begin());
        std::copy(std::begin(data.camera_right), std::end(data.camera_right), frame->camera_right.begin());
        std::copy(std::begin(data.camera_forward), std::end(data.camera_forward), frame->camera_forward.begin());
        frame->camera_fov = data.camera_fov;
        frame->camera_aspect = data.camera_aspect;
        frame->guide_fence = std::move(fence);
        frame->guide_fence_value = fence_value;
        auto set = std::make_shared<DlssMotionVectorSet>();
        set->eyes[0] = std::move(frame);
        set->eye_count = 1;
        return set;
    } catch (...) {
        return {};
    }
}

}  // namespace xrfg

extern "C" __declspec(dllexport) std::uint32_t OFXR_GetProviderApiVersionV3() noexcept {
    return xrfg::dlssg_requested() ? OFXR_PROVIDER_API_VERSION_V3 : 0U;
}

// The producer calls this before it initializes its own NGX (DLSS SR), so
// NGX D3D12 is set up first and both of its per-eye SR features are created
// after it (see dlssg_set_session_device). 1 when DLSS-G is ready.
extern "C" __declspec(dllexport) int OFXR_PrepareFrameGenerationV3() noexcept {
    const bool ready = xrfg::dlssg_prepare_session_device();
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        xrfg::dlssg_log("Producer requested NGX D3D12 ahead of its DLSS SR: %s",
            ready ? "ready" : "not available");
    }
    return ready ? 1 : 0;
}

extern "C" __declspec(dllexport) int OFXR_PublishD3D11GuidesV3(
    const OFXR_D3D11GuidePublicationV3* publication) noexcept {
    try {
        if (publication == nullptr) return 0;
        const bool valid =
            publication->struct_size >= sizeof(OFXR_D3D11GuidePublicationV3) &&
            publication->api_version == OFXR_PROVIDER_API_VERSION_V3 &&
            publication->swapchain != 0 && publication->depth != nullptr &&
            publication->motion_vectors != nullptr && publication->width != 0 &&
            publication->height != 0 && publication->camera_far > publication->camera_near &&
            xrfg::finite_values(publication->clip_to_prev_clip, 16) &&
            xrfg::finite_values(publication->prev_clip_to_clip, 16) &&
            xrfg::finite_values(publication->view_to_clip, 16) &&
            xrfg::finite_values(publication->clip_to_view, 16);
        std::scoped_lock lock(xrfg::guide_mutex());
        if (!valid || !xrfg::dlssg_requested()) {
            if (xrfg::g_rejected++ == 0) xrfg::dlssg_log("D3D11 guide publication rejected (first of its kind)");
            return 0;
        }
        xrfg::HeldGuides held{};
        held.record.depth = publication->depth;
        held.record.motion_vectors = publication->motion_vectors;
        held.record.data = *publication;
        held.record.data.depth = nullptr;
        held.record.data.motion_vectors = nullptr;
        xrfg::held_guides()[publication->swapchain] = std::move(held);
        if (xrfg::g_published++ == 0) {
            xrfg::dlssg_log("First D3D11 guides published: swapchain 0x%llx, %ux%u, serial %llu",
                static_cast<unsigned long long>(publication->swapchain), publication->width,
                publication->height, static_cast<unsigned long long>(publication->serial));
        }
        return 1;
    } catch (...) {
        return 0;
    }
}
