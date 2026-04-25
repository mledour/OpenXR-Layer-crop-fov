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

namespace openxr_api_layer {

    // Forward declaration — full definition in <framework/dispatch.gen.h>,
    // which helmet_overlay.cpp includes. Keeping it as a forward decl here
    // means TUs that only drive the overlay lifecycle do not pull the
    // framework dispatch header transitively.
    class OpenXrApi;

    // User-facing configuration, mirrored from settings.json.
    struct HelmetOverlayConfig {
        bool enabled = false;
        std::string textureRelativePath = "helmet_visor.png";

        // distance_m: how close the helmet sits to the user's face.
        //   - Quad (flat, the path Pimax actually uses) : distance
        //     from eye to the quad's plane, in meters.
        //   - Cylinder (curved, only on runtimes that grant the KHR
        //     extension)                              : radius from
        //     the cylinder axis (= eye position) to the surface.
        // Live-tunable in both modes.
        float distance_m = 0.5f;

        // Quad-mode only: width of the flat quad in meters. Height
        // is derived from the PNG aspect ratio. Live-tunable.
        // (Cylinder mode hardcodes a 130° horizontal span, since the
        // user-facing knob would be dead on every runtime we test —
        // see tools/cylinder_warp.py for offline curvature instead.)
        float width_m = 0.6f;

        // RGB multiplier applied to the texture at upload time. 1.0 =
        // pristine, 0.5 = half brightness, 0.0 = pure black. Useful when
        // the PNG has highlights that look natural in studio lighting
        // but cramée on a bright VR HMD in a dim cockpit. Alpha is
        // never multiplied, so the visor cutout stays transparent.
        // Applied once at session start; needs a session restart to
        // re-apply (no live-edit yet).
        float brightness = 1.0f;
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
        // the layer). Returns true if the overlay is armed and will
        // contribute a layer in appendLayer(); false means "silently
        // degrade to bypass" per best-practices.
        bool initialize(OpenXrApi* api,
                        XrSession session,
                        const void* sessionCreateInfoNextChain,
                        const HelmetOverlayConfig& config,
                        const std::filesystem::path& dllHome);

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

        // Apply a live-edit reload of settings.json. Only fields safe to
        // change without rebuilding swapchain/textures are honoured:
        //   - distance_m  (re-poses the quad in view space)
        //   - width_m     (resizes the quad; height follows the PNG
        //                  aspect ratio captured at init)
        // Toggling enabled, replacing the PNG, or changing brightness
        // still requires a session restart — those would need swapchain
        // / staging-texture reallocation and a fresh initialize() call.
        // No-op if the overlay is not armed.
        void updateLiveTunables(const HelmetOverlayConfig& newConfig);

        bool isArmed() const;

    private:
        // PNG pixels are decoded once in initialize() and handed down
        // as an RGBA8 buffer + dims. The PNG is mandatory — overlays
        // without a PNG asset don't arm. Both backends share the
        // swapchain / staging texture, set up by createSwapchainFromPng();
        // they only differ in the composition-layer struct they
        // build (XrCompositionLayerCylinderKHR vs XrCompositionLayerQuad).
        bool createSwapchainFromPng(const uint8_t* pngPixels, int pngWidth, int pngHeight);
        bool tryInitCylinder(int pngWidth, int pngHeight);
        bool tryInitQuad(int pngWidth, int pngHeight);

        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

} // namespace openxr_api_layer
