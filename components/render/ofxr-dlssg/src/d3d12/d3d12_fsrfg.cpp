// AMD FSR frame generation for the guided generator interface (xrfg/d3d12_frame_generator.hpp), and the choice
// between it and DLSS Frame Generation. Written against AMD's public FSR API headers (FSR SDK 2.4.0); the API's DLLs
// are loaded at run time from VAM_FSR_RUNTIME_PATH and nothing of AMD's is linked. Compiled into the OpenXR layer and
// into the desktop presenter of the d3d11-proxy component, like d3d12_dlssg.cpp: no layer-only dependency here.
#include "xrfg/d3d12_fsrfg.hpp"
#include "xrfg/d3d12_dlssg.hpp"

#include <windows.h>
#include <wrl/client.h>

#include <api/include/ffx_api.h>
#include <api/include/dx12/ffx_api_dx12.h>
#include <framegeneration/include/ffx_framegeneration.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace xrfg {
namespace {

using Microsoft::WRL::ComPtr;

// Consecutive dispatch failures before FSR gives up for the session.
constexpr std::uint32_t kFailureLimit = 8;

// Why FSR frame generation is not available, in words, for running.json. Written under the loader's and the
// factory's own serialisation; a torn read would only garble a report.
std::string g_fsr_why;

std::wstring environment(const wchar_t* name) {
    wchar_t buffer[32768]{};
    const DWORD length = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    return length != 0 && length < std::size(buffer) ? std::wstring(buffer, length) : std::wstring{};
}

// The FSR API: five functions of the loader DLL. The loader finds the frame generation DLL beside itself.
struct FsrApi {
    HMODULE module{};
    PfnFfxCreateContext create{};
    PfnFfxDestroyContext destroy{};
    PfnFfxConfigure configure{};
    PfnFfxQuery query{};
    PfnFfxDispatch dispatch{};
};

// The context is not guaranteed to be thread safe (AMD's documentation); the two eyes' generators and the
// availability query share this.
std::mutex& fsr_mutex() {
    static std::mutex mutex;
    return mutex;
}

// Loaded once per process; null when the folder is not named or the loader is not there.
const FsrApi* fsr_api() noexcept {
    static const FsrApi* const api = []() noexcept -> const FsrApi* {
        try {
            const std::wstring folder = environment(L"VAM_FSR_RUNTIME_PATH");
            if (folder.empty()) {
                g_fsr_why = "VAM_FSR_RUNTIME_PATH is not set";
                dlssg_log("FSR frame generation not offered: VAM_FSR_RUNTIME_PATH is not set");
                return nullptr;
            }
            const std::wstring path = folder + L"\\amd_fidelityfx_loader_dx12.dll";
            // Its own folder first: the loader takes the effect DLLs from there.
            HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (module == nullptr) {
                g_fsr_why = "amd_fidelityfx_loader_dx12.dll could not be loaded from VAM_FSR_RUNTIME_PATH";
                dlssg_log("FSR frame generation not offered: amd_fidelityfx_loader_dx12.dll could not be loaded (error %lu)", GetLastError());
                return nullptr;
            }
            static FsrApi loaded;
            loaded.module = module;
            loaded.create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(module, "ffxCreateContext"));
            loaded.destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(module, "ffxDestroyContext"));
            loaded.configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(module, "ffxConfigure"));
            loaded.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(module, "ffxQuery"));
            loaded.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(module, "ffxDispatch"));
            if (!loaded.create || !loaded.destroy || !loaded.configure || !loaded.query || !loaded.dispatch) {
                g_fsr_why = "the loader DLL lacks an FSR API export";
                dlssg_log("FSR frame generation not offered: the loader DLL lacks an FSR API export");
                return nullptr;
            }
            return &loaded;
        } catch (...) {
            return nullptr;
        }
    }();
    return api;
}

void fsr_message(uint32_t type, const wchar_t* message) {
    char text[512]{};
    WideCharToMultiByte(CP_UTF8, 0, message != nullptr ? message : L"", -1, text, sizeof(text) - 1, nullptr, nullptr);
    dlssg_log("FSR-FG %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", text);
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

// The typed 8-bit format FSR interpolates an image family as (the data is gamma-encoded whatever the view says).
constexpr DXGI_FORMAT unorm_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

// What was found when a generator was last made, for the menu (frame_generators_report).
std::atomic<std::uint32_t> g_report{0};

}  // namespace

bool fsrfg_available(ID3D12Device* device) noexcept {
    // The answer for the device asked about first is kept: one adapter per process.
    static std::atomic<int> known{-1};
    const int seen = known.load();
    if (seen >= 0) return seen != 0;
    const FsrApi* api = fsr_api();
    if (api == nullptr || device == nullptr) {
        if (api == nullptr) known = 0;
        return false;
    }
    try {
        std::scoped_lock lock(fsr_mutex());
        std::uint64_t count = 0;
        ffxQueryDescGetVersions versions{};
        versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        versions.device = device;
        versions.outputCount = &count;
        const ffxReturnCode_t result = api->query(nullptr, &versions.header);
        if (result != FFX_API_RETURN_OK || count == 0) {
            g_fsr_why = "the FSR API lists no frame generation (is amd_fidelityfx_framegeneration_dx12.dll beside the loader?)";
            dlssg_log("FSR frame generation cannot run here: the FSR API lists no frame generation "
                "(code %u; is amd_fidelityfx_framegeneration_dx12.dll beside the loader?)", result);
            known = 0;
            return false;
        }
        const char* names[8]{};
        std::uint64_t ids[8]{};
        count = std::min<std::uint64_t>(count, 8);
        versions.versionIds = ids;
        versions.versionNames = names;
        std::string text;
        if (api->query(nullptr, &versions.header) == FFX_API_RETURN_OK) {
            for (std::uint64_t i = 0; i < count; ++i) {
                if (names[i] != nullptr) text += (text.empty() ? "" : ", ") + std::string(names[i]);
            }
        }
        dlssg_log("FSR frame generation can run here; versions the API offers on this device: %s", text.c_str());
        known = 1;
        return true;
    } catch (...) {
        known = 0;
        return false;
    }
}

struct D3D12FsrFrameGenerator::Impl {
    const FsrApi* api{};
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_RESOURCE_DESC image{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    // FSR's own images, in the typed format it interpolates: B's copy and the generated frame. The caller's images
    // may be typeless, sRGB-typed or not writable as a UAV; a copy of an eye image costs about 0.03 ms.
    ComPtr<ID3D12Resource> input;    // NON_PIXEL_SHADER_RESOURCE at rest
    ComPtr<ID3D12Resource> output;   // UNORDERED_ACCESS at rest
    ffxContext context{};
    // What the context was created from: the API keeps the pointers until the context is destroyed.
    ffxCreateContextDescFrameGeneration create{};
    ffxCreateContextDescFrameGenerationVersion version{};
    ffxCreateBackendDX12Desc backend{};
    UINT render_width{}, render_height{};
    bool depth_inverted{};
    std::string provider;
    std::uint64_t last_serial{};
    // FSR's own frame counter: it must rise by exactly one per dispatch, or FSR resets by itself.
    std::uint64_t frame_id{};
    std::uint64_t evaluations{}, resets{};
    std::uint32_t id{};
    std::uint32_t failures{};
    bool failed{};
    bool ready{};
    // GPU time of what record() puts on the list for one pair (B's copy, FSR's passes, the generated frame's copy):
    // two timestamps per pair, resolved into a mapped buffer and read when a later pair is recorded.
    static constexpr std::uint32_t kTimingSlots = 8;
    ComPtr<ID3D12QueryHeap> timing_heap;
    ComPtr<ID3D12Resource> timing_readback;
    volatile std::uint64_t* timing_mapped{};
    double timing_ms_per_tick{};
    std::array<bool, kTimingSlots> timing_used{};
    std::uint32_t next_timing{};
    double timing_sum{}, timing_max{};
    std::uint64_t timing_count{};

    void create_timing() noexcept {
        UINT64 frequency = 0;
        if (FAILED(queue->GetTimestampFrequency(&frequency)) || frequency == 0) return;
        D3D12_QUERY_HEAP_DESC heap_description{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * kTimingSlots, queue->GetDesc().NodeMask};
        if (FAILED(device->CreateQueryHeap(&heap_description, IID_PPV_ARGS(timing_heap.ReleaseAndGetAddressOf())))) return;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = 2 * kTimingSlots * sizeof(std::uint64_t);
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        void* mapped = nullptr;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(timing_readback.ReleaseAndGetAddressOf()))) ||
            FAILED(timing_readback->Map(0, nullptr, &mapped)) || mapped == nullptr) {
            timing_heap.Reset();
            timing_readback.Reset();
            return;
        }
        timing_mapped = static_cast<volatile std::uint64_t*>(mapped);
        timing_ms_per_tick = 1000.0 / static_cast<double>(frequency);
    }

    void poll_timing() noexcept {
        if (timing_mapped == nullptr) return;
        for (std::uint32_t slot = 0; slot < kTimingSlots; ++slot) {
            if (!timing_used[slot]) continue;
            const std::uint64_t begin = timing_mapped[2 * slot];
            const std::uint64_t end = timing_mapped[2 * slot + 1];
            if (begin == 0 || end == 0) continue;
            timing_used[slot] = false;
            if (end < begin) continue;
            const double ms = static_cast<double>(end - begin) * timing_ms_per_tick;
            timing_sum += ms;
            timing_max = std::max(timing_max, ms);
            ++timing_count;
        }
    }

    ~Impl() {
        // The owner drains its queue before its members are destroyed.
        if (context != nullptr && api != nullptr) {
            std::scoped_lock lock(fsr_mutex());
            api->destroy(&context, nullptr);
        }
        if (timing_readback && timing_mapped != nullptr) timing_readback->Unmap(0, nullptr);
    }

    // See the probe in record().
    ComPtr<ID3D12Resource> second;
    bool probe_second() noexcept {
        static const bool asked = [] {
            wchar_t value[8]{};
            return GetEnvironmentVariableW(L"VAMKIT_FSR_FRAMES", value, 8) == 1 && value[0] == L'2';
        }();
        if (!asked) return false;
        if (!second) {
            if (FAILED(create_texture(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &second))) {
                return false;
            }
            dlssg_log("FSR-FG[%u] probe (VAMKIT_FSR_FRAMES=2): two generated frames are asked for per pair and the "
                "second output is the one shown; it holds B before each dispatch", id);
        }
        return true;
    }

    HRESULT create_texture(D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>* texture) noexcept {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = image.Width;
        description.Height = image.Height;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        description.Flags = flags;
        return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr,
            IID_PPV_ARGS(texture->ReleaseAndGetAddressOf()));
    }

    HRESULT initialize(ID3D12Device* input_device, ID3D12CommandQueue* input_queue,
        const D3D12_RESOURCE_DESC& description) noexcept {
        if (input_device == nullptr || input_queue == nullptr ||
            description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || description.DepthOrArraySize != 1 ||
            description.MipLevels != 1 || description.SampleDesc.Count != 1) {
            return E_INVALIDARG;
        }
        format = unorm_format(description.Format);
        if (format == DXGI_FORMAT_UNKNOWN) return E_INVALIDARG;
        if (!fsrfg_available(input_device)) return E_FAIL;
        api = fsr_api();
        device = input_device;
        queue = input_queue;
        image = description;
        HRESULT result = create_texture(D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &input);
        if (FAILED(result)) return result;
        result = create_texture(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &output);
        if (FAILED(result)) return result;
        create_timing();
        ready = true;
        static std::atomic<std::uint32_t> next_id{0};
        id = ++next_id;
        dlssg_log("FSR-FG[%u] generator ready for %llux%u images, format %u (interpolates as %u); one generated frame per pair",
            id, static_cast<unsigned long long>(image.Width), image.Height, static_cast<unsigned>(image.Format),
            static_cast<unsigned>(format));
        return S_OK;
    }

    bool can_generate(const DlssMotionVectorFrame& guides, std::uint32_t count) const noexcept {
        return ready && !failed && guides.dlssg_camera && guides.depth && guides.motion_vectors &&
            guides.depth_width != 0 && guides.depth_height != 0 && guides.motion_width == guides.depth_width &&
            guides.motion_height == guides.depth_height && count == 1;
    }

    void wait_for_queue() noexcept {
        ComPtr<ID3D12Fence> fence;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()))) ||
            FAILED(queue->Signal(fence.Get(), 1))) {
            return;
        }
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr) return;
        if (fence->GetCompletedValue() < 1 && SUCCEEDED(fence->SetEventOnCompletion(1, event))) {
            WaitForSingleObject(event, 2000);
        }
        CloseHandle(event);
    }

    // The FSR context for guides of this size. The API chooses the implementation for the device.
    HRESULT ensure_context(UINT width, UINT height, bool inverted) noexcept {
        if (context != nullptr && render_width == width && render_height == height && depth_inverted == inverted) return S_OK;
        if (context != nullptr) {
            // Everything that may still use the old context has to be done.
            wait_for_queue();
            api->destroy(&context, nullptr);
            context = nullptr;
        }
        backend = {};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = device.Get();
        version = {};
        version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
        version.header.pNext = &backend.header;
        version.version = FFX_FRAMEGENERATION_VERSION;
        create = {};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        create.header.pNext = &version.header;
        // Render-size motion without the jitter; the 8-bit images are not HDR.
        create.flags = inverted ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0;
        create.displaySize.width = static_cast<uint32_t>(image.Width);
        create.displaySize.height = image.Height;
        create.maxRenderSize.width = width;
        create.maxRenderSize.height = height;
        create.backBufferFormat = ffxApiGetSurfaceFormatDX12(format);
        const ffxReturnCode_t result = api->create(&context, &create.header, nullptr);
        if (result != FFX_API_RETURN_OK || context == nullptr) {
            context = nullptr;
            dlssg_log("FSR-FG[%u]: ffxCreateContext (frame generation) failed, code %u (output %llux%u, render %ux%u)",
                id, result, static_cast<unsigned long long>(image.Width), image.Height, width, height);
            return E_FAIL;
        }
        ffxConfigureDescGlobalDebug debug{};
        debug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
        debug.effectId = FFX_API_EFFECT_ID_FRAMEGENERATION;
        debug.fpMessage = &fsr_message;
        debug.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS;
        api->configure(&context, &debug.header);
        ffxQueryGetProviderVersion running{};
        running.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        provider = api->query(&context, &running.header) == FFX_API_RETURN_OK && running.versionName != nullptr
            ? running.versionName : "unknown";
        render_width = width;
        render_height = height;
        depth_inverted = inverted;
        last_serial = 0;
        dlssg_log("FSR-FG[%u] context created: output %llux%u, render %ux%u, version %s, dispatched by hand (no FSR swap chain)",
            id, static_cast<unsigned long long>(image.Width), image.Height, width, height, provider.c_str());
        return S_OK;
    }

    void copy_fallback(ID3D12GraphicsCommandList* list, ID3D12Resource* backbuffer, const GuidedRecordInput& in) noexcept {
        const std::array<D3D12_RESOURCE_BARRIER, 2> before{
            transition(backbuffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(in.destinations[0], in.destination_state, D3D12_RESOURCE_STATE_COPY_DEST)};
        list->ResourceBarrier(static_cast<UINT>(before.size()), before.data());
        list->CopyResource(in.destinations[0], backbuffer);
        const std::array<D3D12_RESOURCE_BARRIER, 2> after{
            transition(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
            transition(in.destinations[0], D3D12_RESOURCE_STATE_COPY_DEST, in.destination_state)};
        list->ResourceBarrier(static_cast<UINT>(after.size()), after.data());
    }

    HRESULT record(ID3D12GraphicsCommandList* list, const GuidedRecordInput& in, std::uint32_t* generated) noexcept {
        if (generated == nullptr) return E_POINTER;
        *generated = 0;
        if (list == nullptr || in.backbuffer == nullptr || in.guides == nullptr || in.outputs != 1 ||
            in.destinations[0] == nullptr || !can_generate(*in.guides, in.outputs)) {
            return E_INVALIDARG;
        }
        const DlssMotionVectorFrame& g = *in.guides;
        std::scoped_lock lock(fsr_mutex());
        // Nothing recorded yet: a failure here leaves the caller free to take this pair another way.
        HRESULT result = ensure_context(g.depth_width, g.depth_height, g.depth_inverted);
        if (FAILED(result)) {
            failed = true;
            return result;
        }
        ID3D12Resource* const depth = g.depth.Get();
        ID3D12Resource* const motion = g.motion_vectors.Get();
        const bool reset = g.reset || last_serial == 0 || g.serial != last_serial + 1;
        ++frame_id;
        poll_timing();
        const std::uint32_t timing_slot = next_timing;
        const bool timed = timing_heap && !timing_used[timing_slot];
        if (timed) list->EndQuery(timing_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * timing_slot);

        // B into FSR's own typed image; the guides readable.
        {
            const std::array<D3D12_RESOURCE_BARRIER, 2> before{
                transition(in.backbuffer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
                transition(input.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
            list->ResourceBarrier(static_cast<UINT>(before.size()), before.data());
            list->CopyResource(input.Get(), in.backbuffer);
            const std::array<D3D12_RESOURCE_BARRIER, 4> after{
                transition(in.backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
                transition(input.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                transition(depth, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                transition(motion, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
            list->ResourceBarrier(static_cast<UINT>(after.size()), after.data());
        }

        const FfxApiRect2D whole{0, 0, static_cast<int32_t>(image.Width), static_cast<int32_t>(image.Height)};
        // Per frame, in AMD's order: configure, prepare, generate. No swap chain and no callbacks: the generation
        // below is dispatched here, and the flag tells the context not to look for an FSR swap chain.
        ffxConfigureDescFrameGeneration configure{};
        configure.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        configure.frameGenerationEnabled = true;
        configure.allowAsyncWorkloads = false;
        configure.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
        configure.onlyPresentGenerated = false;
        configure.generationRect = whole;
        configure.frameID = frame_id;
        ffxReturnCode_t status = api->configure(&context, &configure.header);

        if (status == FFX_API_RETURN_OK) {
            ffxDispatchDescFrameGenerationPrepareV2 prepare{};
            prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
            prepare.frameID = frame_id;
            prepare.flags = 0;
            prepare.commandList = list;
            prepare.renderSize.width = g.depth_width;
            prepare.renderSize.height = g.depth_height;
            prepare.jitterOffset.x = g.jittered ? g.jitter_x : 0.0F;
            prepare.jitterOffset.y = g.jittered ? g.jitter_y : 0.0F;
            // The producer's scale takes its motion to pixels of the guide size, which is what FSR asks for.
            prepare.motionVectorScale.x = g.scale_x;
            prepare.motionVectorScale.y = g.scale_y;
            prepare.frameTimeDelta = g.frame_time_delta_ms;
            prepare.reset = reset;
            prepare.cameraNear = g.camera_near;
            prepare.cameraFar = g.camera_far;
            prepare.cameraFovAngleVertical = g.camera_fov > 0.0F ? g.camera_fov : 1.0F;
            prepare.viewSpaceToMetersFactor = 1.0F;
            prepare.depth = ffxApiGetResourceDX12(depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
            prepare.motionVectors = ffxApiGetResourceDX12(motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
            std::memcpy(prepare.cameraPosition, g.camera_position.data(), sizeof(prepare.cameraPosition));
            std::memcpy(prepare.cameraUp, g.camera_up.data(), sizeof(prepare.cameraUp));
            std::memcpy(prepare.cameraRight, g.camera_right.data(), sizeof(prepare.cameraRight));
            std::memcpy(prepare.cameraForward, g.camera_forward.data(), sizeof(prepare.cameraForward));
            status = api->dispatch(&context, &prepare.header);
        }
        if (status == FFX_API_RETURN_OK) {
            ffxDispatchDescFrameGeneration generate{};
            generate.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
            generate.commandList = list;
            generate.presentColor = ffxApiGetResourceDX12(input.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            generate.outputs[0] = ffxApiGetResourceDX12(output.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
            generate.numGeneratedFrames = 1;
            // A probe, not a mode (VAMKIT_FSR_FRAMES=2 in the environment): does this provider write more than one
            // frame per pair? The dispatch has room for four outputs and no document says how many it fills. The
            // shown image becomes the SECOND output, which holds a copy of B before the dispatch: a generated frame
            // that equals B exactly afterwards means the provider wrote only the first.
            if (probe_second()) {
                const std::array<D3D12_RESOURCE_BARRIER, 2> before{
                    transition(input.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
                    transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST)};
                list->ResourceBarrier(static_cast<UINT>(before.size()), before.data());
                list->CopyResource(output.Get(), input.Get());
                const std::array<D3D12_RESOURCE_BARRIER, 2> after{
                    transition(input.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                    transition(output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
                list->ResourceBarrier(static_cast<UINT>(after.size()), after.data());
                generate.outputs[0] = ffxApiGetResourceDX12(second.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
                generate.outputs[1] = ffxApiGetResourceDX12(output.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
                generate.numGeneratedFrames = 2;
            }
            generate.reset = reset;
            generate.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
            generate.minMaxLuminance[0] = 0.0F;
            generate.minMaxLuminance[1] = 1000.0F;
            generate.generationRect = whole;
            generate.frameID = frame_id;
            status = api->dispatch(&context, &generate.header);
        }
        {
            const std::array<D3D12_RESOURCE_BARRIER, 2> guides_back{
                transition(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                transition(motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON)};
            list->ResourceBarrier(static_cast<UINT>(guides_back.size()), guides_back.data());
        }
        if (status != FFX_API_RETURN_OK) {
            // The list stays valid: B goes into the synthetic and FSR starts over on the next pair.
            dlssg_log("FSR-FG[%u]: dispatch failed, code %u (frame %llu)", id, status, static_cast<unsigned long long>(frame_id));
            copy_fallback(list, in.backbuffer, in);
            last_serial = 0;
            if (++failures >= kFailureLimit) {
                failed = true;
                dlssg_log("FSR-FG[%u] disabled for this session after %u failed dispatches", id, failures);
            }
            return S_OK;
        }
        failures = 0;
        if (reset) {
            // No frame before B to interpolate from: the synthetic is B, shown with B's pose.
            copy_fallback(list, in.backbuffer, in);
        } else {
            const std::array<D3D12_RESOURCE_BARRIER, 2> before{
                transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
                transition(in.destinations[0], in.destination_state, D3D12_RESOURCE_STATE_COPY_DEST)};
            list->ResourceBarrier(static_cast<UINT>(before.size()), before.data());
            list->CopyResource(in.destinations[0], output.Get());
            const std::array<D3D12_RESOURCE_BARRIER, 2> after{
                transition(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                transition(in.destinations[0], D3D12_RESOURCE_STATE_COPY_DEST, in.destination_state)};
            list->ResourceBarrier(static_cast<UINT>(after.size()), after.data());
        }
        if (timed) {
            list->EndQuery(timing_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * timing_slot + 1);
            timing_mapped[2 * timing_slot] = 0;
            timing_mapped[2 * timing_slot + 1] = 0;
            list->ResolveQueryData(timing_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * timing_slot, 2,
                timing_readback.Get(), 2 * timing_slot * sizeof(std::uint64_t));
            timing_used[timing_slot] = true;
            next_timing = (next_timing + 1U) % kTimingSlots;
        }
        last_serial = g.serial;
        ++evaluations;
        if (reset) ++resets;
        if (evaluations == 1) {
            dlssg_log("FSR-FG[%u] first pair recorded: render %ux%u, serial %llu, version %s", id, g.depth_width,
                g.depth_height, static_cast<unsigned long long>(g.serial), provider.c_str());
        } else if (evaluations % 1800 == 0) {
            dlssg_log("FSR-FG[%u]: %llu pairs, %llu resets, serial %llu; GPU per pair (copies included) avg %.2f ms, max %.2f ms over %llu timed",
                id, static_cast<unsigned long long>(evaluations), static_cast<unsigned long long>(resets),
                static_cast<unsigned long long>(g.serial), timing_count ? timing_sum / static_cast<double>(timing_count) : 0.0,
                timing_max, static_cast<unsigned long long>(timing_count));
            timing_sum = 0.0;
            timing_max = 0.0;
            timing_count = 0;
        }
        *generated = reset ? 0 : 1;
        return S_OK;
    }
};

D3D12FsrFrameGenerator::D3D12FsrFrameGenerator() noexcept = default;
D3D12FsrFrameGenerator::~D3D12FsrFrameGenerator() = default;

HRESULT D3D12FsrFrameGenerator::initialize(ID3D12Device* device, ID3D12CommandQueue* queue,
    const D3D12_RESOURCE_DESC& image) noexcept {
    try {
        auto impl = std::make_unique<Impl>();
        const HRESULT result = impl->initialize(device, queue, image);
        if (FAILED(result)) return result;
        impl_ = std::move(impl);
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

bool D3D12FsrFrameGenerator::can_generate(const DlssMotionVectorFrame& guides, std::uint32_t outputs) const noexcept {
    return impl_ != nullptr && impl_->can_generate(guides, outputs);
}

HRESULT D3D12FsrFrameGenerator::record(ID3D12GraphicsCommandList* list, const GuidedRecordInput& input,
    std::uint32_t* generated) noexcept {
    if (generated != nullptr) *generated = 0;
    if (impl_ == nullptr) return E_UNEXPECTED;
    try {
        return impl_->record(list, input, generated);
    } catch (...) {
        return E_FAIL;
    }
}

void D3D12FsrFrameGenerator::invalidate_history() noexcept {
    if (impl_ != nullptr) impl_->last_serial = 0;
}

bool D3D12FsrFrameGenerator::usable() const noexcept {
    return impl_ != nullptr && impl_->ready && !impl_->failed;
}

// ---- the choice between the two generators ----

FrameGenModel frame_gen_model_wanted() noexcept {
    static std::mutex mutex;
    static ULONGLONG next{}, seen_time{}, seen_size{};
    static bool read{};
    static FrameGenModel wanted{FrameGenModel::dlss};
    try {
        static const std::wstring profile = environment(L"VAM_PERFORMANCE_CONFIG");
        std::scoped_lock lock(mutex);
        const ULONGLONG now = GetTickCount64();
        if (read && now < next) return wanted;
        next = now + 250;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        const bool exists = !profile.empty() && GetFileAttributesExW(profile.c_str(), GetFileExInfoStandard, &data);
        const ULONGLONG time = exists
            ? (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime : 0;
        const ULONGLONG size = exists ? (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow : 0;
        if (read && time == seen_time && size == seen_size) return wanted;
        seen_time = time;
        seen_size = size;
        read = true;
        wchar_t model[16]{};
        if (exists) GetPrivateProfileStringW(L"Performance", L"FrameGenModel", L"DLSS", model, 16, profile.c_str());
        wanted = _wcsicmp(model, L"FSR") == 0 ? FrameGenModel::fsr : FrameGenModel::dlss;
        return wanted;
    } catch (...) {
        return wanted;
    }
}

// What was asked for and what generates, for tools: <OFXR_DLSSG_LOG>\\running.json, written whenever a generator is
// made (the layer makes one per eye; they agree). Read this rather than the log's wording.
void write_running(FrameGenModel wanted, const char* running, std::uint32_t frames_most, bool dlss, bool fsr) noexcept {
    try {
        const std::wstring directory = environment(L"OFXR_DLSSG_LOG");
        if (directory.empty()) return;
        const auto quote = [](const std::string& text) {
            std::string out = "\"";
            for (char c : text) out += (c == '"' || c == '\\') ? '\'' : (static_cast<unsigned char>(c) < 0x20 ? ' ' : c);
            return out + "\"";
        };
        const std::string asked = wanted == FrameGenModel::dlss ? "DLSS" : "FSR";
        const std::string runs = std::strcmp(running, "DLSS-G") == 0 ? "DLSS" : running;
        const std::string dlss_why = dlss ? std::string() : !dlssg_requested() ? std::string("guided generation is not asked for (OFXR_DLSSG)") : dlssg_why_not();
        const std::string fsr_why = fsr ? std::string() : g_fsr_why;
        const std::string reason = runs == asked ? std::string() : wanted == FrameGenModel::dlss ? dlss_why : fsr_why;
        const std::string text = "{\n \"frame_generation\": {\"wanted\": " + quote(asked) + ", \"running\": " + quote(runs) +
            ", \"reason\": " + quote(reason) + ", \"frames_most\": " + std::to_string(frames_most) + "},\n \"can_run\": {\"DLSS\": " +
            (dlss ? "true" : "false") + ", \"FSR\": " + (fsr ? "true" : "false") + "},\n \"why_not\": {\"DLSS\": " + quote(dlss_why) +
            ", \"FSR\": " + quote(fsr_why) + "}\n}\n";
        const std::wstring path = directory + L"\\running.json";
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        CloseHandle(file);
    } catch (...) {
    }
}

std::unique_ptr<D3D12GuidedFrameGenerator> make_guided_frame_generator(ID3D12Device* device, ID3D12CommandQueue* queue,
    const D3D12_RESOURCE_DESC& image, FrameGenModel* made) noexcept {
    try {
        const FrameGenModel wanted = frame_gen_model_wanted();
        std::unique_ptr<D3D12GuidedFrameGenerator> generator;
        FrameGenModel model = wanted;
        // The chosen one first, then the other: a card without DLSS Frame Generation gets FSR, and the reverse.
        for (int attempt = 0; attempt < 2 && !generator; ++attempt) {
            model = attempt == 0 ? wanted : wanted == FrameGenModel::fsr ? FrameGenModel::dlss : FrameGenModel::fsr;
            HRESULT result = E_FAIL;
            if (model == FrameGenModel::dlss) {
                if (dlssg_requested()) {
                    auto dlss = std::make_unique<D3D12DlssgFrameGenerator>();
                    result = dlss->initialize(device, queue, image);
                    if (SUCCEEDED(result)) generator = std::move(dlss);
                }
            } else {
                auto fsr = std::make_unique<D3D12FsrFrameGenerator>();
                result = fsr->initialize(device, queue, image);
                if (SUCCEEDED(result)) generator = std::move(fsr);
            }
            if (!generator) {
                dlssg_log("%s frame generation cannot be made here (0x%08lx)%s", model == FrameGenModel::dlss ? "DLSS" : "FSR",
                    static_cast<unsigned long>(result), attempt == 0 ? "; trying the other generator" : "");
            }
        }
        // For the menu: the one made, and whether the other could run too (DLSS-G by NGX, FSR by its files).
        std::uint32_t found = 0x100;
        if (generator) {
            const bool dlss = model == FrameGenModel::dlss ||
                (wanted == FrameGenModel::fsr && dlssg_requested() && dlssg_available(device));
            const bool fsr = model == FrameGenModel::fsr || fsrfg_available(device);
            found |= (dlss ? 1U : 0U) | (fsr ? 2U : 0U) | (model == FrameGenModel::fsr ? 0x10U : 0U);
        }
        g_report = found;
        if (made != nullptr) *made = model;
        write_running(wanted, generator ? generator->name() : "none", generator ? generator->max_outputs() + 1U : 1U,
            (found & 1U) != 0, (found & 2U) != 0);
        if (generator) {
            dlssg_log("Frame generation model: %s%s", generator->name(),
                model == wanted ? "" : " (the one chosen in [Performance] FrameGenModel cannot run here)");
        }
        return generator;
    } catch (...) {
        return nullptr;
    }
}

std::uint32_t guided_frames_most() noexcept {
    const std::uint32_t report = frame_generators_report();
    const bool dlss = (report & 1U) != 0, fsr = (report & 2U) != 0;
    const bool fsr_runs = frame_gen_model_wanted() == FrameGenModel::fsr ? fsr : fsr && !dlss;
    return fsr_runs ? 2U : 4U;
}

std::uint32_t frame_generators_report() noexcept {
    const std::uint32_t made = g_report.load();
    if ((made & 0x100) != 0) return made;
    // Before a generator exists: what the environment and the files promise.
    std::uint32_t bits = 0;
    if (dlssg_requested() && dlssg_state() != 0) bits |= 1;
    if (fsr_api() != nullptr) bits |= 2;
    return bits;
}

}  // namespace xrfg

// For VaM's in-game menu: the frame generators of this process (xrfg::frame_generators_report). Exported by the
// OpenXR layer and by the desktop presenter's DLL, which both compile this file.
extern "C" __declspec(dllexport) int OFXR_GetFrameGeneratorsV1() noexcept {
    return static_cast<int>(xrfg::frame_generators_report());
}
