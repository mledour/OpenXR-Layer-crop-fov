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
#include <cstdint>

#include <openxr/openxr.h>

namespace openxr_api_layer {

    // Configuration loaded from %LOCALAPPDATA%\<layer-name>\settings.json.
    // Factors are in [0.5, 1.0]; 1.0 means "no crop", 0.5 means "crop half of
    // that edge off". Kept as factors (not percents) because we multiply by
    // them in hot paths like xrLocateViews and xrEndFrame.
    struct CropConfig {
        bool enabled = true;
        float cropLeftFactor = 0.90f;   // percent 10 -> factor 0.90
        float cropRightFactor = 0.90f;  // percent 10 -> factor 0.90
        float cropTopFactor = 0.85f;    // percent 15 -> factor 0.85
        float cropBottomFactor = 0.80f; // percent 20 -> factor 0.80
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

    // Applies the crop to the recommended swapchain dimensions returned by
    // xrEnumerateViewConfigurationViews. Uses the min of left/right for
    // width (and top/bottom for height) so the allocated swapchain is still
    // large enough for the widest individual crop. Result is force-aligned
    // to an even value with a 2-pixel floor so we never request a zero-size
    // swapchain from the runtime.
    inline Extent2D scaleSwapchainExtents(uint32_t origWidth,
                                          uint32_t origHeight,
                                          const CropConfig& cfg) {
        const float widthFactor = std::min(cfg.cropLeftFactor, cfg.cropRightFactor);
        const float heightFactor = std::min(cfg.cropTopFactor, cfg.cropBottomFactor);

        uint32_t newWidth = static_cast<uint32_t>(origWidth * widthFactor);
        uint32_t newHeight = static_cast<uint32_t>(origHeight * heightFactor);
        newWidth = std::max(newWidth & ~1u, 2u);
        newHeight = std::max(newHeight & ~1u, 2u);

        return {newWidth, newHeight};
    }

    // Computes the sub-image rect inside a rendered swapchain that matches
    // the FOV crop: half the crop is taken from each side, so a 10% left
    // factor (= 0.90) yields a 5% offset on the left edge. Returns a zero
    // rect if the resulting area would be non-positive on either axis; the
    // caller should skip the assignment in that case.
    inline XrRect2Di computeCroppedImageRect(uint32_t swapWidth,
                                             uint32_t swapHeight,
                                             const CropConfig& cfg) {
        const float leftCropPixels = swapWidth * (1.0f - cfg.cropLeftFactor);
        const float rightCropPixels = swapWidth * (1.0f - cfg.cropRightFactor);
        const float topCropPixels = swapHeight * (1.0f - cfg.cropTopFactor);
        const float bottomCropPixels = swapHeight * (1.0f - cfg.cropBottomFactor);

        const int32_t newOffsetX = static_cast<int32_t>(leftCropPixels * 0.5f);
        const int32_t newOffsetY = static_cast<int32_t>(topCropPixels * 0.5f);
        const int32_t newWidth =
            static_cast<int32_t>(swapWidth - leftCropPixels * 0.5f - rightCropPixels * 0.5f);
        const int32_t newHeight =
            static_cast<int32_t>(swapHeight - topCropPixels * 0.5f - bottomCropPixels * 0.5f);

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
