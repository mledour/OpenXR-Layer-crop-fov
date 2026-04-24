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

// stb_image gives us single-header PNG decoding with no extra build system
// plumbing. STBI_ONLY_PNG strips JPEG / BMP / TGA / GIF / HDR / etc. so we
// do not pay for code we never run. STB_IMAGE_IMPLEMENTATION is defined
// in exactly one TU (this one); if the overlay is ever split into more
// source files, the IMPLEMENTATION define must move to a dedicated
// stb_image.cpp to avoid multiple-definition errors.
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#pragma warning(push)
#pragma warning(disable : 4244 4456 4457 4459) // stb_image internals
#include <stb_image.h>
#pragma warning(pop)

#include <vector>

// D3D11 helmet-interior overlay.
//
// Two rendering paths, chosen at session init:
//  (1) equirect2 — preferred. Requires the runtime to support
//      XR_KHR_composition_layer_equirect2 AND a readable PNG at
//      dllHome / config.textureRelativePath. The PNG is uploaded once
//      into a static swapchain (XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT)
//      and never re-copied per frame — the compositor samples it
//      directly from the sphere. Gives a 360° head-locked surround
//      that is the natural geometry for an inside-a-helmet view.
//  (2) quad — fallback. Head-locked XrCompositionLayerQuad with a
//      procedurally generated elliptical mask (black periphery,
//      transparent visor cutout). Used whenever the extension is
//      absent, the PNG is missing, or the PNG fails to decode. Per-
//      frame CopyResource of a small staging texture into the
//      swapchain image.
//
// Either way the session's graphics binding must be D3D11; other
// bindings bypass silently (see CLAUDE.md — never crash the host).
// D3D11 entry points are delay-loaded at link time so a Vulkan-only
// game that never enables the overlay never pulls d3d11.dll into its
// process.

namespace openxr_api_layer {

    using namespace log;

    namespace {

        // --- Quad path tunables. Equirect2 is full-sphere, no tuning. --

        constexpr uint32_t kQuadSwapchainWidth = 512;
        constexpr uint32_t kQuadSwapchainHeight = 512;

        // DXGI_FORMAT_R8G8B8A8_UNORM = 28. We ship straight alpha, so
        // using an sRGB target would double-apply gamma to the black
        // mask. Both paths use this format.
        constexpr int64_t kPreferredFormat = 28;

        // Procedural mask for the quad fallback: kQuadSwapchainWidth *
        // kQuadSwapchainHeight RGBA8 bytes. Alpha ramps from 0 inside an
        // ellipse (horizontal visor slit) to 255 outside, with a
        // feathered edge. RGB stays (0,0,0) everywhere so the unmasked
        // region is opaque black.
        void generateVisorMask(uint8_t* out) {
            constexpr float kAx = 0.46f;
            constexpr float kAy = 0.30f;
            constexpr float kFeather = 0.08f;

            const float cx = kQuadSwapchainWidth * 0.5f;
            const float cy = kQuadSwapchainHeight * 0.5f;
            const float rx = kQuadSwapchainWidth * kAx;
            const float ry = kQuadSwapchainHeight * kAy;

            for (uint32_t y = 0; y < kQuadSwapchainHeight; ++y) {
                for (uint32_t x = 0; x < kQuadSwapchainWidth; ++x) {
                    const float dx = (static_cast<float>(x) - cx) / rx;
                    const float dy = (static_cast<float>(y) - cy) / ry;
                    const float r = std::sqrt(dx * dx + dy * dy);

                    float alpha;
                    if (r <= 1.0f)                      alpha = 0.0f;
                    else if (r >= 1.0f + kFeather)      alpha = 1.0f;
                    else                                alpha = (r - 1.0f) / kFeather;

                    const size_t idx = (static_cast<size_t>(y) * kQuadSwapchainWidth + x) * 4u;
                    out[idx + 0] = 0;
                    out[idx + 1] = 0;
                    out[idx + 2] = 0;
                    out[idx + 3] = static_cast<uint8_t>(alpha * 255.0f + 0.5f);
                }
            }
        }

        const void* findInNextChain(const void* head, XrStructureType targetType) {
            const auto* current = reinterpret_cast<const XrBaseInStructure*>(head);
            while (current) {
                if (current->type == targetType) return current;
                current = current->next;
            }
            return nullptr;
        }

        // Returns true iff the supplied granted-extension list contains
        // XR_KHR_composition_layer_equirect2. Called once at session
        // init to decide between the equirect2 and quad paths.
        bool isEquirect2Granted(const std::vector<std::string>& granted) {
            for (const auto& e : granted) {
                if (e == XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME) return true;
            }
            return false;
        }

        // True iff a swapchain format list contains our preferred format.
        bool listHasFormat(const std::vector<int64_t>& formats, int64_t wanted) {
            for (auto f : formats) if (f == wanted) return true;
            return false;
        }

    } // namespace

    enum class HelmetOverlayMode {
        Disabled,
        Quad,
        Equirect2,
    };

    struct HelmetOverlay::Impl {
        HelmetOverlayMode mode = HelmetOverlayMode::Disabled;
        bool loggedBypass = false;
        HelmetOverlayConfig config;

        OpenXrApi* api = nullptr;
        XrSession session = XR_NULL_HANDLE;
        XrSwapchain swapchain = XR_NULL_HANDLE;
        XrSpace viewSpace = XR_NULL_HANDLE;

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;

        // Quad-mode per-frame copy source. Unused in Equirect2 mode.
        ComPtr<ID3D11Texture2D> stagingTexture;
        std::vector<ComPtr<ID3D11Texture2D>> swapchainImages;

        // Composition layer structs kept stable between appendLayer()
        // calls so the pointer we hand to layer.cpp remains valid
        // through xrEndFrame. Exactly one is used, depending on mode.
        XrCompositionLayerQuad quadLayer{};
        XrCompositionLayerEquirect2KHR equirectLayer{};
    };

    HelmetOverlay::HelmetOverlay() : m_impl(std::make_unique<Impl>()) {}
    HelmetOverlay::~HelmetOverlay() { shutdown(); }

    // --- initialize() decomposed into helpers. --------------------------

    namespace {

        // Loads a PNG at `path` into a newly-allocated RGBA8 buffer.
        // Returns true on success, in which case caller takes ownership
        // of `*outPixels` and must eventually free it with stbi_image_free.
        // On failure nothing is allocated and an error is logged.
        bool loadPngRgba8(const std::filesystem::path& path,
                          uint8_t** outPixels,
                          int* outWidth,
                          int* outHeight) {
            int channels = 0;
            // stbi_load returns RGBA when the 4th arg is 4, regardless
            // of the source channel count. stb_image opens files via
            // fopen under the hood; Windows-specific path issues are
            // handled by stb_image's internal wide-char path on _WIN32.
            *outPixels = stbi_load(path.string().c_str(), outWidth, outHeight, &channels, 4);
            if (!*outPixels) {
                const char* reason = stbi_failure_reason();
                Log(fmt::format("HelmetOverlay: stbi_load('{}') failed: {}\n",
                                path.string(), reason ? reason : "unknown"));
                return false;
            }
            return true;
        }

    } // namespace

    bool HelmetOverlay::initialize(OpenXrApi* api,
                                   XrSession session,
                                   const void* sessionCreateInfoNextChain,
                                   const HelmetOverlayConfig& config,
                                   const std::filesystem::path& dllHome) {
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

        // ---- Common: locate the app's D3D11 binding and device. -------
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

        // ---- Common: format support check. ----------------------------
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
        if (!listHasFormat(formats, kPreferredFormat)) {
            Log(fmt::format("HelmetOverlay: runtime does not expose preferred format ({}), bypassing\n",
                            kPreferredFormat));
            return false;
        }

        // ---- Common: head-locked reference space. ---------------------
        XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        rci.poseInReferenceSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        rci.poseInReferenceSpace.position = {0.0f, 0.0f, 0.0f};
        if (XR_FAILED(api->xrCreateReferenceSpace(session, &rci, &m_impl->viewSpace))) {
            Log("HelmetOverlay: xrCreateReferenceSpace(VIEW) failed\n");
            return false;
        }

        // ---- Try equirect2 path first if extension + PNG are both OK.
        const bool extGranted = isEquirect2Granted(api->GetGrantedExtensions());
        const std::filesystem::path pngPath = dllHome / config.textureRelativePath;
        const bool pngExists = std::filesystem::exists(pngPath);

        if (extGranted && pngExists) {
            if (tryInitEquirect2(pngPath)) {
                m_impl->mode = HelmetOverlayMode::Equirect2;
                Log(fmt::format("HelmetOverlay: armed (equirect2 + PNG), texture='{}'\n",
                                pngPath.string()));
                return true;
            }
            Log("HelmetOverlay: equirect2 init failed, falling back to quad+procedural\n");
            // fall through to quad path
        } else {
            if (!extGranted) {
                Log("HelmetOverlay: runtime does not support XR_KHR_composition_layer_equirect2, "
                    "using quad+procedural fallback\n");
            } else {
                Log(fmt::format("HelmetOverlay: no PNG at '{}', using quad+procedural fallback\n",
                                pngPath.string()));
            }
        }

        // ---- Fallback: quad + procedural mask. ------------------------
        if (tryInitQuad()) {
            m_impl->mode = HelmetOverlayMode::Quad;
            Log(fmt::format("HelmetOverlay: armed (quad+procedural). "
                            "distance={:.2f}m, size={:.2f}x{:.2f}m\n",
                            config.distance_m, config.width_m, config.height_m));
            return true;
        }

        Log("HelmetOverlay: both init paths failed, staying inert\n");
        return false;
    }

    // --- Equirect2 init: one-shot PNG upload into a static swapchain. ---

    bool HelmetOverlay::tryInitEquirect2(const std::filesystem::path& pngPath) {
        uint8_t* pixels = nullptr;
        int width = 0, height = 0;
        if (!loadPngRgba8(pngPath, &pixels, &width, &height)) {
            return false;
        }
        // Guard: stbi output is opaque-typed; use a RAII wrapper.
        struct PixelGuard {
            uint8_t* p;
            ~PixelGuard() { if (p) stbi_image_free(p); }
        } guard{pixels};

        if (width <= 0 || height <= 0) {
            Log(fmt::format("HelmetOverlay: PNG has invalid dimensions {}x{}\n", width, height));
            return false;
        }

        // ---- Static swapchain sized to the PNG. -----------------------
        XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sci.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
        sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sci.format = kPreferredFormat;
        sci.sampleCount = 1;
        sci.width = static_cast<uint32_t>(width);
        sci.height = static_cast<uint32_t>(height);
        sci.faceCount = 1;
        sci.arraySize = 1;
        sci.mipCount = 1;
        if (XR_FAILED(m_impl->api->xrCreateSwapchain(m_impl->session, &sci, &m_impl->swapchain))) {
            Log("HelmetOverlay: equirect2 xrCreateSwapchain failed\n");
            return false;
        }

        // ---- Enumerate images (STATIC_IMAGE_BIT guarantees exactly 1).
        uint32_t imageCount = 0;
        if (XR_FAILED(m_impl->api->xrEnumerateSwapchainImages(m_impl->swapchain, 0, &imageCount, nullptr)) ||
            imageCount == 0) {
            Log("HelmetOverlay: equirect2 xrEnumerateSwapchainImages returned 0\n");
            return false;
        }
        std::vector<XrSwapchainImageD3D11KHR> images(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if (XR_FAILED(m_impl->api->xrEnumerateSwapchainImages(
                m_impl->swapchain, imageCount, &imageCount,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())))) {
            Log("HelmetOverlay: equirect2 xrEnumerateSwapchainImages (second call) failed\n");
            return false;
        }

        // ---- Upload the PNG bytes into a staging texture, then
        // CopyResource them into the swapchain image once. After
        // release the compositor samples that image for every frame.
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(width);
        td.Height = static_cast<UINT>(height);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = 0;
        td.MiscFlags = 0;

        D3D11_SUBRESOURCE_DATA sd{};
        sd.pSysMem = pixels;
        sd.SysMemPitch = static_cast<UINT>(width) * 4u;
        sd.SysMemSlicePitch = 0;

        ComPtr<ID3D11Texture2D> staging;
        const HRESULT hr = m_impl->device->CreateTexture2D(&td, &sd, &staging);
        if (FAILED(hr)) {
            Log(fmt::format("HelmetOverlay: equirect2 CreateTexture2D failed, hr=0x{:08X}\n",
                            static_cast<uint32_t>(hr)));
            return false;
        }

        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(m_impl->api->xrAcquireSwapchainImage(m_impl->swapchain, &ai, &imageIndex))) {
            Log("HelmetOverlay: equirect2 xrAcquireSwapchainImage failed\n");
            return false;
        }
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(m_impl->api->xrWaitSwapchainImage(m_impl->swapchain, &wi))) {
            Log("HelmetOverlay: equirect2 xrWaitSwapchainImage failed\n");
            return false;
        }
        m_impl->context->CopyResource(images[imageIndex].texture, staging.Get());
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (XR_FAILED(m_impl->api->xrReleaseSwapchainImage(m_impl->swapchain, &ri))) {
            Log("HelmetOverlay: equirect2 xrReleaseSwapchainImage failed\n");
            return false;
        }

        // ---- Prepare the stable equirect2 layer struct. ---------------
        // Full-sphere coverage: central horizontal = 2π, vertical spans
        // from +π/2 (zenith) to -π/2 (nadir). radius = 0 means "treat
        // the sphere as having infinite radius" per the extension spec —
        // ideal for a head-locked surround that should never feel
        // "closer" or "further" as the user moves.
        constexpr float kPi = 3.14159265358979323846f;
        m_impl->equirectLayer.type = XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR;
        m_impl->equirectLayer.next = nullptr;
        m_impl->equirectLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                           XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        m_impl->equirectLayer.space = m_impl->viewSpace;
        m_impl->equirectLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        m_impl->equirectLayer.subImage.swapchain = m_impl->swapchain;
        m_impl->equirectLayer.subImage.imageRect.offset = {0, 0};
        m_impl->equirectLayer.subImage.imageRect.extent = {width, height};
        m_impl->equirectLayer.subImage.imageArrayIndex = 0;
        m_impl->equirectLayer.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        m_impl->equirectLayer.pose.position = {0.0f, 0.0f, 0.0f};
        m_impl->equirectLayer.radius = 0.0f;
        m_impl->equirectLayer.centralHorizontalAngle = 2.0f * kPi;
        m_impl->equirectLayer.upperVerticalAngle = kPi * 0.5f;
        m_impl->equirectLayer.lowerVerticalAngle = -kPi * 0.5f;

        return true;
    }

    // --- Quad init: unchanged from before the refactor. -----------------

    bool HelmetOverlay::tryInitQuad() {
        XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sci.format = kPreferredFormat;
        sci.sampleCount = 1;
        sci.width = kQuadSwapchainWidth;
        sci.height = kQuadSwapchainHeight;
        sci.faceCount = 1;
        sci.arraySize = 1;
        sci.mipCount = 1;
        if (XR_FAILED(m_impl->api->xrCreateSwapchain(m_impl->session, &sci, &m_impl->swapchain))) {
            Log("HelmetOverlay: quad xrCreateSwapchain failed\n");
            return false;
        }

        uint32_t imageCount = 0;
        if (XR_FAILED(m_impl->api->xrEnumerateSwapchainImages(m_impl->swapchain, 0, &imageCount, nullptr)) ||
            imageCount == 0) {
            Log("HelmetOverlay: quad xrEnumerateSwapchainImages returned 0\n");
            return false;
        }
        std::vector<XrSwapchainImageD3D11KHR> images(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if (XR_FAILED(m_impl->api->xrEnumerateSwapchainImages(
                m_impl->swapchain, imageCount, &imageCount,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())))) {
            Log("HelmetOverlay: quad xrEnumerateSwapchainImages (second call) failed\n");
            return false;
        }
        m_impl->swapchainImages.resize(imageCount);
        for (uint32_t i = 0; i < imageCount; ++i) {
            m_impl->swapchainImages[i] = images[i].texture;
        }

        std::vector<uint8_t> maskBytes(static_cast<size_t>(kQuadSwapchainWidth) * kQuadSwapchainHeight * 4u);
        generateVisorMask(maskBytes.data());

        D3D11_TEXTURE2D_DESC td{};
        td.Width = kQuadSwapchainWidth;
        td.Height = kQuadSwapchainHeight;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = 0;
        td.MiscFlags = 0;

        D3D11_SUBRESOURCE_DATA sd{};
        sd.pSysMem = maskBytes.data();
        sd.SysMemPitch = kQuadSwapchainWidth * 4u;
        sd.SysMemSlicePitch = 0;

        const HRESULT hr = m_impl->device->CreateTexture2D(&td, &sd, &m_impl->stagingTexture);
        if (FAILED(hr)) {
            Log(fmt::format("HelmetOverlay: quad CreateTexture2D(staging) failed, hr=0x{:08X}\n",
                            static_cast<uint32_t>(hr)));
            return false;
        }

        m_impl->quadLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        m_impl->quadLayer.next = nullptr;
        m_impl->quadLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                       XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        m_impl->quadLayer.space = m_impl->viewSpace;
        m_impl->quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        m_impl->quadLayer.subImage.swapchain = m_impl->swapchain;
        m_impl->quadLayer.subImage.imageRect.offset = {0, 0};
        m_impl->quadLayer.subImage.imageRect.extent = {static_cast<int32_t>(kQuadSwapchainWidth),
                                                       static_cast<int32_t>(kQuadSwapchainHeight)};
        m_impl->quadLayer.subImage.imageArrayIndex = 0;
        m_impl->quadLayer.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        m_impl->quadLayer.pose.position = {0.0f, 0.0f, -m_impl->config.distance_m};
        m_impl->quadLayer.size = {m_impl->config.width_m, m_impl->config.height_m};

        return true;
    }

    // --- Per-frame entry point. -----------------------------------------

    bool HelmetOverlay::appendLayer(XrTime /*displayTime*/,
                                    const XrCompositionLayerBaseHeader** outLayer) {
        if (!m_impl || !outLayer) return false;
        if (m_impl->mode == HelmetOverlayMode::Disabled) return false;
        if (!m_impl->api || m_impl->swapchain == XR_NULL_HANDLE) return false;

        if (m_impl->mode == HelmetOverlayMode::Equirect2) {
            // Static swapchain: image contents are already in place, no
            // per-frame copy. Just hand the layer pointer back.
            *outLayer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_impl->equirectLayer);
            return true;
        }

        // HelmetOverlayMode::Quad — per-frame CopyResource sandwiched
        // between acquire/wait and release.
        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(m_impl->api->xrAcquireSwapchainImage(m_impl->swapchain, &ai, &imageIndex))) {
            if (!m_impl->loggedBypass) {
                ErrorLog("HelmetOverlay: xrAcquireSwapchainImage failed, disarming overlay\n");
                m_impl->loggedBypass = true;
            }
            m_impl->mode = HelmetOverlayMode::Disabled;
            return false;
        }

        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(m_impl->api->xrWaitSwapchainImage(m_impl->swapchain, &wi))) {
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
        m_impl->mode = HelmetOverlayMode::Disabled;
    }

    bool HelmetOverlay::isArmed() const {
        return m_impl && m_impl->mode != HelmetOverlayMode::Disabled;
    }

} // namespace openxr_api_layer
