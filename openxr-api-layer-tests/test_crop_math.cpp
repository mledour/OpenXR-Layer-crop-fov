// MIT License
//
// Copyright (c) 2026 mledour
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
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

#include <doctest/doctest.h>

#include <limits>

#include <utils/crop_math.h>

using openxr_api_layer::clampFactor;
using openxr_api_layer::computeCroppedImageRect;
using openxr_api_layer::CropConfig;
using openxr_api_layer::Extent2D;
using openxr_api_layer::narrowFov;
using openxr_api_layer::scaleSwapchainExtents;

// ---------------------------------------------------------------------------
// clampFactor
// ---------------------------------------------------------------------------

TEST_CASE("clampFactor: maps percent to factor = 1 - percent/100") {
    CHECK(clampFactor(0.0f) == doctest::Approx(1.0f));
    CHECK(clampFactor(10.0f) == doctest::Approx(0.9f));
    CHECK(clampFactor(25.0f) == doctest::Approx(0.75f));
    CHECK(clampFactor(50.0f) == doctest::Approx(0.5f));
}

TEST_CASE("clampFactor: clamps negative percents to 1.0 (no crop)") {
    CHECK(clampFactor(-0.01f) == doctest::Approx(1.0f));
    CHECK(clampFactor(-100.0f) == doctest::Approx(1.0f));
}

TEST_CASE("clampFactor: clamps percents above 50 to 0.5 (hard crop limit)") {
    CHECK(clampFactor(50.01f) == doctest::Approx(0.5f));
    CHECK(clampFactor(99.0f) == doctest::Approx(0.5f));
    CHECK(clampFactor(1000.0f) == doctest::Approx(0.5f));
}

TEST_CASE("clampFactor: NaN/infinity policy - infinity is clamped to 0.5") {
    // Not part of the "documented" API contract but worth pinning: +inf
    // percent should not turn into a negative factor. The current
    // implementation treats +inf > 50.0f as true, so it returns 0.5f.
    const float posInf = std::numeric_limits<float>::infinity();
    CHECK(clampFactor(posInf) == doctest::Approx(0.5f));

    const float negInf = -std::numeric_limits<float>::infinity();
    CHECK(clampFactor(negInf) == doctest::Approx(1.0f));
}

// ---------------------------------------------------------------------------
// scaleSwapchainExtents
// ---------------------------------------------------------------------------

TEST_CASE("scaleSwapchainExtents: factors = 1.0 preserve even dimensions") {
    CropConfig cfg;
    cfg.cropLeftFactor = cfg.cropRightFactor = cfg.cropTopFactor = cfg.cropBottomFactor = 1.0f;

    const Extent2D out = scaleSwapchainExtents(1920, 1080, cfg);
    CHECK(out.width == 1920u);
    CHECK(out.height == 1080u);
}

TEST_CASE("scaleSwapchainExtents: symmetric 10% crop shrinks both axes by 10%") {
    CropConfig cfg;
    cfg.cropLeftFactor = cfg.cropRightFactor = 0.9f;
    cfg.cropTopFactor = cfg.cropBottomFactor = 0.9f;

    const Extent2D out = scaleSwapchainExtents(2000, 2000, cfg);
    CHECK(out.width == 1800u);
    CHECK(out.height == 1800u);
}

TEST_CASE("scaleSwapchainExtents: asymmetric factors take the smaller one per axis") {
    // width uses min(left, right); height uses min(top, bottom).
    CropConfig cfg;
    cfg.cropLeftFactor = 0.9f;   // -> would give 1800
    cfg.cropRightFactor = 0.8f;  // -> would give 1600; min wins
    cfg.cropTopFactor = 0.7f;    // -> would give 1400; min wins
    cfg.cropBottomFactor = 0.85f; // -> would give 1700

    const Extent2D out = scaleSwapchainExtents(2000, 2000, cfg);
    CHECK(out.width == 1600u);
    CHECK(out.height == 1400u);
}

TEST_CASE("scaleSwapchainExtents: rounds down to the nearest multiple of 8") {
    CropConfig cfg;
    cfg.cropLeftFactor = cfg.cropRightFactor = 0.9f;
    cfg.cropTopFactor = cfg.cropBottomFactor = 0.9f;

    // 1111 * 0.9 = 999.9 -> trunc 999 -> aligned down to 992 (= 124 * 8).
    const Extent2D out = scaleSwapchainExtents(1111, 1111, cfg);
    CHECK(out.width == 992u);
    CHECK(out.height == 992u);
    CHECK(out.width % 8u == 0u);
    CHECK(out.height % 8u == 0u);
}

TEST_CASE("scaleSwapchainExtents: enforces the alignment value as floor") {
    CropConfig cfg;
    cfg.cropLeftFactor = cfg.cropRightFactor = 0.5f;
    cfg.cropTopFactor = cfg.cropBottomFactor = 0.5f;

    // 1 * 0.5 = 0.5 -> trunc 0 -> floored to 8 (the alignment value).
    const Extent2D out = scaleSwapchainExtents(1, 1, cfg);
    CHECK(out.width == 8u);
    CHECK(out.height == 8u);
}

TEST_CASE("scaleSwapchainExtents: result is always aligned to 8 for any reasonable input") {
    // Property: for any swapchain dimension and any reasonable config,
    // the output is divisible by 8 and >= 8.
    CropConfig cfg;
    cfg.cropLeftFactor = 0.91f;   // non-trivial values likely to produce odd
    cfg.cropRightFactor = 0.87f;  // intermediate widths before alignment
    cfg.cropTopFactor = 0.83f;
    cfg.cropBottomFactor = 0.77f;

    for (uint32_t w = 8; w <= 8192; w += 37) {
        for (uint32_t h = 8; h <= 8192; h += 41) {
            const Extent2D out = scaleSwapchainExtents(w, h, cfg);
            CHECK(out.width % 8u == 0u);
            CHECK(out.height % 8u == 0u);
            CHECK(out.width >= 8u);
            CHECK(out.height >= 8u);
        }
    }
}

// ---------------------------------------------------------------------------
// computeCroppedImageRect
// ---------------------------------------------------------------------------

// Default "typical VR" FOV: 45° half-angle on each side (90° per-eye total).
// tan(π/4) = 1.0 exactly, so tanL = tanR = tanU = tanD = 1 and totalTan = 2
// on both axes — makes hand-checked expected values easy to derive.
static constexpr float kPiOver4 = 0.7853981633974483f; // π / 4 radians = 45°
static constexpr XrFovf kDefaultFov = {-kPiOver4, kPiOver4, kPiOver4, -kPiOver4};

TEST_CASE("computeCroppedImageRect: factors = 1.0 yield a full-swapchain rect") {
    CropConfig cfg;
    cfg.cropLeftFactor = cfg.cropRightFactor = 1.0f;
    cfg.cropTopFactor = cfg.cropBottomFactor = 1.0f;

    const XrRect2Di rect = computeCroppedImageRect(1920, 1080, kDefaultFov, cfg);
    CHECK(rect.offset.x == 0);
    CHECK(rect.offset.y == 0);
    CHECK(rect.extent.width == 1920);
    CHECK(rect.extent.height == 1080);
}

TEST_CASE("computeCroppedImageRect: symmetric 10% crop at 45° half-FOV (tan-correct math)") {
    // tanL = tanR = tan(π/4) = 1.0
    // tanSub = tan(0.9 * π/4) = tan(40.5°) ≈ 0.854081
    // xLeft  = 1000 * (1.0 - 0.854081) / 2.0 ≈ 72.96  -> 73 (round-half-up)
    // xRight = 1000 * (1.0 + 0.854081) / 2.0 ≈ 927.04 -> 927
    // width  = 927.04 - 72.96 ≈ 854.08 -> 854
    // Note: the old linear-in-angle approximation gave 50/900 here. The
    // tan-based math correctly accounts for the non-linear pixel<->angle
    // mapping under perspective projection.
    CropConfig cfg;
    cfg.cropLeftFactor = cfg.cropRightFactor = 0.9f;
    cfg.cropTopFactor = cfg.cropBottomFactor = 0.9f;

    const XrRect2Di rect = computeCroppedImageRect(1000, 1000, kDefaultFov, cfg);
    CHECK(rect.offset.x == 73);
    CHECK(rect.offset.y == 73);
    CHECK(rect.extent.width == 854);
    CHECK(rect.extent.height == 854);
}

TEST_CASE("computeCroppedImageRect: asymmetric factors at 45° half-FOV") {
    // cropLeft=10%, cropRight=20%, cropTop=20%, cropBottom=40%.
    // tan(0.9 * 45°) = tan(40.5°) ≈ 0.854081
    // tan(0.8 * 45°) = tan(36°)   ≈ 0.726543
    // tan(0.6 * 45°) = tan(27°)   ≈ 0.509525
    // xLeft   = 1000 * (1 - 0.854081) / 2 ≈ 72.96  -> 73
    // xRight  = 1000 * (1 + 0.726543) / 2 ≈ 863.27 -> 863
    // width   = 863.27 - 72.96 ≈ 790.31 -> 790
    // yTop    = 1000 * (1 - 0.726543) / 2 ≈ 136.73 -> 137
    // yBottom = 1000 * (1 + 0.509525) / 2 ≈ 754.76 -> 755
    // height  = 754.76 - 136.73 ≈ 618.03 -> 618
    CropConfig cfg;
    cfg.cropLeftFactor = 0.9f;
    cfg.cropRightFactor = 0.8f;
    cfg.cropTopFactor = 0.8f;
    cfg.cropBottomFactor = 0.6f;

    const XrRect2Di rect = computeCroppedImageRect(1000, 1000, kDefaultFov, cfg);
    CHECK(rect.offset.x == 73);
    CHECK(rect.offset.y == 137);
    CHECK(rect.extent.width == 790);
    CHECK(rect.extent.height == 618);
}

TEST_CASE("computeCroppedImageRect: narrow FOV converges to linear-in-angle approximation") {
    // At small half-angles, tan(x) ≈ x, so the tan-based math should
    // collapse toward the old linear-in-angle result of (offset = W*(1-k)/2,
    // extent = W*k). Use a 5° half-FOV and a symmetric 10% crop: expected
    // offset should be close to 50 and width close to 900 on a 1000 px edge.
    constexpr float kFiveDegrees = 0.08726646f; // 5° in radians
    const XrFovf narrowFovIn = {-kFiveDegrees, kFiveDegrees, kFiveDegrees, -kFiveDegrees};
    CropConfig cfg;
    cfg.cropLeftFactor = cfg.cropRightFactor = 0.9f;
    cfg.cropTopFactor = cfg.cropBottomFactor = 0.9f;

    const XrRect2Di rect = computeCroppedImageRect(1000, 1000, narrowFovIn, cfg);
    // Accept ±1 pixel: linear approximation differs from tan by a tiny
    // amount even at 5°. Tight enough to catch a regression but not brittle.
    CHECK(rect.offset.x >= 49);
    CHECK(rect.offset.x <= 51);
    CHECK(rect.extent.width >= 898);
    CHECK(rect.extent.width <= 902);
}

TEST_CASE("computeCroppedImageRect: zero swapchain returns zero rect") {
    CropConfig cfg;
    cfg.cropLeftFactor = cfg.cropRightFactor = 0.9f;
    cfg.cropTopFactor = cfg.cropBottomFactor = 0.9f;

    const XrRect2Di rect = computeCroppedImageRect(0, 0, kDefaultFov, cfg);
    CHECK(rect.offset.x == 0);
    CHECK(rect.offset.y == 0);
    CHECK(rect.extent.width == 0);
    CHECK(rect.extent.height == 0);
}

TEST_CASE("computeCroppedImageRect: degenerate (zero-span) FOV returns zero rect") {
    // If the app ever hands us a zero-angle FOV, totalTanX/Y is 0 and the
    // tan-space division would produce NaN. The defensive guard inside the
    // helper should catch it before the division.
    const XrFovf zeroFov = {0.0f, 0.0f, 0.0f, 0.0f};
    CropConfig cfg;
    cfg.cropLeftFactor = cfg.cropRightFactor = 0.9f;
    cfg.cropTopFactor = cfg.cropBottomFactor = 0.9f;

    const XrRect2Di rect = computeCroppedImageRect(1920, 1080, zeroFov, cfg);
    CHECK(rect.offset.x == 0);
    CHECK(rect.offset.y == 0);
    CHECK(rect.extent.width == 0);
    CHECK(rect.extent.height == 0);
}

TEST_CASE("computeCroppedImageRect: offset + extent always fits inside the swapchain") {
    // Property: for any valid config, offset + extent <= swapchain
    // dimensions. Sweep crop factors AND a handful of FOVs (narrow,
    // typical VR, extra-wide).
    const XrFovf fovs[] = {
        {-0.0873f, 0.0873f, 0.0873f, -0.0873f},  // 5° half
        {-0.5236f, 0.5236f, 0.5236f, -0.5236f},  // 30° half
        kDefaultFov,                              // 45° half
        {-0.9599f, 0.9599f, 0.9599f, -0.9599f},  // 55° half (Index-ish)
    };

    for (const XrFovf& fov : fovs) {
        for (float leftPct : {0.0f, 5.0f, 25.0f, 50.0f}) {
            for (float rightPct : {0.0f, 5.0f, 25.0f, 50.0f}) {
                for (float topPct : {0.0f, 5.0f, 25.0f, 50.0f}) {
                    for (float bottomPct : {0.0f, 5.0f, 25.0f, 50.0f}) {
                        CropConfig cfg;
                        cfg.cropLeftFactor = clampFactor(leftPct);
                        cfg.cropRightFactor = clampFactor(rightPct);
                        cfg.cropTopFactor = clampFactor(topPct);
                        cfg.cropBottomFactor = clampFactor(bottomPct);

                        const uint32_t swapW = 1920;
                        const uint32_t swapH = 1080;
                        const XrRect2Di r =
                            computeCroppedImageRect(swapW, swapH, fov, cfg);

                        // zero rect is a valid "skip" signal
                        if (r.extent.width == 0 && r.extent.height == 0) continue;

                        CHECK(r.offset.x >= 0);
                        CHECK(r.offset.y >= 0);
                        CHECK(r.offset.x + r.extent.width <= static_cast<int32_t>(swapW));
                        CHECK(r.offset.y + r.extent.height <= static_cast<int32_t>(swapH));
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// narrowFov
// ---------------------------------------------------------------------------

TEST_CASE("narrowFov: factors = 1.0 leave the FOV untouched") {
    CropConfig cfg; // all factors 1.0 after we zero them below
    cfg.cropLeftFactor = cfg.cropRightFactor = 1.0f;
    cfg.cropTopFactor = cfg.cropBottomFactor = 1.0f;

    const XrFovf orig = {-0.7f, 0.7f, 0.6f, -0.5f};
    const XrFovf out = narrowFov(orig, cfg);

    CHECK(out.angleLeft == doctest::Approx(orig.angleLeft));
    CHECK(out.angleRight == doctest::Approx(orig.angleRight));
    CHECK(out.angleUp == doctest::Approx(orig.angleUp));
    CHECK(out.angleDown == doctest::Approx(orig.angleDown));
}

TEST_CASE("narrowFov: scales every half-angle by its matching factor") {
    CropConfig cfg;
    cfg.cropLeftFactor = 0.8f;
    cfg.cropRightFactor = 0.9f;
    cfg.cropTopFactor = 0.7f;
    cfg.cropBottomFactor = 0.6f;

    const XrFovf orig = {-1.0f, 1.0f, 1.0f, -1.0f};
    const XrFovf out = narrowFov(orig, cfg);

    CHECK(out.angleLeft == doctest::Approx(-0.8f));
    CHECK(out.angleRight == doctest::Approx(0.9f));
    CHECK(out.angleUp == doctest::Approx(0.7f));
    CHECK(out.angleDown == doctest::Approx(-0.6f));
}

TEST_CASE("narrowFov: negative half-angles become less negative (narrower) under factor < 1") {
    // angleLeft and angleDown are negative per spec. Multiplying a negative
    // by a positive factor < 1 moves it toward zero (narrower FOV), which is
    // the intended behavior. Make sure we didn't accidentally invert it.
    CropConfig cfg;
    cfg.cropLeftFactor = 0.5f;
    cfg.cropRightFactor = 0.5f;
    cfg.cropTopFactor = 0.5f;
    cfg.cropBottomFactor = 0.5f;

    const XrFovf orig = {-1.0f, 1.0f, 1.0f, -1.0f};
    const XrFovf out = narrowFov(orig, cfg);

    CHECK(out.angleLeft > orig.angleLeft); // -0.5 > -1.0
    CHECK(out.angleRight < orig.angleRight);
    CHECK(out.angleUp < orig.angleUp);
    CHECK(out.angleDown > orig.angleDown);
}
