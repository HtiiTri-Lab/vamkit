// The probes' D3D12 helpers: one device, queue, list and fence, the debug layer's messages, buffers and read backs.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include "rtcore.h"          // the RtCore tree under test (build.cmd: /I <rt-core>\include)

using Microsoft::WRL::ComPtr;

struct Gpu {
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList4> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    uint64_t fenceValue = 0, frequency = 0;
    ComPtr<ID3D12InfoQueue> info;
    std::vector<ComPtr<ID3D12Resource>> keep;
    int errors = 0;
} G;

static void Begin() { G.alloc->Reset(); G.list->Reset(G.alloc.Get(), nullptr); }
static void SubmitWait() {
    G.list->Close();
    ID3D12CommandList *l[] = {G.list.Get()};
    G.queue->ExecuteCommandLists(1, l);
    G.queue->Signal(G.fence.Get(), ++G.fenceValue);
    G.fence->SetEventOnCompletion(G.fenceValue, G.event);
    WaitForSingleObject(G.event, INFINITE);
    G.keep.clear();
}
static void Drain(const char *where) {
    if (!G.info) return;
    for (UINT64 i = 0, n = G.info->GetNumStoredMessages(); i < n; i++) {
        SIZE_T bytes = 0;
        G.info->GetMessage(i, nullptr, &bytes);
        std::vector<uint8_t> buf(bytes);
        auto *m = (D3D12_MESSAGE *)buf.data();
        G.info->GetMessage(i, m, &bytes);
        if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR && G.errors++ < 12) printf("D3D12 ERROR (%s): %s\n", where, m->pDescription);
    }
    G.info->ClearStoredMessages();
}
static ComPtr<ID3D12Resource> MakeBuffer(uint64_t bytes, D3D12_HEAP_TYPE heap) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = std::max<uint64_t>(bytes, 256);
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_RESOURCE_STATES s = heap == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                  : heap == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON;
    ComPtr<ID3D12Resource> r;
    if (FAILED(G.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, s, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}
static void Transition(ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x = {};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition.pResource = r;
    x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    x.Transition.StateBefore = a;
    x.Transition.StateAfter = b;
    G.list->ResourceBarrier(1, &x);
}
// Reads one subresource of a texture left in `state` into tight rows.
static std::vector<uint16_t> ReadTexture(ID3D12Resource *t, uint32_t slice, D3D12_RESOURCE_STATES state, uint32_t comps) {
    const D3D12_RESOURCE_DESC d = t->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 total = 0;
    G.device->GetCopyableFootprints(&d, slice, 1, 0, &fp, nullptr, nullptr, &total);
    auto rb = MakeBuffer(total, D3D12_HEAP_TYPE_READBACK);
    Begin();
    Transition(t, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src = {t, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    src.SubresourceIndex = slice;
    G.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(t, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    SubmitWait();
    const uint32_t w = (uint32_t)d.Width, h = d.Height;
    std::vector<uint16_t> out((size_t)w * h * comps);
    uint8_t *p = nullptr;
    rb->Map(0, nullptr, (void **)&p);
    for (uint32_t y = 0; y < h; y++) memcpy(&out[(size_t)y * w * comps], p + fp.Offset + (size_t)y * fp.Footprint.RowPitch, (size_t)w * comps * 2);
    rb->Unmap(0, nullptr);
    return out;
}
