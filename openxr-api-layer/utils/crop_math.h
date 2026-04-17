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

#pragma once

// Pure crop-math helpers. No logging, no file I/O, no dependency on the layer
// framework or on the OpenXR loader — only the OpenXR types. Lets this header
// be pulled into a standalone test binary that doesn't link the layer DLL.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <openxr/openxr.h>

namespace openxr_api_layer {

    // Configuration loaded from %LOCALAPPDATA%\<layer-name>\settings.json.
    // Factors are in [0.5, 1.0]; 1.0 means "no crop", 0.5 means "crop half of
    // that edge off". Kept as factors (not percents) because we multiply by
    // them in hot paths like xrLocateViews and xrEndFrame.
    struct CropConfig {
        // Opt-in by default: the layer is a no-op until the user explicitly
        // sets "enabled": true in their per-app settings file.
        bool enabled = false;
        float cropLeftFactor = 0.90f;   // percent 10 -> factor 0.90
        float cropRightFactor = 0.90f;  // percent 10 -> factor 0.90
        float cropTopFactor = 0.85f;    // percent 15 -> factor 0.85
        float cropBottomFactor = 0.80f; // percent 20 -> factor 0.80

        // When true, the layer re-reads settings.json every ~1 second (90
        // frames) to pick up changes without restarting the game. Intended
        // for tuning sessions — leave false in normal use.
        bool liveEdit = false;
    };

    // Maps a user-facing "crop X percent" value in [0, 50] to a factor in
    // [0.5, 1.0]. Out-of-range inputs are clamped so a malformed config
    // never produces a factor that would flip the FOV sign or collapse it
    // to zero.
    inline float clampFactor(float percent) {
        if (percent < 0.0f) percent = 0.0f;
        if (percent > 50.0f) percent = 50.0f;
        return 1.0f - (percent / 100.0f);
    }

    struct Extent2D {
        uint32_t width;
        uint32_t height;
    };

    // Swapchain dimensions are aligned down to this many pixels on each axis.
    // 8 covers the common cases that runtimes and downstream passes care about:
    //   - block-compressed formats (BC1-BC7 use 4x4 blocks)
    //   - GPU tiled memory layouts (typically 8x8 or 16x16)
    //   - foveated rendering / DLSS / FSR2 tile sizes (8 or 16)
    //   - compositor internal passes on stricter runtimes
    // Picking 8 costs at most 7 pixels per axis (~0.4% of a 2000px swapchain)
    // while sidestepping edge cases that single-pair (~1u) alignment exposes.
    // Must be a power of two for the bitmask trick below.
    constexpr uint32_t kDimensionAlignment = 8u;

    // Applies the crop to the recommended swapchain dimensions returned by
    // xrEnumerateViewConfigurationViews. Uses the min of left/right for
    // width (and top/bottom for height) so the allocated swapchain is still
    // large enough for the widest individual crop. Result is force-aligned
    // down to a multiple of kDimensionAlignment with that same value as a
    // floor, so the runtime never receives a zero-size or awkwardly-aligned
    // swapchain.
    inline Extent2D scaleSwapchainExtents(uint32_t origWidth,
                                          uint32_t origHeight,
                                          const CropConfig& cfg) {
        const float widthFactor = std::min(cfg.cropLeftFactor, cfg.cropRightFactor);
        const float heightFactor = std::min(cfg.cropTopFactor, cfg.cropBottomFactor);

        uint32_t newWidth = static_cast<uint32_t>(origWidth * widthFactor);
        uint32_t newHeight = static_cast<uint32_t>(origHeight * heightFactor);

        constexpr uint32_t mask = ~(kDimensionAlignment - 1u);
        newWidth = std::max(newWidth & mask, kDimensionAlignment);
        newHeight = std::max(newHeight & mask, kDimensionAlignment);

        return {newWidth, newHeight};
    }

    // Computes the sub-image rect inside a rendered swapchain that matches
    // the FOV crop. Returns a zero rect if the area would be non-positive on
    // either axis (caller should skip the assignment in that case).
    //
    // `renderedFov` is the FOV the app used when rendering its swapchain,
    // i.e. the value in XrCompositionLayerProjectionView::fov at xrEndFrame
    // time BEFORE this layer narrows it further. We need it because pixels
    // on the swapchain are uniformly distributed in the projection plane
    // (tan-space), NOT in angle-space. For VR-sized half-FOVs (45-55°) the
    // non-linearity of tan is large enough that a linear-in-angle mapping
    // drifts the rect by tens of pixels per edge.
    //
    // Pipeline recap: the app rendered to the whole swapchain with
    // renderedFov. We're about to submit with renderedFov * cropFactor (per
    // side), which is a narrower sub-FOV. We need to tell the runtime WHICH
    // pixels on the swapchain correspond to that narrower FOV. That
    // sub-region is the image of `renderedFov * cropFactor` under the
    // perspective projection that the app used — hence the tan().
    //
    // The "+ 0.5f then static_cast" pattern is round-half-up. Float factors
    // like 0.8f are not exact in IEEE 754 (they're ~0.80000001), so a plain
    // truncating cast yields off-by-one pixel errors. All quantities here
    // are non-negative so a naive +0.5 is sufficient — no need for
    // std::lround.
    inline XrRect2Di computeCroppedImageRect(uint32_t swapWidth,
                                             uint32_t swapHeight,
                                             const XrFovf& renderedFov,
                                             const CropConfig& cfg) {
        // OpenXR convention: angleLeft and angleDown are negative, angleRight
        // and angleUp are positive. Take magnitudes for tan() — safer than
        // relying on tan() being odd when the caller passes weird values.
        const float absL = std::abs(renderedFov.angleLeft);
        const float absR = std::abs(renderedFov.angleRight);
        const float absU = std::abs(renderedFov.angleUp);
        const float absD = std::abs(renderedFov.angleDown);

        const float tanL = std::tan(absL);
        const float tanR = std::tan(absR);
        const float tanU = std::tan(absU);
        const float tanD = std::tan(absD);

        // Submitted (narrower) half-angles. Multiply the magnitude by the
        // factor, not the signed angle, so cfg.cropLeftFactor < 1 always
        // shrinks the FOV regardless of sign conventions.
        const float tanSubL = std::tan(absL * cfg.cropLeftFactor);
        const float tanSubR = std::tan(absR * cfg.cropRightFactor);
        const float tanSubU = std::tan(absU * cfg.cropTopFactor);
        const float tanSubD = std::tan(absD * cfg.cropBottomFactor);

        const float totalTanX = tanL + tanR;
        const float totalTanY = tanU + tanD;

        // Defensive guard against degenerate FOV inputs.
        if (totalTanX <= 0.0f || totalTanY <= 0.0f) {
            return XrRect2Di{};
        }

        // X axis: pixel 0 = -tanL (angleLeft side), pixel W = +tanR (right).
        // Submitted range [-tanSubL, +tanSubR] maps to [xLeft, xRight].
        const float xLeftF = static_cast<float>(swapWidth) * (tanL - tanSubL) / totalTanX;
        const float xRightF = static_cast<float>(swapWidth) * (tanL + tanSubR) / totalTanX;

        // Y axis: in the texture coordinate system used by XrSwapchain (D3D,
        // Vulkan, OpenGL with XR_KHR_*_flip), pixel 0 is at the TOP of the
        // image. OpenXR angleUp > 0 is at the top. So +tanSubU maps to LOW
        // y, -tanSubD to HIGH y.
        const float yTopF = static_cast<float>(swapHeight) * (tanU - tanSubU) / totalTanY;
        const float yBottomF = static_cast<float>(swapHeight) * (tanU + tanSubD) / totalTanY;

        const int32_t newOffsetX = static_cast<int32_t>(xLeftF + 0.5f);
        const int32_t newOffsetY = static_cast<int32_t>(yTopF + 0.5f);
        const int32_t newWidth = static_cast<int32_t>(xRightF - xLeftF + 0.5f);
        const int32_t newHeight = static_cast<int32_t>(yBottomF - yTopF + 0.5f);

        if (newWidth <= 0 || newHeight <= 0) {
            return XrRect2Di{};
        }
        return XrRect2Di{{newOffsetX, newOffsetY}, {newWidth, newHeight}};
    }

    // Multiplies each half-angle of the FOV by its matching factor. The
    // OpenXR convention has angleLeft negative and angleRight positive (both
    // in radians), so multiplying by a positive factor < 1 narrows the FOV
    // symmetrically toward zero — which is what we want.
    inline XrFovf narrowFov(const XrFovf& origFov, const CropConfig& cfg) {
        XrFovf fov = origFov;
        fov.angleLeft *= cfg.cropLeftFactor;
        fov.angleRight *= cfg.cropRightFactor;
        fov.angleUp *= cfg.cropTopFactor;
        fov.angleDown *= cfg.cropBottomFactor;
        return fov;
    }

} // namespace openxr_api_layer
