#include "../OpenOVR/Compositor/vam_nr_region.h"
#include <cstdio>
#include <limits>
#include <stdexcept>

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        const XrFovf left{std::atan(-1.5f), std::atan(1.f), std::atan(.8f), std::atan(-1.2f)};
        const XrFovf right{std::atan(-1.f), std::atan(1.5f), left.angleUp, left.angleDown};
        const auto l = CalculateVamNrRegion(2000, 2000, .66f, left);
        const auto r = CalculateVamNrRegion(2000, 2000, .66f, right);
        // These frusta project forward to (1200, 800) and (800, 800).
        // The old texture-center placement (340, 340) fails both eyes.
        Require(l.x == 540 && l.y == 140 && l.width == 1320 && l.height == 1320,
                "left forward ray / top-down image orientation");
        Require(r.x == 140 && r.y == 140 && r.width == l.width && r.height == l.height,
                "right eye must mirror horizontal placement, not vertical placement");

        const auto full = CalculateVamNrRegion(1785, 1898, 1.f, left);
        Require(full.x == 0 && full.y == 0 && full.width == 1785 && full.height == 1898,
                "100 percent covers the complete eye, including non-aligned dimensions");
        const auto large = CalculateVamNrRegion(2000, 2000, .9f, left);
        Require(large.x == 200 && large.y == 0 && large.width == 1800 && large.height == 1800,
                "large region keeps its size and stops at texture boundaries");
        const auto unknown = CalculateVamNrRegion(2000, 2000, .66f, {});
        Require(unknown.x == 340 && unknown.y == 340, "missing startup FOV uses safe centered fallback");
        auto invalid = left;
        invalid.angleLeft = std::numeric_limits<float>::quiet_NaN();
        const auto fallback = CalculateVamNrRegion(2000, 2000, .66f, invalid);
        Require(fallback.x == 340 && fallback.y == 140, "invalid horizontal FOV preserves valid vertical alignment");
        invalid = left;
        std::swap(invalid.angleUp, invalid.angleDown);
        const auto reversed = CalculateVamNrRegion(2000, 2000, .66f, invalid);
        Require(reversed.x == 540 && reversed.y == 340, "reversed vertical FOV cannot produce unsafe offsets");

        // Project the actual rectangle center back into view space. Whenever
        // the region fits without clipping, it must land on the forward ray
        // within half a pixel, at every supported scale and odd eye size.
        unsigned checks = 0;
        for (const auto& fov : {left, right}) {
            for (const unsigned width : {4u, 1785u, 2000u, 3072u}) {
                const unsigned height = width + 113;
                for (int percent = 10; percent <= 100; ++percent) {
                    const auto region = CalculateVamNrRegion(width, height, percent / 100.f, fov);
                    Require(region.width > 0 && region.height > 0 && region.x + region.width <= width && region.y + region.height <= height,
                            "all supported area sizes stay within the eye image");
                    if (region.x > 0 && region.x + region.width < width) {
                        const float span = std::tan(fov.angleRight) - std::tan(fov.angleLeft);
                        const float ray = std::tan(fov.angleLeft) + (region.x + region.width * .5f) / width * span;
                        Require(std::abs(ray) <= span * .501f / width, "horizontal rectangle center projects to forward ray");
                    }
                    if (region.y > 0 && region.y + region.height < height) {
                        const float span = std::tan(fov.angleUp) - std::tan(fov.angleDown);
                        const float ray = std::tan(fov.angleUp) - (region.y + region.height * .5f) / height * span;
                        Require(std::abs(ray) <= span * .501f / height, "vertical rectangle center projects to forward ray");
                    }
                    ++checks;
                }
            }
        }
        std::printf("PASS: asymmetric stereo, vertical orientation, full area, edge clamping, invalid FOV; %u geometry cases. CPU only.\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
