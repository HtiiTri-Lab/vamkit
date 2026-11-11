#pragma once

// Build-only stand-in for the AMD FidelityFX SDK v1.1.4 optical-flow host API.
// This fork is built without the pinned SDK checkout: every entry point
// reports failure, so the FidelityFX backend never initializes and the
// synthesizer refuses it. The NVIDIA Optical Flow and DLSS Frame Generation
// paths do not use any of this.

#include <cstddef>
#include <cstdint>

using FfxErrorCode = std::int32_t;
constexpr FfxErrorCode FFX_OK = 0;
constexpr FfxErrorCode FFX_ERROR_BACKEND_API_ERROR = static_cast<FfxErrorCode>(0x80000005);

constexpr std::uint32_t FFX_OPTICALFLOW_CONTEXT_COUNT = 1;

enum FfxResourceStates : std::uint32_t {
    FFX_RESOURCE_STATE_UNORDERED_ACCESS = 1u << 0,
    FFX_RESOURCE_STATE_COMPUTE_READ = 1u << 1,
    FFX_RESOURCE_STATE_PIXEL_READ = 1u << 2,
};

enum FfxBackbufferTransferFunction : std::uint32_t {
    FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB = 0,
};

enum FfxSurfaceFormat : std::uint32_t {
    FFX_SURFACE_FORMAT_UNKNOWN = 0,
};

struct FfxDimensions2D {
    std::uint32_t width;
    std::uint32_t height;
};

struct FfxFloatCoords2D {
    float x;
    float y;
};

struct FfxResourceDescription {
    std::uint32_t type;
    FfxSurfaceFormat format;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t depth;
    std::uint32_t mipCount;
    std::uint32_t flags;
    std::uint32_t usage;
};

struct FfxResource {
    void* resource;
    FfxResourceDescription description;
    FfxResourceStates state;
};

using FfxCommandList = void*;
using FfxDevice = void*;

struct FfxInterface {
    void* scratchBuffer;
    std::size_t scratchBufferSize;
};

struct FfxCreateResourceDescription {
    FfxResourceDescription resourceDescription;
};

struct FfxOpticalflowContextDescription {
    FfxInterface backendInterface;
    std::uint32_t flags;
    FfxDimensions2D resolution;
};

struct FfxOpticalflowContext {
    std::uint32_t data[4];
};

struct FfxOpticalflowSharedResourceDescriptions {
    FfxCreateResourceDescription opticalFlowVector;
    FfxCreateResourceDescription opticalFlowSCD;
};

struct FfxOpticalflowDispatchDescription {
    FfxCommandList commandList;
    FfxResource color;
    FfxResource opticalFlowVector;
    FfxResource opticalFlowSCD;
    bool reset;
    FfxBackbufferTransferFunction backbufferTransferFunction;
    FfxFloatCoords2D minMaxLuminance;
};

inline FfxErrorCode ffxOpticalflowContextCreate(
    FfxOpticalflowContext*, const FfxOpticalflowContextDescription*) noexcept {
    return FFX_ERROR_BACKEND_API_ERROR;
}

inline FfxErrorCode ffxOpticalflowGetSharedResourceDescriptions(
    FfxOpticalflowContext*, FfxOpticalflowSharedResourceDescriptions*) noexcept {
    return FFX_ERROR_BACKEND_API_ERROR;
}

inline FfxErrorCode ffxOpticalflowContextDispatch(
    FfxOpticalflowContext*, const FfxOpticalflowDispatchDescription*) noexcept {
    return FFX_ERROR_BACKEND_API_ERROR;
}

inline FfxErrorCode ffxOpticalflowContextDestroy(FfxOpticalflowContext*) noexcept {
    return FFX_OK;
}
