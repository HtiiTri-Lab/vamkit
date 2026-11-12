// unwrap_probe.exe (installed as vamkit/bin/unwrap_probe.exe): does a d3d11on12.dll stop its own queue when
// UnwrapUnderlyingResource names that queue?
//
// Before Microsoft's fix (microsoft/D3D11On12 issue #51, 2025-09-25) the unwrap put a Wait for the command list that
// holds D3D11 work on the texture onto the queue it is given, ahead of that list's Signal. Given 11on12's own queue,
// the queue never runs again. Windows 10 keeps that d3d11on12.dll; Windows 11 has it until an update brings the fix
// (24H2's first build 26100.1 has it, 26100.9549 does not). Naming a queue that runs no work instead is safe on every
// build but costs about 5 % of the frame rate in VR without path tracing (2026-11-12, the dance scene on the simulated
// headset: 108.6 fps on average over five sessions naming 11on12's queue, 103.1 over three naming ours; sessions vary
// by about 4 %). So the proxy runs this probe in a process of its own at start-up, on the d3d11on12.dll
// VaM loaded, and unwraps name 11on12's queue only once it has come back with exit 0 (d3d11_proxy.cpp,
// D3DInterop_UnwrapOnOwnQueue; the compositor's VamOn12UnwrapTarget, the skin path tracer's PathUnwrapQueue). A probe
// that stops only stops itself and is ended.
//
// unwrap_probe.exe <own|other> [dll <path>] [worker0] [flushfirst] [rounds N]
//   own:        unwrap naming 11on12's own queue (what the proxy asks)
//   other:      unwrap naming a queue that runs no work; the D3D12 work still runs on 11on12's queue
//   dll <path>: load this d3d11on12.dll first (the D3D11 runtime then takes it): the one VaM loaded
//   worker0:    D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS (no 11on12 worker thread)
//   flushfirst: Flush the D3D11 context before the unwrap too (does not help)
// Each round: clear a texture in D3D11, unwrap it, Flush, copy it on 11on12's queue in D3D12, signal, wait up to 3 s.
// Exit 0: every round completed. Exit 2: 11on12's queue stopped (an old d3d11on12.dll and `own`). Exit 3: something
// failed before (no adapter, no 11on12). The process is ended with TerminateProcess either way: a device whose queue
// stopped would never be released.
//
// To try an old d3d11on12.dll on a machine with the fix, pass it with `dll` (Windows keeps old builds in
// C:\Windows\WinSxS\amd64_microsoft-windows-directx-direct3d11on12_*\), or copy it beside VaM.exe in a test copy of
// VaM: the D3D11 runtime loads d3d11on12.dll from the application's folder first. Never into System32.
#include <windows.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
using Microsoft::WRL::ComPtr;

static void Hr(HRESULT h, const char* what) {
    if (FAILED(h)) { printf("FAILED %s: 0x%08lx\n", what, static_cast<unsigned long>(h)); fflush(stdout); TerminateProcess(GetCurrentProcess(), 3); }
}

static std::string Version(const wchar_t* path) {
    DWORD ignored = 0, size = GetFileVersionInfoSizeW(path, &ignored);
    if (!size) return "?";
    std::string block(size, '\0');
    VS_FIXEDFILEINFO* fi = nullptr; UINT n = 0;
    if (!GetFileVersionInfoW(path, 0, size, block.data()) || !VerQueryValueW(block.data(), L"\\", reinterpret_cast<void**>(&fi), &n) || !fi) return "?";
    char v[64];
    snprintf(v, sizeof v, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    return v;
}

int wmain(int argc, wchar_t** argv) {
    bool other = false, worker0 = false, flushFirst = false;
    int rounds = 5;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"other")) other = true;
        else if (!wcscmp(argv[i], L"own")) other = false;
        else if (!wcscmp(argv[i], L"worker0")) worker0 = true;
        else if (!wcscmp(argv[i], L"flushfirst")) flushFirst = true;
        else if (!wcscmp(argv[i], L"rounds") && i + 1 < argc) rounds = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"dll") && i + 1 < argc) {
            // Loaded before the D3D11 runtime asks for d3d11on12.dll by name, so the runtime gets this one.
            if (!LoadLibraryW(argv[++i])) { printf("FAILED to load %ls: %lu\n", argv[i], GetLastError()); fflush(stdout); TerminateProcess(GetCurrentProcess(), 3); }
        }
    }
    ComPtr<IDXGIFactory6> factory; Hr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    ComPtr<IDXGIAdapter1> adapter; Hr(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)), "adapter");
    DXGI_ADAPTER_DESC1 ad{}; adapter->GetDesc1(&ad);
    ComPtr<ID3D12Device> d12; Hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12)), "D3D12CreateDevice");
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue, idle;
    Hr(d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    Hr(d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&idle)), "queue that runs no work");
    const UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED | (worker0 ? D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS : 0);
    ComPtr<ID3D11Device> d11; ComPtr<ID3D11DeviceContext> ctx;
    IUnknown* queues[] = { queue.Get() };
    // System32's d3d11.dll by its full path, not by import: vamkit\bin, this program's folder, also holds the proxy
    // d3d11.dll, which the loader would take first.
    wchar_t system[MAX_PATH] = L"";
    GetSystemDirectoryW(system, MAX_PATH);
    HMODULE d3d11 = LoadLibraryW((std::wstring(system) + L"\\d3d11.dll").c_str());
    const auto create = d3d11 ? reinterpret_cast<PFN_D3D11ON12_CREATE_DEVICE>(GetProcAddress(d3d11, "D3D11On12CreateDevice")) : nullptr;
    if (!create) Hr(E_NOINTERFACE, "System32's D3D11On12CreateDevice");
    Hr(create(d12.Get(), flags, nullptr, 0, queues, 1, 0, &d11, &ctx, nullptr), "D3D11On12CreateDevice");
    ComPtr<ID3D11On12Device2> on12; Hr(d11.As(&on12), "ID3D11On12Device2");
    wchar_t path[MAX_PATH] = L"(not loaded)";
    if (HMODULE m = GetModuleHandleW(L"d3d11on12.dll")) GetModuleFileNameW(m, path, MAX_PATH);
    printf("adapter %ls; %ls %s; flags 0x%x; the unwrap names %s; flush before it: %s\n", ad.Description, path, Version(path).c_str(), flags,
           other ? "a queue that runs no work" : "11on12's own queue", flushFirst ? "yes" : "no");

    D3D11_TEXTURE2D_DESC td{}; td.Width = td.Height = 256; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex; Hr(d11->CreateTexture2D(&td, nullptr, &tex), "texture");
    ComPtr<ID3D11RenderTargetView> rtv; Hr(d11->CreateRenderTargetView(tex.Get(), nullptr, &rtv), "render target view");
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = rd.Height = 256; rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> copy; Hr(d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&copy)), "copy target");
    ComPtr<ID3D12CommandAllocator> alloc; Hr(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "allocator");
    ComPtr<ID3D12GraphicsCommandList> list; Hr(d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
    list->Close();
    ComPtr<ID3D12Fence> fence; Hr(d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    double total = 0;
    for (int round = 1; round <= rounds; ++round) {
        const auto t0 = std::chrono::steady_clock::now();
        const float color[4] = { round * 0.1f, 0.5f, 0.25f, 1.f };
        ctx->ClearRenderTargetView(rtv.Get(), color);              // D3D11 work on the texture, not yet submitted
        if (flushFirst) ctx->Flush();
        ComPtr<ID3D12Resource> r12;
        Hr(on12->UnwrapUnderlyingResource(tex.Get(), other ? idle.Get() : queue.Get(), IID_PPV_ARGS(&r12)), "UnwrapUnderlyingResource");
        ctx->Flush();
        Hr(alloc->Reset(), "allocator reset"); Hr(list->Reset(alloc.Get(), nullptr), "list reset");
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = { r12.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE };
        list->ResourceBarrier(1, &b);
        list->CopyResource(copy.Get(), r12.Get());
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        list->ResourceBarrier(1, &b);
        Hr(list->Close(), "list close");
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);                       // on 11on12's queue, in order with the D3D11 work
        Hr(queue->Signal(fence.Get(), round), "signal");
        Hr(fence->SetEventOnCompletion(round, event), "fence event");
        if (WaitForSingleObject(event, 3000) != WAIT_OBJECT_0) {
            printf("round %d: STOPPED - 11on12's queue did not reach the signal after the unwrap in 3 s (fence at %llu, wanted %d)\n",
                   round, fence->GetCompletedValue(), round);
            fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 2);
        }
        on12->ReturnUnderlyingResource(tex.Get(), 0, nullptr, nullptr);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (round > 1) total += ms;
    }
    printf("all %d rounds completed; mean round after the first %.3f ms\n", rounds, rounds > 1 ? total / (rounds - 1) : 0.0);
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);
}
