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

#pragma once

// Head-locked composition layer that draws a motorcycle-helmet-interior
// mask on top of the application's rendering. The visor cutout is sized
// so the user sees the game through it while the periphery is occluded,
// producing the "inside a helmet" effect.
//
// This header is deliberately free of any D3D/Vulkan/GL includes so that
// the existing pch.h invariant — "the layer pulls in no graphics API" —
// holds for translation units that do not opt in to the overlay. The
// actual rendering backend lives in helmet_overlay.cpp and is gated
// behind XR_USE_GRAPHICS_API_D3D11 inside that TU only.

#include <filesystem>
#include <memory>
#include <string>

// Header is self-sufficient regarding OpenXR types: pulled in here so
// translation units that include this without going through pch.h
// (e.g. the standalone unit tests via helmet_config_parser.h) still
// see XrSession, XrTime, XrCompositionLayerBaseHeader, etc.
#include <openxr/openxr.h>

namespace openxr_api_layer {

    // Forward declaration — full definition in <framework/dispatch.gen.h>,
    // which helmet_overlay.cpp includes. Keeping it as a forward decl here
    // means TUs that only drive the overlay lifecycle do not pull the
    // framework dispatch header transitively.
    class OpenXrApi;

    // User-facing configuration, mirrored from settings.json.
    struct HelmetOverlayConfig {
        bool enabled = false;
        std::string imageRelativePath = "helmet_visor.png";

        // distance_m: distance from the eye to the quad's plane, in
        // meters. Controls the depth-feel (stereo disparity) — at
        // 0.15 m the helmet feels glued to your face, at 0.5 m it
        // feels like a TV in front of you. Live-tunable.
        float distance_m = 0.5f;

        // horizontal_fov_deg: angular width of the quad in the user's
        // view, in degrees. The physical quad width in meters is
        // derived as 2 * distance_m * tan(horizontal_fov_deg / 2),
        // so changing distance_m no longer also changes coverage —
        // the two parameters are orthogonal. Height follows the PNG
        // aspect so the image is never stretched. Live-tunable.
        // For an apparent helmet curvature, pre-warp the PNG offline
        // with tools/cylinder_warp.py — the layer renders a flat
        // quad either way.
        float horizontal_fov_deg = 130.0f;

        // vertical_offset_deg: shifts the quad up (+) or down (-) by a
        // given angle in the user's view, in degrees. Decoupled from
        // distance_m — at any distance, "+5°" always shifts the helmet
        // up by 5° in the user's FOV. Internally converted to a
        // world-space Y offset of distance_m × tan(deg). Useful when
        // the user's HMD lens position or asymmetric crop_top/bottom
        // leaves the helmet feeling slightly above or below their
        // gaze line. Live-tunable.
        float vertical_offset_deg = 0.0f;

        // RGB multiplier applied to the texture at upload time. 1.0 =
        // pristine, 0.5 = half brightness, 0.0 = pure black. Useful when
        // the PNG has highlights that look natural in studio lighting
        // but cramée on a bright VR HMD in a dim cockpit. Alpha is
        // never multiplied, so the visor cutout stays transparent.
        // Applied once at session start; needs a session restart to
        // re-apply (no live-edit yet).
        float brightness = 1.0f;

        // When true, the layer generates a side-by-side stereo image
        // from the (mono) PNG at session init: each output column is
        // sampled from the source with a horizontal disparity offset
        // computed from a cosine depth profile (edges closer to the eye
        // than the center, like a real visor wrapping around the face),
        // the IPD, and stereo_depth_amplitude_m below. The two halves
        // are uploaded to a single 2×W × H swapchain; the layer then
        // submits two XrCompositionLayerQuad pointing at it via split
        // subImage.imageRect with EYE_VISIBILITY_LEFT / EYE_VISIBILITY_RIGHT.
        // Single texture, single upload, two layer pointers per frame.
        //
        // The user's PNG stays mono — no external tool required. The
        // quad's physical width (driven by horizontal_fov_deg and
        // distance_m) and aspect (driven by the mono PNG aspect) are
        // both unchanged from mono mode; only the swapchain is wider.
        //
        // Set at session start; needs a session restart to toggle.
        bool stereo_sbs = false;

        // Depth amplitude for the cosine profile, in meters. The visor
        // edges are simulated as Δ closer to the eye than the center:
        //   Z(u) = distance_m − stereo_depth_amplitude_m · sin²(π(u−0.5))
        // Disparity scales with (Z − D)/Z, so the perceived depth grows
        // with this value and shrinks with distance_m.
        // Effect rule of thumb: at distance_m=0.15, ~3 cm gives a soft
        // wrap; ~10 cm pushes the edges noticeably toward the face.
        // At distance_m=0.5, you typically need 10–20 cm to get a
        // perceptible effect. Set at init; not live-tunable (would
        // require destroying and recreating the static-image swapchain).
        // Clamped to [0, 0.5] m by the parser. Ignored when stereo_sbs
        // is false.
        float stereo_depth_amplitude_m = 0.05f;
    };

    // Opaque backend — hides D3D types from every TU that only needs to
    // drive the overlay lifecycle (layer.cpp is one of those).
    class HelmetOverlay {
    public:
        HelmetOverlay();
        ~HelmetOverlay();

        HelmetOverlay(const HelmetOverlay&) = delete;
        HelmetOverlay& operator=(const HelmetOverlay&) = delete;

        // Called from xrCreateSession after the runtime has accepted the
        // session. sessionCreateInfoNextChain is the XrSessionCreateInfo::next
        // pointer (void*) — the overlay walks it to find a graphics
        // binding it can use. `api` is kept by the overlay so it can
        // reach downstream xrCreateSwapchain / xrAcquire…Image / etc.
        // through the layer's own dispatch (same as every other part of
        // the layer). `helmetsDir` is the directory the texture path
        // (config.imageRelativePath) is resolved against — typically
        // %LOCALAPPDATA%\XR_APILAYER_MLEDOUR_fov_crop\helmets so users
        // can drop PNGs without admin elevation. Returns true if the
        // overlay is armed and will contribute layer(s) in appendLayers();
        // false means "silently degrade to bypass" per best-practices.
        bool initialize(OpenXrApi* api,
                        XrSession session,
                        const void* sessionCreateInfoNextChain,
                        const HelmetOverlayConfig& config,
                        const std::filesystem::path& helmetsDir);

        // Called from xrDestroySession before the session handle becomes
        // invalid. Always safe to call, even if initialize() returned false.
        void shutdown();

        // Maximum number of composition layers appendLayers() can emit.
        // 1 in mono mode, 2 in stereo SBS mode (one quad per eye).
        static constexpr size_t kMaxLayers = 2;

        // Called from xrEndFrame. If the overlay is armed, writes up to
        // kMaxLayers pointers into outLayers[] (each pointing at an
        // XrCompositionLayerQuad owned by *this*, stable until the next
        // appendLayers() call) and returns the number written. Returns
        // 0 if the overlay is not armed or could not produce layers
        // this frame, in which case outLayers is left untouched.
        // Caller must provide an array of at least kMaxLayers slots.
        size_t appendLayers(XrTime displayTime,
                            const XrCompositionLayerBaseHeader** outLayers);

        // Apply a live-edit reload of settings.json. Only fields safe
        // to change without rebuilding swapchain/textures are honoured:
        //   - distance_m            (re-poses the quad in view space)
        //   - horizontal_fov_deg    (resizes the quad's apparent FOV;
        //                            physical width is recomputed from
        //                            distance_m × tan(fov/2))
        //   - vertical_offset_deg   (shifts the quad up/down by an
        //                            angle in the user's view; world-
        //                            space Y is recomputed from
        //                            distance_m × tan(deg))
        // Toggling enabled, replacing the PNG, or changing brightness
        // still requires a session restart — those would need swapchain
        // / staging-texture reallocation and a fresh initialize() call.
        // No-op if the overlay is not armed.
        void updateLiveTunables(const HelmetOverlayConfig& newConfig);

        bool isArmed() const;

    private:
        // PNG pixels are decoded once in initialize() and handed down
        // as an RGBA8 buffer + dims. The PNG is mandatory — overlays
        // without a PNG asset don't arm. createSwapchainFromPng()
        // sets up the XrSwapchain + the staging texture (with the
        // brightness multiplier baked in); tryInitQuad() then builds
        // the XrCompositionLayerQuad on top of those.
        bool createSwapchainFromPng(const uint8_t* pngPixels, int pngWidth, int pngHeight);
        bool tryInitQuad(int pngWidth, int pngHeight);

        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

} // namespace openxr_api_layer
