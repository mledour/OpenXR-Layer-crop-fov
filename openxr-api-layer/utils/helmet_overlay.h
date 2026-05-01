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

        // Opt-in for the helmet's contribution to xrGetVisibilityMaskKHR.
        // Default true: when the runtime grants XR_KHR_visibility_mask,
        // the layer augments the runtime's hidden-triangle mesh with a
        // bbox of the helmet's opaque foam region so apps can stencil-
        // reject those pixels and skip shading. Disabling this knob
        // keeps the helmet quad rendering normally but stops the mask
        // contribution — useful as an escape hatch for games whose
        // stencil-setup doesn't tolerate non-trivial mask geometry.
        // Live-tunable; toggling fires a XR_TYPE_EVENT_DATA_VISIBILITY_
        // MASK_CHANGED_KHR so apps that listen pick up the change
        // mid-session.
        bool use_visibility_mask = true;

        // Maps the visibility-mask vertices from NDC [-1, +1] (spec-
        // correct for OpenXR's xrGetVisibilityMaskKHR) to UV [0, 1]
        // before emitting them. Use case: OpenVR-style apps reached
        // via OpenComposite. OpenComposite passes our vertices through
        // verbatim, but the OpenVR HiddenAreaMesh contract is UV
        // [0, 1] (origin bottom-left). With our NDC values, DiRT
        // Rally 2 (and likely other OpenVR titles) clips/discards
        // any triangle with y < 0 — half our mesh is silently
        // dropped. Setting this flag remaps every vertex via
        // (v + 1) / 2 so they all land in [0, 1] and DiRT renders
        // them in full. Per-app opt-in (default false to keep the
        // spec-correct path for native OpenXR apps).
        bool visibility_mask_uv_space = false;

        // Inverts the visibility-mask geometry: instead of emitting 4
        // strips around the visor bbox (the foam, the spec-correct
        // "hidden" area), emits a single rect that IS the visor bbox.
        // Use case: apps that interpret HIDDEN_TRIANGLE_MESH inversely
        // (observed on DiRT Rally 2 + OpenComposite + PimaxXR — the
        // app renders the scene where the mesh IS, skips elsewhere).
        // For those apps, inverting our mesh restores the correct
        // visual result. For spec-conforming apps the inverted mesh
        // is wrong (visor opening becomes black). Per-app opt-in via
        // settings.json. Live-tunable (fires a mask-changed event).
        bool invert_visibility_mask = false;

        // Debug aid: when true, the helmet overlay quad is NOT
        // appended to the frame, while the visibility mask is still
        // emitted normally via xrGetVisibilityMaskKHR. The user sees
        // the rendered scene with the foam region replaced by the
        // app's clear color (typically black or the sky) — i.e.,
        // exactly what the app stencil-skipped after the full
        // pipeline (this layer → OpenComposite → app's stencil mesh).
        // Off by default. Live-tunable: takes effect on the next
        // frame, no swapchain rebuild. Use this to verify visually
        // that the mask geometry is correct end-to-end; an earlier
        // version that tinted the helmet overlay red was misleading
        // because it only validated the UV bbox detection, not the
        // downstream NDC projection.
        bool debug_visibility_mask = false;
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
        // overlay is armed and will contribute a layer in appendLayer();
        // false means "silently degrade to bypass" per best-practices.
        bool initialize(OpenXrApi* api,
                        XrSession session,
                        const void* sessionCreateInfoNextChain,
                        const HelmetOverlayConfig& config,
                        const std::filesystem::path& helmetsDir);

        // Called from xrDestroySession before the session handle becomes
        // invalid. Always safe to call, even if initialize() returned false.
        void shutdown();

        // Called from xrEndFrame. If the overlay is armed, renders the
        // visor into its swapchain and writes a pointer to a
        // XrCompositionLayerQuad (owned by *this*, stable until the next
        // appendLayer() call) into *outLayer. Returns false if the overlay
        // is not armed or could not produce a layer this frame, in which
        // case *outLayer is left untouched.
        bool appendLayer(XrTime displayTime,
                         const XrCompositionLayerBaseHeader** outLayer);

        // Apply a live-edit reload of settings.json. Honoured fields:
        //   - distance_m            (re-poses the quad in view space)
        //   - horizontal_fov_deg    (resizes the quad's apparent FOV;
        //                            physical width is recomputed from
        //                            distance_m × tan(fov/2))
        //   - vertical_offset_deg   (shifts the quad up/down by an
        //                            angle in the user's view; world-
        //                            space Y is recomputed from
        //                            distance_m × tan(deg))
        //   - debug_visibility_mask (just flips a flag; on next frame
        //                            xrEndFrame either appends the
        //                            helmet quad or doesn't, exposing
        //                            the app's stencil output for a
        //                            visual sanity-check)
        // Toggling enabled, replacing the PNG, or changing brightness
        // still requires a session restart — those would need a fresh
        // initialize() call.
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
