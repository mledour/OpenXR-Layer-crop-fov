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
// Renders the user's helmet_visor.png as a head-locked
// XrCompositionLayerQuad. distance_m is the plane distance from the
// eye; width_m is the quad's physical width; height follows the PNG
// aspect ratio so the image is never stretched.
//
// The swapchain is created with XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT
// — content is uploaded once at session start (acquire / wait /
// CopyResource(staging→swapchain) / release, then we drop the
// staging texture). The runtime composites that single image every
// frame on its own; appendLayer() just hands back the cached layer
// pointer with no per-frame texture work, which is the entire point
// of the static-image flag.
//
// Earlier revisions tried a XrCompositionLayerCylinderKHR backend
// for natural edge curvature, but neither runtime we test
// against (Pimax OpenXR official, PimaxXR) grants the extension.
// The kept-simple answer is to pre-warp the PNG offline with
// tools/cylinder_warp.py — the atan-based 1-D horizontal warp
// reconstructs the cylinder projection in the asset itself, the
// DLL stays a flat quad, and the result works on every runtime.
//
// The PNG is mandatory: if no helmet_visor.png is found at
// dllHome / config.textureRelativePath, the overlay does not arm.
// Decoded once via stb_image at session init, optionally darkened by
// config.brightness, then uploaded to a staging texture. Per-frame
// work is one CopyResource of that staging into the acquired
// swapchain image; final alpha blending happens in the OpenXR
// compositor via XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT.
//
// A spherical XR_KHR_composition_layer_equirect2 path was prototyped
// and removed because Pimax OpenXR does not expose that extension.
// Bringing it back later is just a matter of restoring the implicit
// extension request and a tryInitEquirect2() that reuses the PNG.
//
// The session's graphics binding must be D3D11; other bindings
// bypass silently (see CLAUDE.md — never crash the host). D3D11
// entry points are delay-loaded at link time so a Vulkan-only game
// that never enables the overlay never pulls d3d11.dll into its
// process.

namespace openxr_api_layer {

    using namespace log;

    namespace {

        // DXGI format constants. Avoid pulling <dxgiformat.h> just for
        // these two values — the underlying types are stable.
        //   - 28: DXGI_FORMAT_R8G8B8A8_UNORM       (linear interpretation)
        //   - 29: DXGI_FORMAT_R8G8B8A8_UNORM_SRGB  (sRGB-decoded on sample)
        //
        // We prefer SRGB because PNG photo content is stored in sRGB
        // space, and the OpenXR compositor blends in linear space. Using
        // UNORM would skip the sRGB decode and the compositor would
        // treat midtones / highlights as already-linear, blowing them
        // out. Fallback to UNORM is kept for runtimes that don't expose
        // SRGB (rare, but possible).
        constexpr int64_t kFormatSRGB  = 29;
        constexpr int64_t kFormatUNORM = 28;

        const void* findInNextChain(const void* head, XrStructureType targetType) {
            const auto* current = reinterpret_cast<const XrBaseInStructure*>(head);
            while (current) {
                if (current->type == targetType) return current;
                current = current->next;
            }
            return nullptr;
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
    };

    struct HelmetOverlay::Impl {
        HelmetOverlayMode mode = HelmetOverlayMode::Disabled;
        HelmetOverlayConfig config;

        OpenXrApi* api = nullptr;
        XrSession session = XR_NULL_HANDLE;
        // Static-image swapchain — written once in createSwapchainFromPng,
        // composited every frame by the runtime without our help. Only
        // the handle is kept alive for the session lifetime; the
        // staging texture and the swapchain image pointer are released
        // at the end of the init function.
        XrSwapchain swapchain = XR_NULL_HANDLE;
        XrSpace viewSpace = XR_NULL_HANDLE;

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;

        // Composition layer kept stable between appendLayer() calls so
        // the pointer we hand back to layer.cpp remains valid through
        // xrEndFrame.
        XrCompositionLayerQuad quadLayer{};

        // PNG aspect ratio captured at init so live-edit can recompute
        // the quad height when the user changes width_m, without
        // re-decoding the PNG.
        float pngAspectRatio = 1.0f;  // height / width

        // DXGI format selected at init, used for both the (transient)
        // staging texture and the XR swapchain. SRGB when the runtime
        // exposes it, UNORM otherwise. Storing it on Impl avoids
        // re-querying the format list during the init helpers.
        int64_t swapchainFormat = kFormatUNORM;
        DXGI_FORMAT d3dFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
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

        // Prefer SRGB so PNG sRGB content gamma-decodes correctly in the
        // compositor. UNORM is the fallback for runtimes that only
        // expose linear formats.
        if (listHasFormat(formats, kFormatSRGB)) {
            m_impl->swapchainFormat = kFormatSRGB;
            m_impl->d3dFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            Log("HelmetOverlay: using R8G8B8A8_UNORM_SRGB swapchain (correct gamma for PNG content)\n");
        } else if (listHasFormat(formats, kFormatUNORM)) {
            m_impl->swapchainFormat = kFormatUNORM;
            m_impl->d3dFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            Log("HelmetOverlay: SRGB unavailable, falling back to R8G8B8A8_UNORM "
                "(PNG photo content may appear slightly over-bright)\n");
        } else {
            Log("HelmetOverlay: runtime exposes neither RGBA8 UNORM nor SRGB, bypassing\n");
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

        // ---- Load PNG (mandatory). -----------------------------------
        // No fallback: if the PNG is absent or fails to decode, the
        // overlay does not arm. The build ships a default helmet_visor.png
        // alongside the DLL so this only fails when the user explicitly
        // deleted it.
        const std::filesystem::path pngPath = dllHome / config.textureRelativePath;
        if (!std::filesystem::exists(pngPath)) {
            Log(fmt::format("HelmetOverlay: no PNG at '{}', overlay will not arm\n",
                            pngPath.string()));
            return false;
        }

        uint8_t* pngPixels = nullptr;
        int pngW = 0, pngH = 0;
        struct PixelGuard {
            uint8_t* p;
            ~PixelGuard() { if (p) stbi_image_free(p); }
        };
        if (!loadPngRgba8(pngPath, &pngPixels, &pngW, &pngH)) {
            return false;
        }
        PixelGuard guard{pngPixels};

        if (!createSwapchainFromPng(pngPixels, pngW, pngH) ||
            !tryInitQuad(pngW, pngH)) {
            Log("HelmetOverlay: quad init failed, staying inert\n");
            return false;
        }

        m_impl->mode = HelmetOverlayMode::Quad;
        Log(fmt::format("HelmetOverlay: armed. distance={:.2f}m, size={:.2f}x{:.2f}m\n",
                        config.distance_m,
                        m_impl->quadLayer.size.width,
                        m_impl->quadLayer.size.height));
        return true;
    }

    // --- Quad init. The PNG is mandatory: swapchain + staging are
    // sized to it, and the quad height in meters is derived from the
    // PNG aspect ratio so the image is never stretched — the user
    // controls width_m, the height follows automatically.

    // Shared swapchain + staging-texture setup used by both backends.
    // Creates the XrSwapchain (single static image — see
    // XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) at PNG dimensions and
    // uploads the PNG bytes (with brightness multiplier applied) into
    // it ONCE. After this, the runtime composites that one image every
    // frame on its own — the layer never touches the swapchain again,
    // so the per-frame hot path drops the acquire/wait/copy/release
    // dance entirely. Both staging and the swapchain image pointer
    // are released at the end of the function; only the XrSwapchain
    // handle survives, used by m_impl->quadLayer.subImage.swapchain.
    bool HelmetOverlay::createSwapchainFromPng(const uint8_t* pngPixels, int pngWidth, int pngHeight) {
        if (!pngPixels || pngWidth <= 0 || pngHeight <= 0) {
            Log("HelmetOverlay: createSwapchainFromPng called with invalid PNG\n");
            return false;
        }
        const uint32_t texW = static_cast<uint32_t>(pngWidth);
        const uint32_t texH = static_cast<uint32_t>(pngHeight);

        XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        // STATIC_IMAGE_BIT: runtime allocates a single image, we acquire
        // it once below, write it, and release it. Subsequent frames
        // never re-acquire — the runtime keeps using the same image.
        sci.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
        sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sci.format = m_impl->swapchainFormat;
        sci.sampleCount = 1;
        sci.width = texW;
        sci.height = texH;
        sci.faceCount = 1;
        sci.arraySize = 1;
        sci.mipCount = 1;
        if (XR_FAILED(m_impl->api->xrCreateSwapchain(m_impl->session, &sci, &m_impl->swapchain))) {
            Log("HelmetOverlay: xrCreateSwapchain failed\n");
            return false;
        }

        // STATIC_IMAGE_BIT guarantees imageCount == 1.
        uint32_t imageCount = 0;
        if (XR_FAILED(m_impl->api->xrEnumerateSwapchainImages(m_impl->swapchain, 0, &imageCount, nullptr)) ||
            imageCount == 0) {
            Log("HelmetOverlay: xrEnumerateSwapchainImages returned 0\n");
            return false;
        }
        std::vector<XrSwapchainImageD3D11KHR> images(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if (XR_FAILED(m_impl->api->xrEnumerateSwapchainImages(
                m_impl->swapchain, imageCount, &imageCount,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())))) {
            Log("HelmetOverlay: xrEnumerateSwapchainImages (second call) failed\n");
            return false;
        }
        ID3D11Texture2D* swapchainImage = images[0].texture; // not AddRef'd, runtime owns it

        // Build the source pixel buffer we own and can mutate (so the
        // brightness multiplier below can run without touching
        // stb_image's internal allocation).
        const size_t pixelBytes = static_cast<size_t>(texW) * texH * 4u;
        std::vector<uint8_t> uploadBytes(pixelBytes);
        std::memcpy(uploadBytes.data(), pngPixels, pixelBytes);

        // Apply brightness multiplier on RGB before upload. Alpha is
        // left untouched so the visor cutout stays transparent even at
        // brightness=0. Skipped when the multiplier is ~1.0 (saves
        // ~50 ms on a 6K image).
        const float bright = m_impl->config.brightness;
        if (bright < 0.999f) {
            for (size_t i = 0; i + 3 < uploadBytes.size(); i += 4) {
                uploadBytes[i + 0] = static_cast<uint8_t>(uploadBytes[i + 0] * bright);
                uploadBytes[i + 1] = static_cast<uint8_t>(uploadBytes[i + 1] * bright);
                uploadBytes[i + 2] = static_cast<uint8_t>(uploadBytes[i + 2] * bright);
            }
            Log(fmt::format("HelmetOverlay: applied brightness={:.2f} to texture\n", bright));
        }

        D3D11_TEXTURE2D_DESC td{};
        td.Width = texW;
        td.Height = texH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        // Match the swapchain's format family so CopyResource stays
        // a pure byte-level copy. SRGB and UNORM share the same
        // R8G8B8A8_TYPELESS family.
        td.Format = m_impl->d3dFormat;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = 0;
        td.MiscFlags = 0;

        D3D11_SUBRESOURCE_DATA sd{};
        sd.pSysMem = uploadBytes.data();
        sd.SysMemPitch = texW * 4u;
        sd.SysMemSlicePitch = 0;

        ComPtr<ID3D11Texture2D> staging;
        const HRESULT hr = m_impl->device->CreateTexture2D(&td, &sd, &staging);
        if (FAILED(hr)) {
            Log(fmt::format("HelmetOverlay: CreateTexture2D(staging) failed, hr=0x{:08X}\n",
                            static_cast<uint32_t>(hr)));
            return false;
        }

        // ---- One-shot fill: acquire → wait → copy → release ----------
        // Per spec, a STATIC_IMAGE swapchain image must be acquired
        // exactly once. We do it here and never again.
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t imageIndex = 0;
        if (XR_FAILED(m_impl->api->xrAcquireSwapchainImage(m_impl->swapchain, &ai, &imageIndex))) {
            Log("HelmetOverlay: one-shot xrAcquireSwapchainImage failed\n");
            return false;
        }
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(m_impl->api->xrWaitSwapchainImage(m_impl->swapchain, &wi))) {
            // Try to release what we acquired so the runtime doesn't
            // wedge on a phantom in-flight image.
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            m_impl->api->xrReleaseSwapchainImage(m_impl->swapchain, &ri);
            Log("HelmetOverlay: one-shot xrWaitSwapchainImage failed\n");
            return false;
        }
        m_impl->context->CopyResource(swapchainImage, staging.Get());
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (XR_FAILED(m_impl->api->xrReleaseSwapchainImage(m_impl->swapchain, &ri))) {
            Log("HelmetOverlay: one-shot xrReleaseSwapchainImage failed\n");
            return false;
        }
        // staging goes out of scope here → ComPtr releases the GPU
        // texture. We never need it again because the runtime now
        // owns the only copy we care about (the static swapchain image).

        // PNG aspect ratio recorded so live-edit can recompute the
        // quad height when the user changes width_m.
        m_impl->pngAspectRatio = static_cast<float>(pngHeight) / static_cast<float>(pngWidth);
        return true;
    }

    // --- Quad backend. Flat plane head-locked at distance_m. -----------
    bool HelmetOverlay::tryInitQuad(int pngWidth, int pngHeight) {
        const float quadW = m_impl->config.width_m;
        const float quadH = quadW * m_impl->pngAspectRatio;

        m_impl->quadLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        m_impl->quadLayer.next = nullptr;
        m_impl->quadLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                       XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        m_impl->quadLayer.space = m_impl->viewSpace;
        m_impl->quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        m_impl->quadLayer.subImage.swapchain = m_impl->swapchain;
        m_impl->quadLayer.subImage.imageRect.offset = {0, 0};
        m_impl->quadLayer.subImage.imageRect.extent = {pngWidth, pngHeight};
        m_impl->quadLayer.subImage.imageArrayIndex = 0;
        m_impl->quadLayer.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        m_impl->quadLayer.pose.position = {0.0f, 0.0f, -m_impl->config.distance_m};
        m_impl->quadLayer.size = {quadW, quadH};

        return true;
    }

    // --- Per-frame entry point. -----------------------------------------

    bool HelmetOverlay::appendLayer(XrTime /*displayTime*/,
                                    const XrCompositionLayerBaseHeader** outLayer) {
        if (!m_impl || !outLayer) return false;
        if (m_impl->mode == HelmetOverlayMode::Disabled) return false;
        if (m_impl->swapchain == XR_NULL_HANDLE) return false;

        // The static-image swapchain was filled once at init (see
        // createSwapchainFromPng). The runtime composites that single
        // image every frame on its own — we just hand back the layer
        // pointer. No acquire / wait / release / CopyResource here.
        *outLayer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_impl->quadLayer);
        return true;
    }

    void HelmetOverlay::updateLiveTunables(const HelmetOverlayConfig& newConfig) {
        if (!m_impl || m_impl->mode != HelmetOverlayMode::Quad) return;

        // Bail out cheaply if nothing changed (avoids spamming the log
        // when the file is touched but the helmet block is identical).
        const bool sameDistance =
            std::abs(m_impl->config.distance_m - newConfig.distance_m) < 1e-4f;
        const bool sameWidth =
            std::abs(m_impl->config.width_m - newConfig.width_m) < 1e-4f;
        if (sameDistance && sameWidth) return;

        m_impl->config.distance_m = newConfig.distance_m;
        m_impl->config.width_m    = newConfig.width_m;

        const float quadW = newConfig.width_m;
        const float quadH = quadW * m_impl->pngAspectRatio;
        m_impl->quadLayer.pose.position.z = -newConfig.distance_m;
        m_impl->quadLayer.size = {quadW, quadH};

        Log(fmt::format("HelmetOverlay: live-tuned distance={:.2f}m, size={:.2f}x{:.2f}m\n",
                        newConfig.distance_m, quadW, quadH));
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
