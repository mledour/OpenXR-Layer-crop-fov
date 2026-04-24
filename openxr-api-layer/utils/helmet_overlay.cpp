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

#include <framework/dispatch.gen.h>
#include <log.h>

#include <vector>

// D3D11 helmet-interior overlay.
//
// Design notes:
//  - Rendered as a single head-locked XrCompositionLayerQuad sitting at
//    a fixed distance in front of the viewer, so the visor moves with
//    the head. The OpenXR runtime composites it on top of the game's
//    own layers using the alpha we ship in the texture.
//  - The texture is generated procedurally at session start: a black
//    field with a soft-edged elliptical transparent cutout. No PNG
//    asset dependency yet — the textureRelativePath config field is
//    reserved for a future iteration that swaps the procedural mask
//    for a painted helmet interior.
//  - Per-frame work is *just* CopyResource(staging -> swapchainImage)
//    plus the acquire/wait/release dance. No shader, no sampler, no
//    blend state at render time; blending happens in the OpenXR
//    compositor via XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT.
//  - D3D11 entry points are delay-loaded at link time (see vcxproj),
//    so a Vulkan-only game that never enables the overlay never pulls
//    d3d11.dll / d3dcompiler_47.dll into its process.

namespace openxr_api_layer {

    using namespace log;

    namespace {

        constexpr uint32_t kSwapchainWidth = 512;
        constexpr uint32_t kSwapchainHeight = 512;

        // DXGI_FORMAT_R8G8B8A8_UNORM = 28. Avoid including <dxgiformat.h>
        // just for the enum; the D3D11 header is already pulled in by
        // pch.h and the constant is stable. Using UNORM (not sRGB)
        // because we ship straight alpha and an sRGB target would
        // double-apply gamma to the black mask.
        constexpr int64_t kPreferredFormat = 28;

        // Procedural mask: `out` is kSwapchainWidth * kSwapchainHeight
        // RGBA8 bytes. Alpha ramps from 0 inside an ellipse (horizontal
        // visor slit) to 255 outside, with a feathered edge. RGB stays
        // (0,0,0) everywhere so the unmasked region is opaque black.
        void generateVisorMask(uint8_t* out) {
            // Ellipse half-axes as fractions of the texture dimensions.
            // 0.46 wide x 0.30 tall gives a roughly motorcycle-visor
            // aspect — wider horizontally than vertically.
            constexpr float kAx = 0.46f;
            constexpr float kAy = 0.30f;
            // Feather band width (in normalized radius units).
            constexpr float kFeather = 0.08f;

            const float cx = kSwapchainWidth * 0.5f;
            const float cy = kSwapchainHeight * 0.5f;
            const float rx = kSwapchainWidth * kAx;
            const float ry = kSwapchainHeight * kAy;

            for (uint32_t y = 0; y < kSwapchainHeight; ++y) {
                for (uint32_t x = 0; x < kSwapchainWidth; ++x) {
                    const float dx = (static_cast<float>(x) - cx) / rx;
                    const float dy = (static_cast<float>(y) - cy) / ry;
                    const float r = std::sqrt(dx * dx + dy * dy);

                    // r < 1 => inside ellipse (transparent), r > 1+feather
                    // => fully opaque. Between the two, linear ramp.
                    float alpha;
                    if (r <= 1.0f) {
                        alpha = 0.0f;
                    } else if (r >= 1.0f + kFeather) {
                        alpha = 1.0f;
                    } else {
                        alpha = (r - 1.0f) / kFeather;
                    }

                    const size_t idx = (static_cast<size_t>(y) * kSwapchainWidth + x) * 4u;
                    out[idx + 0] = 0;
                    out[idx + 1] = 0;
                    out[idx + 2] = 0;
                    out[idx + 3] = static_cast<uint8_t>(alpha * 255.0f + 0.5f);
                }
            }
        }

        // Walks a next-chain starting at `head` for the first struct
        // whose `type` field equals `targetType`. Returns nullptr if
        // not found or if the chain is empty.
        const void* findInNextChain(const void* head, XrStructureType targetType) {
            const auto* current = reinterpret_cast<const XrBaseInStructure*>(head);
            while (current) {
                if (current->type == targetType) return current;
                current = current->next;
            }
            return nullptr;
        }

    } // namespace

    struct HelmetOverlay::Impl {
        bool armed = false;
        bool loggedBypass = false;
        HelmetOverlayConfig config;

        OpenXrApi* api = nullptr;
        XrSession session = XR_NULL_HANDLE;
        XrSwapchain swapchain = XR_NULL_HANDLE;
        XrSpace viewSpace = XR_NULL_HANDLE;

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11Texture2D> stagingTexture;

        // Retained swapchain image resources. We keep the ID3D11Texture2D
        // pointers so CopyResource() can target them directly without
        // re-enumerating every frame. Indexed by the image index the
        // runtime returns from xrAcquireSwapchainImage.
        std::vector<ComPtr<ID3D11Texture2D>> swapchainImages;

        // Composition layer kept stable between appendLayer() calls so
        // the pointer we hand to layer.cpp remains valid through the
        // xrEndFrame forward.
        XrCompositionLayerQuad quadLayer{};
    };

    HelmetOverlay::HelmetOverlay() : m_impl(std::make_unique<Impl>()) {}
    HelmetOverlay::~HelmetOverlay() { shutdown(); }

    bool HelmetOverlay::initialize(OpenXrApi* api,
                                   XrSession session,
                                   const void* sessionCreateInfoNextChain,
                                   const HelmetOverlayConfig& config,
                                   const std::filesystem::path& dllHome) {
        (void)dllHome;

        m_impl->config = config;
        m_impl->api = api;
        m_impl->session = session;

        if (!config.enabled) {
            Log("HelmetOverlay: disabled by config, staying inert\n");
            return false;
        }
        if (!api) {
            ErrorLog("HelmetOverlay: OpenXrApi* is null, cannot arm\n");
            return false;
        }

        // ---- 1. Find the app's D3D11 binding in the session next-chain.
        const auto* d3d11Binding = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(
            findInNextChain(sessionCreateInfoNextChain, XR_TYPE_GRAPHICS_BINDING_D3D11_KHR));
        if (!d3d11Binding || !d3d11Binding->device) {
            Log("HelmetOverlay: session is not D3D11 (Vulkan or D3D12 host?), "
                "overlay will not run. Future versions will add a D3D11on12 "
                "path for D3D12 hosts.\n");
            return false;
        }
        m_impl->device = d3d11Binding->device;
        m_impl->device->GetImmediateContext(&m_impl->context);
        if (!m_impl->context) {
            ErrorLog("HelmetOverlay: ID3D11Device had no immediate context\n");
            return false;
        }

        // ---- 2. Verify the runtime exposes a usable swapchain format.
        // If our preferred format is not offered we bypass rather than
        // guess — a mismatched format would make the per-frame
        // CopyResource fail.
        uint32_t formatCount = 0;
        if (XR_FAILED(api->xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr)) ||
            formatCount == 0) {
            Log("HelmetOverlay: xrEnumerateSwapchainFormats returned 0 formats\n");
            return false;
        }
        std::vector<int64_t> formats(formatCount);
        if (XR_FAILED(api->xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()))) {
            Log("HelmetOverlay: xrEnumerateSwapchainFormats failed on second call\n");
            return false;
        }
        bool formatSupported = false;
        for (const auto f : formats) {
            if (f == kPreferredFormat) { formatSupported = true; break; }
        }
        if (!formatSupported) {
            Log(fmt::format("HelmetOverlay: runtime does not expose preferred format ({}), bypassing\n",
                            kPreferredFormat));
            return false;
        }

        // ---- 3. Create the XR swapchain.
        XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sci.format = kPreferredFormat;
        sci.sampleCount = 1;
        sci.width = kSwapchainWidth;
        sci.height = kSwapchainHeight;
        sci.faceCount = 1;
        sci.arraySize = 1;
        sci.mipCount = 1;
        if (XR_FAILED(api->xrCreateSwapchain(session, &sci, &m_impl->swapchain))) {
            Log("HelmetOverlay: xrCreateSwapchain failed\n");
            return false;
        }

        // ---- 4. Enumerate the swapchain images and retain each
        // ID3D11Texture2D*. We do not AddRef these textures — the
        // OpenXR runtime owns their lifetimes, and they remain valid
        // for the lifetime of the XrSwapchain.
        uint32_t imageCount = 0;
        if (XR_FAILED(api->xrEnumerateSwapchainImages(m_impl->swapchain, 0, &imageCount, nullptr)) ||
            imageCount == 0) {
            Log("HelmetOverlay: xrEnumerateSwapchainImages returned 0\n");
            return false;
        }
        std::vector<XrSwapchainImageD3D11KHR> images(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if (XR_FAILED(api->xrEnumerateSwapchainImages(
                m_impl->swapchain,
                imageCount,
                &imageCount,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())))) {
            Log("HelmetOverlay: xrEnumerateSwapchainImages (second call) failed\n");
            return false;
        }
        m_impl->swapchainImages.resize(imageCount);
        for (uint32_t i = 0; i < imageCount; ++i) {
            m_impl->swapchainImages[i] = images[i].texture;
        }

        // ---- 5. Create the head-locked reference space.
        XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        rci.poseInReferenceSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        rci.poseInReferenceSpace.position = {0.0f, 0.0f, 0.0f};
        if (XR_FAILED(api->xrCreateReferenceSpace(session, &rci, &m_impl->viewSpace))) {
            Log("HelmetOverlay: xrCreateReferenceSpace(VIEW) failed\n");
            return false;
        }

        // ---- 6. Build the staging texture with the procedural mask.
        std::vector<uint8_t> maskBytes(static_cast<size_t>(kSwapchainWidth) * kSwapchainHeight * 4u);
        generateVisorMask(maskBytes.data());

        D3D11_TEXTURE2D_DESC td{};
        td.Width = kSwapchainWidth;
        td.Height = kSwapchainHeight;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        // D3D11_USAGE_IMMUTABLE requires BindFlags != 0. SHADER_RESOURCE
        // is the lightest flag that satisfies the validator; we never
        // actually sample from the texture — it is only ever the source
        // of a CopyResource into the swapchain image — but the runtime
        // would reject (hresult 0x80070057 / E_INVALIDARG) without it.
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = 0;
        td.MiscFlags = 0;

        D3D11_SUBRESOURCE_DATA sd{};
        sd.pSysMem = maskBytes.data();
        sd.SysMemPitch = kSwapchainWidth * 4u;
        sd.SysMemSlicePitch = 0;

        const HRESULT hr = m_impl->device->CreateTexture2D(&td, &sd, &m_impl->stagingTexture);
        if (FAILED(hr)) {
            Log(fmt::format("HelmetOverlay: CreateTexture2D(staging) failed, hr=0x{:08X}\n",
                            static_cast<uint32_t>(hr)));
            return false;
        }

        // ---- 7. Pre-fill the stable quad layer fields. `pose.position.z`
        // is negative because -Z is forward in OpenXR's view space.
        m_impl->quadLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        m_impl->quadLayer.next = nullptr;
        m_impl->quadLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                       XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        m_impl->quadLayer.space = m_impl->viewSpace;
        m_impl->quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        m_impl->quadLayer.subImage.swapchain = m_impl->swapchain;
        m_impl->quadLayer.subImage.imageRect.offset = {0, 0};
        m_impl->quadLayer.subImage.imageRect.extent = {static_cast<int32_t>(kSwapchainWidth),
                                                       static_cast<int32_t>(kSwapchainHeight)};
        m_impl->quadLayer.subImage.imageArrayIndex = 0;
        m_impl->quadLayer.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        m_impl->quadLayer.pose.position = {0.0f, 0.0f, -config.distance_m};
        m_impl->quadLayer.size = {config.width_m, config.height_m};

        m_impl->armed = true;
        Log(fmt::format(
            "HelmetOverlay: armed. swapchain={}x{}, images={}, "
            "distance={:.2f}m, size={:.2f}x{:.2f}m\n",
            kSwapchainWidth, kSwapchainHeight, imageCount,
            config.distance_m, config.width_m, config.height_m));
        return true;
    }

    void HelmetOverlay::shutdown() {
        if (!m_impl) return;
        if (m_impl->api) {
            if (m_impl->viewSpace != XR_NULL_HANDLE) {
                m_impl->api->xrDestroySpace(m_impl->viewSpace);
                m_impl->viewSpace = XR_NULL_HANDLE;
            }
            if (m_impl->swapchain != XR_NULL_HANDLE) {
                m_impl->api->xrDestroySwapchain(m_impl->swapchain);
                m_impl->swapchain = XR_NULL_HANDLE;
            }
        }
        m_impl->swapchainImages.clear();
        m_impl->stagingTexture.Reset();
        m_impl->context.Reset();
        m_impl->device.Reset();
        m_impl->session = XR_NULL_HANDLE;
        m_impl->api = nullptr;
        m_impl->armed = false;
    }

    bool HelmetOverlay::appendLayer(XrTime /*displayTime*/,
                                    const XrCompositionLayerBaseHeader** outLayer) {
        if (!m_impl || !m_impl->armed || !outLayer) return false;
        if (!m_impl->api || m_impl->swapchain == XR_NULL_HANDLE) return false;

        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(m_impl->api->xrAcquireSwapchainImage(m_impl->swapchain, &ai, &imageIndex))) {
            // A single failure disarms the overlay to avoid spamming the
            // host with per-frame error logs on a misbehaving runtime.
            if (!m_impl->loggedBypass) {
                ErrorLog("HelmetOverlay: xrAcquireSwapchainImage failed, disarming overlay\n");
                m_impl->loggedBypass = true;
            }
            m_impl->armed = false;
            return false;
        }

        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(m_impl->api->xrWaitSwapchainImage(m_impl->swapchain, &wi))) {
            // Release what we acquired so the runtime's internal queue
            // doesn't stall on a phantom in-flight image.
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            m_impl->api->xrReleaseSwapchainImage(m_impl->swapchain, &ri);
            return false;
        }

        if (imageIndex < m_impl->swapchainImages.size() &&
            m_impl->swapchainImages[imageIndex] &&
            m_impl->stagingTexture &&
            m_impl->context) {
            m_impl->context->CopyResource(m_impl->swapchainImages[imageIndex].Get(),
                                          m_impl->stagingTexture.Get());
        }

        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (XR_FAILED(m_impl->api->xrReleaseSwapchainImage(m_impl->swapchain, &ri))) {
            return false;
        }

        *outLayer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_impl->quadLayer);
        return true;
    }

    bool HelmetOverlay::isArmed() const {
        return m_impl && m_impl->armed;
    }

} // namespace openxr_api_layer
