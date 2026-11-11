// GPU smoke test for the DLSS Frame Generation path, without OpenXR or a game:
// D3D11 guides -> D3D11GuideTransfer -> D3D12DlssgFrameGenerator, on a moving
// white bar. The stereo case runs two instances created and submitted in eye
// order, as OFXR does for VR, and checks both outputs.
// Usage: xrfg_dlssg_smoke <dir with nvngx_dlssg.dll> <log dir> [bench | bench-lite | pan | pan-lite [variant]
//        | replay | replay-lite <capture burst dir> [options]]
// bench: GPU time of the two eyes' DLSS-G work per frame pair at VaM's Quest 3
// sizes (output 3072x3264, render 2180x2316), 2X and 3X; bench-lite the same
// with OFXR_DLSSG_LITE=1 (NVIDIA's lighter mode, Preset C). pan: where the
// generated frame lands during a head turn (see run_pan).
#include "xrfg/d3d11_guides.hpp"
#include "xrfg/d3d12_dlssg.hpp"

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kOutputWidth = 1024;
constexpr UINT kOutputHeight = 512;
constexpr UINT kRenderWidth = kOutputWidth / 2;
constexpr UINT kRenderHeight = kOutputHeight / 2;
constexpr int kBarStart = 160;  // output pixels, frame 0
constexpr int kBarStep = 64;    // output pixels per real frame
constexpr int kBarWidth = 24;
constexpr int kFrames = 8;
constexpr int kEyeOffset = 200; // output pixels between the two eyes' bars
// Benchmark: per-eye sizes of the VaM run on Quest 3 (VDXR 3072x3264, launcher
// render scale 71), motion of kBenchStep output pixels per real frame.
constexpr UINT kBenchOutputWidth = 3072;
constexpr UINT kBenchOutputHeight = 3264;
constexpr UINT kBenchRenderWidth = 2180;
constexpr UINT kBenchRenderHeight = 2316;
constexpr int kBenchStep = 16;
constexpr int kBenchWarmup = 20;
constexpr int kBenchFrames = 120;

struct Gpu {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    UINT64 value{};
    HANDLE event{};
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;

    HRESULT begin() {
        HRESULT hr = allocator->Reset();
        return FAILED(hr) ? hr : list->Reset(allocator.Get(), nullptr);
    }
    HRESULT submit_and_wait() {
        HRESULT hr = list->Close();
        if (FAILED(hr)) return hr;
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        return wait_queue();
    }
    HRESULT wait_queue() {
        HRESULT hr = queue->Signal(fence.Get(), ++value);
        if (FAILED(hr)) return hr;
        if (fence->GetCompletedValue() < value) {
            fence->SetEventOnCompletion(value, event);
            if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        }
        return device->GetDeviceRemovedReason();
    }
};

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    return x;
}

ComPtr<ID3D12Resource> texture(ID3D12Device* device, UINT w, UINT h, DXGI_FORMAT format) {
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> r;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(r.GetAddressOf()));
    return r;
}

// v13 in-place check: XRFG_SMOKE_BACKBUFFER / XRFG_SMOKE_DEST = typeless |
// unorm make B / the destinations R8G8B8A8_TYPELESS or _UNORM (destinations
// with UAV and render-target flags, as OFXR's staging), so the generator
// reads / writes them in place; unset = sRGB, through its own copies.
ComPtr<ID3D12Resource> smoke_texture(ID3D12Device* device, UINT w, UINT h, const char* variable, bool destination) {
    char value[32]{};
    const DWORD n = GetEnvironmentVariableA(variable, value, sizeof(value));
    const std::string kind = n > 0 && n < sizeof(value) ? std::string(value, n) : std::string{};
    if (kind != "typeless" && kind != "unorm") return texture(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = kind == "typeless" ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    if (destination) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> r;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(r.GetAddressOf()));
    return r;
}

ComPtr<ID3D12Resource> buffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap{type};
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
        type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(r.GetAddressOf()));
    return r;
}

// Uploads tightly packed rows of `bytes_per_pixel` into a COMMON texture.
HRESULT upload(Gpu& gpu, ID3D12Resource* target, const void* pixels, UINT bytes_per_pixel) {
    const D3D12_RESOURCE_DESC d = target->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 total = 0;
    gpu.device->GetCopyableFootprints(&d, 0, 1, 0, &layout, nullptr, nullptr, &total);
    auto staging = buffer(gpu.device.Get(), total, D3D12_HEAP_TYPE_UPLOAD);
    std::uint8_t* mapped = nullptr;
    staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    for (UINT y = 0; y < d.Height; ++y) {
        std::memcpy(mapped + layout.Offset + y * layout.Footprint.RowPitch,
            static_cast<const std::uint8_t*>(pixels) + static_cast<size_t>(y) * d.Width * bytes_per_pixel,
            static_cast<size_t>(d.Width) * bytes_per_pixel);
    }
    staging->Unmap(0, nullptr);
    HRESULT hr = gpu.begin();
    if (FAILED(hr)) return hr;
    auto b = transition(target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    gpu.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{target, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src{staging.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint = layout;
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b = transition(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    gpu.list->ResourceBarrier(1, &b);
    return gpu.submit_and_wait();
}

// Reads one row of a COMMON texture back as raw bytes.
HRESULT read_row(Gpu& gpu, ID3D12Resource* source, UINT row, UINT bytes_per_pixel, std::vector<std::uint8_t>* out) {
    const D3D12_RESOURCE_DESC d = source->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 total = 0;
    gpu.device->GetCopyableFootprints(&d, 0, 1, 0, &layout, nullptr, nullptr, &total);
    auto readback = buffer(gpu.device.Get(), total, D3D12_HEAP_TYPE_READBACK);
    HRESULT hr = gpu.begin();
    if (FAILED(hr)) return hr;
    auto b = transition(source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = layout;
    D3D12_TEXTURE_COPY_LOCATION src{source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b = transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    gpu.list->ResourceBarrier(1, &b);
    hr = gpu.submit_and_wait();
    if (FAILED(hr)) return hr;
    std::uint8_t* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
    readback->Map(0, &range, reinterpret_cast<void**>(&mapped));
    out->assign(mapped + layout.Offset + row * layout.Footprint.RowPitch,
        mapped + layout.Offset + row * layout.Footprint.RowPitch + static_cast<size_t>(d.Width) * bytes_per_pixel);
    readback->Unmap(0, nullptr);
    return S_OK;
}

std::vector<std::uint32_t> bar_image(int frame, int offset) {
    std::vector<std::uint32_t> pixels(static_cast<size_t>(kOutputWidth) * kOutputHeight, 0xFF404040U);
    const int left = kBarStart + offset + frame * kBarStep;
    for (UINT y = kOutputHeight / 4; y < kOutputHeight * 3 / 4; ++y) {
        for (int x = left; x < left + kBarWidth && x < static_cast<int>(kOutputWidth); ++x) {
            pixels[static_cast<size_t>(y) * kOutputWidth + x] = 0xFFFFFFFFU;
        }
    }
    return pixels;
}

// Centre of the bright pixels in an RGBA8 row, or -1.
double bar_centre(const std::vector<std::uint8_t>& row, double* coverage) {
    double sum = 0.0, weight = 0.0;
    for (size_t x = 0; x < row.size() / 4; ++x) {
        const double v = (row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2]) / 3.0;
        const double w = v > 0x40 + 16 ? (v - 0x40) / (255.0 - 0x40) : 0.0;
        sum += w * static_cast<double>(x);
        weight += w;
    }
    *coverage = weight;
    return weight > 0.0 ? sum / weight + 0.5 : -1.0;
}

std::uint16_t half(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    const std::uint32_t sign = (bits >> 16) & 0x8000U;
    const int exponent = static_cast<int>((bits >> 23) & 0xFF) - 127 + 15;
    const std::uint32_t mantissa = bits & 0x7FFFFFU;
    if (exponent <= 0) return static_cast<std::uint16_t>(sign);
    if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7C00U);
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13));
}

float unhalf(std::uint16_t value) {
    const std::uint32_t sign = (value & 0x8000U) << 16;
    const std::uint32_t exponent = (value >> 10) & 0x1F;
    const std::uint32_t mantissa = value & 0x3FFU;
    std::uint32_t bits = sign;
    if (exponent != 0) bits |= ((exponent - 15 + 127) << 23) | (mantissa << 13);
    float result = 0;
    std::memcpy(&result, &bits, 4);
    return result;
}

bool invert(const float* m, float* out) {
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-12) return false;
        for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[p][k]);
        const double s = 1.0 / a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] *= s;
        for (int r = 0; r < 4; ++r)
            if (r != c) {
                const double f = a[r][c];
                for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
            }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = static_cast<float>(a[r][c + 4]);
    return true;
}

// One DLSS-G instance with its own guides, images and command list.
struct EyeCase {
    xrfg::D3D12DlssgFrameGenerator generator;
    xrfg::D3D11GuideTransfer transfer;
    xrfg::D3D11GuideRecord record;
    ComPtr<ID3D12Resource> backbuffer;
    ComPtr<ID3D12Resource> destinations[xrfg::kDlssgMaxOutputs];
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    std::shared_ptr<const xrfg::DlssMotionVectorSet> guides;
    int offset{};
};

HRESULT setup_eye(Gpu& gpu, ID3D11Device* d3d11, ID3D11DeviceContext* context, std::uint64_t swapchain, int offset,
    EyeCase* eye, UINT output_width = kOutputWidth, UINT output_height = kOutputHeight,
    UINT render_width = kRenderWidth, UINT render_height = kRenderHeight, int step = kBarStep,
    ID3D12CommandQueue* queue = nullptr) {
    if (queue == nullptr) queue = gpu.queue.Get();
    D3D12_RESOURCE_DESC image{};
    image.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    image.Width = output_width;
    image.Height = output_height;
    image.DepthOrArraySize = 1;
    image.MipLevels = 1;
    image.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    image.SampleDesc.Count = 1;
    eye->offset = offset;
    HRESULT hr = eye->generator.initialize(gpu.device.Get(), queue, image);
    if (FAILED(hr)) {
        std::printf("FAIL generator initialize 0x%08lx (see dlssg.log)\n", static_cast<unsigned long>(hr));
        return hr;
    }
    hr = eye->transfer.initialize(d3d11, context, gpu.device.Get(), queue);
    if (FAILED(hr)) {
        std::printf("FAIL guide transfer initialize 0x%08lx\n", static_cast<unsigned long>(hr));
        return hr;
    }

    // D3D11 guides: constant mid depth; every pixel moved right by one step,
    // so current -> previous is one step left, in render pixels.
    const float motion_x = -static_cast<float>(step) * render_width / output_width;
    std::vector<float> depth(static_cast<size_t>(render_width) * render_height, 0.5F);
    std::vector<std::uint16_t> motion(static_cast<size_t>(render_width) * render_height * 2);
    for (size_t i = 0; i < motion.size(); i += 2) {
        motion[i] = half(motion_x);
        motion[i + 1] = half(0.0F);
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = render_width;
    td.Height = render_height;
    td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    D3D11_SUBRESOURCE_DATA init{depth.data(), render_width * 4, 0};
    ComPtr<ID3D11Texture2D> depth11, motion11;
    hr = d3d11->CreateTexture2D(&td, &init, depth11.GetAddressOf());
    td.Format = DXGI_FORMAT_R16G16_FLOAT;
    init = {motion.data(), render_width * 4, 0};
    if (SUCCEEDED(hr)) hr = d3d11->CreateTexture2D(&td, &init, motion11.GetAddressOf());
    if (FAILED(hr)) {
        std::printf("FAIL D3D11 guide textures 0x%08lx\n", static_cast<unsigned long>(hr));
        return hr;
    }

    // Static camera: reversed-Z perspective, D3D view (+z forward), row vectors.
    const float n = 0.1F, f = 100.0F, a = 1.0F, b = 2.0F;
    const float column[16] = {a, 0, 0, 0, 0, b, 0, 0, 0, 0, -n / (f - n), n * f / (f - n), 0, 0, 1, 0};
    eye->record.depth = depth11;
    eye->record.motion_vectors = motion11;
    OFXR_D3D11GuidePublicationV3& g = eye->record.data;
    g.swapchain = swapchain;
    g.width = render_width;
    g.height = render_height;
    g.camera_near = n;
    g.camera_far = f;
    g.camera_fov = 2.0F * std::atan(1.0F / b);
    g.camera_aspect = b / a;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) g.view_to_clip[c * 4 + r] = column[r * 4 + c];
    invert(g.view_to_clip, g.clip_to_view);
    for (int i = 0; i < 16; ++i) g.clip_to_prev_clip[i] = g.prev_clip_to_clip[i] = i % 5 == 0 ? 1.0F : 0.0F;
    const float up[3] = {0, 1, 0}, right[3] = {1, 0, 0}, forward[3] = {0, 0, 1};
    std::memcpy(g.camera_up, up, sizeof(up));
    std::memcpy(g.camera_right, right, sizeof(right));
    std::memcpy(g.camera_forward, forward, sizeof(forward));

    eye->backbuffer = smoke_texture(gpu.device.Get(), output_width, output_height, "XRFG_SMOKE_BACKBUFFER", false);
    for (auto& destination : eye->destinations) {
        destination = smoke_texture(gpu.device.Get(), output_width, output_height, "XRFG_SMOKE_DEST", true);
    }
    hr = gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(eye->allocator.GetAddressOf()));
    if (SUCCEEDED(hr)) {
        hr = gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, eye->allocator.Get(), nullptr,
            IID_PPV_ARGS(eye->list.GetAddressOf()));
    }
    if (SUCCEEDED(hr)) hr = eye->list->Close();
    bool destinations_made = true;
    for (const auto& destination : eye->destinations) destinations_made = destinations_made && destination;
    if (FAILED(hr) || !eye->backbuffer || !destinations_made) {
        std::printf("FAIL D3D12 resources 0x%08lx\n", static_cast<unsigned long>(hr));
        return FAILED(hr) ? hr : E_OUTOFMEMORY;
    }
    return S_OK;
}

// `eye_count` instances (1, or 2 for stereo: created, recorded and submitted
// left first, on one queue, each on its own list as OFXR's synthesizers do).
int run_case(Gpu& gpu, ID3D11Device* d3d11, ID3D11DeviceContext* context, std::uint32_t outputs,
    std::uint32_t eye_count, std::uint64_t swapchain) {
    std::printf("--- %u instance(s), %u generated frame(s) per pair ---\n", eye_count, outputs);
    std::vector<std::unique_ptr<EyeCase>> eyes;
    for (std::uint32_t e = 0; e < eye_count; ++e) {
        eyes.push_back(std::make_unique<EyeCase>());
        if (FAILED(setup_eye(gpu, d3d11, context, swapchain + e, static_cast<int>(e) * kEyeOffset, eyes.back().get()))) {
            return 1;
        }
    }
    const float motion_x = -static_cast<float>(kBarStep) * kRenderWidth / kOutputWidth;
    int failures = 0;
    bool checked_guides = false;
    for (int frame = 0; frame < kFrames; ++frame) {
        for (auto& eye : eyes) {
            const auto pixels = bar_image(frame, eye->offset);
            HRESULT hr = upload(gpu, eye->backbuffer.Get(), pixels.data(), 4);
            if (FAILED(hr)) {
                std::printf("FAIL upload 0x%08lx\n", static_cast<unsigned long>(hr));
                return 1;
            }
            eye->record.data.serial = static_cast<std::uint64_t>(frame) + 1;
            eye->record.data.reset = frame == 0 ? 1 : 0;
            eye->guides.reset();
            hr = eye->transfer.transfer(eye->record, static_cast<std::uint32_t>(frame % 3), &eye->guides);
            if (FAILED(hr) || !eye->guides) {
                std::printf("FAIL guide transfer frame %d 0x%08lx\n", frame, static_cast<unsigned long>(hr));
                return 1;
            }
        }
        if (!checked_guides) {
            // The slot copy must hold what D3D11 wrote.
            const xrfg::DlssMotionVectorFrame& frame_guides = *eyes[0]->guides->eyes[0];
            gpu.queue->Wait(frame_guides.guide_fence.Get(), frame_guides.guide_fence_value);
            std::vector<std::uint8_t> row;
            read_row(gpu, frame_guides.motion_vectors.Get(), kRenderHeight / 2, 4, &row);
            std::uint16_t raw[2]{};
            std::memcpy(raw, row.data() + 4 * 10, 4);
            const float mv[2] = {unhalf(raw[0]), unhalf(raw[1])};
            read_row(gpu, frame_guides.depth.Get(), kRenderHeight / 2, 4, &row);
            float d = 0;
            std::memcpy(&d, row.data() + 4 * 10, 4);
            const bool ok = mv[0] == motion_x && mv[1] == 0.0F && d == 0.5F;
            std::printf("%s guide transfer: depth %.3f motion (%.1f, %.1f), fence %llu\n", ok ? "PASS" : "FAIL", d, mv[0],
                mv[1], static_cast<unsigned long long>(frame_guides.guide_fence_value));
            failures += ok ? 0 : 1;
            checked_guides = true;
        }
        if (!eyes[0]->generator.can_generate(*eyes[0]->guides->eyes[0], outputs)) {
            std::printf("SKIP %u output(s): not supported by this GPU/runtime\n", outputs);
            return failures;
        }
        std::vector<std::uint32_t> generated(eyes.size());
        for (size_t e = 0; e < eyes.size(); ++e) {
            EyeCase& eye = *eyes[e];
            const xrfg::DlssMotionVectorFrame& frame_guides = *eye.guides->eyes[0];
            HRESULT hr = eye.allocator->Reset();
            if (SUCCEEDED(hr)) hr = eye.list->Reset(eye.allocator.Get(), nullptr);
            if (FAILED(hr)) return 1;
            xrfg::DlssgRecordInput input{};
            input.backbuffer = eye.backbuffer.Get();
            input.guides = &frame_guides;
            input.outputs = outputs;
            for (std::uint32_t k = 0; k < xrfg::kDlssgMaxOutputs; ++k) input.destinations[k] = eye.destinations[k].Get();
            input.destination_state = D3D12_RESOURCE_STATE_COMMON;
            hr = eye.generator.record(eye.list.Get(), input, &generated[e]);
            const HRESULT closed = eye.list->Close();
            if (FAILED(hr) || FAILED(closed)) {
                std::printf("FAIL record eye %zu frame %d 0x%08lx\n", e, frame,
                    static_cast<unsigned long>(FAILED(hr) ? hr : closed));
                return 1;
            }
            // As the synthesizer does: the queue waits for the guides first.
            gpu.queue->Wait(frame_guides.guide_fence.Get(), frame_guides.guide_fence_value);
            ID3D12CommandList* lists[] = {eye.list.Get()};
            gpu.queue->ExecuteCommandLists(1, lists);
        }
        const HRESULT hr = gpu.wait_queue();
        if (FAILED(hr)) {
            std::printf("FAIL execute frame %d 0x%08lx (device %s)\n", frame, static_cast<unsigned long>(hr),
                FAILED(gpu.device->GetDeviceRemovedReason()) ? "removed" : "ok");
            return 1;
        }
        for (size_t e = 0; e < eyes.size(); ++e) {
            for (std::uint32_t k = 0; k < outputs; ++k) {
                std::vector<std::uint8_t> row;
                read_row(gpu, eyes[e]->destinations[k].Get(), kOutputHeight / 2, 4, &row);
                double coverage = 0;
                const double centre = bar_centre(row, &coverage);
                const double current = kBarStart + eyes[e]->offset + frame * kBarStep + kBarWidth / 2.0;
                const double expected = generated[e] == 0
                    ? current
                    : current - kBarStep + kBarStep * static_cast<double>(k + 1) / (outputs + 1);
                const bool ok = std::fabs(centre - expected) <= 6.0 && coverage > kBarWidth * 0.5 &&
                    coverage < kBarWidth * 2.0;
                if (frame >= 2) failures += ok ? 0 : 1;
                std::printf("%s eye %zu frame %d output %u: generated=%u bar centre %.1f (expected %.1f, previous %.1f, current %.1f) coverage %.1f px\n",
                    frame < 2 ? "INFO" : (ok ? "PASS" : "FAIL"), e, frame, k + 1, generated[e], centre, expected,
                    current - kBarStep, current, coverage);
            }
        }
    }
    return failures;
}

// Scene-like texture (blocky noise over a vertical gradient), wide enough to
// slide a benchmark-size window across it by kBenchStep per frame.
std::vector<std::uint32_t> bench_texture(UINT width, UINT height) {
    std::vector<std::uint32_t> pixels(static_cast<size_t>(width) * height);
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            std::uint32_t n = ((x / 6U) * 73856093U) ^ ((y / 6U) * 19349663U);
            n ^= n >> 13;
            n *= 0x5BD1E995U;
            n ^= n >> 15;
            const std::uint32_t g = y * 160U / height;
            const std::uint32_t r = 40U + g / 2U + (n & 0x7FU);
            const std::uint32_t gg = 40U + ((n >> 8) & 0x7FU);
            const std::uint32_t b = 40U + (160U - g) / 2U + ((n >> 16) & 0x7FU);
            pixels[static_cast<size_t>(y) * width + x] = 0xFF000000U | (b << 16) | (gg << 8) | r;
        }
    }
    return pixels;
}

// GPU time of each eye's DLSS-G list (B copy, evaluations, copies out) per
// real-frame pair: two instances recorded and submitted in eye order on one
// queue, as OFXR's synthesizers do; the scene moves right kBenchStep pixels a
// frame, as the motion vectors say.
HRESULT read_texture(Gpu& gpu, ID3D12Resource* source, UINT bytes_per_pixel, std::vector<std::uint8_t>* out);

// FNV-1a of a destination's bytes, to compare the in-place and copying routes.
std::uint64_t image_hash(Gpu& gpu, ID3D12Resource* source) {
    std::vector<std::uint8_t> bytes;
    if (FAILED(read_texture(gpu, source, 4, &bytes))) return 0;
    std::uint64_t h = 1469598103934665603ULL;
    for (std::uint8_t b : bytes) h = (h ^ b) * 1099511628211ULL;
    return h;
}

// bench-packed: one instance on both eyes side by side, kPackGap output
// columns apart (as NR's PackEyes), timed the same way.
// bench-parallel: eye 1 on a DIRECT queue of its own, so the GPU may run the
// eyes at the same time; "span" = first start to last end of the pair.
constexpr UINT kPackGap = 64;

int run_bench(Gpu& gpu, ID3D11Device* d3d11, ID3D11DeviceContext* context, std::uint32_t outputs, bool lite,
    bool packed = false, bool parallel = false) {
    const UINT instances = packed ? 1 : 2;
    const UINT out_w = packed ? 2 * kBenchOutputWidth + kPackGap : kBenchOutputWidth;
    const UINT render_w = packed ? 2 * kBenchRenderWidth + kPackGap * kBenchRenderWidth / kBenchOutputWidth
                                 : kBenchRenderWidth;
    std::printf("--- bench %ux: %u instance(s)%s, output %ux%u, render %ux%u, mode %s ---\n", outputs + 1, instances,
        parallel ? " on two queues" : "", out_w, kBenchOutputHeight, render_w, kBenchRenderHeight,
        lite ? "lite (Preset C)" : "default");
    ComPtr<ID3D12CommandQueue> second;
    ComPtr<ID3D12Fence> second_fence;
    UINT64 second_value = 0;
    if (parallel) {
        D3D12_COMMAND_QUEUE_DESC sd{D3D12_COMMAND_LIST_TYPE_DIRECT};
        if (FAILED(gpu.device->CreateCommandQueue(&sd, IID_PPV_ARGS(second.GetAddressOf()))) ||
            FAILED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(second_fence.GetAddressOf())))) {
            std::printf("FAIL second queue\n");
            return 1;
        }
    }
    ID3D12CommandQueue* const queues[2] = {gpu.queue.Get(), parallel ? second.Get() : gpu.queue.Get()};
    constexpr int total = kBenchWarmup + kBenchFrames;
    const UINT wide = out_w + static_cast<UINT>(kBenchStep) * total;
    const auto scene = bench_texture(wide, kBenchOutputHeight);
    std::vector<std::unique_ptr<EyeCase>> eyes;
    for (std::uint32_t e = 0; e < instances; ++e) {
        eyes.push_back(std::make_unique<EyeCase>());
        if (FAILED(setup_eye(gpu, d3d11, context, 0x5000 + outputs * 0x10 + e + (packed ? 8 : 0) + (parallel ? 4 : 0),
                0, eyes.back().get(), out_w, kBenchOutputHeight, render_w, kBenchRenderHeight, kBenchStep,
                queues[e]))) {
            return 1;
        }
    }
    // Each queue's timestamps on the CPU clock, to put the two eyes on one line.
    LARGE_INTEGER qpc_frequency{};
    QueryPerformanceFrequency(&qpc_frequency);
    UINT64 queue_frequency[2]{}, calibration_gpu[2]{}, calibration_cpu[2]{};
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 4, 0};
    ComPtr<ID3D12QueryHeap> heap;
    UINT64 frequency = 0;
    auto readback = buffer(gpu.device.Get(), 4 * sizeof(UINT64), D3D12_HEAP_TYPE_READBACK);
    if (FAILED(gpu.device->CreateQueryHeap(&qd, IID_PPV_ARGS(heap.GetAddressOf()))) || !readback ||
        FAILED(gpu.queue->GetTimestampFrequency(&frequency)) || frequency == 0) {
        std::printf("FAIL timestamp setup\n");
        return 1;
    }
    for (UINT e = 0; e < 2; ++e) {
        if (FAILED(queues[e]->GetTimestampFrequency(&queue_frequency[e])) || queue_frequency[e] == 0) {
            std::printf("FAIL timestamp setup\n");
            return 1;
        }
    }
    std::vector<double> times[5];  // eye 0, eye 1, sum of the eyes, span of the pair, CPU wall
    int not_generated = 0;
    std::vector<std::uint32_t> frame(static_cast<size_t>(out_w) * kBenchOutputHeight);
    for (int f = 0; f < total; ++f) {
        // This frame's window: the scene shifted right by f steps.
        const UINT left = static_cast<UINT>(kBenchStep) * static_cast<UINT>(total - f);
        for (UINT y = 0; y < kBenchOutputHeight; ++y) {
            std::memcpy(frame.data() + static_cast<size_t>(y) * out_w,
                scene.data() + static_cast<size_t>(y) * wide + left, out_w * sizeof(std::uint32_t));
        }
        for (auto& eye : eyes) {
            HRESULT hr = upload(gpu, eye->backbuffer.Get(), frame.data(), 4);
            if (FAILED(hr)) {
                std::printf("FAIL upload 0x%08lx\n", static_cast<unsigned long>(hr));
                return 1;
            }
            eye->record.data.serial = static_cast<std::uint64_t>(f) + 1;
            eye->record.data.reset = f == 0 ? 1 : 0;
            eye->guides.reset();
            hr = eye->transfer.transfer(eye->record, static_cast<std::uint32_t>(f % 3), &eye->guides);
            if (FAILED(hr) || !eye->guides) {
                std::printf("FAIL guide transfer frame %d 0x%08lx\n", f, static_cast<unsigned long>(hr));
                return 1;
            }
        }
        if (!eyes[0]->generator.can_generate(*eyes[0]->guides->eyes[0], outputs)) {
            std::printf("SKIP %u output(s): not supported by this GPU/runtime\n", outputs);
            return 0;
        }
        if (f == kBenchWarmup) {
            for (UINT e = 0; e < 2; ++e) queues[e]->GetClockCalibration(&calibration_gpu[e], &calibration_cpu[e]);
        }
        LARGE_INTEGER wall_start{}, wall_end{};
        QueryPerformanceCounter(&wall_start);
        std::uint32_t generated[2]{outputs, outputs};
        for (UINT e = 0; e < instances; ++e) {
            EyeCase& eye = *eyes[e];
            const xrfg::DlssMotionVectorFrame& frame_guides = *eye.guides->eyes[0];
            HRESULT hr = eye.allocator->Reset();
            if (SUCCEEDED(hr)) hr = eye.list->Reset(eye.allocator.Get(), nullptr);
            if (FAILED(hr)) return 1;
            eye.list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * e);
            xrfg::DlssgRecordInput input{};
            input.backbuffer = eye.backbuffer.Get();
            input.guides = &frame_guides;
            input.outputs = outputs;
            for (std::uint32_t k = 0; k < xrfg::kDlssgMaxOutputs; ++k) input.destinations[k] = eye.destinations[k].Get();
            input.destination_state = D3D12_RESOURCE_STATE_COMMON;
            hr = eye.generator.record(eye.list.Get(), input, &generated[e]);
            eye.list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * e + 1);
            eye.list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * e, 2, readback.Get(),
                2 * e * sizeof(UINT64));
            const HRESULT closed = eye.list->Close();
            if (FAILED(hr) || FAILED(closed)) {
                std::printf("FAIL record eye %u frame %d 0x%08lx\n", e, f,
                    static_cast<unsigned long>(FAILED(hr) ? hr : closed));
                return 1;
            }
            queues[e]->Wait(frame_guides.guide_fence.Get(), frame_guides.guide_fence_value);
            ID3D12CommandList* lists[] = {eye.list.Get()};
            queues[e]->ExecuteCommandLists(1, lists);
        }
        if (parallel) {
            if (FAILED(second->Signal(second_fence.Get(), ++second_value))) return 1;
            if (second_fence->GetCompletedValue() < second_value) {
                second_fence->SetEventOnCompletion(second_value, gpu.event);
                if (WaitForSingleObject(gpu.event, 10000) != WAIT_OBJECT_0) {
                    std::printf("FAIL second queue wait frame %d\n", f);
                    return 1;
                }
            }
        }
        const HRESULT hr = gpu.wait_queue();
        QueryPerformanceCounter(&wall_end);
        if (FAILED(hr)) {
            std::printf("FAIL execute frame %d 0x%08lx\n", f, static_cast<unsigned long>(hr));
            return 1;
        }
        if (f < kBenchWarmup) continue;
        if ((f - kBenchWarmup) % 40 == 0 || f == total - 1) {
            for (UINT e = 0; e < instances; ++e) {
                for (std::uint32_t k = 0; k < outputs; ++k) {
                    std::printf("HASH %ux frame %d eye %u output %u generated %u: %016llx\n", outputs + 1, f, e, k + 1,
                        generated[e], static_cast<unsigned long long>(image_hash(gpu, eyes[e]->destinations[k].Get())));
                }
            }
        }
        if (generated[0] != outputs || generated[1] != outputs) {
            ++not_generated;
            continue;
        }
        UINT64* stamps = nullptr;
        D3D12_RANGE range{0, 4 * sizeof(UINT64)};
        if (FAILED(readback->Map(0, &range, reinterpret_cast<void**>(&stamps)))) return 1;
        const double ms = 1000.0 / static_cast<double>(frequency);
        const double eye0 = static_cast<double>(stamps[1] - stamps[0]) * ms;
        const double eye1 = instances == 2 ? static_cast<double>(stamps[3] - stamps[2]) * ms : 0.0;
        // ms on the CPU clock: calibration point + ticks since it on that queue.
        const auto on_cpu = [&](UINT e, UINT64 stamp) {
            return 1000.0 * static_cast<double>(calibration_cpu[e]) / static_cast<double>(qpc_frequency.QuadPart) +
                1000.0 * (static_cast<double>(stamp) - static_cast<double>(calibration_gpu[e])) /
                    static_cast<double>(queue_frequency[e]);
        };
        double first = on_cpu(0, stamps[0]), last = on_cpu(0, stamps[1]);
        if (instances == 2) {
            first = std::min(first, on_cpu(1, stamps[2]));
            last = std::max(last, on_cpu(1, stamps[3]));
        }
        D3D12_RANGE none{0, 0};
        readback->Unmap(0, &none);
        times[0].push_back(eye0);
        if (instances == 2) times[1].push_back(eye1);
        times[2].push_back(eye0 + eye1);
        times[3].push_back(last - first);
        times[4].push_back(1000.0 * static_cast<double>(wall_end.QuadPart - wall_start.QuadPart) /
            static_cast<double>(qpc_frequency.QuadPart));
    }
    const char* names[] = {"eye 0", "eye 1", packed ? "both eyes packed" : "both eyes", "pair span", "CPU wall"};
    for (int i = 0; i < 5; ++i) {
        auto& t = times[i];
        if (t.empty()) continue;
        std::sort(t.begin(), t.end());
        double sum = 0;
        for (double v : t) sum += v;
        std::printf("BENCH %ux %s %s: avg %.3f ms, median %.3f, p90 %.3f, max %.3f (%zu pairs)\n", outputs + 1,
            lite ? "lite" : "default", names[i], sum / static_cast<double>(t.size()), t[t.size() / 2],
            t[t.size() * 9 / 10], t.back(), t.size());
    }
    if (not_generated) std::printf("INFO %d measured pair(s) not generated (reset or fallback), left out\n", not_generated);
    return times[2].empty() ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Camera pan (modes pan / pan-lite): a head turn as DLSS-G sees it in VaM.
// Two eye cameras with VaM's asymmetric Quest 3 frusta (GetProjectionRaw in
// opencomposite-current.log) look into a textured box room while the head
// moves; colour, depth, motion (current -> previous, render pixels, camera
// motion included) and camera matrices are exact. Each generated frame is
// compared with the room rendered at a fraction s of the way from the
// previous pose to the current one: 0.5 is right; 0 or 1 is a real frame
// handed back, which OFXR shows at the in-between pose: a double image.

constexpr double kPi = 3.14159265358979323846;

struct V3 {
    double x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// Tangents of the four half angles, all positive.
struct Frustum {
    double left, right, up, down;
};

// World space as Unity's (x right, y up, z forward); the view axes in it.
struct PanCamera {
    V3 position, right, up, forward;
    Frustum frustum;
};

struct PanPose {
    double yaw;  // radians, turning right
    V3 head;
};

struct PanVariant {
    const char* name;
    double yaw_step_deg;   // per real frame
    double strafe_step_m;  // head x per real frame
    bool symmetric;        // symmetric frusta instead of VaM's
    bool identity_matrices;
    bool fov_degrees;
    bool zero_motion;
    double motion_scale = 1.0;  // systematic motion-vector error
    bool jitter = false;        // DLSS SR's Halton jitter on depth / motion, as VaM renders
    float frame_ms = 16.6667F;  // frameTimeDeltaMs
    int reset_every = 0;        // DLSS-G reset on every n-th real frame (VaM: after a repeated frame)
    int bad_region = 0;         // 1: the pillars have no motion (like UI / transparent / hair in VaM), 2: a centre box
    int still_frames = 0;       // real frames without motion before the turn starts (sample-capture trigger)
};

constexpr double kPanNear = 0.01, kPanFar = 2000.0;  // VaM's camera
constexpr int kPanFrames = 12;
constexpr int kPanFirstMeasured = 4;
constexpr int kPanImageFrame = 8;

PanCamera pan_camera(int eye, const PanPose& pose, bool symmetric) {
    const double s = std::sin(pose.yaw), c = std::cos(pose.yaw);
    PanCamera cam{};
    cam.right = {c, 0.0, -s};
    cam.up = {0.0, 1.0, 0.0};
    cam.forward = {s, 0.0, c};
    cam.position = pose.head + cam.right * (eye == 0 ? -0.032 : 0.032);
    if (symmetric) {
        cam.frustum = {1.1, 1.1, 1.2, 1.2};
    } else {
        cam.frustum = eye == 0 ? Frustum{1.37638187, 0.839099586, 1.42814791, 0.965688765}
                               : Frustum{0.839099586, 1.37638187, 1.42814791, 0.965688765};
    }
    return cam;
}

double halton(int index, int base) {
    double f = 1.0, r = 0.0;
    for (int i = index; i > 0; i /= base) {
        f /= base;
        r += f * (i % base);
    }
    return r;
}

bool pan_reset(const PanVariant& v, int frame) { return v.reset_every > 0 && frame > 0 && frame % v.reset_every == 0; }

PanPose pan_pose(const PanVariant& v, double frame) {
    const double moving = std::max(0.0, frame - v.still_frames);
    return {v.yaw_step_deg * kPi / 180.0 * moving, V3{v.strafe_step_m * moving, 0.0, 0.0}};
}

PanPose pan_lerp(const PanPose& a, const PanPose& b, double s) {
    return {a.yaw + (b.yaw - a.yaw) * s, a.head + (b.head - a.head) * s};
}

// Column-vector matrices, row-major storage (M * v).
void pan_projection(const Frustum& f, double* m) {
    const double a = 2.0 / (f.right + f.left), cx = -(f.right - f.left) / (f.right + f.left);
    const double b = 2.0 / (f.up + f.down), cy = -(f.up - f.down) / (f.up + f.down);
    const double p[16] = {a, 0, cx, 0, 0, b, cy, 0, 0, 0, -kPanNear / (kPanFar - kPanNear),
        kPanNear * kPanFar / (kPanFar - kPanNear), 0, 0, 1, 0};
    std::memcpy(m, p, sizeof(p));
}

void pan_world_to_view(const PanCamera& c, double* m) {
    const double v[16] = {c.right.x, c.right.y, c.right.z, -dot(c.right, c.position), c.up.x, c.up.y, c.up.z,
        -dot(c.up, c.position), c.forward.x, c.forward.y, c.forward.z, -dot(c.forward, c.position), 0, 0, 0, 1};
    std::memcpy(m, v, sizeof(v));
}

void pan_multiply(const double* a, const double* b, double* out) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) sum += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = sum;
        }
}

bool pan_invert(const double* m, double* out) {
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-300) return false;
        for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[p][k]);
        const double s = 1.0 / a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] *= s;
        for (int r = 0; r < 4; ++r)
            if (r != c) {
                const double f = a[r][c];
                for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
            }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r][c + 4];
    return true;
}

// OFXR's publication layout: row-major for row vectors (the transpose).
void pan_publish(const double* column, float* out) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[c * 4 + r] = static_cast<float>(column[r * 4 + c]);
}

// The room: walls x = +-3, z = +-4, floor y = -1.6, ceiling y = 1.4, two
// pillars. `dir` has view z = 1, so t is the view depth.
bool pan_hit(V3 o, V3 dir, double* t_out, V3* p_out, int* id_out) {
    double best = 1e30;
    int id = -1;
    const double walls[6][2] = {{0, 3.0}, {0, -3.0}, {1, -1.6}, {1, 1.4}, {2, 4.0}, {2, -4.0}};
    for (int i = 0; i < 6; ++i) {
        const int axis = static_cast<int>(walls[i][0]);
        const double oa = axis == 0 ? o.x : axis == 1 ? o.y : o.z;
        const double da = axis == 0 ? dir.x : axis == 1 ? dir.y : dir.z;
        if (std::fabs(da) < 1e-12) continue;
        const double t = (walls[i][1] - oa) / da;
        if (t > 1e-6 && t < best) {
            best = t;
            id = i;
        }
    }
    const double pillars[2][3] = {{0.9, 2.2, 0.25}, {-1.2, 1.4, 0.15}};
    for (int i = 0; i < 2; ++i) {
        const double ox = o.x - pillars[i][0], oz = o.z - pillars[i][1];
        const double a = dir.x * dir.x + dir.z * dir.z;
        const double b = 2.0 * (ox * dir.x + oz * dir.z);
        const double c = ox * ox + oz * oz - pillars[i][2] * pillars[i][2];
        const double disc = b * b - 4.0 * a * c;
        if (a < 1e-12 || disc < 0.0) continue;
        const double t = (-b - std::sqrt(disc)) / (2.0 * a);
        if (t > 1e-6 && t < best) {
            best = t;
            id = 6 + i;
        }
    }
    if (id < 0) return false;
    *t_out = best;
    *p_out = o + dir * best;
    *id_out = id;
    return true;
}

std::uint32_t pan_hash(int a, int b, int c) {
    std::uint32_t n = static_cast<std::uint32_t>(a) * 73856093U ^ static_cast<std::uint32_t>(b) * 19349663U ^
        static_cast<std::uint32_t>(c) * 83492791U;
    n ^= n >> 13;
    n *= 0x5BD1E995U;
    n ^= n >> 15;
    return n;
}

// Albedo as 0..255 per channel: blocky colours, dark grid lines, a pale dot
// in each cell; stripes and bands on the pillars.
void pan_albedo(int id, V3 p, double* rgb) {
    double s = 0, t = 0;
    if (id < 6) {
        const int axis = id / 2;
        s = axis == 0 ? p.z : p.x;
        t = axis == 1 ? p.z : p.y;
    } else {
        const double cx = id == 6 ? 0.9 : -1.2, cz = id == 6 ? 2.2 : 1.4;
        s = std::atan2(p.z - cz, p.x - cx) * 0.3;
        t = p.y;
    }
    const double cell = 0.25;
    const double fs = s / cell - std::floor(s / cell), ft = t / cell - std::floor(t / cell);
    const std::uint32_t h = pan_hash(static_cast<int>(std::floor(s / cell)), static_cast<int>(std::floor(t / cell)), id);
    if (fs < 0.08 || ft < 0.08) {
        rgb[0] = rgb[1] = rgb[2] = 24.0;
    } else if ((fs - 0.55) * (fs - 0.55) + (ft - 0.55) * (ft - 0.55) < 0.02) {
        rgb[0] = rgb[1] = rgb[2] = 235.0;
    } else {
        rgb[0] = 60.0 + (h & 127U);
        rgb[1] = 60.0 + ((h >> 8) & 127U);
        rgb[2] = 60.0 + ((h >> 16) & 127U);
    }
}

V3 pan_ray(const PanCamera& c, double u, double v, UINT w, UINT h) {
    const double xn = u / w * 2.0 - 1.0, yn = 1.0 - v / h * 2.0;
    const Frustum& f = c.frustum;
    const double tx = (xn * (f.right + f.left) + (f.right - f.left)) * 0.5;
    const double ty = (yn * (f.up + f.down) + (f.up - f.down)) * 0.5;
    return c.right * tx + c.up * ty + c.forward;
}

template <class F>
void pan_parallel(UINT count, F&& body) {
    const unsigned n = std::max(1U, std::thread::hardware_concurrency());
    std::atomic<UINT> next{0};
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < n; ++i) {
        pool.emplace_back([&] {
            for (UINT r = next++; r < count; r = next++) body(r);
        });
    }
    for (auto& thread : pool) thread.join();
}

// Rows `rows` of the colour image, 2x2 supersampled, RGBA8.
void pan_color(const PanCamera& c, UINT w, UINT h, const std::vector<UINT>& rows, std::uint32_t* out) {
    pan_parallel(static_cast<UINT>(rows.size()), [&](UINT i) {
        const UINT y = rows[i];
        for (UINT x = 0; x < w; ++x) {
            double sum[3]{};
            for (int sy = 0; sy < 2; ++sy)
                for (int sx = 0; sx < 2; ++sx) {
                    double t = 0;
                    V3 p{};
                    int id = 0;
                    double rgb[3] = {0, 0, 0};
                    if (pan_hit(c.position, pan_ray(c, x + 0.25 + 0.5 * sx, y + 0.25 + 0.5 * sy, w, h), &t, &p, &id))
                        pan_albedo(id, p, rgb);
                    for (int k = 0; k < 3; ++k) sum[k] += rgb[k];
                }
            const auto r = static_cast<std::uint32_t>(sum[0] / 4.0 + 0.5);
            const auto g = static_cast<std::uint32_t>(sum[1] / 4.0 + 0.5);
            const auto b = static_cast<std::uint32_t>(sum[2] / 4.0 + 0.5);
            out[static_cast<size_t>(i) * w + x] = 0xFF000000U | (b << 16) | (g << 8) | r;
        }
    });
}

// Reversed-Z depth and motion (current -> previous, render pixels) at pixel centres.
void pan_guides(const PanCamera& c, const PanCamera& previous, UINT w, UINT h, bool zero_motion,
    double motion_scale, double jitter_x, double jitter_y, int bad_region, std::vector<float>* depth,
    std::vector<std::uint16_t>* motion) {
    depth->assign(static_cast<size_t>(w) * h, 0.0F);
    motion->assign(static_cast<size_t>(w) * h * 2, 0);
    const Frustum& f = previous.frustum;
    const double a = 2.0 / (f.right + f.left), cx = -(f.right - f.left) / (f.right + f.left);
    const double b = 2.0 / (f.up + f.down), cy = -(f.up - f.down) / (f.up + f.down);
    pan_parallel(h, [&](UINT y) {
        for (UINT x = 0; x < w; ++x) {
            const double u = x + 0.5 - jitter_x, v = y + 0.5 - jitter_y;
            double t = 0;
            V3 p{};
            int id = 0;
            const size_t i = static_cast<size_t>(y) * w + x;
            if (!pan_hit(c.position, pan_ray(c, u, v, w, h), &t, &p, &id)) continue;
            (*depth)[i] = static_cast<float>(kPanNear * kPanFar / ((kPanFar - kPanNear) * t) - kPanNear / (kPanFar - kPanNear));
            if (zero_motion) continue;
            if (bad_region == 1 && id >= 6) continue;
            if (bad_region == 2 && x > w * 35 / 100 && x < w * 65 / 100 && y > h * 35 / 100 && y < h * 65 / 100) continue;
            const V3 q = p - previous.position;
            const double vx = dot(q, previous.right), vy = dot(q, previous.up), vz = dot(q, previous.forward);
            if (vz <= 1e-6) continue;
            const double up = (a * vx / vz + cx + 1.0) * 0.5 * w;
            const double vp = (1.0 - (b * vy / vz + cy)) * 0.5 * h;
            (*motion)[i * 2] = half(static_cast<float>((up - u) * motion_scale));
            (*motion)[i * 2 + 1] = half(static_cast<float>((vp - v) * motion_scale));
        }
    });
}

void pan_camera_data(const PanVariant& variant, const PanCamera& c, const PanCamera& previous,
    OFXR_D3D11GuidePublicationV3* g) {
    double p[16], view[16], view_previous[16], p_previous[16], vp[16], vp_previous[16];
    pan_projection(c.frustum, p);
    pan_projection(previous.frustum, p_previous);
    pan_world_to_view(c, view);
    pan_world_to_view(previous, view_previous);
    pan_multiply(p, view, vp);
    pan_multiply(p_previous, view_previous, vp_previous);
    double inverse_vp[16], inverse_vp_previous[16], clip_to_prev[16], prev_to_clip[16], clip_to_view[16];
    pan_invert(vp, inverse_vp);
    pan_invert(vp_previous, inverse_vp_previous);
    pan_multiply(vp_previous, inverse_vp, clip_to_prev);
    pan_multiply(vp, inverse_vp_previous, prev_to_clip);
    pan_invert(p, clip_to_view);
    if (variant.identity_matrices) {
        for (int i = 0; i < 16; ++i) clip_to_prev[i] = prev_to_clip[i] = i % 5 == 0 ? 1.0 : 0.0;
    }
    pan_publish(p, g->view_to_clip);
    pan_publish(clip_to_view, g->clip_to_view);
    pan_publish(clip_to_prev, g->clip_to_prev_clip);
    pan_publish(prev_to_clip, g->prev_clip_to_clip);
    const Frustum& f = c.frustum;
    // As OpenComposite sends it: vertical field of view in radians, width / height.
    g->camera_fov = static_cast<float>(std::atan(f.up) + std::atan(f.down));
    if (variant.fov_degrees) g->camera_fov = static_cast<float>(g->camera_fov * 180.0 / kPi);
    g->camera_aspect = static_cast<float>((f.right + f.left) / (f.up + f.down));
    g->camera_near = static_cast<float>(kPanNear);
    g->camera_far = static_cast<float>(kPanFar);
    const V3 axes[4] = {c.position, c.up, c.right, c.forward};
    float* outs[4] = {g->camera_position, g->camera_up, g->camera_right, g->camera_forward};
    for (int i = 0; i < 4; ++i) {
        outs[i][0] = static_cast<float>(axes[i].x);
        outs[i][1] = static_cast<float>(axes[i].y);
        outs[i][2] = static_cast<float>(axes[i].z);
    }
}

HRESULT read_texture(Gpu& gpu, ID3D12Resource* source, UINT bytes_per_pixel, std::vector<std::uint8_t>* out) {
    const D3D12_RESOURCE_DESC d = source->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT64 total = 0;
    gpu.device->GetCopyableFootprints(&d, 0, 1, 0, &layout, nullptr, nullptr, &total);
    auto readback = buffer(gpu.device.Get(), total, D3D12_HEAP_TYPE_READBACK);
    HRESULT hr = gpu.begin();
    if (FAILED(hr)) return hr;
    auto b = transition(source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.list->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = layout;
    D3D12_TEXTURE_COPY_LOCATION src{source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    gpu.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b = transition(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    gpu.list->ResourceBarrier(1, &b);
    hr = gpu.submit_and_wait();
    if (FAILED(hr)) return hr;
    std::uint8_t* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
    hr = readback->Map(0, &range, reinterpret_cast<void**>(&mapped));
    if (FAILED(hr)) return hr;
    const size_t row_bytes = static_cast<size_t>(d.Width) * bytes_per_pixel;
    out->resize(row_bytes * d.Height);
    for (UINT y = 0; y < d.Height; ++y)
        std::memcpy(out->data() + y * row_bytes, mapped + layout.Offset + y * layout.Footprint.RowPitch, row_bytes);
    D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    return S_OK;
}

// Binary PPM at half size (2x2 box filter).
void write_ppm(const std::wstring& path, const std::uint32_t* rgba, UINT w, UINT h) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) return;
    const UINT hw = w / 2, hh = h / 2;
    std::fprintf(file, "P6\n%u %u\n255\n", hw, hh);
    std::vector<std::uint8_t> row(static_cast<size_t>(hw) * 3);
    for (UINT y = 0; y < hh; ++y) {
        for (UINT x = 0; x < hw; ++x)
            for (int k = 0; k < 3; ++k) {
                unsigned sum = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx)
                        sum += (rgba[static_cast<size_t>(2 * y + dy) * w + 2 * x + dx] >> (8 * k)) & 0xFFU;
                row[static_cast<size_t>(x) * 3 + k] = static_cast<std::uint8_t>((sum + 2) / 4);
            }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
}

struct PanScore {
    double best_s{};
    double error_best{};
    double error_expected{};
    double error_ab{};
};

// Mean absolute RGB difference between `image` and the room at fraction s
// between the two poses, over every 8th row inside a 6% border: where the
// image fits best, and how well it fits at `expected`.
PanScore pan_score(const std::uint32_t* image, UINT w, UINT h, int eye, const PanVariant& v,
    const PanPose& previous, const PanPose& current, double expected) {
    std::vector<UINT> rows;
    for (UINT y = h * 6 / 100; y < h * 94 / 100; y += 8) rows.push_back(y);
    const UINT x0 = w * 6 / 100, x1 = w * 94 / 100;
    std::vector<std::uint32_t> truth(rows.size() * w), other(rows.size() * w);
    auto mad = [&](const std::uint32_t* a, bool a_compact, const std::uint32_t* b) {
        double sum = 0;
        size_t n = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            const std::uint32_t* ra = a + (a_compact ? i : rows[i]) * static_cast<size_t>(w);
            const std::uint32_t* rb = b + i * static_cast<size_t>(w);
            for (UINT x = x0; x < x1; ++x) {
                for (int k = 0; k < 3; ++k)
                    sum += std::abs(static_cast<int>((ra[x] >> (8 * k)) & 0xFFU) - static_cast<int>((rb[x] >> (8 * k)) & 0xFFU));
                n += 3;
            }
        }
        return sum / static_cast<double>(n);
    };
    auto error_at = [&](double s) {
        pan_color(pan_camera(eye, pan_lerp(previous, current, s), v.symmetric), w, h, rows, truth.data());
        return mad(image, false, truth.data());
    };
    constexpr int kSteps = 41;  // s = -0.5 .. 1.5
    double errors[kSteps];
    int best = 0;
    for (int i = 0; i < kSteps; ++i) {
        errors[i] = error_at(-0.5 + 0.05 * i);
        if (errors[i] < errors[best]) best = i;
    }
    PanScore score;
    score.best_s = -0.5 + 0.05 * best;
    score.error_best = errors[best];
    if (best > 0 && best < kSteps - 1) {
        const double l = errors[best - 1], c = errors[best], r = errors[best + 1];
        const double denominator = l - 2.0 * c + r;
        if (denominator > 1e-9) score.best_s += 0.05 * 0.5 * (l - r) / denominator;
    }
    score.error_expected = error_at(expected);
    pan_color(pan_camera(eye, previous, v.symmetric), w, h, rows, other.data());
    pan_color(pan_camera(eye, current, v.symmetric), w, h, rows, truth.data());
    score.error_ab = mad(other.data(), true, truth.data());
    return score;
}

int run_pan(Gpu& gpu, ID3D11Device* d3d11, ID3D11DeviceContext* context, const PanVariant& v, bool lite,
    std::uint32_t outputs, const std::wstring& log_dir, std::uint64_t swapchain) {
    const UINT ow = kBenchOutputWidth, oh = kBenchOutputHeight, rw = kBenchRenderWidth, rh = kBenchRenderHeight;
    std::printf("--- pan %s %ux (%s): yaw %.2f deg and head %.3f m per real frame, %s frusta, 2 instances, output %ux%u, render %ux%u ---\n",
        v.name, outputs + 1, lite ? "lite" : "default", v.yaw_step_deg, v.strafe_step_m,
        v.symmetric ? "symmetric" : "VaM's asymmetric", ow, oh, rw, rh);
    std::vector<std::unique_ptr<EyeCase>> eyes;
    for (int e = 0; e < 2; ++e) {
        eyes.push_back(std::make_unique<EyeCase>());
        if (FAILED(setup_eye(gpu, d3d11, context, swapchain + e, 0, eyes.back().get(), ow, oh, rw, rh, 0))) return 1;
    }
    std::vector<UINT> all_rows(oh);
    for (UINT y = 0; y < oh; ++y) all_rows[y] = y;
    std::vector<std::uint32_t> color(static_cast<size_t>(ow) * oh);
    std::vector<float> depth;
    std::vector<std::uint16_t> motion;
    double sum_s[2][xrfg::kDlssgMaxOutputs]{}, sum_expected[2][xrfg::kDlssgMaxOutputs]{},
        sum_best[2][xrfg::kDlssgMaxOutputs]{}, sum_ab[2][xrfg::kDlssgMaxOutputs]{};
    int measured[2][xrfg::kDlssgMaxOutputs]{}, not_generated = 0;
    // A still start shifts every measured frame by its length.
    const int still = v.still_frames;
    for (int f = 0; f < kPanFrames + still; ++f) {
        const PanPose pose = pan_pose(v, f), previous_pose = pan_pose(v, f == 0 ? 0 : f - 1);
        for (int e = 0; e < 2; ++e) {
            EyeCase& eye = *eyes[e];
            const PanCamera cam = pan_camera(e, pose, v.symmetric), previous = pan_camera(e, previous_pose, v.symmetric);
            pan_color(cam, ow, oh, all_rows, color.data());
            HRESULT hr = upload(gpu, eye.backbuffer.Get(), color.data(), 4);
            // The bridge's Halton(2, 3) content shift in render pixels, as VaM renders.
            const double jx = v.jitter ? halton(f + 1, 2) - 0.5 : 0.0, jy = v.jitter ? halton(f + 1, 3) - 0.5 : 0.0;
            pan_guides(cam, previous, rw, rh, v.zero_motion, v.motion_scale, jx, jy, v.bad_region, &depth, &motion);
            context->UpdateSubresource(eye.record.depth.Get(), 0, nullptr, depth.data(), rw * 4, 0);
            context->UpdateSubresource(eye.record.motion_vectors.Get(), 0, nullptr, motion.data(), rw * 4, 0);
            OFXR_D3D11GuidePublicationV3& g = eye.record.data;
            pan_camera_data(v, cam, previous, &g);
            g.serial = static_cast<std::uint64_t>(f) + 1;
            g.reset = f == 0 || pan_reset(v, f) ? 1 : 0;
            g.jitter_x = static_cast<float>(jx);
            g.jitter_y = static_cast<float>(jy);
            g.frame_time_delta_ms = v.frame_ms;
            eye.guides.reset();
            if (SUCCEEDED(hr)) hr = eye.transfer.transfer(eye.record, static_cast<std::uint32_t>(f % 3), &eye.guides);
            if (FAILED(hr) || !eye.guides) {
                std::printf("FAIL pan frame %d eye %d upload/transfer 0x%08lx\n", f, e, static_cast<unsigned long>(hr));
                return 1;
            }
        }
        if (!eyes[0]->generator.can_generate(*eyes[0]->guides->eyes[0], outputs)) {
            std::printf("SKIP pan %ux: not supported by this GPU/runtime\n", outputs + 1);
            return 0;
        }
        std::uint32_t generated[2]{};
        for (int e = 0; e < 2; ++e) {
            EyeCase& eye = *eyes[e];
            const xrfg::DlssMotionVectorFrame& frame_guides = *eye.guides->eyes[0];
            HRESULT hr = eye.allocator->Reset();
            if (SUCCEEDED(hr)) hr = eye.list->Reset(eye.allocator.Get(), nullptr);
            if (FAILED(hr)) return 1;
            xrfg::DlssgRecordInput input{};
            input.backbuffer = eye.backbuffer.Get();
            input.guides = &frame_guides;
            input.outputs = outputs;
            for (std::uint32_t k = 0; k < xrfg::kDlssgMaxOutputs; ++k) input.destinations[k] = eye.destinations[k].Get();
            input.destination_state = D3D12_RESOURCE_STATE_COMMON;
            hr = eye.generator.record(eye.list.Get(), input, &generated[e]);
            const HRESULT closed = eye.list->Close();
            if (FAILED(hr) || FAILED(closed)) {
                std::printf("FAIL pan record eye %d frame %d 0x%08lx\n", e, f, static_cast<unsigned long>(FAILED(hr) ? hr : closed));
                return 1;
            }
            gpu.queue->Wait(frame_guides.guide_fence.Get(), frame_guides.guide_fence_value);
            ID3D12CommandList* lists[] = {eye.list.Get()};
            gpu.queue->ExecuteCommandLists(1, lists);
        }
        if (FAILED(gpu.wait_queue())) {
            std::printf("FAIL pan execute frame %d\n", f);
            return 1;
        }
        if (f < 1 + still) continue;
        bool steady = f >= kPanFirstMeasured + still;
        for (int back = 0; back < 3; ++back) steady = steady && !pan_reset(v, f - back);
        if (f == kPanFirstMeasured + still && outputs == 1 && v.yaw_step_deg + v.strafe_step_m > 0.0) {
            // The scoring itself, on the real frames: previous at 0, current at 1.
            for (int which = 0; which < 2; ++which) {
                pan_color(pan_camera(0, which == 0 ? previous_pose : pose, v.symmetric), ow, oh, all_rows, color.data());
                const PanScore s = pan_score(color.data(), ow, oh, 0, v, previous_pose, pose, which);
                std::printf("PANCHECK %s %s real frame: s=%.2f (expected %d), error %.2f\n", v.name,
                    which == 0 ? "previous" : "current", s.best_s, which, s.error_best);
            }
        }
        for (int e = 0; e < 2; ++e) {
            if (generated[e] != outputs) {
                ++not_generated;
                std::printf("INFO pan %s frame %d eye %d: %u of %u generated\n", v.name, f, e, generated[e], outputs);
                continue;
            }
            for (std::uint32_t k = 0; k < outputs; ++k) {
                std::vector<std::uint8_t> bytes;
                if (FAILED(read_texture(gpu, eyes[e]->destinations[k].Get(), 4, &bytes))) return 1;
                const auto* image = reinterpret_cast<const std::uint32_t*>(bytes.data());
                const double expected = static_cast<double>(k + 1) / (outputs + 1);
                const PanScore s = pan_score(image, ow, oh, e, v, previous_pose, pose, expected);
                std::printf("PAN %s %ux frame %d%s eye %d output %u: sits at s=%.2f (expected %.2f; 0 previous, 1 current); "
                            "error there %.2f, at the expected s %.2f; previous vs current %.2f\n",
                    v.name, outputs + 1, f, pan_reset(v, f) ? " (reset)" : "", e, k + 1, s.best_s, expected, s.error_best,
                    s.error_expected, s.error_ab);
                if (!steady) continue;
                sum_s[e][k] += s.best_s;
                sum_best[e][k] += s.error_best;
                sum_expected[e][k] += s.error_expected;
                sum_ab[e][k] += s.error_ab;
                ++measured[e][k];
                if (f == kPanImageFrame + still && e == 0 && v.reset_every == 0) {
                    const std::wstring stem = log_dir + L"\\pan-" + std::wstring(v.name, v.name + std::strlen(v.name)) +
                        L"-" + std::to_wstring(outputs + 1) + L"x-" + (lite ? L"lite" : L"default") + L"-out" + std::to_wstring(k + 1);
                    write_ppm(stem + L"-generated.ppm", image, ow, oh);
                    pan_color(pan_camera(e, pan_lerp(previous_pose, pose, expected), v.symmetric), ow, oh, all_rows, color.data());
                    write_ppm(stem + L"-truth.ppm", color.data(), ow, oh);
                }
            }
        }
    }
    for (int e = 0; e < 2; ++e)
        for (std::uint32_t k = 0; k < outputs; ++k) {
            if (measured[e][k] == 0) continue;
            const double n = measured[e][k];
            std::printf("PANSUM %s %ux %s eye %d output %u: mean s=%.2f (expected %.2f), error at expected %.2f (%.0f%% of previous vs current), best %.2f, %d frames\n",
                v.name, outputs + 1, lite ? "lite" : "default", e, k + 1, sum_s[e][k] / n,
                static_cast<double>(k + 1) / (outputs + 1), sum_expected[e][k] / n,
                sum_ab[e][k] > 0 ? 100.0 * sum_expected[e][k] / sum_ab[e][k] : 0.0, sum_best[e][k] / n, measured[e][k]);
        }
    if (not_generated) std::printf("INFO pan %s: %d pair(s) not fully generated\n", v.name, not_generated);
    return 0;
}

// ---------------------------------------------------------------------------
// Replay (modes replay / replay-lite <burst dir> [options]): the pairs of a
// capture burst (OFXR_DLSSG_CAPTURE, d3d12_dlssg.cpp) through DLSS-G again,
// one instance, the first pair as a reset. Options, comma-separated:
//   scale=<s>  motion vectors times s
//   shift=<n>  motion, depth and camera of the pair n later (n may be negative)
//   zero       no motion (all vectors 0)
// Each generated frame goes to <log dir>\replay-pair<k>-out<j>.bin (capture
// format), and its difference to DLSS-G's in-game output is printed.

struct CaptureFile {
    std::uint32_t width{}, height{}, format{}, bytes_per_pixel{};
    std::vector<std::uint8_t> data;
};

bool read_capture_file(const std::wstring& path, CaptureFile* out) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) return false;
    std::uint32_t header[8]{};
    bool ok = std::fread(header, sizeof(header), 1, file) == 1 && header[0] == 0x4358464FU && header[1] == 1U;
    if (ok) {
        out->width = header[2];
        out->height = header[3];
        out->format = header[4];
        out->bytes_per_pixel = header[5];
        out->data.resize(static_cast<size_t>(header[2]) * header[5] * header[6]);
        ok = std::fread(out->data.data(), 1, out->data.size(), file) == out->data.size();
    }
    std::fclose(file);
    return ok;
}

bool write_capture_file(const std::wstring& path, const CaptureFile& image) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) return false;
    const std::uint32_t header[8] = {0x4358464FU, 1U, image.width, image.height, image.format, image.bytes_per_pixel,
        image.height, 0U};
    const bool ok = std::fwrite(header, sizeof(header), 1, file) == 1 &&
                    std::fwrite(image.data.data(), 1, image.data.size(), file) == image.data.size();
    std::fclose(file);
    return ok;
}

// "name v1 v2 ..." per line.
bool read_capture_text(const std::wstring& path, std::vector<std::pair<std::string, std::vector<double>>>* out) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) return false;
    char line[2048];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        char* context = nullptr;
        char* token = strtok_s(line, " \r\n", &context);
        if (token == nullptr) continue;
        std::pair<std::string, std::vector<double>> entry{token, {}};
        while ((token = strtok_s(nullptr, " \r\n", &context)) != nullptr) entry.second.push_back(std::strtod(token, nullptr));
        out->push_back(std::move(entry));
    }
    std::fclose(file);
    return !out->empty();
}

const std::vector<double>* capture_value(const std::vector<std::pair<std::string, std::vector<double>>>& text,
    const char* name, size_t count) {
    for (const auto& entry : text)
        if (entry.first == name && entry.second.size() >= count) return &entry.second;
    return nullptr;
}

struct CapturePairFiles {
    std::vector<std::pair<std::string, std::vector<double>>> text;
    CaptureFile color, depth, motion, out[xrfg::kDlssgMaxOutputs];
};

double rgb_difference(const CaptureFile& a, const CaptureFile& b) {
    if (a.data.size() != b.data.size() || a.bytes_per_pixel != 4) return -1.0;
    double sum = 0.0;
    for (size_t i = 0; i < a.data.size(); i += 4)
        for (int k = 0; k < 3; ++k) sum += std::abs(static_cast<int>(a.data[i + k]) - static_cast<int>(b.data[i + k]));
    return sum / (static_cast<double>(a.data.size()) / 4.0 * 3.0);
}

int run_replay(Gpu& gpu, ID3D11Device* d3d11, ID3D11DeviceContext* context, const std::wstring& burst,
    const std::wstring& options, bool lite, const std::wstring& log_dir) {
    double scale = 1.0;
    int shift = 0;
    bool zero = false;
    for (size_t start = 0; start < options.size();) {
        size_t end = options.find(L',', start);
        if (end == std::wstring::npos) end = options.size();
        const std::wstring option = options.substr(start, end - start);
        if (option.rfind(L"scale=", 0) == 0) scale = std::wcstod(option.c_str() + 6, nullptr);
        else if (option.rfind(L"shift=", 0) == 0) shift = static_cast<int>(std::wcstol(option.c_str() + 6, nullptr, 10));
        else if (option == L"zero") zero = true;
        else if (!option.empty()) {
            std::wprintf(L"FAIL unknown replay option %s\n", option.c_str());
            return 1;
        }
        start = end + 1;
    }
    std::vector<CapturePairFiles> pairs;
    for (int k = 0;; ++k) {
        const std::wstring stem = burst + L"\\pair" + std::to_wstring(k);
        CapturePairFiles p;
        if (!read_capture_text(stem + L".txt", &p.text)) break;
        if (!read_capture_file(stem + L"-color.bin", &p.color) || !read_capture_file(stem + L"-depth.bin", &p.depth) ||
            !read_capture_file(stem + L"-motion.bin", &p.motion)) {
            std::wprintf(L"FAIL replay: pair %d files incomplete in %s\n", k, burst.c_str());
            return 1;
        }
        for (std::uint32_t j = 0; j < xrfg::kDlssgMaxOutputs; ++j)
            read_capture_file(stem + L"-out" + std::to_wstring(j + 1) + L".bin", &p.out[j]);
        pairs.push_back(std::move(p));
    }
    if (pairs.size() < 2) {
        std::wprintf(L"FAIL replay: fewer than two pairs in %s\n", burst.c_str());
        return 1;
    }
    const CapturePairFiles& first = pairs[0];
    const auto* outputs_value = capture_value(first.text, "outputs", 1);
    const std::uint32_t outputs = outputs_value ? static_cast<std::uint32_t>((*outputs_value)[0]) : 1U;
    const UINT ow = first.color.width, oh = first.color.height, rw = first.depth.width, rh = first.depth.height;
    if (first.color.bytes_per_pixel != 4 || first.depth.bytes_per_pixel != 4 || first.motion.bytes_per_pixel != 4 ||
        first.motion.width != rw || first.motion.height != rh || outputs < 1 || outputs > xrfg::kDlssgMaxOutputs) {
        std::printf("FAIL replay: unexpected formats (colour %u bpp, depth %u, motion %u, %u outputs)\n",
            first.color.bytes_per_pixel, first.depth.bytes_per_pixel, first.motion.bytes_per_pixel, outputs);
        return 1;
    }
    std::wprintf(L"--- replay %s (%s): %zu pairs, output %ux%u, render %ux%u, %ux; motion x%.3f, shift %d%s ---\n",
        burst.c_str(), lite ? L"lite" : L"default", pairs.size(), ow, oh, rw, rh, outputs + 1, scale, shift,
        zero ? L", zero motion" : L"");
    EyeCase eye;
    if (FAILED(setup_eye(gpu, d3d11, context, 0x9000, 0, &eye, ow, oh, rw, rh, 0))) return 1;
    std::vector<std::uint16_t> motion(static_cast<size_t>(rw) * rh * 2);
    for (size_t k = 0; k < pairs.size(); ++k) {
        const int source = static_cast<int>(k) + shift;
        if (source < 0 || source >= static_cast<int>(pairs.size())) {
            std::printf("REPLAY pair %zu: no pair %d to take guides from; skipped (history reset)\n", k, source);
            eye.generator.invalidate_history();
            continue;
        }
        const CapturePairFiles& p = pairs[k];
        const CapturePairFiles& guide = pairs[static_cast<size_t>(source)];
        HRESULT hr = upload(gpu, eye.backbuffer.Get(), p.color.data.data(), 4);
        std::memcpy(motion.data(), guide.motion.data.data(), motion.size() * 2);
        if (zero || scale != 1.0) {
            for (auto& value : motion) value = half(zero ? 0.0F : static_cast<float>(unhalf(value) * scale));
        }
        context->UpdateSubresource(eye.record.depth.Get(), 0, nullptr, guide.depth.data.data(), rw * 4, 0);
        context->UpdateSubresource(eye.record.motion_vectors.Get(), 0, nullptr, motion.data(), rw * 4, 0);
        OFXR_D3D11GuidePublicationV3& g = eye.record.data;
        const auto copy = [&](const char* name, float* target, size_t count) {
            if (const auto* v = capture_value(guide.text, name, count))
                for (size_t i = 0; i < count; ++i) target[i] = static_cast<float>((*v)[i]);
        };
        copy("clip_to_prev_clip", g.clip_to_prev_clip, 16);
        copy("prev_clip_to_clip", g.prev_clip_to_clip, 16);
        copy("view_to_clip", g.view_to_clip, 16);
        copy("clip_to_view", g.clip_to_view, 16);
        copy("camera_position", g.camera_position, 3);
        copy("camera_up", g.camera_up, 3);
        copy("camera_right", g.camera_right, 3);
        copy("camera_forward", g.camera_forward, 3);
        float jitter[2]{}, mvec[2]{1.0F, 1.0F}, camera[4]{0.1F, 1000.0F, 1.0F, 1.0F}, frame_ms[1]{16.6667F}, inverted[1]{1.0F};
        copy("jitter", jitter, 2);
        copy("mvec_scale", mvec, 2);
        copy("near_far_fov_aspect", camera, 4);
        copy("frame_time_delta_ms", frame_ms, 1);
        copy("depth_inverted", inverted, 1);
        g.jitter_x = jitter[0];
        g.jitter_y = jitter[1];
        g.motion_scale_x = mvec[0];
        g.motion_scale_y = mvec[1];
        g.camera_near = camera[0];
        g.camera_far = camera[1];
        g.camera_fov = camera[2];
        g.camera_aspect = camera[3];
        g.frame_time_delta_ms = frame_ms[0];
        g.depth_inverted = inverted[0] != 0.0F ? 1 : 0;
        g.serial = static_cast<std::uint64_t>(k) + 1;
        g.reset = k == 0 ? 1 : 0;
        eye.guides.reset();
        if (SUCCEEDED(hr)) hr = eye.transfer.transfer(eye.record, static_cast<std::uint32_t>(k % 3), &eye.guides);
        if (FAILED(hr) || !eye.guides) {
            std::printf("FAIL replay pair %zu upload/transfer 0x%08lx\n", k, static_cast<unsigned long>(hr));
            return 1;
        }
        const xrfg::DlssMotionVectorFrame& frame_guides = *eye.guides->eyes[0];
        if (!eye.generator.can_generate(frame_guides, outputs)) {
            std::printf("SKIP replay: %ux not supported by this GPU/runtime\n", outputs + 1);
            return 0;
        }
        hr = eye.allocator->Reset();
        if (SUCCEEDED(hr)) hr = eye.list->Reset(eye.allocator.Get(), nullptr);
        if (FAILED(hr)) return 1;
        xrfg::DlssgRecordInput input{};
        input.backbuffer = eye.backbuffer.Get();
        input.guides = &frame_guides;
        input.outputs = outputs;
        for (std::uint32_t j = 0; j < xrfg::kDlssgMaxOutputs; ++j) input.destinations[j] = eye.destinations[j].Get();
        input.destination_state = D3D12_RESOURCE_STATE_COMMON;
        std::uint32_t generated = 0;
        hr = eye.generator.record(eye.list.Get(), input, &generated);
        const HRESULT closed = eye.list->Close();
        if (FAILED(hr) || FAILED(closed)) {
            std::printf("FAIL replay record pair %zu 0x%08lx\n", k, static_cast<unsigned long>(FAILED(hr) ? hr : closed));
            return 1;
        }
        gpu.queue->Wait(frame_guides.guide_fence.Get(), frame_guides.guide_fence_value);
        ID3D12CommandList* lists[] = {eye.list.Get()};
        gpu.queue->ExecuteCommandLists(1, lists);
        if (FAILED(gpu.wait_queue())) {
            std::printf("FAIL replay execute pair %zu\n", k);
            return 1;
        }
        for (std::uint32_t j = 0; j < outputs; ++j) {
            CaptureFile result{ow, oh, first.color.format, 4, {}};
            if (FAILED(read_texture(gpu, eye.destinations[j].Get(), 4, &result.data))) return 1;
            write_capture_file(log_dir + L"\\replay-pair" + std::to_wstring(k) + L"-out" + std::to_wstring(j + 1) + L".bin", result);
            const double in_game = rgb_difference(result, p.out[j]);
            const double to_b = rgb_difference(result, p.color);
            const double to_a = k > 0 ? rgb_difference(result, pairs[k - 1].color) : -1.0;
            std::printf("REPLAY pair %zu out %u%s: |replay - in-game| %.2f, |replay - A| %.2f, |replay - B| %.2f\n",
                k, j + 1, generated == outputs ? "" : " (copy of B)", in_game, to_a, to_b);
        }
    }
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: xrfg_dlssg_smoke <runtime dir> <log dir> [bench | bench-lite]\n");
        return 2;
    }
    const std::wstring mode = argc > 3 ? argv[3] : L"";
    const bool packed_bench = mode == L"bench-packed" || mode == L"bench-packed-lite";
    const bool parallel_bench = mode == L"bench-parallel" || mode == L"bench-parallel-lite";
    const bool bench = mode == L"bench" || mode == L"bench-lite" || packed_bench || parallel_bench;
    const bool pan = mode == L"pan" || mode == L"pan-lite";
    const bool replay = mode == L"replay" || mode == L"replay-lite";
    const bool lite = mode == L"bench-lite" || mode == L"pan-lite" || mode == L"replay-lite" ||
        mode == L"bench-packed-lite" || mode == L"bench-parallel-lite";
    if (replay && argc < 5) {
        std::printf("usage: xrfg_dlssg_smoke <runtime dir> <log dir> replay|replay-lite <burst dir> [scale=<s>,shift=<n>,zero]\n");
        return 2;
    }
    // Before the first DLSS-G use: the generator reads it once per process.
    if (lite) SetEnvironmentVariableW(L"OFXR_DLSSG_LITE", L"1");
    SetEnvironmentVariableW(L"OFXR_DLSSG", L"1");
    SetEnvironmentVariableW(L"OFXR_DLSSG_RUNTIME_PATH", argv[1]);
    SetEnvironmentVariableW(L"OFXR_DLSSG_LOG", argv[2]);
    CreateDirectoryW(argv[2], nullptr);
    if (!xrfg::dlssg_requested()) return 2;

    Gpu gpu;
    ComPtr<IDXGIFactory6> factory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(factory.GetAddressOf()));
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                         IID_PPV_ARGS(gpu.adapter.ReleaseAndGetAddressOf())) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        gpu.adapter->GetDesc1(&desc);
        if (desc.VendorId == 0x10DE) {
            std::wprintf(L"GPU: %s\n", desc.Description);
            break;
        }
    }
    HRESULT hr = D3D12CreateDevice(gpu.adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(gpu.device.GetAddressOf()));
    if (FAILED(hr)) {
        std::printf("FAIL D3D12CreateDevice 0x%08lx\n", static_cast<unsigned long>(hr));
        return 1;
    }
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    gpu.device->CreateCommandQueue(&qd, IID_PPV_ARGS(gpu.queue.GetAddressOf()));
    gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(gpu.fence.GetAddressOf()));
    gpu.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(gpu.allocator.GetAddressOf()));
    gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(), nullptr,
        IID_PPV_ARGS(gpu.list.GetAddressOf()));
    gpu.list->Close();

    ComPtr<ID3D11Device> d3d11;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    hr = D3D11CreateDevice(gpu.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
        d3d11.GetAddressOf(), nullptr, context.GetAddressOf());
    if (FAILED(hr)) {
        std::printf("FAIL D3D11CreateDevice 0x%08lx\n", static_cast<unsigned long>(hr));
        return 1;
    }

    if (pan) {
        const PanVariant variants[] = {
            {"static", 0.0, 0.0, false, false, false, false},
            {"turn", 2.0, 0.0, false, false, false, false},
            {"turn-symmetric", 2.0, 0.0, true, false, false, false},
            {"turn-identity-matrices", 2.0, 0.0, false, true, false, false},
            {"turn-fov-degrees", 2.0, 0.0, false, false, true, false},
            {"turn-zero-motion", 2.0, 0.0, false, false, false, true},
            {"strafe", 0.0, 0.02, false, false, false, false},
            {"turn-jitter", 2.0, 0.0, false, false, false, false, 1.0, true},
            {"turn-25ms", 2.0, 0.0, false, false, false, false, 1.0, false, 25.0F},
            {"turn-motion-80pct", 2.0, 0.0, false, false, false, false, 0.8},
            {"turn-motion-120pct", 2.0, 0.0, false, false, false, false, 1.2},
            {"turn-resets", 2.0, 0.0, false, false, false, false, 1.0, false, 16.6667F, 5},
            {"turn-vam", 2.0, 0.0, false, false, false, false, 1.0, true, 25.0F, 5},
            // Real head turns: 120 / 200 / 320 deg/s at VaM's 40 fps (3x at 120 Hz).
            {"turn-3deg", 3.0, 0.0, false, false, false, false, 1.0, true, 25.0F},
            {"turn-5deg", 5.0, 0.0, false, false, false, false, 1.0, true, 25.0F},
            {"turn-8deg", 8.0, 0.0, false, false, false, false, 1.0, true, 25.0F},
            {"turn-bad-pillars", 5.0, 0.0, false, false, false, false, 1.0, true, 25.0F, 0, 1},
            {"turn-bad-centre", 5.0, 0.0, false, false, false, false, 1.0, true, 25.0F, 0, 2},
            // Sample capture (OFXR_DLSSG_CAPTURE): a turn after the head was still starts a
            // burst; after too short a pause it does not.
            {"turn-after-still", 2.0, 0.0, false, false, false, false, 1.0, false, 16.6667F, 0, 0, 20},
            {"turn-after-short-still", 2.0, 0.0, false, false, false, false, 1.0, false, 16.6667F, 0, 0, 8},
        };
        // argv[4]: optional comma-separated variant names.
        const std::wstring only = argc > 4 ? L"," + std::wstring(argv[4]) + L"," : L"";
        int failures = 0;
        std::uint64_t swapchain = 0x7000;
        for (const PanVariant& v : variants) {
            if (!only.empty() && only.find(L"," + std::wstring(v.name, v.name + std::strlen(v.name)) + L",") == std::wstring::npos)
                continue;
            for (std::uint32_t outputs : {1U, 2U, 3U}) {
                failures += run_pan(gpu, d3d11.Get(), context.Get(), v, lite, outputs, argv[2], swapchain);
                swapchain += 0x10;
            }
        }
        std::printf(failures == 0 ? "RESULT: PAN DONE\n" : "RESULT: PAN %d FAILURE(S)\n", failures);
        return failures == 0 ? 0 : 1;
    }
    if (replay) {
        const int failures = run_replay(gpu, d3d11.Get(), context.Get(), argv[4], argc > 5 ? argv[5] : L"", lite, argv[2]);
        std::printf(failures == 0 ? "RESULT: REPLAY DONE\n" : "RESULT: REPLAY FAILED\n");
        return failures;
    }
    if (packed_bench || parallel_bench) {
        // Two instances on one queue, then the packed canvas or the eyes on
        // two queues, at 2X and 4X.
        int failures = 0;
        for (std::uint32_t outputs : {1U, 3U}) {
            failures += run_bench(gpu, d3d11.Get(), context.Get(), outputs, lite, false);
            failures += run_bench(gpu, d3d11.Get(), context.Get(), outputs, lite, packed_bench, parallel_bench);
        }
        std::printf(failures == 0 ? "RESULT: BENCH DONE\n" : "RESULT: BENCH %d FAILURE(S)\n", failures);
        return failures == 0 ? 0 : 1;
    }
    if (bench) {
        int failures = run_bench(gpu, d3d11.Get(), context.Get(), 1, lite);
        failures += run_bench(gpu, d3d11.Get(), context.Get(), 2, lite);
        failures += run_bench(gpu, d3d11.Get(), context.Get(), 3, lite);
        std::printf(failures == 0 ? "RESULT: BENCH DONE\n" : "RESULT: BENCH %d FAILURE(S)\n", failures);
        return failures == 0 ? 0 : 1;
    }
    for (DXGI_FORMAT format : {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R32G32_FLOAT,
             DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT}) {
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support{format};
        d3d11->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support, sizeof(support));
        std::printf("D3D11 format %u shareable=%d\n", static_cast<unsigned>(format),
            (support.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_SHAREABLE) != 0 ? 1 : 0);
    }
    int failures = run_case(gpu, d3d11.Get(), context.Get(), 1, 1, 0x1000);
    failures += run_case(gpu, d3d11.Get(), context.Get(), 2, 1, 0x2000);
    failures += run_case(gpu, d3d11.Get(), context.Get(), 1, 2, 0x3000);
    failures += run_case(gpu, d3d11.Get(), context.Get(), 3, 1, 0x4000);
    std::printf(failures == 0 ? "RESULT: PASS\n" : "RESULT: %d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
