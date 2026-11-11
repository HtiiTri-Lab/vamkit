// SimXR self-test client: a minimal D3D11 OpenXR application for checking the
// simulated headset without VaM. It renders, per eye, a flat colour that
// encodes the eye and the head yaw it was rendered for, plus a white 64x64
// square at (100, 200), and submits a projection layer every frame.
//
//   simxr_selftest.exe <openxr_loader.dll> <seconds> [--sleep-ms N] [--gpu-ms N] [--log file]
//
// --gpu-ms N makes every frame cost about N ms of GPU time before its swapchain
// image is released (copies of a 256 MB texture, calibrated with timestamp
// queries at start): a GPU-bound application, for the display model's GPU
// queue limit.
//
// Colour of eye e (0/1) rendered for yaw Y (degrees):
//   R = (frame number) mod 256, G = round(Y + 128) clamped 0..255, B = 64 + 128 * e
// written to an R8G8B8A8_UNORM swapchain (bytes as written; the capture reads
// them back unchanged). The XR_RUNTIME_JSON environment variable picks the runtime.
// Exit code 0 when the session ran and frames were submitted.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <algorithm>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

    PFN_xrGetInstanceProcAddr g_getProc = nullptr;
    FILE* g_log = stdout;

#define XR_FN(name) PFN_##name name = nullptr
    struct Xr {
        XR_FN(xrDestroyInstance);
        XR_FN(xrGetInstanceProperties);
        XR_FN(xrGetSystem);
        XR_FN(xrGetSystemProperties);
        XR_FN(xrGetD3D11GraphicsRequirementsKHR);
        XR_FN(xrCreateSession);
        XR_FN(xrDestroySession);
        XR_FN(xrCreateReferenceSpace);
        XR_FN(xrEnumerateViewConfigurationViews);
        XR_FN(xrEnumerateSwapchainFormats);
        XR_FN(xrCreateSwapchain);
        XR_FN(xrEnumerateSwapchainImages);
        XR_FN(xrAcquireSwapchainImage);
        XR_FN(xrWaitSwapchainImage);
        XR_FN(xrReleaseSwapchainImage);
        XR_FN(xrPollEvent);
        XR_FN(xrBeginSession);
        XR_FN(xrEndSession);
        XR_FN(xrRequestExitSession);
        XR_FN(xrWaitFrame);
        XR_FN(xrBeginFrame);
        XR_FN(xrEndFrame);
        XR_FN(xrLocateViews);
    } xr;

    void Logf(const char* fmt, ...) {
        va_list va;
        va_start(va, fmt);
        vfprintf(g_log, fmt, va);
        va_end(va);
        fflush(g_log);
    }

    bool Check(XrResult result, const char* what) {
        if (XR_FAILED(result)) {
            Logf("FAIL %s: %d\n", what, (int)result);
            return false;
        }
        return true;
    }

    template <typename T>
    void Load(XrInstance instance, const char* name, T& fn) {
        g_getProc(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&fn));
    }

    float YawDegrees(const XrQuaternionf& q) {
        // Same decomposition as SimXR (yaw about +Y, then pitch, then roll).
        const float siny = 2.f * (q.w * q.y + q.x * q.z);
        const float cosy = 1.f - 2.f * (q.y * q.y + q.x * q.x);
        return std::atan2(siny, cosy) * 180.f / 3.14159265f;
    }

    // A crash names the module and offset (no debugger needed).
    LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
        const void* address = info->ExceptionRecord->ExceptionAddress;
        HMODULE module = nullptr;
        wchar_t name[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)address, &module)) {
            GetModuleFileNameW(module, name, MAX_PATH);
        }
        Logf("CRASH code 0x%08lx at %ls+0x%llx (thread %lu)\n",
             info->ExceptionRecord->ExceptionCode,
             name,
             (unsigned long long)((const char*)address - (const char*)module),
             GetCurrentThreadId());
        return EXCEPTION_EXECUTE_HANDLER;
    }

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetUnhandledExceptionFilter(CrashFilter);
    if (argc < 3) {
        fprintf(stderr, "usage: simxr_selftest.exe <openxr_loader.dll> <seconds> [--sleep-ms N] [--gpu-ms N] [--log file]\n");
        return 3;
    }
    const double seconds = _wtof(argv[2]);
    int sleepMs = 0;
    double gpuMs = 0;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!wcscmp(argv[i], L"--sleep-ms")) {
            sleepMs = _wtoi(argv[i + 1]);
        } else if (!wcscmp(argv[i], L"--gpu-ms")) {
            gpuMs = _wtof(argv[i + 1]);
        } else if (!wcscmp(argv[i], L"--log")) {
            _wfopen_s(&g_log, argv[i + 1], L"w");
            if (!g_log) {
                g_log = stdout;
            }
        }
    }

    HMODULE loader = LoadLibraryW(argv[1]);
    if (!loader) {
        Logf("FAIL loader %lu\n", GetLastError());
        return 3;
    }
    g_getProc = (PFN_xrGetInstanceProcAddr)GetProcAddress(loader, "xrGetInstanceProcAddr");
    auto createInstance = (PFN_xrCreateInstance)GetProcAddress(loader, "xrCreateInstance");
    if (!g_getProc || !createInstance) {
        Logf("FAIL loader exports\n");
        return 3;
    }

    const char* extensions[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(instanceInfo.applicationInfo.applicationName, "SimXR selftest");
    instanceInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    instanceInfo.enabledExtensionCount = 1;
    instanceInfo.enabledExtensionNames = extensions;
    XrInstance instance = XR_NULL_HANDLE;
    if (!Check(createInstance(&instanceInfo, &instance), "xrCreateInstance")) {
        return 3;
    }
    Load(instance, "xrDestroyInstance", xr.xrDestroyInstance);
    Load(instance, "xrGetInstanceProperties", xr.xrGetInstanceProperties);
    Load(instance, "xrGetSystem", xr.xrGetSystem);
    Load(instance, "xrGetSystemProperties", xr.xrGetSystemProperties);
    Load(instance, "xrGetD3D11GraphicsRequirementsKHR", xr.xrGetD3D11GraphicsRequirementsKHR);
    Load(instance, "xrCreateSession", xr.xrCreateSession);
    Load(instance, "xrDestroySession", xr.xrDestroySession);
    Load(instance, "xrCreateReferenceSpace", xr.xrCreateReferenceSpace);
    Load(instance, "xrEnumerateViewConfigurationViews", xr.xrEnumerateViewConfigurationViews);
    Load(instance, "xrEnumerateSwapchainFormats", xr.xrEnumerateSwapchainFormats);
    Load(instance, "xrCreateSwapchain", xr.xrCreateSwapchain);
    Load(instance, "xrEnumerateSwapchainImages", xr.xrEnumerateSwapchainImages);
    Load(instance, "xrAcquireSwapchainImage", xr.xrAcquireSwapchainImage);
    Load(instance, "xrWaitSwapchainImage", xr.xrWaitSwapchainImage);
    Load(instance, "xrReleaseSwapchainImage", xr.xrReleaseSwapchainImage);
    Load(instance, "xrPollEvent", xr.xrPollEvent);
    Load(instance, "xrBeginSession", xr.xrBeginSession);
    Load(instance, "xrEndSession", xr.xrEndSession);
    Load(instance, "xrRequestExitSession", xr.xrRequestExitSession);
    Load(instance, "xrWaitFrame", xr.xrWaitFrame);
    Load(instance, "xrBeginFrame", xr.xrBeginFrame);
    Load(instance, "xrEndFrame", xr.xrEndFrame);
    Load(instance, "xrLocateViews", xr.xrLocateViews);

    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    xr.xrGetInstanceProperties(instance, &properties);
    Logf("runtime=%s\n", properties.runtimeName);

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    if (!Check(xr.xrGetSystem(instance, &systemInfo, &system), "xrGetSystem")) {
        return 3;
    }
    XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
    xr.xrGetSystemProperties(instance, system, &systemProperties);
    Logf("system=%s\n", systemProperties.systemName);

    XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!Check(xr.xrGetD3D11GraphicsRequirementsKHR(instance, system, &requirements), "xrGetD3D11GraphicsRequirementsKHR")) {
        return 3;
    }
    ComPtr<IDXGIFactory1> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (!memcmp(&desc.AdapterLuid, &requirements.adapterLuid, sizeof(LUID))) {
            Logf("adapter=%ls\n", desc.Description);
            break;
        }
    }
    if (!adapter) {
        Logf("FAIL no adapter for the runtime's LUID\n");
        return 3;
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &device, nullptr, &context))) {
        Logf("FAIL D3D11CreateDevice\n");
        return 3;
    }
    ComPtr<ID3D11DeviceContext1> context1;
    context.As(&context1);

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device.Get();
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = system;
    XrSession session = XR_NULL_HANDLE;
    if (!Check(xr.xrCreateSession(instance, &sessionInfo, &session), "xrCreateSession")) {
        return 3;
    }

    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.f;
    XrSpace space = XR_NULL_HANDLE;
    if (!Check(xr.xrCreateReferenceSpace(session, &spaceInfo, &space), "xrCreateReferenceSpace")) {
        return 3;
    }

    uint32_t viewCount = 0;
    xr.xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
    std::vector<XrViewConfigurationView> configViews(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xr.xrEnumerateViewConfigurationViews(
        instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, configViews.data());
    if (viewCount != 2) {
        Logf("FAIL view count %u\n", viewCount);
        return 3;
    }
    const uint32_t width = configViews[0].recommendedImageRectWidth;
    const uint32_t height = configViews[0].recommendedImageRectHeight;
    Logf("recommended=%ux%u\n", width, height);

    struct Eye {
        XrSwapchain swapchain{XR_NULL_HANDLE};
        std::vector<ComPtr<ID3D11RenderTargetView>> rtvs;
    } eyes[2];
    for (int e = 0; e < 2; e++) {
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        info.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        info.sampleCount = 1;
        info.width = width;
        info.height = height;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (!Check(xr.xrCreateSwapchain(session, &info, &eyes[e].swapchain), "xrCreateSwapchain")) {
            return 3;
        }
        uint32_t count = 0;
        xr.xrEnumerateSwapchainImages(eyes[e].swapchain, 0, &count, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xr.xrEnumerateSwapchainImages(eyes[e].swapchain, count, &count, (XrSwapchainImageBaseHeader*)images.data());
        for (auto& image : images) {
            D3D11_RENDER_TARGET_VIEW_DESC rtv{};
            rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            ComPtr<ID3D11RenderTargetView> view;
            device->CreateRenderTargetView(image.texture, &rtv, &view);
            eyes[e].rtvs.push_back(view);
        }
    }

    // GPU load: copies between two 4096x4096 RGBA32F textures (256 MB each), as many per frame
    // as make gpuMs on this GPU (measured once with timestamp queries).
    ComPtr<ID3D11Texture2D> loadSrc, loadDst;
    int copiesPerFrame = 0;
    if (gpuMs > 0) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = 4096;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &loadSrc)) || FAILED(device->CreateTexture2D(&desc, nullptr, &loadDst))) {
            Logf("FAIL GPU load textures\n");
            return 3;
        }
        D3D11_QUERY_DESC disjointDesc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, stampDesc{D3D11_QUERY_TIMESTAMP, 0};
        ComPtr<ID3D11Query> disjoint, begin, end;
        device->CreateQuery(&disjointDesc, &disjoint);
        device->CreateQuery(&stampDesc, &begin);
        device->CreateQuery(&stampDesc, &end);
        const int probe = 20;
        double probeMs = 0;
        for (int attempt = 0; attempt < 2; attempt++) { // the first round warms the GPU clocks up
            context->Begin(disjoint.Get());
            context->End(begin.Get());
            for (int i = 0; i < probe; i++) {
                context->CopyResource((i & 1) ? loadSrc.Get() : loadDst.Get(), (i & 1) ? loadDst.Get() : loadSrc.Get());
            }
            context->End(end.Get());
            context->End(disjoint.Get());
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing{};
            UINT64 t0 = 0, t1 = 0;
            while (context->GetData(disjoint.Get(), &timing, sizeof(timing), 0) != S_OK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            while (context->GetData(begin.Get(), &t0, sizeof(t0), 0) != S_OK || context->GetData(end.Get(), &t1, sizeof(t1), 0) != S_OK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!timing.Disjoint && timing.Frequency) {
                probeMs = (double)(t1 - t0) * 1000.0 / (double)timing.Frequency;
            }
        }
        const double perCopy = probeMs > 0 ? probeMs / probe : 0.3;
        copiesPerFrame = std::max(1, (int)std::lround(gpuMs / perCopy));
        Logf("GPU load: %.3f ms per copy, %d copies per frame for %.1f ms\n", perCopy, copiesPerFrame, gpuMs);
    }

    bool running = false, exitRequested = false, quit = false;
    uint64_t frames = 0;
    const auto start = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
    while (!quit) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        while (xr.xrPollEvent(instance, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const auto& changed = *(XrEventDataSessionStateChanged*)&event;
                Logf("state=%d at %.3f s\n", (int)changed.state, elapsed());
                if (changed.state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (!Check(xr.xrBeginSession(session, &begin), "xrBeginSession")) {
                        return 3;
                    }
                    running = true;
                } else if (changed.state == XR_SESSION_STATE_STOPPING) {
                    xr.xrEndSession(session);
                    running = false;
                } else if (changed.state == XR_SESSION_STATE_EXITING || changed.state == XR_SESSION_STATE_LOSS_PENDING) {
                    quit = true;
                }
            }
            event = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (!running) {
            if (elapsed() > seconds + 10) {
                Logf("FAIL session never ran / never stopped\n");
                return 3;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (!exitRequested && elapsed() > seconds) {
            xr.xrRequestExitSession(session);
            exitRequested = true;
        }

        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        if (!Check(xr.xrWaitFrame(session, nullptr, &frameState), "xrWaitFrame") ||
            !Check(xr.xrBeginFrame(session, nullptr), "xrBeginFrame")) {
            return 3;
        }
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = frameState.predictedDisplayTime;
        locate.space = space;
        XrViewState viewState{XR_TYPE_VIEW_STATE};
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t located = 0;
        xr.xrLocateViews(session, &locate, &viewState, 2, &located, views);

        XrCompositionLayerProjectionView projection[2]{};
        for (int e = 0; e < 2; e++) {
            uint32_t index = 0;
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = XR_INFINITE_DURATION;
            xr.xrAcquireSwapchainImage(eyes[e].swapchain, nullptr, &index);
            xr.xrWaitSwapchainImage(eyes[e].swapchain, &wait);
            const float yaw = YawDegrees(views[e].pose.orientation);
            const float g = std::fmin(255.f, std::fmax(0.f, std::round(yaw + 128.f)));
            const float color[4] = {(float)(frames % 256) / 255.f, g / 255.f, (64.f + 128.f * e) / 255.f, 1.f};
            context->ClearRenderTargetView(eyes[e].rtvs[index].Get(), color);
            if (e == 0) {
                // Before the release, so the frame is only complete once the load has run.
                for (int i = 0; i < copiesPerFrame; i++) {
                    context->CopyResource((i & 1) ? loadSrc.Get() : loadDst.Get(), (i & 1) ? loadDst.Get() : loadSrc.Get());
                }
            }
            if (context1) {
                const float white[4] = {1, 1, 1, 1};
                const D3D11_RECT square{100, 200, 164, 264};
                context1->ClearView(eyes[e].rtvs[index].Get(), white, &square, 1);
            }
            xr.xrReleaseSwapchainImage(eyes[e].swapchain, nullptr);

            projection[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            projection[e].pose = views[e].pose;
            projection[e].fov = views[e].fov;
            projection[e].subImage.swapchain = eyes[e].swapchain;
            projection[e].subImage.imageRect.extent = {(int32_t)width, (int32_t)height};
        }
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        layer.space = space;
        layer.viewCount = 2;
        layer.views = projection;
        const XrCompositionLayerBaseHeader* layers[] = {(const XrCompositionLayerBaseHeader*)&layer};
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = frameState.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = frameState.shouldRender ? 1 : 0;
        end.layers = layers;
        if (!Check(xr.xrEndFrame(session, &end), "xrEndFrame")) {
            return 3;
        }
        frames++;
        if (sleepMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        }
    }
    Logf("frames=%llu in %.3f s\n", (unsigned long long)frames, elapsed());
    // Swapchains go with the session.
    Logf("destroying session\n");
    xr.xrDestroySession(session);
    Logf("destroying instance\n");
    xr.xrDestroyInstance(instance);
    Logf("destroyed\n");
    Logf("RESULT: %s\n", frames > 0 ? "PASS" : "FAIL");
    return frames > 0 ? 0 : 1;
}
