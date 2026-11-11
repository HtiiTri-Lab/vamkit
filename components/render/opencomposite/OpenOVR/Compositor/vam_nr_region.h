#pragma once
#include <algorithm>
#include <cmath>
#include <openxr/openxr.h>

struct VamNrRegion {
    unsigned x, y, width, height;
    float centerX, centerY;
};

// Coordinates are in the complete, upright eye image AFTER OpenVR crop/flip.
// Project the forward ray (0, 0, -1) through each eye's asymmetric frustum.
// shiftY ([NR] RegionShiftY / 100) moves that centre up by this share of the
// image height (negative: down).
inline VamNrRegion CalculateVamNrRegion(unsigned width, unsigned height, float scale, const XrFovf& fov, float shiftY = 0.f) {
    const float left = std::tan(fov.angleLeft), right = std::tan(fov.angleRight);
    const float up = std::tan(fov.angleUp), down = std::tan(fov.angleDown);
    const auto center = [](float numerator, float span) {
        if (!std::isfinite(numerator) || !std::isfinite(span) || span <= 1e-6f) return .5f;
        const float value = numerator / span;
        return std::isfinite(value) && value >= 0.f && value <= 1.f ? value : .5f;
    };
    const float cx = center(-left, right - left);
    const float cy = std::clamp(center(up, up - down) - shiftY, 0.f, 1.f); // D3D image y increases downwards.
    const unsigned rw = std::min(width, std::max(4u, (static_cast<unsigned>(width * scale) + 3u) & ~3u));
    const unsigned rh = std::min(height, std::max(4u, (static_cast<unsigned>(height * scale) + 3u) & ~3u));
    const auto offset = [](unsigned extent, unsigned region, float c) {
        // Preserve the chosen area size. Large regions slide only as far as
        // the image edges allow; 100% stays the complete eye image at (0, 0).
        return static_cast<unsigned>(std::clamp(std::lround(c * extent - region * .5f),
                                               0L, static_cast<long>(extent - region)));
    };
    return {offset(width, rw, cx), offset(height, rh, cy), rw, rh, cx, cy};
}
