#pragma once

// Build-only stand-in for the FidelityFX DX12 backend; see ffx_opticalflow.h.
// A zero scratch size makes create_fidelityfx_resources fail before anything
// else here is reached.

#include <d3d12.h>

#include "../../ffx_opticalflow.h"

inline std::size_t ffxGetScratchMemorySizeDX12(std::size_t) noexcept {
    return 0;
}

inline FfxDevice ffxGetDeviceDX12(ID3D12Device* device) noexcept {
    return device;
}

inline FfxErrorCode ffxGetInterfaceDX12(
    FfxInterface*, FfxDevice, void*, std::size_t, std::uint32_t) noexcept {
    return FFX_ERROR_BACKEND_API_ERROR;
}

inline DXGI_FORMAT ffxGetDX12FormatFromSurfaceFormat(FfxSurfaceFormat) noexcept {
    return DXGI_FORMAT_UNKNOWN;
}

inline FfxCommandList ffxGetCommandListDX12(ID3D12GraphicsCommandList* list) noexcept {
    return list;
}

inline FfxResourceDescription ffxGetResourceDescriptionDX12(ID3D12Resource*) noexcept {
    return {};
}

inline FfxResource ffxGetResourceDX12(
    ID3D12Resource* resource,
    FfxResourceDescription description,
    const wchar_t*,
    FfxResourceStates state) noexcept {
    return {resource, description, state};
}
