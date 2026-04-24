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

    // User-facing configuration, mirrored from settings.json.
    struct HelmetOverlayConfig {
        bool enabled = false;
        std::string textureRelativePath = "helmet_visor.png";
        float distance_m = 0.5f;
        float width_m = 0.6f;
        float height_m = 0.4f;
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
        // binding it can use. Returns true if the overlay is armed and
        // will contribute a layer in appendLayer(); false means "silently
        // degrade to bypass" per best-practices.
        bool initialize(XrSession session,
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

        bool isArmed() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

} // namespace openxr_api_layer
