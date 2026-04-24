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

#include "pch.h"

#include "helmet_overlay.h"

#include <log.h>

// Stub implementation. This TU is the only place where we will eventually
// introduce D3D11 (XR_USE_GRAPHICS_API_D3D11 + d3d11.h) so the rest of the
// layer keeps its "no graphics API" invariant documented in pch.h.
//
// Until the rendering backend lands, initialize() always returns false,
// so layer.cpp can already be wired against the final public API without
// altering its behaviour: appendLayer() never contributes a composition
// layer and the FOV-crop path stays strictly passthrough-equivalent.

namespace openxr_api_layer {

    using namespace log;

    struct HelmetOverlay::Impl {
        bool armed = false;
        HelmetOverlayConfig config;
        XrSession session{XR_NULL_HANDLE};
        std::filesystem::path texturePath;
    };

    HelmetOverlay::HelmetOverlay() : m_impl(std::make_unique<Impl>()) {}
    HelmetOverlay::~HelmetOverlay() { shutdown(); }

    bool HelmetOverlay::initialize(XrSession session,
                                   const void* sessionCreateInfoNextChain,
                                   const HelmetOverlayConfig& config,
                                   const std::filesystem::path& dllHome) {
        (void)sessionCreateInfoNextChain;

        m_impl->config = config;
        m_impl->session = session;
        m_impl->texturePath = dllHome / "assets" / config.textureRelativePath;

        if (!config.enabled) {
            Log("HelmetOverlay: disabled by config, staying inert\n");
            m_impl->armed = false;
            return false;
        }

        // TODO(helmet-overlay): walk sessionCreateInfoNextChain for a
        // XrGraphicsBindingD3D11KHR, create swapchain, load texture,
        // create XR_REFERENCE_SPACE_TYPE_VIEW space. Until that backend
        // lands we deliberately fail soft: log once, then behave as
        // if the feature were disabled. This preserves the
        // "never crash the host process" contract.
        Log(fmt::format(
            "HelmetOverlay: enabled in config but rendering backend is not "
            "yet implemented, staying inert. texture={}\n",
            m_impl->texturePath.string()));
        m_impl->armed = false;
        return false;
    }

    void HelmetOverlay::shutdown() {
        if (!m_impl) return;
        // TODO(helmet-overlay): release XrSwapchain, XrSpace, and D3D
        // resources here when the backend is in place.
        m_impl->armed = false;
        m_impl->session = XR_NULL_HANDLE;
    }

    bool HelmetOverlay::appendLayer(XrTime displayTime,
                                    const XrCompositionLayerBaseHeader** outLayer) {
        (void)displayTime;
        (void)outLayer;
        return false;
    }

    bool HelmetOverlay::isArmed() const {
        return m_impl && m_impl->armed;
    }

} // namespace openxr_api_layer
