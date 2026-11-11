// VaM Scene Load 1.6.0: how full video memory is, for unloading inactive appearance items when it fills
// (VaMSceneLoad's SceneSwitch.cs). The adapter is the hardware one with the most dedicated memory (the one VaM renders
// on in this setup); usage is this process's on it (DXGI QueryVideoMemoryInfo, local segment group), budget what the
// OS gives this process there.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_4.h>
#include <stdint.h>

static SRWLOCK gLock = SRWLOCK_INIT;
static IDXGIAdapter3* gAdapter = nullptr;
static bool gTried = false;

static IDXGIAdapter3* Adapter()
{
    AcquireSRWLockExclusive(&gLock);
    if (!gTried) {
        gTried = true;
        IDXGIFactory1* f = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f))) {
            SIZE_T best = 0;
            IDXGIAdapter1* a = nullptr;
            for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
                DXGI_ADAPTER_DESC1 d;
                IDXGIAdapter3* a3 = nullptr;
                if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory > best
                    && SUCCEEDED(a->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&a3))) {
                    if (gAdapter) gAdapter->Release();
                    gAdapter = a3;
                    best = d.DedicatedVideoMemory;
                }
                a->Release();
            }
            f->Release();
        }
    }
    IDXGIAdapter3* r = gAdapter;
    ReleaseSRWLockExclusive(&gLock);
    return r;
}

// bytes this process uses on the adapter's local memory and its budget there; 1, or 0 when there is no such adapter
extern "C" __declspec(dllexport) int SL_VideoMemory(int64_t* usage, int64_t* budget)
{
    IDXGIAdapter3* a = Adapter();
    DXGI_QUERY_VIDEO_MEMORY_INFO m;
    if (!a || FAILED(a->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &m))) { *usage = 0; *budget = 0; return 0; }
    *usage = (int64_t)m.CurrentUsage;
    *budget = (int64_t)m.Budget;
    return 1;
}
