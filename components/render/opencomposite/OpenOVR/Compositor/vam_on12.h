#pragma once
// D3D11On12 (the d3d11-proxy component): with its d3d11.dll in VaM's folder, Unity's D3D11 device is a D3D11On12
// device on the process's D3D12 device, and everything VaM draws goes to one D3D12 queue of the proxy's. D3D12 work
// executed on that queue after a Flush of the D3D11 context runs in order with VaM's frame: no fence, no second
// queue, no round trip (measured: each D3D11<->D3D12 round trip cost ~0.3-0.45 ms of GPU).
//
// D3D11 textures reach D3D12 with UnwrapUnderlyingResource (COMMON state, on that queue), then a Flush (the unwrap
// submits nothing: D3D11 work recorded before it must be flushed for D3D12 to see it), and go back with
// ReturnUnderlyingResource (no fences: the same queue orders it). Buffers cannot be unwrapped.
// Each unwrap and return still queues a run of syncs on the queue (~12-15 us of GPU idle a texture with HAGS,
// docs/knowledge/d3d11on12.md): textures handed over every frame are made in D3D12 and wrapped for D3D11 instead
// (CreateWrappedResource, COMMON in and out, kept acquired), and go over with ReleaseWrappedResources (a barrier at
// the next Flush) and come back with AcquireWrappedResources.
#include <d3d11on12.h>
#include <d3d12.h>
#include <psapi.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

// Constant buffers written every frame or every draw are DYNAMIC and written with Map(WRITE_DISCARD), as Unity does.
// Under D3D11On12 an UpdateSubresource of a DEFAULT constant buffer is a copy between two barriers: ~1.6 us of GPU idle
// each (on12_probe cbupdate: 400 updates between draws 0.65 ms of GPU, WRITE_DISCARD 0.005 ms; native D3D11 both ~0).
// The object-motion pass did ~350 a frame: 0.11 -> 0.68 ms in SimXR.
template <class Context>
inline void VamWriteConstants(Context& context, ID3D11Buffer* buffer, const void* data, size_t size) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) throw std::runtime_error("constant buffer Map(WRITE_DISCARD) failed");
    std::memcpy(mapped.pData, data, size);
    context->Unmap(buffer, 0);
}

struct VamOn12 {
    Microsoft::WRL::ComPtr<ID3D11On12Device2> on12;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    explicit operator bool() const { return queue != nullptr; }
};

// The proxy module: the one exporting D3DInterop_GetOn12ForDevice. Not found by name: it renames itself
// d3d11_proxy.dll in the loader's list, but the loader's name hash still files it as d3d11.dll, so
// GetModuleHandle(L"d3d11_proxy.dll") does not find it (seen in SimXR, 2026-10-21).
inline HMODULE VamOn12Proxy() {
    static HMODULE found = [] {
        HMODULE modules[1024];
        DWORD bytes = 0;
        if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) return HMODULE{};
        for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 1024; ++i)
            if (GetProcAddress(modules[i], "D3DInterop_GetOn12ForDevice")) return modules[i];
        return HMODULE{};
    }();
    return found;
}

// The D3D12 device and queue under `device11` when it is the proxy's 11on12 device (Unity's); empty otherwise.
inline VamOn12 VamOn12For(ID3D11Device* device11) {
    VamOn12 r;
    if (!device11 || FAILED(device11->QueryInterface(IID_PPV_ARGS(&r.on12)))) return {};
    HMODULE proxy = VamOn12Proxy();
    using GetForDevice = HRESULT(WINAPI*)(ID3D11Device*, ID3D12Device**, ID3D12CommandQueue**);
    auto get = proxy ? reinterpret_cast<GetForDevice>(GetProcAddress(proxy, "D3DInterop_GetOn12ForDevice")) : nullptr;
    if (!get || FAILED(get(device11, &r.device, &r.queue))) return {};
    return r;
}

// The D3D12 device under Unity's 11on12 device, or null (no proxy, or Unity's device is a real D3D11 one).
inline Microsoft::WRL::ComPtr<ID3D12Device> VamOn12ProcessDevice() {
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    HMODULE proxy = VamOn12Proxy();
    using Get = HRESULT(WINAPI*)(ID3D12Device**, ID3D12CommandQueue**);
    auto get = proxy ? reinterpret_cast<Get>(GetProcAddress(proxy, "D3DInterop_GetOn12")) : nullptr;
    if (!get || FAILED(get(&device, nullptr))) return nullptr;
    return device;
}

// Command lists for the 11on12 queue: a ring of allocators, each reset once the queue has passed its last use.
class VamOn12Commands {
public:
    void Init(ID3D12Device* d, ID3D12CommandQueue* q) {
        if (device) return;
        device = d;
        queue = q;
        Hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "11on12 fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) throw std::runtime_error("11on12 fence event");
    }
    ~VamOn12Commands() { if (event) CloseHandle(event); }
    // An open list; the slot's previous use has finished on the GPU.
    ID3D12GraphicsCommandList* Begin() {
        auto& s = slots[next];
        if (open) { s.list->Close(); open = false; }  // left open by an exception: discarded, never executed
        WaitCpu(s.done);
        if (!s.allocator) {
            Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator)), "11on12 command allocator");
            Hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator.Get(), nullptr, IID_PPV_ARGS(&s.list)), "11on12 command list");
        } else {
            Hr(s.allocator->Reset(), "11on12 allocator reset");
            Hr(s.list->Reset(s.allocator.Get(), nullptr), "11on12 list reset");
        }
        open = true;
        return s.list.Get();
    }
    // Closes and executes the list from Begin, after everything already on the queue.
    void Submit() {
        if (!open) return;
        open = false;
        auto& s = slots[next];
        next = (next + 1) % slots.size();
        const HRESULT closed = s.list->Close();
        if (SUCCEEDED(closed)) {
            ID3D12CommandList* lists[] = {s.list.Get()};
            queue->ExecuteCommandLists(1, lists);
        }
        s.done = ++value;
        Hr(queue->Signal(fence.Get(), value), "11on12 signal");
        Hr(closed, "11on12 list close");
    }
    // Everything executed so far has finished on the GPU (not the D3D11 work after it).
    void Drain() {
        if (!device) return;
        Hr(queue->Signal(fence.Get(), ++value), "11on12 signal");
        WaitCpu(value);
    }
    UINT64 CompletedValue() const { return fence?fence->GetCompletedValue():0; }
    UINT64 SubmittedValue() const { return value; }
    unsigned CurrentSlot() const { if(!open)throw std::runtime_error("11on12 list is not open");return next; }
    void Discard() { if(open){slots[next].list->Close();open=false;} }

private:
    static void Hr(HRESULT hr, const char* what) {
        if (FAILED(hr)) {
            char text[160];
            snprintf(text, sizeof(text), "%s: HRESULT 0x%08lx", what, static_cast<unsigned long>(hr));
            throw std::runtime_error(text);
        }
    }
    void WaitCpu(UINT64 v) {
        if (!v) return;
        const UINT64 completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX) throw std::runtime_error("11on12 device removed");
        if (completed >= v) return;
        Hr(fence->SetEventOnCompletion(v, event), "11on12 fence wait");
        if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0) throw std::runtime_error("11on12 GPU work timed out");
    }
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        UINT64 done = 0;
    };
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 value = 0;
    std::array<Slot, 8> slots;
    unsigned next = 0;
    bool open = false;
};

// A D3D11 texture for a D3D12 list: unwrapped, or released when `wrapped` is its D3D12 resource (one made in D3D12,
// wrapped with COMMON in and out and acquired whenever no D3D12 list has it).
struct VamOn12Texture {
    VamOn12Texture(ID3D11Resource* r = nullptr, ID3D12Resource* wrapped = nullptr) : r11(r), r12(wrapped) {}
    ID3D11Resource* r11;
    ID3D12Resource* r12;
};

// D3D11 textures handed to one D3D12 list; given back to D3D11 in the destructor (also after an exception).
class VamOn12Unwrapped {
public:
    VamOn12Unwrapped(const VamOn12& on12, std::initializer_list<VamOn12Texture> resources) : on12(on12) {
        if (resources.size() > 16) throw std::runtime_error("Too many 11on12 resources");
        ID3D11Resource* release[16];
        UINT releases = 0;
        for (const VamOn12Texture& t : resources) {
            ID3D11Resource* r = t.r11;
            if (!r) { r12[count] = nullptr; r11[count++] = nullptr; continue; }
            if (t.r12) {
                wrapped[count] = true;
                r11[count] = release[releases++] = r;
                r12[count++] = t.r12;
                continue;
            }
            Microsoft::WRL::ComPtr<ID3D12Resource> u;
            const HRESULT hr = on12.on12->UnwrapUnderlyingResource(r, on12.queue.Get(), IID_PPV_ARGS(&u));
            if (FAILED(hr)) {
                ReturnAll();
                char text[96];
                snprintf(text, sizeof(text), "UnwrapUnderlyingResource: HRESULT 0x%08lx", static_cast<unsigned long>(hr));
                throw std::runtime_error(text);
            }
            r11[count] = r;
            r12[count++] = u;
        }
        if (releases) {
            on12.on12->ReleaseWrappedResources(release, releases);
            released = true;
        }
    }
    ~VamOn12Unwrapped() { ReturnAll(); }
    ID3D12Resource* operator[](size_t i) const { return r12[i].Get(); }
    // COMMON -> the given states for the non-null resources (and back with Restore).
    void Transition(ID3D12GraphicsCommandList* list, std::initializer_list<D3D12_RESOURCE_STATES> states) {
        Barriers(list, states, false);
    }
    void Restore(ID3D12GraphicsCommandList* list, std::initializer_list<D3D12_RESOURCE_STATES> states) {
        Barriers(list, states, true);
    }

private:
    void Barriers(ID3D12GraphicsCommandList* list, std::initializer_list<D3D12_RESOURCE_STATES> states, bool back) {
        D3D12_RESOURCE_BARRIER b[16]{};
        UINT n = 0;
        size_t i = 0;
        for (D3D12_RESOURCE_STATES s : states) {
            if (i < count && r12[i] && s != D3D12_RESOURCE_STATE_COMMON) {
                b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b[n].Transition.pResource = r12[i].Get();
                b[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b[n].Transition.StateBefore = back ? s : D3D12_RESOURCE_STATE_COMMON;
                b[n].Transition.StateAfter = back ? D3D12_RESOURCE_STATE_COMMON : s;
                ++n;
            }
            ++i;
        }
        if (n) list->ResourceBarrier(n, b);
    }
    void ReturnAll() {
        ID3D11Resource* acquire[16];
        UINT acquires = 0;
        for (size_t i = 0; i < count; ++i) {
            if (r11[i] && wrapped[i]) {
                if (released) acquire[acquires++] = r11[i];
            } else if (r11[i]) {
                on12.on12->ReturnUnderlyingResource(r11[i], 0, nullptr, nullptr);
            }
            r11[i] = nullptr;
            r12[i].Reset();
            wrapped[i] = false;
        }
        if (acquires) on12.on12->AcquireWrappedResources(acquire, acquires);
        count = 0;
        released = false;
    }
    const VamOn12& on12;
    ID3D11Resource* r11[16]{};
    Microsoft::WRL::ComPtr<ID3D12Resource> r12[16];
    bool wrapped[16]{};
    bool released = false;
    size_t count = 0;
};
