// MIT License
//
// Copyright (c) 2026 Michael Ledour
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

#include <cmath>
#include <limits>
#include <random>

#include <utils/crop_math.h>
#include <utils/name_utils.h>

using openxr_api_layer::clampFactor;
using openxr_api_layer::computeCroppedImageRect;
using openxr_api_layer::CropConfig;
using openxr_api_layer::Extent2D;
using openxr_api_layer::narrowFov;
using openxr_api_layer::resolvePerAppConfigPath;
using openxr_api_layer::sanitizeForFilename;
using openxr_api_layer::scaleSwapchainExtents;

// ---------------------------------------------------------------------------
// clampFactor
// ---------------------------------------------------------------------------

TEST_CASE("clampFactor: maps percent to factor = 1 - percent/50") {
    // The percent is the fraction of the image covered by the bar on
    // that edge, so percent = 50 is the max (bar reaches the image
    // center) and maps to factor 0 (the tangent on that edge collapses
    // to zero).
    CHECK(clampFactor(0.0f) == doctest::Approx(1.0f));
    CHECK(clampFactor(10.0f) == doctest::Approx(0.8f));
    CHECK(clampFactor(25.0f) == doctest::Approx(0.5f));
    CHECK(clampFactor(50.0f) == doctest::Approx(0.0f));
}

TEST_CASE("clampFactor: clamps negative percents to 1.0 (no crop)") {
    CHECK(clampFactor(-0.01f) == doctest::Approx(1.0f));
    CHECK(clampFactor(-100.0f) == doctest::Approx(1.0f));
}

TEST_CASE("clampFactor: clamps percents above 50 to 0.0 (hard crop limit)") {
    CHECK(clampFactor(50.01f) == doctest::Approx(0.0f));
    CHECK(clampFactor(99.0f) == doctest::Approx(0.0f));
    CHECK(clampFactor(1000.0f) == doctest::Approx(0.0f));
}

TEST_CASE("clampFactor: NaN/infinity policy - infinity is clamped to 0.0") {
    // Not part of the "documented" API contract but worth pinning: +inf
    // percent should not turn into a negative factor. The current
    // implementation treats +inf > 50.0f as true, so it returns 0.0f.
    const float posInf = std::numeric_limits<float>::infinity();
    CHECK(clampFactor(posInf) == doctest::Approx(0.0f));

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

TEST_CASE("scaleSwapchainExtents: asymmetric factors are averaged per axis") {
    // width uses (left + right) / 2; height uses (top + bottom) / 2.
    // This matches the actual tan-extent of a narrowed FOV for symmetric
    // inputs and avoids collapsing the swapchain to zero when one edge
    // is at factor 0 (e.g. crop_bottom 50% -> bar at middle).
    CropConfig cfg;
    cfg.cropLeftFactor = 0.9f;    // avg(0.9, 0.8) = 0.85 -> 2000 * 0.85 = 1700 -> aligned 1696
    cfg.cropRightFactor = 0.8f;
    cfg.cropTopFactor = 0.7f;     // avg(0.7, 0.85) = 0.775 -> 2000 * 0.775 = 1550 -> aligned 1544
    cfg.cropBottomFactor = 0.85f;

    const Extent2D out = scaleSwapchainExtents(2000, 2000, cfg);
    CHECK(out.width == 1696u);
    CHECK(out.height == 1544u);
}

TEST_CASE("scaleSwapchainExtents: factor 0 on a single edge halves the axis (no zero swapchain)") {
    // Regression guard: crop_bottom_percent = 50 produces cropBottomFactor = 0,
    // and the old min-based version dropped the swapchain height to 0 (then
    // floored to 8), which crashed the downstream runtime on real HMDs
    // (observed with Pimax Crystal Light + Le Mans Ultimate).
    CropConfig cfg;
    cfg.cropLeftFactor = 1.0f;
    cfg.cropRightFactor = 1.0f;
    cfg.cropTopFactor = 1.0f;
    cfg.cropBottomFactor = 0.0f;   // bar at middle -> half the axis is content

    const Extent2D out = scaleSwapchainExtents(4352, 5102, cfg);
    // Width unchanged (1.0 + 1.0) / 2 = 1.0 -> 4352 (already aligned).
    CHECK(out.width == 4352u);
    // Height = 5102 * 0.5 = 2551 -> aligned down to 2544 (not 8).
    CHECK(out.height == 2544u);
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

TEST_CASE("narrowFov: scales the tangent of each half-angle by its matching factor") {
    // We work in tan-space so the factor lines up with pixel fractions on
    // the swapchain. For factor f on an input half-angle a, the output is
    // atan(tan(a) * f) — NOT a * f.
    CropConfig cfg;
    cfg.cropLeftFactor = 0.8f;
    cfg.cropRightFactor = 0.9f;
    cfg.cropTopFactor = 0.7f;
    cfg.cropBottomFactor = 0.6f;

    const XrFovf orig = {-1.0f, 1.0f, 1.0f, -1.0f};
    const XrFovf out = narrowFov(orig, cfg);

    CHECK(out.angleLeft == doctest::Approx(std::atan(std::tan(-1.0f) * 0.8f)));
    CHECK(out.angleRight == doctest::Approx(std::atan(std::tan(1.0f) * 0.9f)));
    CHECK(out.angleUp == doctest::Approx(std::atan(std::tan(1.0f) * 0.7f)));
    CHECK(out.angleDown == doctest::Approx(std::atan(std::tan(-1.0f) * 0.6f)));
}

TEST_CASE("narrowFov: factor of 0.5 puts each edge at 50%% of the original tangent") {
    // Concrete invariant the user cares about: crop_bottom_percent: 50 should
    // put the bottom black bar at the vertical center of the image. That
    // means tan(narrowed_angleDown) == 0.5 * tan(original_angleDown).
    CropConfig cfg;
    cfg.cropLeftFactor = 0.5f;
    cfg.cropRightFactor = 0.5f;
    cfg.cropTopFactor = 0.5f;
    cfg.cropBottomFactor = 0.5f;

    const XrFovf orig = {-1.0f, 1.0f, 0.8f, -0.9f};
    const XrFovf out = narrowFov(orig, cfg);

    CHECK(std::tan(out.angleLeft) == doctest::Approx(std::tan(orig.angleLeft) * 0.5f));
    CHECK(std::tan(out.angleRight) == doctest::Approx(std::tan(orig.angleRight) * 0.5f));
    CHECK(std::tan(out.angleUp) == doctest::Approx(std::tan(orig.angleUp) * 0.5f));
    CHECK(std::tan(out.angleDown) == doctest::Approx(std::tan(orig.angleDown) * 0.5f));
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

// ---------------------------------------------------------------------------
// Property-based: randomized invariants
//
// Goal: catch regressions that sit in the gap between the hand-written cases
// above. We generate N random (CropConfig, XrFovf, swapchain) triples from a
// deterministically-seeded PRNG and check invariants that must hold for ALL
// valid inputs. Determinism matters — when CI fails we want the exact same
// triple on the next run so we can debug it. The seed below is arbitrary; if
// it ever masks a class of bugs, bump it and check the new run still passes.
// ---------------------------------------------------------------------------

namespace {

constexpr uint32_t kPropertySeed = 0xB00B5u;
constexpr int kPropertyIterations = 200;

struct Sample {
    CropConfig cfg;
    XrFovf fov;
    uint32_t swapWidth;
    uint32_t swapHeight;
};

// Generator: factors in [0.5, 0.95], FOVs in realistic HMD territory
// (half-angles 0.2..1.2 rad ≈ 11°..69°), swapchain dims in [256, 4096] —
// wide enough to exercise alignment edges, narrow enough to keep the loop
// cheap.
//
// The 0.95 upper bound is a concession to rounding: with factor >~0.99 the
// per-edge pixel-space delta is below the round-half-up step in
// computeCroppedImageRect, so the "strict shrink" invariant becomes a
// probabilistic flake. 0.95 keeps every axis a guaranteed-observable crop
// without losing meaningful coverage — real configs rarely sit above 0.9.
Sample makeSample(std::mt19937& rng) {
    std::uniform_real_distribution<float> factor(0.5f, 0.95f);
    std::uniform_real_distribution<float> halfAngle(0.20f, 1.20f);
    std::uniform_int_distribution<uint32_t> dim(256u, 4096u);

    Sample s{};
    s.cfg.enabled = true;
    s.cfg.cropLeftFactor = factor(rng);
    s.cfg.cropRightFactor = factor(rng);
    s.cfg.cropTopFactor = factor(rng);
    s.cfg.cropBottomFactor = factor(rng);
    // OpenXR convention: angleLeft, angleDown are negative; angleRight,
    // angleUp are positive. Each side's magnitude is independent — this is
    // what gives WMR/Varjo asymmetric FOVs.
    s.fov.angleLeft = -halfAngle(rng);
    s.fov.angleRight = halfAngle(rng);
    s.fov.angleUp = halfAngle(rng);
    s.fov.angleDown = -halfAngle(rng);
    s.swapWidth = dim(rng);
    s.swapHeight = dim(rng);
    return s;
}

} // namespace

TEST_CASE("property: narrowFov scales tan(half-angle) by the per-edge factor and preserves sign") {
    std::mt19937 rng(kPropertySeed);
    for (int i = 0; i < kPropertyIterations; ++i) {
        const Sample s = makeSample(rng);
        const XrFovf out = narrowFov(s.fov, s.cfg);

        // Magnitude relationship in tan-space: tan(|out|) == tan(|in|) * factor.
        // Using absolute values keeps the sign convention (L/D negative) out of
        // the arithmetic; tan is odd so tan(|x|) is the same as |tan(x)| for
        // our input range (|half-angle| < 90°).
        INFO("iteration ", i);
        CHECK(std::tan(std::abs(out.angleLeft)) ==
              doctest::Approx(std::tan(std::abs(s.fov.angleLeft)) * s.cfg.cropLeftFactor));
        CHECK(std::tan(std::abs(out.angleRight)) ==
              doctest::Approx(std::tan(std::abs(s.fov.angleRight)) * s.cfg.cropRightFactor));
        CHECK(std::tan(std::abs(out.angleUp)) ==
              doctest::Approx(std::tan(std::abs(s.fov.angleUp)) * s.cfg.cropTopFactor));
        CHECK(std::tan(std::abs(out.angleDown)) ==
              doctest::Approx(std::tan(std::abs(s.fov.angleDown)) * s.cfg.cropBottomFactor));

        // Sign preservation. An abs()-then-multiply regression (which would
        // still pass the magnitude check above on positive inputs) gets
        // caught here.
        CHECK(out.angleLeft <= 0.0f);
        CHECK(out.angleRight >= 0.0f);
        CHECK(out.angleUp >= 0.0f);
        CHECK(out.angleDown <= 0.0f);
    }
}

TEST_CASE("property: narrowFov preserves left<right and down<up ordering") {
    std::mt19937 rng(kPropertySeed + 1);
    for (int i = 0; i < kPropertyIterations; ++i) {
        const Sample s = makeSample(rng);
        const XrFovf out = narrowFov(s.fov, s.cfg);
        INFO("iteration ", i);
        // Input always satisfies this by construction; a narrowed FOV that
        // flips the ordering would mean one edge crossed zero, which is
        // only possible with a negative factor — clampFactor would catch
        // that upstream but we defend-in-depth here.
        CHECK(out.angleLeft < out.angleRight);
        CHECK(out.angleDown < out.angleUp);
    }
}

TEST_CASE("property: scaleSwapchainExtents produces smaller, aligned, >= 8 dims") {
    std::mt19937 rng(kPropertySeed + 2);
    for (int i = 0; i < kPropertyIterations; ++i) {
        const Sample s = makeSample(rng);
        const Extent2D out = scaleSwapchainExtents(s.swapWidth, s.swapHeight, s.cfg);

        INFO("iteration ", i, " input=", s.swapWidth, "x", s.swapHeight,
             " -> ", out.width, "x", out.height);

        // Never exceed the input on either axis — we only ever shrink.
        CHECK(out.width <= s.swapWidth);
        CHECK(out.height <= s.swapHeight);
        // Alignment: must be a multiple of 8 (kDimensionAlignment) so BC
        // formats and tiled memory layouts are happy. Masking with 7 is the
        // inverse of the `& ~(8-1)` in the implementation.
        CHECK((out.width & 7u) == 0u);
        CHECK((out.height & 7u) == 0u);
        // Floor at 8 so no runtime ever sees a zero-dim swapchain.
        CHECK(out.width >= 8u);
        CHECK(out.height >= 8u);
    }
}

TEST_CASE("property: computeCroppedImageRect stays inside the swapchain and shrinks it") {
    std::mt19937 rng(kPropertySeed + 3);
    for (int i = 0; i < kPropertyIterations; ++i) {
        const Sample s = makeSample(rng);
        const XrRect2Di rect = computeCroppedImageRect(
            s.swapWidth, s.swapHeight, s.fov, s.cfg);

        INFO("iteration ", i,
             " swap=", s.swapWidth, "x", s.swapHeight,
             " rect=(", rect.offset.x, ",", rect.offset.y,
             ";", rect.extent.width, "x", rect.extent.height, ")");

        // Non-degenerate: positive extents and a non-negative offset.
        CHECK(rect.extent.width > 0);
        CHECK(rect.extent.height > 0);
        CHECK(rect.offset.x >= 0);
        CHECK(rect.offset.y >= 0);
        // Fits entirely inside the swapchain. Off-by-one here is how
        // submissions start hitting XR_ERROR_SWAPCHAIN_RECT_INVALID on
        // strict runtimes (Varjo in particular).
        CHECK(rect.offset.x + rect.extent.width <= static_cast<int32_t>(s.swapWidth));
        CHECK(rect.offset.y + rect.extent.height <= static_cast<int32_t>(s.swapHeight));

        // When ANY factor is strictly < 1 the cropped axis must strictly
        // shrink (the other axis may still equal the swapchain, so we test
        // per-axis). Skips the case where all factors are exactly 1.0 —
        // possible but vanishingly unlikely from the uniform distribution
        // above. We still guard it for deterministic correctness.
        const bool xShrinks =
            s.cfg.cropLeftFactor < 1.0f || s.cfg.cropRightFactor < 1.0f;
        const bool yShrinks =
            s.cfg.cropTopFactor < 1.0f || s.cfg.cropBottomFactor < 1.0f;
        if (xShrinks) {
            CHECK(static_cast<uint32_t>(rect.extent.width) < s.swapWidth);
        }
        if (yShrinks) {
            CHECK(static_cast<uint32_t>(rect.extent.height) < s.swapHeight);
        }
    }
}

TEST_CASE("property: computeCroppedImageRect matches tan-space of the narrowed FOV") {
    // This is the tightest invariant: the rect's edges, interpreted as pixel
    // fractions of the swapchain in tan-space of the rendered FOV, must
    // match the tan of the narrowed half-angles. If this holds, the
    // runtime composites the submitted image with the correct frustum.
    //
    // Pixels in an XrSwapchain are uniform in tan-space: fraction
    //   (rect.offset.x) / swapWidth  = (tan(|L|) - tan(|L|*fL)) / (tan(|L|) + tan(|R|))
    // with a matching identity for the other edges. We rebuild the four
    // fractions from the layer's rect and compare against the formula
    // directly. A ~1 px tolerance covers the round-half-up quantization.
    std::mt19937 rng(kPropertySeed + 4);
    for (int i = 0; i < kPropertyIterations; ++i) {
        const Sample s = makeSample(rng);
        const XrRect2Di rect = computeCroppedImageRect(
            s.swapWidth, s.swapHeight, s.fov, s.cfg);

        const float absL = std::abs(s.fov.angleLeft);
        const float absR = std::abs(s.fov.angleRight);
        const float absU = std::abs(s.fov.angleUp);
        const float absD = std::abs(s.fov.angleDown);

        const float tanL = std::tan(absL);
        const float tanR = std::tan(absR);
        const float tanU = std::tan(absU);
        const float tanD = std::tan(absD);
        const float totalX = tanL + tanR;
        const float totalY = tanU + tanD;

        const float expectLeftFrac = (tanL - std::tan(absL * s.cfg.cropLeftFactor)) / totalX;
        const float expectRightFrac = (tanL + std::tan(absR * s.cfg.cropRightFactor)) / totalX;
        const float expectTopFrac = (tanU - std::tan(absU * s.cfg.cropTopFactor)) / totalY;
        const float expectBotFrac = (tanU + std::tan(absD * s.cfg.cropBottomFactor)) / totalY;

        const float gotLeftFrac = rect.offset.x / static_cast<float>(s.swapWidth);
        const float gotRightFrac = (rect.offset.x + rect.extent.width) /
                                   static_cast<float>(s.swapWidth);
        const float gotTopFrac = rect.offset.y / static_cast<float>(s.swapHeight);
        const float gotBotFrac = (rect.offset.y + rect.extent.height) /
                                 static_cast<float>(s.swapHeight);

        INFO("iteration ", i);
        // 1px of tolerance on each side, expressed as a fraction of the
        // respective axis. Round-half-up in the implementation can move
        // each edge by up to 0.5 pixels; we allow a bit more to absorb
        // the tan()+float arithmetic jitter.
        const float tolX = 1.0f / static_cast<float>(s.swapWidth);
        const float tolY = 1.0f / static_cast<float>(s.swapHeight);
        CHECK(std::abs(gotLeftFrac - expectLeftFrac) < tolX);
        CHECK(std::abs(gotRightFrac - expectRightFrac) < tolX);
        CHECK(std::abs(gotTopFrac - expectTopFrac) < tolY);
        CHECK(std::abs(gotBotFrac - expectBotFrac) < tolY);
    }
}

// ---------------------------------------------------------------------------
// sanitizeForFilename
// ---------------------------------------------------------------------------

TEST_CASE("sanitizeForFilename: lowercases ASCII letters") {
    CHECK(sanitizeForFilename("HELLO") == "hello");
    CHECK(sanitizeForFilename("Hello") == "hello");
    CHECK(sanitizeForFilename("hello") == "hello");
}

TEST_CASE("sanitizeForFilename: replaces non-alphanumeric with underscore") {
    CHECK(sanitizeForFilename("DiRT Rally 2.0") == "dirt_rally_2_0");
    CHECK(sanitizeForFilename("Le Mans Ultimate") == "le_mans_ultimate");
    CHECK(sanitizeForFilename("iRacing Simulator") == "iracing_simulator");
}

TEST_CASE("sanitizeForFilename: keeps already-safe identifiers unchanged") {
    CHECK(sanitizeForFilename("hello_xr") == "hello_xr");
    CHECK(sanitizeForFilename("game42") == "game42");
}

TEST_CASE("sanitizeForFilename: collapses consecutive separators") {
    CHECK(sanitizeForFilename("foo   bar") == "foo_bar");
    CHECK(sanitizeForFilename("foo...bar") == "foo_bar");
    CHECK(sanitizeForFilename("foo -- bar") == "foo_bar");
}

TEST_CASE("sanitizeForFilename: strips leading and trailing separators") {
    CHECK(sanitizeForFilename("  hello  ") == "hello");
    CHECK(sanitizeForFilename("___hello___") == "hello");
    CHECK(sanitizeForFilename(".hello.") == "hello");
}

TEST_CASE("sanitizeForFilename: falls back to unknown_app for empty or garbage input") {
    CHECK(sanitizeForFilename("") == "unknown_app");
    CHECK(sanitizeForFilename("   ") == "unknown_app");
    CHECK(sanitizeForFilename("!!!") == "unknown_app");
    CHECK(sanitizeForFilename("___") == "unknown_app");
}

// ---------------------------------------------------------------------------
// resolvePerAppConfigPath
// ---------------------------------------------------------------------------

TEST_CASE("resolvePerAppConfigPath: composes <dir>/<slug>_settings.json") {
    const std::filesystem::path dir = "C:/tmp/layer";
    CHECK(resolvePerAppConfigPath(dir, "DiRT Rally 2.0").filename().string() ==
          "dirt_rally_2_0_settings.json");
    CHECK(resolvePerAppConfigPath(dir, "hello_xr").filename().string() ==
          "hello_xr_settings.json");
    CHECK(resolvePerAppConfigPath(dir, "").filename().string() ==
          "unknown_app_settings.json");
}
