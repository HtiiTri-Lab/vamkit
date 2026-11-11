// MIT License
//
// Copyright(c) 2025-2026 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// SimXR simulated Quest 3 (based on OVRNull's NullDriver).
//
// Display model. A vsync thread ticks at the display rate on a fixed grid. At
// each vsync the "headset" shows the newest submitted frame that was submitted
// at least latchMs before that vsync and whose GPU work has finished (a D3D11
// fence signalled at submission), as a real compositor would. Otherwise the
// previous frame is shown again (a repeat). A submitted frame that a newer one
// replaced before any vsync showed it is dropped. Every vsync goes to
// simxr-vsync.csv and every frame to simxr-frames.csv (SIMXR_LOG_DIR), with the
// angle between the head orientation the frame was rendered for and the true
// head orientation at that vsync (pose_err_deg): 0 for a frame shown on time
// with an exact prediction, the head's motion meanwhile for a late or repeated one.
//
// Pacing. ovr_WaitToBeginFrame returns at the next vsync, so an application (or
// VDXR's submission thread) that misses a vsync waits for the one after: the
// same half-rate fall-back a real compositor imposes. After the vsync it holds
// the frame back until at most SIMXR_MAX_QUEUED_FRAMES (default 1) submitted
// frames still have GPU work running (the fence again): a GPU-bound application
// is throttled to its GPU's rate instead of queueing frames that would reach the
// display several refreshes late. (Holding before the vsync wait, as SimXR-v2/v3
// did, turned a short wait that crossed a vsync into a whole lost refresh: an
// application with 7 ms of GPU work per frame showed 105 instead of 113 fps.)
// That a compositor throttles like this is an assumption (VDXR itself does not;
// what Virtual Desktop's runtime does behind ovr_WaitToBeginFrame is not
// visible); SIMXR_MAX_QUEUED_FRAMES=0 turns it off.
//
// Capture. On request (control port), the eye images of the next submitted
// frames are drawn into an RGBA8 target (crop/scale), copied to staging,
// read back without stalling and written by worker threads as PNG or raw RGBA,
// with capture.jsonl beside them (sequence, frame index, render pose).

#include "pch.h"

#include "constantsbuffer.h"
#include "log.h"
#include "driver.h"
#include "sim.h"
#include "utils.h"

#include "ReprojectVS.h"
#include "ReprojectPS.h"
#include "CaptureVS.h"
#include "CapturePS.h"

using namespace ovrnull::driver;
using namespace ovrnull::log;
using namespace ovrnull::utils;

namespace {

    struct CaptureConstants {
        float sourceOffset[2];
        float sourceExtent[2];
        uint32_t flipY;
        uint32_t padding[3];
    };

    struct FrameRecord {
        uint64_t seq{0};
        long long frameIndex{0};
        double submitTime{0};
        uint64_t fenceValue{0};
        double completeTime{-1};
        int64_t firstVsync{-1};
        int displayCount{0};
        bool dropped{false};
        bool hasPose{false};
        ovrPosef pose[2]{};
        double sensorTime{0};
        int layers{0};
        int eyeWidth{0}, eyeHeight{0};
        float queueWaitMs{0}; // held back by the GPU queue limit before this frame began
    };

    struct PendingReadback {
        ComPtr<ID3D11Texture2D> staging;
        uint32_t width{0}, height{0};
        std::filesystem::path path;
        bool png{true};
        int captureId{0};
        int polls{0};
    };

    struct WriteJob {
        std::filesystem::path path;
        uint32_t width{0}, height{0};
        std::vector<uint8_t> rgba;
        bool png{true};
        int captureId{0};
    };

    float AngleBetweenDeg(const OVR::Quatf& a, const OVR::Quatf& b) {
        const float dot = std::min(1.f, std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w));
        return (float)(2.0 * acos(dot) * 180.0 / M_PI);
    }

    const char* FormatName(DXGI_FORMAT format) {
        switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8X8_UNORM:
            return "B8G8R8X8_UNORM";
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            return "B8G8R8X8_UNORM_SRGB";
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R11G11B10_FLOAT:
            return "R11G11B10_FLOAT";
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return "R10G10B10A2_UNORM";
        default:
            return "other";
        }
    }

    bool IsSrgb(DXGI_FORMAT format) {
        return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
               format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    }

    bool IsFloat(DXGI_FORMAT format) {
        return format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R11G11B10_FLOAT ||
               format == DXGI_FORMAT_R32G32B32A32_FLOAT;
    }

    class SimDriver : public IDriver, public simxr::IDriverControl {
      private:
        static constexpr int k_SwapchainLength = 3;
        struct Swapchain {
            ComPtr<ID3D11Texture2D> textures[k_SwapchainLength];
            ComPtr<ID3D11ShaderResourceView> SRVs[k_SwapchainLength];
            ComPtr<ID3D11RenderTargetView> RTVs[k_SwapchainLength];
            ovrTextureSwapChainDesc desc{};
            DXGI_FORMAT viewFormat{DXGI_FORMAT_UNKNOWN};
            int length{k_SwapchainLength};
            // OVR semantics: the application renders into `current`, then commits it.
            int current{0};
            int lastCommitted{-1};
        };

      public:
        SimDriver() : m_config(simxr::GetConfig()) {
            {
                ComPtr<IDXGIFactory6> factory;
                CreateDXGIFactory2(0, IID_PPV_ARGS(factory.ReleaseAndGetAddressOf()));
                ComPtr<IDXGIAdapter1> adapter;
                factory->EnumAdapterByGpuPreference(
                    0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.ReleaseAndGetAddressOf()));
                DXGI_ADAPTER_DESC1 desc;
                adapter->GetDesc1(&desc);
                m_adapterLuid = desc.AdapterLuid;
                char name[128]{};
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
                m_adapterName = name;
            }
            m_displayRate = m_config.displayRate;
            m_period = 1.0 / m_displayRate;
            m_eyePose[ovrEye_Left].Position.x = -m_config.ipd / 2;
            m_eyePose[ovrEye_Right].Position.x = m_config.ipd / 2;
            m_eyePose[ovrEye_Left].Orientation.w = m_eyePose[ovrEye_Right].Orientation.w = 1.f;
            m_eyeFov[ovrEye_Left] = m_config.fov[ovrEye_Left];
            m_eyeFov[ovrEye_Right] = m_config.fov[ovrEye_Right];
            m_recommendedResolution = {m_config.width, m_config.height};
            m_nextFramePredictedDisplayTime = simxr::Now() + m_period;

            simxr::Log("SimXR headset created: %s, %dx%d per eye, %.1f Hz, IPD %.1f mm, head %.2f m, adapter %s, logs %ls\n",
                       GetModelNumber().c_str(),
                       m_config.width,
                       m_config.height,
                       m_displayRate,
                       m_config.ipd * 1000,
                       m_config.headHeight,
                       m_adapterName.c_str(),
                       m_config.logDir.c_str());

            if (m_config.vsyncLog) {
                m_vsyncCsv.open(m_config.logDir / L"simxr-vsync.csv", std::ios_base::app);
                m_framesCsv.open(m_config.logDir / L"simxr-frames.csv", std::ios_base::app);
                m_vsyncCsv << "vsync,t_ms,seq,new,repeat_streak,age_ms,gpu_late,dropped_before,pose_err_deg,head_yaw,head_pitch\n";
                m_framesCsv << "seq,frame_index,submit_ms,complete_ms,first_vsync,display_count,dropped,layers,eye_w,eye_h,"
                               "pos_x,pos_y,pos_z,yaw,pitch,roll,sensor_ms,queue_wait_ms\n";
            }

            for (int i = 0; i < 3; i++) {
                m_writers.emplace_back([this] { WriterThread(); });
            }
            m_vsyncThread = std::thread([this] { VsyncThread(); });

            simxr::SetLiveDriver(this);
        }

        ~SimDriver() override {
            simxr::ClearLiveDriver(this);

            m_terminate = true;
            if (m_vsyncThread.joinable()) {
                m_vsyncThread.join();
            }
            if (m_queueEvent) {
                CloseHandle(m_queueEvent);
                m_queueEvent = nullptr;
            }
            {
                std::unique_lock lock(m_writeMutex);
                m_writeCv.notify_all();
            }
            for (auto& writer : m_writers) {
                if (writer.joinable()) {
                    writer.join();
                }
            }
            FlushCsv(true);

            std::unique_lock lock(m_swapchainMutex);
            for (void* swapchain : m_swapchains) {
                delete (Swapchain*)swapchain;
            }
            m_swapchains.clear();
            simxr::Log("SimXR headset destroyed after %llu submitted frames\n", (unsigned long long)m_nextSeq);
        }

        void SetSubmissionDevice(ID3D11Device* device) override {
            if (m_submissionDevice) {
                return;
            }
            m_submissionDevice = device;
            m_submissionDevice->GetImmediateContext(m_submissionContext.ReleaseAndGetAddressOf());

            winrt::check_hresult(m_submissionDevice->CreateVertexShader(
                k_ReprojectVS, sizeof(k_ReprojectVS), nullptr, m_reprojectVS.ReleaseAndGetAddressOf()));
            winrt::check_hresult(m_submissionDevice->CreatePixelShader(
                k_ReprojectPS, sizeof(k_ReprojectPS), nullptr, m_reprojectPS.ReleaseAndGetAddressOf()));
            winrt::check_hresult(m_submissionDevice->CreateVertexShader(
                k_CaptureVS, sizeof(k_CaptureVS), nullptr, m_captureVS.ReleaseAndGetAddressOf()));
            winrt::check_hresult(m_submissionDevice->CreatePixelShader(
                k_CapturePS, sizeof(k_CapturePS), nullptr, m_capturePS.ReleaseAndGetAddressOf()));
            {
                D3D11_BUFFER_DESC desc{};
                desc.ByteWidth = ((sizeof(ConstantsBuffer) + 15) / 16) * 16;
                desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                desc.Usage = D3D11_USAGE_DYNAMIC;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                winrt::check_hresult(
                    m_submissionDevice->CreateBuffer(&desc, nullptr, m_constantsBuffer.ReleaseAndGetAddressOf()));
                desc.ByteWidth = ((sizeof(CaptureConstants) + 15) / 16) * 16;
                winrt::check_hresult(
                    m_submissionDevice->CreateBuffer(&desc, nullptr, m_captureConstants.ReleaseAndGetAddressOf()));
            }
            {
                D3D11_DEPTH_STENCIL_DESC desc{};
                winrt::check_hresult(
                    m_submissionDevice->CreateDepthStencilState(&desc, m_noDepthTestState.ReleaseAndGetAddressOf()));
            }
            {
                D3D11_BLEND_DESC desc{};
                desc.RenderTarget[0].BlendEnable = TRUE;
                desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
                desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
                desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
                desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
                desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
                desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
                desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
                winrt::check_hresult(m_submissionDevice->CreateBlendState(&desc, m_blendState.ReleaseAndGetAddressOf()));
                D3D11_BLEND_DESC opaque{};
                opaque.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
                winrt::check_hresult(m_submissionDevice->CreateBlendState(&opaque, m_opaqueState.ReleaseAndGetAddressOf()));
            }
            {
                D3D11_RASTERIZER_DESC desc{};
                desc.FillMode = D3D11_FILL_SOLID;
                desc.CullMode = D3D11_CULL_NONE;
                desc.DepthClipEnable = TRUE;
                winrt::check_hresult(
                    m_submissionDevice->CreateRasterizerState(&desc, m_rasterizerState.ReleaseAndGetAddressOf()));
            }
            {
                D3D11_SAMPLER_DESC desc{};
                desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
                desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
                desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
                desc.MaxAnisotropy = 1;
                desc.MinLOD = D3D11_MIP_LOD_BIAS_MIN;
                desc.MaxLOD = D3D11_MIP_LOD_BIAS_MAX;
                winrt::check_hresult(
                    m_submissionDevice->CreateSamplerState(&desc, m_linearSampler.ReleaseAndGetAddressOf()));
                desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
                winrt::check_hresult(
                    m_submissionDevice->CreateSamplerState(&desc, m_pointSampler.ReleaseAndGetAddressOf()));
            }

            // The fence that tells the display model when a frame's GPU work is done.
            ComPtr<ID3D11Device5> device5;
            if (SUCCEEDED(m_submissionDevice.As(&device5)) && SUCCEEDED(m_submissionContext.As(&m_submissionContext4)) &&
                SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.ReleaseAndGetAddressOf())))) {
                simxr::Log("Submission device ready (GPU completion tracked with a D3D11 fence)\n");
            } else {
                m_fence.Reset();
                simxr::Log("Submission device ready WITHOUT a D3D11 fence: frames count as GPU-complete at submission\n");
            }

            simxr::EnsureControlServer();
        }

        void* CreateSwapchain(const ovrTextureSwapChainDesc& ovrDesc) override {
            const DXGI_FORMAT format = ToDxgiTextureFormat(ovrDesc.Format);
            const bool isDepthSwapchain = (ovrDesc.BindFlags & ovrTextureBind_DX_DepthStencil) || IsDepthFormat(format);

            D3D11_TEXTURE2D_DESC desc{};
            desc.Format = format;
            // Per OVR documentation, depth swapchains are always typeless.
            if ((ovrDesc.MiscFlags & ovrTextureMisc_DX_Typeless) || isDepthSwapchain) {
                desc.Format = GetTypelessFormat(desc.Format);
            }
            desc.Width = ovrDesc.Width;
            desc.Height = ovrDesc.Height;
            desc.SampleDesc.Count = ovrDesc.SampleCount;
            desc.MipLevels = ovrDesc.MipLevels;
            desc.ArraySize = ovrDesc.ArraySize;
            desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
            if (ovrDesc.BindFlags & ovrTextureBind_DX_RenderTarget) {
                desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
            }
            if (ovrDesc.BindFlags & ovrTextureBind_DX_UnorderedAccess) {
                desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
            }
            if (ovrDesc.BindFlags & ovrTextureBind_DX_DepthStencil) {
                desc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
            }
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            if (ovrDesc.Type == ovrTexture_Cube) {
                desc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
            }
            if (ovrDesc.MiscFlags & ovrTextureMisc_AllowGenerateMips) {
                desc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
            }

            Swapchain* swapchain = new Swapchain;
            swapchain->desc = ovrDesc;
            swapchain->viewFormat = format;
            swapchain->length = ovrDesc.StaticImage ? 1 : k_SwapchainLength;

            for (int i = 0; i < swapchain->length; i++) {
                winrt::check_hresult(
                    m_submissionDevice->CreateTexture2D(&desc, nullptr, swapchain->textures[i].ReleaseAndGetAddressOf()));

                if (!isDepthSwapchain && ovrDesc.Type == ovrTexture_2D) {
                    if (ovrDesc.BindFlags & ovrTextureBind_DX_RenderTarget) {
                        D3D11_RENDER_TARGET_VIEW_DESC rtv{};
                        rtv.ViewDimension = ovrDesc.ArraySize > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DARRAY
                                                                  : D3D11_RTV_DIMENSION_TEXTURE2D;
                        rtv.Texture2DArray.ArraySize = ovrDesc.ArraySize;
                        rtv.Format = format;
                        if (ovrDesc.SampleCount == 1) {
                            m_submissionDevice->CreateRenderTargetView(
                                swapchain->textures[i].Get(), &rtv, swapchain->RTVs[i].ReleaseAndGetAddressOf());
                        }
                    }
                    if (ovrDesc.SampleCount == 1) {
                        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
                        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                        srv.Format = format;
                        srv.Texture2D.MipLevels = (UINT)-1;
                        m_submissionDevice->CreateShaderResourceView(
                            swapchain->textures[i].Get(), &srv, swapchain->SRVs[i].ReleaseAndGetAddressOf());
                    }
                }
            }
            {
                std::unique_lock lock(m_swapchainMutex);
                m_swapchains.insert(swapchain);
                m_swapchainsCreated++;
            }
            if (!(ovrDesc.StaticImage && ovrDesc.Width <= 128)) {
                simxr::Log("Swapchain %dx%d x%d %s%s%s samples=%d\n",
                           ovrDesc.Width,
                           ovrDesc.Height,
                           ovrDesc.ArraySize,
                           ToString(ovrDesc.Format),
                           isDepthSwapchain ? " depth" : "",
                           ovrDesc.StaticImage ? " static" : "",
                           ovrDesc.SampleCount);
            }
            return swapchain;
        }

        void DestroySwapchain(void* swapchain) override {
            std::unique_lock lock(m_swapchainMutex);
            if (m_swapchains.erase(swapchain)) {
                delete (Swapchain*)swapchain;
            }
        }

        ovrTextureSwapChainDesc GetSwapchainDesc(void* swapchain) const override {
            std::shared_lock lock(m_swapchainMutex);
            ovrTextureSwapChainDesc desc{};
            if (m_swapchains.count(swapchain)) {
                desc = ((Swapchain*)swapchain)->desc;
            }
            return desc;
        }

        int GetSwapchainLength(void* swapchain) const override {
            std::shared_lock lock(m_swapchainMutex);
            return m_swapchains.count(swapchain) ? ((Swapchain*)swapchain)->length : 0;
        }

        ID3D11Texture2D* GetSwapchainImage(void* swapchain, int index) const override {
            std::shared_lock lock(m_swapchainMutex);
            if (m_swapchains.count(swapchain)) {
                Swapchain* object = (Swapchain*)swapchain;
                if (index >= 0 && index < object->length) {
                    return object->textures[index].Get();
                }
            }
            return nullptr;
        }

        int GetSwapchainImageIndex(void* swapchain) const override {
            std::shared_lock lock(m_swapchainMutex);
            return m_swapchains.count(swapchain) ? ((Swapchain*)swapchain)->current : -1;
        }

        bool CommitSwapchainImage(void* swapchain) override {
            std::shared_lock lock(m_swapchainMutex);
            if (!m_swapchains.count(swapchain)) {
                return false;
            }
            Swapchain* object = (Swapchain*)swapchain;
            object->lastCommitted = object->current;
            object->current = (object->current + 1) % object->length;
            return true;
        }

        void SetMirrorTexture(void* swapchain) override {
            if (swapchain) {
                std::shared_lock lock(m_swapchainMutex);
                if (m_swapchains.count(swapchain)) {
                    Swapchain* object = (Swapchain*)swapchain;
                    m_mirrorRTV = object->RTVs[0];
                    m_mirrorWidth = object->desc.Width;
                    m_mirrorHeight = object->desc.Height;
                }
            } else {
                m_mirrorRTV.Reset();
            }
        }

        double GetPredictedDisplayTime(long long frameIndex) const override {
            std::unique_lock lock(m_frameMutex);
            if (frameIndex <= m_lastWaitedFrame) {
                return m_nextFramePredictedDisplayTime;
            }
            // Not released yet: it will be at the next vsync and shown one refresh later. Not the index
            // distance: VDXR's submission thread sometimes waits on the same index twice and skips one.
            return m_nextFramePredictedDisplayTime + m_period;
        }

        void WaitForVsync(long long frameIndex) override {
            WaitForVsyncOnly(frameIndex);
            ThrottleGpuQueue();
        }

        void WaitForVsyncOnly(long long frameIndex) {
            std::unique_lock lock(m_frameMutex);
            // Only wait when a future frame is being waited on (the application never waits beyond N+1).
            if (frameIndex > m_lastWaitedFrame) {
                using namespace std::chrono_literals;
                // The vsync thread records the released frame together with the new vsync time, so
                // GetPredictedDisplayTime() never sees one updated without the other.
                m_pendingWaitFrame = std::max(m_pendingWaitFrame, frameIndex);
                const auto lastSignaledVsync = m_lastSignaledVsync;
                if (!m_frameVsync.wait_for(lock, 100ms, [&]() { return m_lastSignaledVsync > lastSignaledVsync; })) {
                    m_lastWaitedFrame = std::max(m_lastWaitedFrame, frameIndex);
                }
            }
        }

        // The GPU queue limit (see Pacing above): frame N begins once the frame submitted
        // maxQueuedFrames before the latest one has finished on the GPU (100 ms at most).
        void ThrottleGpuQueue() {
            const int limit = m_config.maxQueuedFrames;
            if (limit <= 0 || !m_fence) {
                return;
            }
            const uint64_t submitted = m_fenceValue.load();
            if (submitted <= (uint64_t)limit) {
                return;
            }
            const uint64_t target = submitted - (uint64_t)limit;
            if (m_fence->GetCompletedValue() >= target) {
                return;
            }
            if (!m_queueEvent) {
                m_queueEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            }
            const double start = simxr::Now();
            if (m_queueEvent && SUCCEEDED(m_fence->SetEventOnCompletion(target, m_queueEvent))) {
                WaitForSingleObject(m_queueEvent, 100);
            }
            const float waited = (float)((simxr::Now() - start) * 1000);
            std::unique_lock lock(m_frameMutex);
            m_pendingQueueWaitMs += waited;
            m_stats.queueWaits++;
            m_stats.queueWaitMs.push_back(waited);
        }

        bool SubmitFrame(long long frameIndex, const std::vector<const ovrLayerHeader*>& layers) override {
            FrameRecord record;
            record.frameIndex = frameIndex;
            record.layers = (int)layers.size();

            std::shared_lock swapchainLock(m_swapchainMutex);

            // The projection layer: what the frame was rendered for.
            const ovrLayerEyeFov* eyeFov = nullptr;
            for (const ovrLayerHeader* header : layers) {
                if (header && (header->Type == ovrLayerType_EyeFov || header->Type == ovrLayerType_EyeFovDepth)) {
                    eyeFov = &((const ovrLayer_Union*)header)->EyeFov;
                    break;
                }
            }
            if (eyeFov) {
                record.hasPose = true;
                record.pose[0] = eyeFov->RenderPose[0];
                record.pose[1] = eyeFov->RenderPose[1];
                record.sensorTime = eyeFov->SensorSampleTime;
                record.eyeWidth = eyeFov->Viewport[0].Size.w;
                record.eyeHeight = eyeFov->Viewport[0].Size.h;
            }

            if (m_submissionContext) {
                if (m_mirrorRTV) {
                    RenderMirror(layers);
                }
                if (eyeFov) {
                    MaybeCapture(*eyeFov, record);
                }
                if (m_fence) {
                    record.fenceValue = ++m_fenceValue;
                    m_submissionContext4->Signal(m_fence.Get(), record.fenceValue);
                }
                m_submissionContext->Flush();
                PollReadbacks();
            }
            swapchainLock.unlock();

            {
                std::unique_lock lock(m_frameMutex);
                record.seq = ++m_nextSeq;
                record.submitTime = simxr::Now();
                record.queueWaitMs = m_pendingQueueWaitMs;
                m_pendingQueueWaitMs = 0;
                if (m_lastSubmitTime > 0) {
                    m_stats.submitIntervalsMs.push_back((float)((record.submitTime - m_lastSubmitTime) * 1000));
                }
                m_lastSubmitTime = record.submitTime;
                m_stats.submitted++;
                m_frames.push_back(record);
            }
            return true;
        }

        std::string GetManufacturerName() const override {
            return "Oculus";
        }

        std::string GetModelNumber() const override {
            return "Meta Quest 3";
        }

        LUID GetAdapterLuid() const override {
            return m_adapterLuid;
        }

        float GetDisplayRate() const override {
            return m_displayRate;
        }

        ovrSizei GetRecommendedResolution() const override {
            return m_recommendedResolution;
        }

        ovrFovPort GetEyeFov(ovrEyeType eye) const override {
            return m_eyeFov[eye];
        }

        ovrPosef GetEyePose(ovrEyeType eye) const override {
            return m_eyePose[eye];
        }

        void SetStageTracking(bool useStage) override {
            m_useStageTracking = useStage;
        }

        bool IsStageTracking() const override {
            return m_useStageTracking;
        }

        float GetEyeHeight() const override {
            return m_config.headHeight;
        }

        bool IsAswActive() const override {
            return false;
        }

        ovrPoseStatef GetHmdPose(double absTime) const override {
            return simxr::State::Get().HeadPose(absTime, m_useStageTracking);
        }

        bool HasController(ovrHandType side) const override {
            return simxr::State::Get().HasController(side);
        }

        ovrPoseStatef GetControllerPose(ovrHandType side, double absTime) const override {
            return simxr::State::Get().ControllerPose(side, absTime, m_useStageTracking);
        }

        ovrInputState GetControllerButtons() const override {
            return simxr::State::Get().Input(simxr::Now());
        }

        void SetControllerVibration(ovrHandType side, float frequency, float amplitude) override {
        }

        // ------------------------------------------------------ IDriverControl

        std::string StatusJson() const override {
            const double now = simxr::Now();
            std::string swapchains;
            int count = 0;
            {
                std::shared_lock lock(m_swapchainMutex);
                for (void* handle : m_swapchains) {
                    const Swapchain* s = (const Swapchain*)handle;
                    if (s->desc.StaticImage && s->desc.Width <= 128) {
                        continue;
                    }
                    char item[160];
                    sprintf_s(item,
                              "%s{\"w\":%d,\"h\":%d,\"array\":%d,\"format\":\"%s\",\"static\":%s}",
                              count++ ? "," : "",
                              s->desc.Width,
                              s->desc.Height,
                              s->desc.ArraySize,
                              FormatName(s->viewFormat),
                              s->desc.StaticImage ? "true" : "false");
                    swapchains += item;
                }
            }
            std::unique_lock lock(m_frameMutex);
            const FrameRecord* last = m_frames.empty() ? nullptr : &m_frames.back();
            char text[1024];
            sprintf_s(text,
                      "\"device\":{\"model\":\"%s\",\"adapter\":\"%s\",\"display_hz\":%.3f,\"eye_w\":%d,\"eye_h\":%d,"
                      "\"ipd_mm\":%.2f,\"head_height_m\":%.3f,\"floor_origin\":%s,\"gpu_fence\":%s,\"mirror\":%s,"
                      "\"max_queued_frames\":%d},"
                      "\"frames\":{\"submitted\":%llu,\"vsyncs\":%llu,\"last_submit_age_ms\":%.3f,"
                      "\"last_frame_index\":%lld,\"last_eye_w\":%d,\"last_eye_h\":%d,\"displayed_seq\":%llu},"
                      "\"uptime_s\":%.3f,\"log_dir\":\"%s\"",
                      GetModelNumber().c_str(),
                      simxr::JsonEscape(m_adapterName).c_str(),
                      m_displayRate,
                      m_recommendedResolution.w,
                      m_recommendedResolution.h,
                      m_config.ipd * 1000,
                      m_config.headHeight,
                      m_useStageTracking ? "true" : "false",
                      m_fence ? "true" : "false",
                      m_mirrorRTV ? "true" : "false",
                      m_config.maxQueuedFrames,
                      (unsigned long long)m_nextSeq,
                      (unsigned long long)m_vsyncIndex,
                      last ? (now - last->submitTime) * 1000 : -1.0,
                      last ? last->frameIndex : -1LL,
                      last ? last->eyeWidth : 0,
                      last ? last->eyeHeight : 0,
                      (unsigned long long)m_displayedSeq,
                      now - simxr::StartTime(),
                      simxr::JsonEscape(std::filesystem::path(m_config.logDir).u8string()).c_str());
            return std::string(text) + ",\"swapchains\":[" + swapchains + "]";
        }

        std::string StatsJson(bool reset) override {
            simxr::Stats stats;
            const double now = simxr::Now();
            {
                std::unique_lock lock(m_frameMutex);
                stats = m_stats;
                if (reset) {
                    m_stats = simxr::Stats{};
                    m_stats.since = now;
                    m_repeatStreak = 0;
                }
            }
            auto percentiles = [](std::vector<float> values) {
                if (values.empty()) {
                    return std::string("null");
                }
                std::sort(values.begin(), values.end());
                auto at = [&](double p) { return values[std::min(values.size() - 1, (size_t)(p * (values.size() - 1) + 0.5))]; };
                double sum = 0;
                for (float v : values) {
                    sum += v;
                }
                char text[256];
                sprintf_s(text,
                          "{\"n\":%zu,\"mean\":%.4f,\"p50\":%.4f,\"p95\":%.4f,\"p99\":%.4f,\"max\":%.4f,\"min\":%.4f}",
                          values.size(),
                          sum / values.size(),
                          at(0.5),
                          at(0.95),
                          at(0.99),
                          values.back(),
                          values.front());
                return std::string(text);
            };
            const double window = stats.since > 0 ? now - stats.since : now - simxr::StartTime();
            char text[640];
            sprintf_s(text,
                      "\"window_s\":%.3f,\"vsyncs\":%llu,\"submitted\":%llu,\"new_frames\":%llu,\"repeats\":%llu,"
                      "\"dropped\":%llu,\"gpu_late\":%llu,\"longest_repeat_streak\":%llu,\"simulator_late\":%llu,"
                      "\"displayed_fps\":%.3f,\"submitted_fps\":%.3f,\"display_hz\":%.3f,\"gpu_queue_waits\":%llu",
                      window,
                      (unsigned long long)stats.vsyncs,
                      (unsigned long long)stats.submitted,
                      (unsigned long long)stats.newFrames,
                      (unsigned long long)stats.repeats,
                      (unsigned long long)stats.dropped,
                      (unsigned long long)stats.gpuLate,
                      (unsigned long long)stats.longestRepeatStreak,
                      (unsigned long long)stats.simulatorLate,
                      window > 0 ? stats.newFrames / window : 0.0,
                      window > 0 ? stats.submitted / window : 0.0,
                      m_displayRate,
                      (unsigned long long)stats.queueWaits);
            return std::string(text) + ",\"submit_interval_ms\":" + percentiles(stats.submitIntervalsMs) +
                   ",\"age_at_display_ms\":" + percentiles(stats.ageAtDisplayMs) +
                   ",\"pose_error_deg\":" + percentiles(stats.poseErrorDeg) +
                   ",\"gpu_queue_wait_ms\":" + percentiles(stats.queueWaitMs);
        }

        int StartCapture(const simxr::CaptureRequest& request, std::string& error) override {
            std::unique_lock lock(m_captureMutex);
            if (m_capture && m_captureStatus.active) {
                error = "a capture is already running";
                return 0;
            }
            std::error_code ec;
            std::filesystem::create_directories(request.dir, ec);
            if (ec) {
                error = "cannot create " + request.dir.u8string();
                return 0;
            }
            m_capture = request;
            m_capture->id = ++m_captureCounter;
            m_captureSeen = 0;
            m_captureStatus = simxr::CaptureStatus{};
            m_captureStatus.id = m_capture->id;
            m_captureStatus.active = true;
            m_captureStatus.dir = request.dir.u8string();
            m_captureIndex.close();
            m_captureIndex.open(request.dir / L"capture.jsonl", std::ios_base::app);
            simxr::Log("Capture %d: %d frame(s) every %d into %s (scale %.3f, crop %d,%d %dx%d, %s)\n",
                       m_capture->id,
                       request.frames,
                       request.every,
                       request.dir.u8string().c_str(),
                       request.scale,
                       request.crop[0],
                       request.crop[1],
                       request.crop[2],
                       request.crop[3],
                       request.png ? "png" : "raw rgba");
            return m_capture->id;
        }

        simxr::CaptureStatus GetCaptureStatus() const override {
            std::unique_lock lock(m_captureMutex);
            simxr::CaptureStatus status = m_captureStatus;
            {
                std::unique_lock writeLock(m_writeMutex);
                status.pending = (int)(m_pendingReadbacks.size() + m_writeQueue.size()) + m_writing;
            }
            return status;
        }

        void Mark(const std::string& label) override {
            std::unique_lock lock(m_frameMutex);
            if (!m_markersCsv.is_open()) {
                m_markersCsv.open(m_config.logDir / L"simxr-markers.csv", std::ios_base::app);
                m_markersCsv << "t_ms,vsync,submitted,label\n";
            }
            m_markersCsv << std::fixed << (simxr::Now() - simxr::StartTime()) * 1000 << "," << m_vsyncIndex << ","
                         << m_nextSeq << ",\"" << label << "\"\n";
            m_markersCsv.flush();
        }

      private:
        // --------------------------------------------------------- vsync loop

        void VsyncThread() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            SetThreadDescription(GetCurrentThread(), L"SimXR vsync");
            timeBeginPeriod(1);
            HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

            const double origin = simxr::Now();
            int64_t k = 0;
            while (!m_terminate) {
                k++;
                double target = origin + k * m_period;
                // Sleep until ~0.3 ms before the vsync, then spin.
                for (;;) {
                    const double remaining = target - simxr::Now();
                    if (remaining <= 0.0003) {
                        break;
                    }
                    if (timer) {
                        LARGE_INTEGER due;
                        due.QuadPart = -(LONGLONG)((remaining - 0.0003) * 1e7);
                        if (due.QuadPart < 0 && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                            WaitForSingleObject(timer, 20);
                        }
                    } else {
                        Sleep(remaining > 0.0015 ? 1 : 0);
                    }
                    if (m_terminate) {
                        break;
                    }
                }
                while (simxr::Now() < target) {
                    _mm_pause();
                }
                const double late = simxr::Now() - target;
                bool simulatorLate = late > 0.0005;
                if (late > m_period) {
                    // We overslept a whole vsync (system hiccup): skip to the current one.
                    k += (int64_t)(late / m_period);
                    target = origin + k * m_period;
                }

                {
                    std::unique_lock lock(m_frameMutex);
                    m_vsyncIndex = k;
                    if (simulatorLate) {
                        m_stats.simulatorLate++;
                    }
                    ScanOut(k, target);
                    m_nextFramePredictedDisplayTime = target + m_period;
                    m_lastWaitedFrame = std::max(m_lastWaitedFrame, m_pendingWaitFrame);
                    m_lastSignaledVsync++;
                }
                m_frameVsync.notify_all();
                if (k % 60 == 0) {
                    FlushCsv(false);
                }
            }
            if (timer) {
                CloseHandle(timer);
            }
            timeEndPeriod(1);
        }

        // Under m_frameMutex.
        void ScanOut(int64_t vsync, double vsyncTime) {
            const double latch = vsyncTime - m_config.latchMs / 1000.0;
            const uint64_t completed = m_fence ? m_fence->GetCompletedValue() : UINT64_MAX;
            for (auto& frame : m_frames) {
                if (frame.completeTime < 0 && frame.fenceValue <= completed) {
                    frame.completeTime = vsyncTime;
                }
            }

            FrameRecord* shown = nullptr;
            bool gpuLate = false;
            for (auto it = m_frames.rbegin(); it != m_frames.rend(); ++it) {
                if (it->seq <= m_displayedSeq) {
                    break;
                }
                if (it->submitTime > latch) {
                    continue;
                }
                if (it->completeTime >= 0) {
                    shown = &*it;
                    break;
                }
                gpuLate = true;
            }

            m_stats.vsyncs++;
            if (gpuLate) {
                m_stats.gpuLate++;
            }
            int droppedBefore = 0;
            if (shown) {
                for (auto& frame : m_frames) {
                    if (frame.seq > m_displayedSeq && frame.seq < shown->seq && frame.displayCount == 0 && !frame.dropped) {
                        frame.dropped = true;
                        droppedBefore++;
                    }
                }
                m_stats.dropped += droppedBefore;
                m_displayedSeq = shown->seq;
                shown->firstVsync = vsync;
                shown->displayCount = 1;
                m_stats.newFrames++;
                m_stats.ageAtDisplayMs.push_back((float)((vsyncTime - shown->submitTime) * 1000));
                m_repeatStreak = 0;
            } else if (m_displayedSeq) {
                for (auto& frame : m_frames) {
                    if (frame.seq == m_displayedSeq) {
                        shown = &frame;
                        break;
                    }
                }
                if (shown) {
                    shown->displayCount++;
                }
                m_stats.repeats++;
                m_repeatStreak++;
                m_stats.longestRepeatStreak = std::max(m_stats.longestRepeatStreak, m_repeatStreak);
            }

            // How far the picture is from where the head really is at this vsync.
            const ovrPoseStatef truth = simxr::State::Get().HeadPose(vsyncTime, m_useStageTracking);
            const OVR::Quatf trueRotation(truth.ThePose.Orientation);
            const OVR::Vector3f trueYpr = simxr::QuatToYpr(trueRotation);
            float poseError = -1;
            if (shown && shown->hasPose) {
                poseError = AngleBetweenDeg(OVR::Quatf(shown->pose[0].Orientation), trueRotation);
                m_stats.poseErrorDeg.push_back(poseError);
            }

            if (m_vsyncCsv.is_open()) {
                char line[256];
                sprintf_s(line,
                          "%lld,%.3f,%llu,%d,%llu,%.3f,%d,%d,%.4f,%.3f,%.3f\n",
                          (long long)vsync,
                          (vsyncTime - simxr::StartTime()) * 1000,
                          (unsigned long long)(shown ? shown->seq : 0),
                          shown && shown->displayCount == 1 ? 1 : 0,
                          (unsigned long long)m_repeatStreak,
                          shown ? (vsyncTime - shown->submitTime) * 1000 : -1.0,
                          gpuLate ? 1 : 0,
                          droppedBefore,
                          poseError,
                          trueYpr.x,
                          trueYpr.y);
                m_vsyncBuffer += line;
            }

            // Retire frames that can no longer be shown: older than the one on screen (shown
            // for the last time, or dropped), or never completed on the GPU for 2 s.
            while (!m_frames.empty()) {
                const FrameRecord& oldest = m_frames.front();
                const bool superseded = oldest.seq < m_displayedSeq;
                const bool stale = oldest.seq > m_displayedSeq && vsyncTime - oldest.submitTime > 2.0;
                if (!superseded && !stale) {
                    break;
                }
                WriteFrameLine(oldest);
                m_frames.pop_front();
            }
        }

        // Under m_frameMutex.
        void WriteFrameLine(const FrameRecord& frame) {
            if (!m_framesCsv.is_open()) {
                return;
            }
            OVR::Vector3f ypr{0, 0, 0}, pos{0, 0, 0};
            if (frame.hasPose) {
                ypr = simxr::QuatToYpr(OVR::Quatf(frame.pose[0].Orientation));
                pos = OVR::Vector3f(frame.pose[0].Position);
            }
            char line[320];
            sprintf_s(line,
                      "%llu,%lld,%.3f,%.3f,%lld,%d,%d,%d,%d,%d,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f,%.3f,%.3f\n",
                      (unsigned long long)frame.seq,
                      frame.frameIndex,
                      (frame.submitTime - simxr::StartTime()) * 1000,
                      frame.completeTime >= 0 ? (frame.completeTime - simxr::StartTime()) * 1000 : -1.0,
                      (long long)frame.firstVsync,
                      frame.displayCount,
                      frame.dropped ? 1 : 0,
                      frame.layers,
                      frame.eyeWidth,
                      frame.eyeHeight,
                      pos.x,
                      pos.y,
                      pos.z,
                      ypr.x,
                      ypr.y,
                      ypr.z,
                      frame.sensorTime > 0 ? (frame.sensorTime - simxr::StartTime()) * 1000 : -1.0,
                      frame.queueWaitMs);
            m_framesBuffer += line;
        }

        void FlushCsv(bool final) {
            std::string vsyncs, frames;
            {
                std::unique_lock lock(m_frameMutex);
                if (final) {
                    for (const auto& frame : m_frames) {
                        WriteFrameLine(frame);
                    }
                    m_frames.clear();
                }
                vsyncs.swap(m_vsyncBuffer);
                frames.swap(m_framesBuffer);
            }
            if (m_vsyncCsv.is_open() && !vsyncs.empty()) {
                m_vsyncCsv << vsyncs;
                m_vsyncCsv.flush();
            }
            if (m_framesCsv.is_open() && !frames.empty()) {
                m_framesCsv << frames;
                m_framesCsv.flush();
            }
        }

        // ------------------------------------------------------------- mirror

        void RenderMirror(const std::vector<const ovrLayerHeader*>& layers) {
            const float clearColor[] = {0.f, 0.f, 0.f, 1.f};
            m_submissionContext->ClearRenderTargetView(m_mirrorRTV.Get(), clearColor);
            m_submissionContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_submissionContext->OMSetRenderTargets(1, m_mirrorRTV.GetAddressOf(), nullptr);
            m_submissionContext->VSSetConstantBuffers(0, 1, m_constantsBuffer.GetAddressOf());
            m_submissionContext->PSSetSamplers(0, 1, m_linearSampler.GetAddressOf());
            m_submissionContext->PSSetConstantBuffers(0, 1, m_constantsBuffer.GetAddressOf());
            m_submissionContext->VSSetShader(m_reprojectVS.Get(), nullptr, 0);
            m_submissionContext->PSSetShader(m_reprojectPS.Get(), nullptr, 0);
            m_submissionContext->RSSetState(m_rasterizerState.Get());
            m_submissionContext->OMSetDepthStencilState(m_noDepthTestState.Get(), 0xff);
            m_submissionContext->OMSetBlendState(m_blendState.Get(), nullptr, 0xffffffff);

            for (const ovrLayerHeader* layerHeader : layers) {
                const ovrLayer_Union* layer = (ovrLayer_Union*)layerHeader;
                if (!layer || (layer->Header.Type != ovrLayerType_EyeFov && layer->Header.Type != ovrLayerType_EyeFovDepth)) {
                    continue;
                }
                const auto eyeFov = &layer->EyeFov;
                ConstantsBuffer constants{};
                constants.flipY = layer->Header.Flags & ovrLayerFlag_TextureOriginAtBottomLeft;
                for (uint32_t eye = 0; eye < ovrEye_Count; eye++) {
                    const auto baseProjection = LoadOvrProjection(m_eyeFov[eye], 0.01f, 1000.f);
                    const auto projection = LoadOvrProjection(eyeFov->Fov[eye], 0.01f, 1000.f);
                    DirectX::XMStoreFloat4x4(
                        &constants.reprojectionMatrix,
                        DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, baseProjection) * projection));
                    const auto handle =
                        eye == 0 || !eyeFov->ColorTexture[eye] ? eyeFov->ColorTexture[0] : eyeFov->ColorTexture[eye];
                    if (!m_swapchains.count(handle)) {
                        continue;
                    }
                    const Swapchain* swapchain = (const Swapchain*)handle;
                    const int index = swapchain->lastCommitted;
                    if (index < 0 || !swapchain->SRVs[index]) {
                        continue;
                    }
                    constants.imageRectNormalized.offset = {(float)eyeFov->Viewport[eye].Pos.x / swapchain->desc.Width,
                                                            (float)eyeFov->Viewport[eye].Pos.y / swapchain->desc.Height};
                    constants.imageRectNormalized.extent = {(float)eyeFov->Viewport[eye].Size.w / swapchain->desc.Width,
                                                            (float)eyeFov->Viewport[eye].Size.h / swapchain->desc.Height};
                    D3D11_MAPPED_SUBRESOURCE mapped;
                    if (FAILED(m_submissionContext->Map(m_constantsBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                        continue;
                    }
                    memcpy(mapped.pData, &constants, sizeof(constants));
                    m_submissionContext->Unmap(m_constantsBuffer.Get(), 0);

                    D3D11_VIEWPORT viewport{};
                    viewport.Width = m_mirrorWidth / 2.f;
                    viewport.TopLeftX = eye * viewport.Width;
                    viewport.Height = (float)m_mirrorHeight;
                    viewport.MaxDepth = 1.f;
                    m_submissionContext->RSSetViewports(1, &viewport);
                    m_submissionContext->PSSetShaderResources(0, 1, swapchain->SRVs[index].GetAddressOf());
                    m_submissionContext->Draw(3, 0);
                }
            }
            ID3D11RenderTargetView* nullRTV[] = {nullptr};
            m_submissionContext->OMSetRenderTargets(1, nullRTV, nullptr);
            ID3D11ShaderResourceView* nullSRV[] = {nullptr};
            m_submissionContext->PSSetShaderResources(0, 1, nullSRV);
        }

        // ------------------------------------------------------------ capture

        // Under the shared swapchain lock, on the submission thread.
        void MaybeCapture(const ovrLayerEyeFov& eyeFov, const FrameRecord& record) {
            std::unique_lock lock(m_captureMutex);
            if (!m_capture || !m_captureStatus.active) {
                return;
            }
            const simxr::CaptureRequest& request = *m_capture;
            const double now = simxr::Now();
            if (now < request.startAt) {
                return;
            }
            const int ordinal = m_captureSeen++;
            if (ordinal % std::max(1, request.every) != 0) {
                return;
            }
            const uint64_t seq = m_nextSeq + 1; // the sequence number SubmitFrame is about to give this frame

            for (int eye = 0; eye < 2; eye++) {
                if (!(request.eyes & (1 << eye))) {
                    continue;
                }
                const auto handle = eye == 0 || !eyeFov.ColorTexture[eye] ? eyeFov.ColorTexture[0] : eyeFov.ColorTexture[eye];
                if (!m_swapchains.count(handle)) {
                    continue;
                }
                const Swapchain* swapchain = (const Swapchain*)handle;
                const int index = swapchain->lastCommitted;
                if (index < 0 || !swapchain->SRVs[index]) {
                    m_captureStatus.lastError = "eye image not readable (MSAA, array or depth)";
                    continue;
                }
                const ovrRecti viewport = eyeFov.Viewport[eye];
                int crop[4] = {request.crop[0], request.crop[1], request.crop[2], request.crop[3]};
                if (crop[2] <= 0 || crop[3] <= 0) {
                    crop[0] = crop[1] = 0;
                    crop[2] = viewport.Size.w;
                    crop[3] = viewport.Size.h;
                }
                crop[0] = std::clamp(crop[0], 0, viewport.Size.w - 1);
                crop[1] = std::clamp(crop[1], 0, viewport.Size.h - 1);
                crop[2] = std::clamp(crop[2], 1, viewport.Size.w - crop[0]);
                crop[3] = std::clamp(crop[3], 1, viewport.Size.h - crop[1]);
                const uint32_t outW = std::max(1u, (uint32_t)std::lround(crop[2] * request.scale));
                const uint32_t outH = std::max(1u, (uint32_t)std::lround(crop[3] * request.scale));
                const size_t bytes = (size_t)outW * outH * 4;
                if (m_captureBytes + bytes > m_config.captureMemoryMB * 1024ull * 1024ull) {
                    m_captureStatus.dropped++;
                    m_captureStatus.lastError = "capture memory limit reached: frame skipped";
                    continue;
                }

                // Same bytes as the source for 8-bit formats; float sources are encoded to sRGB.
                const DXGI_FORMAT targetFormat =
                    IsSrgb(swapchain->viewFormat) || IsFloat(swapchain->viewFormat) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                                                     : DXGI_FORMAT_R8G8B8A8_UNORM;
                ComPtr<ID3D11Texture2D> target, staging;
                ComPtr<ID3D11RenderTargetView> rtv;
                {
                    D3D11_TEXTURE2D_DESC desc{};
                    desc.Width = outW;
                    desc.Height = outH;
                    desc.MipLevels = desc.ArraySize = 1;
                    desc.Format = targetFormat;
                    desc.SampleDesc.Count = 1;
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
                    if (FAILED(m_submissionDevice->CreateTexture2D(&desc, nullptr, target.ReleaseAndGetAddressOf())) ||
                        FAILED(m_submissionDevice->CreateRenderTargetView(target.Get(), nullptr, rtv.ReleaseAndGetAddressOf()))) {
                        m_captureStatus.lastError = "cannot create the capture target";
                        continue;
                    }
                    desc.Usage = D3D11_USAGE_STAGING;
                    desc.BindFlags = 0;
                    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    if (FAILED(m_submissionDevice->CreateTexture2D(&desc, nullptr, staging.ReleaseAndGetAddressOf()))) {
                        m_captureStatus.lastError = "cannot create the staging texture";
                        continue;
                    }
                }

                CaptureConstants constants{};
                constants.sourceOffset[0] = (float)(viewport.Pos.x + crop[0]) / swapchain->desc.Width;
                constants.sourceOffset[1] = (float)(viewport.Pos.y + crop[1]) / swapchain->desc.Height;
                constants.sourceExtent[0] = (float)crop[2] / swapchain->desc.Width;
                constants.sourceExtent[1] = (float)crop[3] / swapchain->desc.Height;
                constants.flipY = 0;
                D3D11_MAPPED_SUBRESOURCE mapped;
                if (FAILED(m_submissionContext->Map(m_captureConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                    continue;
                }
                memcpy(mapped.pData, &constants, sizeof(constants));
                m_submissionContext->Unmap(m_captureConstants.Get(), 0);

                D3D11_VIEWPORT vp{};
                vp.Width = (float)outW;
                vp.Height = (float)outH;
                vp.MaxDepth = 1.f;
                m_submissionContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                m_submissionContext->IASetInputLayout(nullptr);
                m_submissionContext->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
                m_submissionContext->RSSetViewports(1, &vp);
                m_submissionContext->RSSetState(m_rasterizerState.Get());
                m_submissionContext->OMSetDepthStencilState(m_noDepthTestState.Get(), 0xff);
                m_submissionContext->OMSetBlendState(m_opaqueState.Get(), nullptr, 0xffffffff);
                m_submissionContext->VSSetShader(m_captureVS.Get(), nullptr, 0);
                m_submissionContext->PSSetShader(m_capturePS.Get(), nullptr, 0);
                m_submissionContext->PSSetConstantBuffers(0, 1, m_captureConstants.GetAddressOf());
                ID3D11SamplerState* sampler = (outW == (uint32_t)crop[2] && outH == (uint32_t)crop[3])
                                                  ? m_pointSampler.Get()
                                                  : m_linearSampler.Get();
                m_submissionContext->PSSetSamplers(0, 1, &sampler);
                m_submissionContext->PSSetShaderResources(0, 1, swapchain->SRVs[index].GetAddressOf());
                m_submissionContext->Draw(3, 0);
                ID3D11RenderTargetView* nullRTV[] = {nullptr};
                m_submissionContext->OMSetRenderTargets(1, nullRTV, nullptr);
                ID3D11ShaderResourceView* nullSRV[] = {nullptr};
                m_submissionContext->PSSetShaderResources(0, 1, nullSRV);
                m_submissionContext->CopyResource(staging.Get(), target.Get());

                wchar_t file[160];
                swprintf_s(file,
                           L"%hs-s%06llu-%hs.%hs",
                           request.name.c_str(),
                           (unsigned long long)seq,
                           eye == 0 ? "L" : "R",
                           request.png ? "png" : "rgba");
                PendingReadback pending;
                pending.staging = staging;
                pending.width = outW;
                pending.height = outH;
                pending.path = request.dir / file;
                pending.png = request.png;
                pending.captureId = request.id;
                {
                    std::unique_lock writeLock(m_writeMutex);
                    m_pendingReadbacks.push_back(std::move(pending));
                }
                m_captureBytes += bytes;

                if (m_captureIndex.is_open()) {
                    const OVR::Vector3f ypr = simxr::QuatToYpr(OVR::Quatf(eyeFov.RenderPose[eye].Orientation));
                    const OVR::Vector3f trueYpr =
                        simxr::QuatToYpr(OVR::Quatf(simxr::State::Get().HeadPose(now, m_useStageTracking).ThePose.Orientation));
                    char line[1024];
                    sprintf_s(line,
                              "{\"file\":\"%ls\",\"seq\":%llu,\"eye\":\"%s\",\"frame_index\":%lld,\"t_ms\":%.3f,"
                              "\"width\":%u,\"height\":%u,\"encoding\":\"%s\",\"source_format\":\"%s\","
                              "\"eye_viewport\":[%d,%d,%d,%d],\"crop\":[%d,%d,%d,%d],\"scale\":%.4f,"
                              "\"render_pos\":[%.5f,%.5f,%.5f],\"render_ypr\":[%.4f,%.4f,%.4f],"
                              "\"head_ypr_at_submit\":[%.4f,%.4f,%.4f]}\n",
                              file,
                              (unsigned long long)seq,
                              eye == 0 ? "left" : "right",
                              record.frameIndex,
                              (now - simxr::StartTime()) * 1000,
                              outW,
                              outH,
                              targetFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? "rgba8_srgb" : "rgba8_unorm",
                              FormatName(swapchain->viewFormat),
                              viewport.Pos.x,
                              viewport.Pos.y,
                              viewport.Size.w,
                              viewport.Size.h,
                              crop[0],
                              crop[1],
                              crop[2],
                              crop[3],
                              request.scale,
                              eyeFov.RenderPose[eye].Position.x,
                              eyeFov.RenderPose[eye].Position.y,
                              eyeFov.RenderPose[eye].Position.z,
                              ypr.x,
                              ypr.y,
                              ypr.z,
                              trueYpr.x,
                              trueYpr.y,
                              trueYpr.z);
                    m_captureIndex << line;
                    m_captureIndex.flush();
                }
            }
            m_captureStatus.captured++;
            if (m_captureStatus.captured >= request.frames) {
                m_captureStatus.active = false;
                simxr::Log("Capture %d: %d frame(s) taken\n", request.id, m_captureStatus.captured);
            }
        }

        // On the submission thread: read back finished copies without waiting.
        void PollReadbacks() {
            std::deque<PendingReadback> pending;
            {
                std::unique_lock writeLock(m_writeMutex);
                if (m_pendingReadbacks.empty()) {
                    return;
                }
                pending.swap(m_pendingReadbacks);
            }
            std::deque<PendingReadback> stillPending;
            for (auto& item : pending) {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                // Long-waiting copies are read with a wait so that a capture always finishes.
                const UINT flags = item.polls++ < 30 ? D3D11_MAP_FLAG_DO_NOT_WAIT : 0;
                const HRESULT hr = m_submissionContext->Map(item.staging.Get(), 0, D3D11_MAP_READ, flags, &mapped);
                if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
                    stillPending.push_back(std::move(item));
                    continue;
                }
                if (FAILED(hr)) {
                    std::unique_lock lock(m_captureMutex);
                    m_captureStatus.dropped++;
                    m_captureStatus.lastError = "readback failed";
                    m_captureBytes -= (size_t)item.width * item.height * 4;
                    continue;
                }
                WriteJob job;
                job.path = item.path;
                job.width = item.width;
                job.height = item.height;
                job.png = item.png;
                job.captureId = item.captureId;
                job.rgba.resize((size_t)item.width * item.height * 4);
                for (uint32_t y = 0; y < item.height; y++) {
                    memcpy(job.rgba.data() + (size_t)y * item.width * 4,
                           (const uint8_t*)mapped.pData + (size_t)y * mapped.RowPitch,
                           (size_t)item.width * 4);
                }
                m_submissionContext->Unmap(item.staging.Get(), 0);
                std::unique_lock writeLock(m_writeMutex);
                m_writeQueue.push_back(std::move(job));
                m_writeCv.notify_one();
            }
            if (!stillPending.empty()) {
                std::unique_lock writeLock(m_writeMutex);
                for (auto& item : stillPending) {
                    m_pendingReadbacks.push_front(std::move(item));
                }
            }
        }

        void WriterThread() {
            SetThreadDescription(GetCurrentThread(), L"SimXR capture writer");
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            const HRESULT coinit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            ComPtr<IWICImagingFactory> wic;
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.ReleaseAndGetAddressOf()));
            for (;;) {
                WriteJob job;
                {
                    std::unique_lock lock(m_writeMutex);
                    m_writeCv.wait(lock, [&] { return m_terminate || !m_writeQueue.empty(); });
                    if (m_writeQueue.empty()) {
                        break;
                    }
                    job = std::move(m_writeQueue.front());
                    m_writeQueue.pop_front();
                    m_writing++;
                }
                std::string error;
                const bool ok = job.png ? WritePng(wic.Get(), job, error) : WriteRaw(job, error);
                {
                    std::unique_lock lock(m_captureMutex);
                    m_captureBytes -= job.rgba.size();
                    if (job.captureId == m_captureStatus.id) {
                        if (ok) {
                            m_captureStatus.written++;
                        } else {
                            m_captureStatus.dropped++;
                            m_captureStatus.lastError = error;
                        }
                    }
                }
                std::unique_lock lock(m_writeMutex);
                m_writing--;
            }
            // Release WIC while COM is still initialized on this thread.
            wic.Reset();
            if (SUCCEEDED(coinit)) {
                CoUninitialize();
            }
        }

        static bool WriteRaw(const WriteJob& job, std::string& error) {
            std::ofstream out(job.path, std::ios_base::binary | std::ios_base::trunc);
            out.write((const char*)job.rgba.data(), job.rgba.size());
            if (!out) {
                error = "cannot write " + job.path.u8string();
                return false;
            }
            return true;
        }

        static bool WritePng(IWICImagingFactory* wic, const WriteJob& job, std::string& error) {
            if (!wic) {
                error = "WIC unavailable";
                return false;
            }
            // Opaque 24-bit BGR.
            std::vector<uint8_t> bgr((size_t)job.width * job.height * 3);
            for (size_t i = 0, n = (size_t)job.width * job.height; i < n; i++) {
                bgr[i * 3 + 0] = job.rgba[i * 4 + 2];
                bgr[i * 3 + 1] = job.rgba[i * 4 + 1];
                bgr[i * 3 + 2] = job.rgba[i * 4 + 0];
            }
            ComPtr<IWICStream> stream;
            ComPtr<IWICBitmapEncoder> encoder;
            ComPtr<IWICBitmapFrameEncode> frame;
            WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
            HRESULT hr = wic->CreateStream(stream.ReleaseAndGetAddressOf());
            if (SUCCEEDED(hr)) {
                hr = stream->InitializeFromFilename(job.path.c_str(), GENERIC_WRITE);
            }
            if (SUCCEEDED(hr)) {
                hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.ReleaseAndGetAddressOf());
            }
            if (SUCCEEDED(hr)) {
                hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
            }
            if (SUCCEEDED(hr)) {
                hr = encoder->CreateNewFrame(frame.ReleaseAndGetAddressOf(), nullptr);
            }
            if (SUCCEEDED(hr)) {
                hr = frame->Initialize(nullptr);
            }
            if (SUCCEEDED(hr)) {
                hr = frame->SetSize(job.width, job.height);
            }
            if (SUCCEEDED(hr)) {
                hr = frame->SetPixelFormat(&format);
            }
            if (SUCCEEDED(hr) && format != GUID_WICPixelFormat24bppBGR) {
                hr = E_FAIL;
            }
            if (SUCCEEDED(hr)) {
                hr = frame->WritePixels(job.height, job.width * 3, (UINT)bgr.size(), bgr.data());
            }
            if (SUCCEEDED(hr)) {
                hr = frame->Commit();
            }
            if (SUCCEEDED(hr)) {
                hr = encoder->Commit();
            }
            if (FAILED(hr)) {
                char text[128];
                sprintf_s(text, "PNG encode failed (0x%08x)", (unsigned)hr);
                error = text;
                return false;
            }
            return true;
        }

        // ------------------------------------------------------------ members

        const simxr::Config& m_config;
        LUID m_adapterLuid{};
        std::string m_adapterName;
        std::atomic<bool> m_terminate{false};
        std::thread m_vsyncThread;

        ComPtr<ID3D11Device> m_submissionDevice;
        ComPtr<ID3D11DeviceContext> m_submissionContext;
        ComPtr<ID3D11DeviceContext4> m_submissionContext4;
        ComPtr<ID3D11Fence> m_fence;
        std::atomic<uint64_t> m_fenceValue{0};
        HANDLE m_queueEvent{nullptr};
        float m_pendingQueueWaitMs{0}; // under m_frameMutex
        ComPtr<ID3D11VertexShader> m_reprojectVS;
        ComPtr<ID3D11PixelShader> m_reprojectPS;
        ComPtr<ID3D11VertexShader> m_captureVS;
        ComPtr<ID3D11PixelShader> m_capturePS;
        ComPtr<ID3D11Buffer> m_constantsBuffer;
        ComPtr<ID3D11Buffer> m_captureConstants;
        ComPtr<ID3D11DepthStencilState> m_noDepthTestState;
        ComPtr<ID3D11BlendState> m_blendState;
        ComPtr<ID3D11BlendState> m_opaqueState;
        ComPtr<ID3D11RasterizerState> m_rasterizerState;
        ComPtr<ID3D11SamplerState> m_linearSampler;
        ComPtr<ID3D11SamplerState> m_pointSampler;

        float m_displayRate{};
        double m_period{};
        ovrSizei m_recommendedResolution{};
        ovrFovPort m_eyeFov[ovrEye_Count]{};
        ovrPosef m_eyePose[ovrEye_Count]{};
        bool m_useStageTracking{false};

        mutable std::shared_mutex m_swapchainMutex;
        std::unordered_set<void*> m_swapchains;
        uint64_t m_swapchainsCreated{0};
        ComPtr<ID3D11RenderTargetView> m_mirrorRTV;
        uint32_t m_mirrorWidth{0};
        uint32_t m_mirrorHeight{0};

        // Display model (m_frameMutex).
        mutable std::mutex m_frameMutex;
        std::condition_variable m_frameVsync;
        long long m_lastSignaledVsync{};
        long long m_lastWaitedFrame{-1};
        long long m_pendingWaitFrame{-1};
        double m_nextFramePredictedDisplayTime{0.0};
        int64_t m_vsyncIndex{0};
        std::deque<FrameRecord> m_frames;
        uint64_t m_nextSeq{0};
        uint64_t m_displayedSeq{0};
        uint64_t m_repeatStreak{0};
        double m_lastSubmitTime{0};
        simxr::Stats m_stats;
        std::ofstream m_vsyncCsv, m_framesCsv, m_markersCsv;
        std::string m_vsyncBuffer, m_framesBuffer;

        // Capture (m_captureMutex; the queues under m_writeMutex).
        mutable std::mutex m_captureMutex;
        std::optional<simxr::CaptureRequest> m_capture;
        simxr::CaptureStatus m_captureStatus;
        int m_captureCounter{0};
        int m_captureSeen{0};
        size_t m_captureBytes{0};
        std::ofstream m_captureIndex;
        mutable std::mutex m_writeMutex;
        std::condition_variable m_writeCv;
        std::deque<PendingReadback> m_pendingReadbacks;
        std::deque<WriteJob> m_writeQueue;
        int m_writing{0};
        std::vector<std::thread> m_writers;
    };

} // namespace

namespace ovrnull::driver {

    IDriver* CreateDriver() {
        return new SimDriver();
    }

} // namespace ovrnull::driver
