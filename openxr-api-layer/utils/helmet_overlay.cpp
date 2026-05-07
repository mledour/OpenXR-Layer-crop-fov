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
// eye; horizontal_fov_deg is the quad's apparent horizontal FOV in
// degrees (physical width = 2 × distance × tan(fov/2)); height
// follows the PNG aspect ratio so the image is never stretched.
//
// The swapchain is created with XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT
// — content is uploaded once at session start (acquire / wait /
// CopyResource(staging→swapchain) / release, then we drop the
// staging texture). The runtime composites that single image every
// frame on its own; appendLayers() just hands back the cached layer
// pointer(s) with no per-frame texture work, which is the entire
// point of the static-image flag.
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
// helmetsDir / config.imageRelativePath, the overlay does not arm.
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
        // Composition swapchain.
        //
        // Mono mode: a single STATIC_IMAGE_BIT swapchain — written once
        // in createSwapchainFromPng, composited every frame by the
        // runtime without our help. swapchainStandby stays NULL.
        //
        // Stereo SBS mode: two regular (non-static) swapchains so we
        // can re-bake the disparity texture when the user live-edits
        // distance_m or stereo_depth_amplitude_m. The active one is
        // referenced by both quads' subImage.swapchain; the standby
        // is the next destination of a re-bake. On rebake we acquire/
        // wait/copy/release the standby, swap pointers, and the next
        // xrEndFrame composites from the new texture without any
        // visible flash (the previously-active swapchain remains
        // valid — it just stops being referenced by any layer).
        XrSwapchain swapchain = XR_NULL_HANDLE;
        XrSwapchain swapchainStandby = XR_NULL_HANDLE;
        XrSpace viewSpace = XR_NULL_HANDLE;

        // Mono PNG pixels retained for live re-baking. Empty in mono
        // mode (no need — the texture is static). Owned by Impl
        // (std::vector); released at shutdown.
        std::vector<uint8_t> monoPixelsRgba8;
        int monoPngWidth = 0;
        int monoPngHeight = 0;

        // Helmets directory (e.g. %LOCALAPPDATA%\<layer>\helmets)
        // captured at initialize() time so live-edit of
        // config.imageRelativePath can resolve the new path against
        // the same root. Empty until initialize() runs successfully.
        std::filesystem::path helmetsDir;

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;

        // Composition layer(s) kept stable between appendLayers() calls
        // so the pointers we hand back to layer.cpp remain valid through
        // xrEndFrame.
        //
        // Mono mode: only quadLeft is used, with EYE_VISIBILITY_BOTH and
        // the full PNG as imageRect. numQuads = 1.
        //
        // Stereo SBS mode: both quads point at the same swapchain but
        // with split imageRect (left half / right half), each restricted
        // to its eye via eyeVisibility. numQuads = 2.
        XrCompositionLayerQuad quadLeft{};
        XrCompositionLayerQuad quadRight{};
        size_t numQuads = 0;

        // Per-eye PNG aspect ratio captured at init so live-edit can
        // recompute the quad height when the user changes
        // horizontal_fov_deg or distance_m, without re-decoding the PNG.
        // In mono mode this is pngHeight / pngWidth; in stereo SBS mode
        // it's pngHeight / (pngWidth/2) since the quad shows one half.
        float pngAspectRatio = 1.0f;  // height / per-eye-width

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

        // Hardcoded interpupillary distance, in meters, baked into the
        // stereo SBS disparity. Median adult value (Wikipedia: 63 mm
        // mean, 64 mm popular round number used by IPD calibration in
        // SteamVR / Meta). Users with significantly different IPD
        // (~58 mm or ~70 mm) will see a slightly off depth feel — the
        // proper fix is to query the runtime via xrLocateViews at
        // session init and bake per-user, which is a future
        // enhancement (needs a wait/locate dance during initialize()).
        constexpr float kStereoIpdMeters = 0.064f;

        // Cosine depth profile centered on u=0.5: 0 at the center,
        // grows to 1 at the edges. Equivalent to (1 − cos(2π(u−0.5)))/2,
        // expressed via sin²(π(u−0.5)) for one fewer trig call.
        inline float depthProfileCosine(float u) {
            constexpr float kPi = 3.14159265358979323846f;
            const float s = std::sin(kPi * (u - 0.5f));
            return s * s;
        }

        // Build a 2*pngW × pngH RGBA8 buffer where the left half is the
        // mono PNG with each column sampled at (x − shift) and the right
        // half at (x + shift). Shift is computed per-column from a
        // cosine depth profile + IPD + quad geometry, baking binocular
        // disparity into the asset so the user perceives the helmet as
        // wrapping toward the face at the edges. Round-to-nearest pixel
        // (no bilinear filtering) — sufficient for a feathered visor
        // edge; bilinear is a future enhancement if a hard-alpha asset
        // shows seams.
        //
        // The math (eye at ±IPD/2, screen at depth D, virtual point at
        // depth Z(u)):
        //   parallax(u) = IPD × (Z(u) − D) / Z(u)        [meters on screen]
        //   shift_norm  = parallax / (2 × quadW)          [normalized 0..1]
        //   shift_px    = round(shift_norm × pngW)        [mono pixels]
        // For Z < D (helmet edges closer than screen) shift > 0, so the
        // left-eye output sees the source content shifted *right* in
        // its own image (i.e. left[output_x] = mono[output_x − shift]),
        // which is the correct crossed disparity for a closer-than-
        // screen point. Symmetric for the right eye.
        void bakeStereoSbsBuffer(const uint8_t* mono,
                                 int pngW, int pngH,
                                 float depthAmpM,
                                 float distanceM,
                                 float quadWMeters,
                                 std::vector<uint8_t>& outBuffer) {
            const int swapW = 2 * pngW;
            outBuffer.assign(static_cast<size_t>(swapW) * pngH * 4u, 0);

            // Guard against pathological config (distance_m near zero
            // or quad width near zero would blow up the formula). The
            // parser already clamps amplitude to [0, 0.5] and FOV to
            // [10°, 270°], so quadW > 0 and D > 0 in practice; this is
            // belt-and-braces for live-tunable distance_m which could
            // be set to 0 by a hand-edited settings.json.
            if (distanceM < 0.01f || quadWMeters < 0.001f || pngW <= 0 || pngH <= 0) {
                return;
            }

            const float D = distanceM;
            const float invTwoQuadW = 1.0f / (2.0f * quadWMeters);
            const float pngWf = static_cast<float>(pngW);

            for (int y = 0; y < pngH; ++y) {
                const uint8_t* srcRow = mono + (static_cast<size_t>(y) * pngW) * 4u;
                uint8_t* dstRow = outBuffer.data() +
                                  (static_cast<size_t>(y) * swapW) * 4u;

                for (int x = 0; x < pngW; ++x) {
                    const float u = (x + 0.5f) / pngWf;
                    // Z floor of 1 cm: keeps the (D/Z − 1) term finite
                    // even if a future config lets Δ exceed D − 0.01.
                    const float Z = std::max(0.01f, D - depthAmpM * depthProfileCosine(u));
                    const float shiftNorm = kStereoIpdMeters * invTwoQuadW * (D / Z - 1.0f);
                    const int shift = static_cast<int>(std::lround(shiftNorm * pngWf));

                    const int leftSrc =
                        std::max(0, std::min(pngW - 1, x - shift));
                    const int rightSrc =
                        std::max(0, std::min(pngW - 1, x + shift));

                    std::memcpy(dstRow + static_cast<size_t>(x) * 4u,
                                srcRow + static_cast<size_t>(leftSrc) * 4u, 4);
                    std::memcpy(dstRow + (static_cast<size_t>(pngW) + x) * 4u,
                                srcRow + static_cast<size_t>(rightSrc) * 4u, 4);
                }
            }
        }

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
                                   const std::filesystem::path& helmetsDir) {
        m_impl->config = config;
        m_impl->api = api;
        m_impl->session = session;
        m_impl->helmetsDir = helmetsDir;

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
        const std::filesystem::path pngPath = helmetsDir / config.imageRelativePath;
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
        Log(fmt::format("HelmetOverlay: armed. distance={:.2f}m, size={:.2f}x{:.2f}m, "
                        "stereo_sbs={} ({} quad{})\n",
                        config.distance_m,
                        m_impl->quadLeft.size.width,
                        m_impl->quadLeft.size.height,
                        config.stereo_sbs ? "true" : "false",
                        m_impl->numQuads,
                        m_impl->numQuads == 1 ? "" : "s"));
        return true;
    }

    // --- Quad init. The PNG is mandatory: swapchain + staging are
    // sized to it, and the quad height in meters is derived from the
    // PNG aspect ratio so the image is never stretched — the user
    // controls horizontal_fov_deg + distance_m, both width and height
    // follow automatically.

    // ---- Per-swapchain helpers (declared in helmet_overlay.h) -------

    bool HelmetOverlay::createBackingSwapchain(XrSwapchain* outSwapchain,
                                               uint32_t width, uint32_t height,
                                               bool staticImage) {
        XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sci.createFlags = staticImage ? XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT : 0;
        sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                         XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sci.format = m_impl->swapchainFormat;
        sci.sampleCount = 1;
        sci.width = width;
        sci.height = height;
        sci.faceCount = 1;
        sci.arraySize = 1;
        sci.mipCount = 1;
        if (XR_FAILED(m_impl->api->xrCreateSwapchain(m_impl->session, &sci, outSwapchain))) {
            Log("HelmetOverlay: xrCreateSwapchain failed\n");
            return false;
        }
        return true;
    }

    bool HelmetOverlay::uploadBufferToSwapchain(XrSwapchain swapchain,
                                                uint32_t width, uint32_t height,
                                                const unsigned char* bytes) {
        if (!bytes || swapchain == XR_NULL_HANDLE) return false;

        // Enumerate the swapchain images. STATIC_IMAGE_BIT swapchains
        // have imageCount == 1; non-static may have >1, but we always
        // write to the image whose index xrAcquireSwapchainImage hands
        // us.
        uint32_t imageCount = 0;
        if (XR_FAILED(m_impl->api->xrEnumerateSwapchainImages(swapchain, 0, &imageCount, nullptr)) ||
            imageCount == 0) {
            Log("HelmetOverlay: xrEnumerateSwapchainImages returned 0\n");
            return false;
        }
        std::vector<XrSwapchainImageD3D11KHR> images(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if (XR_FAILED(m_impl->api->xrEnumerateSwapchainImages(
                swapchain, imageCount, &imageCount,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())))) {
            Log("HelmetOverlay: xrEnumerateSwapchainImages (second call) failed\n");
            return false;
        }

        // Acquire / wait. On any failure here we release best-effort
        // before returning, so the runtime doesn't end up with a
        // phantom in-flight image.
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t imageIndex = 0;
        if (XR_FAILED(m_impl->api->xrAcquireSwapchainImage(swapchain, &ai, &imageIndex))) {
            Log("HelmetOverlay: xrAcquireSwapchainImage failed\n");
            return false;
        }
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(m_impl->api->xrWaitSwapchainImage(swapchain, &wi))) {
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            m_impl->api->xrReleaseSwapchainImage(swapchain, &ri);
            Log("HelmetOverlay: xrWaitSwapchainImage failed\n");
            return false;
        }

        // Build a transient staging texture from the CPU buffer and
        // CopyResource into the swapchain image. Staging is destroyed
        // when the ComPtr goes out of scope below — the runtime owns
        // the only copy that matters (the swapchain image).
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = m_impl->d3dFormat;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA sd{};
        sd.pSysMem = bytes;
        sd.SysMemPitch = width * 4u;

        ComPtr<ID3D11Texture2D> staging;
        const HRESULT hr = m_impl->device->CreateTexture2D(&td, &sd, &staging);
        if (FAILED(hr)) {
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            m_impl->api->xrReleaseSwapchainImage(swapchain, &ri);
            Log(fmt::format("HelmetOverlay: CreateTexture2D(staging) failed, hr=0x{:08X}\n",
                            static_cast<uint32_t>(hr)));
            return false;
        }

        m_impl->context->CopyResource(images[imageIndex].texture, staging.Get());

        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (XR_FAILED(m_impl->api->xrReleaseSwapchainImage(swapchain, &ri))) {
            Log("HelmetOverlay: xrReleaseSwapchainImage failed\n");
            return false;
        }
        return true;
    }

    void HelmetOverlay::applyBrightness(std::vector<unsigned char>& buffer) {
        const float bright = m_impl->config.brightness;
        if (bright >= 0.999f) return;
        for (size_t i = 0; i + 3 < buffer.size(); i += 4) {
            buffer[i + 0] = static_cast<unsigned char>(buffer[i + 0] * bright);
            buffer[i + 1] = static_cast<unsigned char>(buffer[i + 1] * bright);
            buffer[i + 2] = static_cast<unsigned char>(buffer[i + 2] * bright);
        }
    }

    void HelmetOverlay::buildStereoUploadBuffer(float distanceM, float depthAmpM,
                                                std::vector<unsigned char>& outBuffer) {
        constexpr float kPi = 3.14159265358979323846f;
        const float halfFov = m_impl->config.horizontal_fov_deg * 0.5f * kPi / 180.0f;
        const float quadWMeters = 2.0f * distanceM * std::tan(halfFov);
        bakeStereoSbsBuffer(m_impl->monoPixelsRgba8.data(),
                            m_impl->monoPngWidth, m_impl->monoPngHeight,
                            depthAmpM, distanceM, quadWMeters,
                            outBuffer);
        applyBrightness(outBuffer);
    }

    bool HelmetOverlay::uploadStereoToSwapchain(XrSwapchain swapchain,
                                                float distanceM, float depthAmpM) {
        std::vector<unsigned char> bytes;
        buildStereoUploadBuffer(distanceM, depthAmpM, bytes);
        return uploadBufferToSwapchain(swapchain,
                                       static_cast<uint32_t>(2 * m_impl->monoPngWidth),
                                       static_cast<uint32_t>(m_impl->monoPngHeight),
                                       bytes.data());
    }

    bool HelmetOverlay::rebakeStereoTexture(float newDistanceM, float newDepthAmpM) {
        if (m_impl->swapchainStandby == XR_NULL_HANDLE) {
            Log("HelmetOverlay: rebakeStereoTexture called but standby swapchain is null "
                "(stereo_sbs not active or init failed)\n");
            return false;
        }
        if (m_impl->monoPixelsRgba8.empty()) {
            Log("HelmetOverlay: rebakeStereoTexture called but mono pixels are not retained\n");
            return false;
        }

        // Bake into the standby swapchain. On failure we leave the
        // active swapchain untouched — the user keeps seeing the
        // previous (correct) texture rather than a partial / black one.
        if (!uploadStereoToSwapchain(m_impl->swapchainStandby, newDistanceM, newDepthAmpM)) {
            Log("HelmetOverlay: rebake upload failed; keeping previous active swapchain\n");
            return false;
        }

        // Atomic swap: standby is now the new active. Both quads start
        // referencing the new active swapchain on the next xrEndFrame —
        // since appendLayers() returns the existing quad pointers and
        // those quads' subImage.swapchain is updated here in-place.
        std::swap(m_impl->swapchain, m_impl->swapchainStandby);
        m_impl->quadLeft.subImage.swapchain = m_impl->swapchain;
        if (m_impl->numQuads >= 2) {
            m_impl->quadRight.subImage.swapchain = m_impl->swapchain;
        }

        constexpr float kPi = 3.14159265358979323846f;
        const float halfFov = m_impl->config.horizontal_fov_deg * 0.5f * kPi / 180.0f;
        const float quadWMeters = 2.0f * newDistanceM * std::tan(halfFov);
        Log(fmt::format("HelmetOverlay: rebaked stereo SBS (amp={:.3f} m, "
                        "distance={:.2f} m, quadW={:.2f} m)\n",
                        newDepthAmpM, newDistanceM, quadWMeters));
        return true;
    }

    // ---- One-shot init driver. Owns the high-level branching:
    //   * Mono mode: single STATIC_IMAGE_BIT swapchain, runtime can
    //     optimize it as a never-changes-after-init asset; the layer
    //     never re-acquires.
    //   * Stereo SBS mode: two regular (non-static) swapchains so live-
    //     edit of distance_m / stereo_depth_amplitude_m can re-bake
    //     into the standby and atomically swap. Initial bake fills
    //     both swapchains with the same texture so the standby is
    //     already in a known-good state if the first re-bake fails.
    bool HelmetOverlay::createSwapchainFromPng(const uint8_t* pngPixels, int pngWidth, int pngHeight) {
        if (!pngPixels || pngWidth <= 0 || pngHeight <= 0) {
            Log("HelmetOverlay: createSwapchainFromPng called with invalid PNG\n");
            return false;
        }

        const bool sbs = m_impl->config.stereo_sbs;
        const uint32_t texW = static_cast<uint32_t>(sbs ? 2 * pngWidth : pngWidth);
        const uint32_t texH = static_cast<uint32_t>(pngHeight);

        // Stereo path retains a copy of the mono pixels for live re-bake.
        // Mono path doesn't need them — the texture is permanent.
        if (sbs) {
            const size_t monoBytes = static_cast<size_t>(pngWidth) * pngHeight * 4u;
            m_impl->monoPixelsRgba8.assign(pngPixels, pngPixels + monoBytes);
            m_impl->monoPngWidth = pngWidth;
            m_impl->monoPngHeight = pngHeight;
        }

        // Active swapchain: STATIC_IMAGE for mono, regular for stereo.
        if (!createBackingSwapchain(&m_impl->swapchain, texW, texH, /*staticImage=*/!sbs)) {
            return false;
        }
        // Standby swapchain (stereo only). Always non-static — its whole
        // job is to be re-acquired on each live-edit.
        if (sbs) {
            if (!createBackingSwapchain(&m_impl->swapchainStandby, texW, texH,
                                        /*staticImage=*/false)) {
                return false;
            }
        }

        // Build the upload buffer once, share it between active and
        // standby (stereo) or use it once (mono). Brightness is applied
        // here so the loaded mono pixels in monoPixelsRgba8 stay
        // unmodified — re-bakes use the original brightness=1 source
        // and re-apply the (potentially new) brightness multiplier.
        std::vector<unsigned char> uploadBytes;
        if (sbs) {
            buildStereoUploadBuffer(m_impl->config.distance_m,
                                    m_impl->config.stereo_depth_amplitude_m,
                                    uploadBytes);
            constexpr float kPi = 3.14159265358979323846f;
            const float halfFov = m_impl->config.horizontal_fov_deg * 0.5f * kPi / 180.0f;
            const float quadWMeters = 2.0f * m_impl->config.distance_m * std::tan(halfFov);
            Log(fmt::format("HelmetOverlay: baked stereo SBS texture "
                            "(amp={:.3f} m, IPD=64 mm, quadW={:.2f} m)\n",
                            m_impl->config.stereo_depth_amplitude_m, quadWMeters));
        } else {
            const size_t monoBytes = static_cast<size_t>(pngWidth) * pngHeight * 4u;
            uploadBytes.resize(monoBytes);
            std::memcpy(uploadBytes.data(), pngPixels, monoBytes);
            applyBrightness(uploadBytes);
        }

        // Upload to active swapchain.
        if (!uploadBufferToSwapchain(m_impl->swapchain, texW, texH, uploadBytes.data())) {
            Log("HelmetOverlay: initial upload to active swapchain failed\n");
            return false;
        }
        // Stereo: also seed standby with the same initial bake. Means
        // the very first live re-bake fails-safe to a correct image
        // even if the rebake itself errors out partway.
        if (sbs) {
            if (!uploadBufferToSwapchain(m_impl->swapchainStandby, texW, texH, uploadBytes.data())) {
                Log("HelmetOverlay: initial upload to standby swapchain failed; "
                    "live re-bake will not work this session\n");
                // Don't fail init — the active swapchain is fine, the
                // user just won't be able to live-tune until next run.
                if (m_impl->swapchainStandby != XR_NULL_HANDLE) {
                    m_impl->api->xrDestroySwapchain(m_impl->swapchainStandby);
                    m_impl->swapchainStandby = XR_NULL_HANDLE;
                }
            }
        }

        // pngAspectRatio is set in tryInitQuad() because it depends on
        // whether the PNG is interpreted as mono or as side-by-side
        // stereo (per-eye width = full vs half).
        return true;
    }

    // --- Quad backend. Flat plane head-locked at distance_m. -----------
    // Physical quad width is derived from the angular FOV the user
    // wants and the distance: quadW = 2 * distance_m * tan(fov/2).
    // That decouples coverage (set by horizontal_fov_deg) from depth
    // feel (set by distance_m).
    //
    // In stereo_sbs mode the (mono) PNG was expanded into a 2W × H SBS
    // swapchain by createSwapchainFromPng with per-eye disparity baked
    // in. We submit two quads pointing at the same swapchain with a
    // split imageRect and per-eye visibility. Single texture, single
    // upload, two layer pointers per frame — see
    // HelmetOverlay::appendLayers for the per-frame consumer side. The
    // per-eye imageRect width is the *mono* pngWidth (the input dim);
    // the swapchain is twice that.
    bool HelmetOverlay::tryInitQuad(int pngWidth, int pngHeight) {
        constexpr float kPi = 3.14159265358979323846f;

        const bool sbs = m_impl->config.stereo_sbs;

        // Aspect ratio is per-eye: pngHeight / pngWidth in BOTH modes,
        // because pngWidth is the mono input width and the per-eye
        // image always has those dims (the stereo path expanded the
        // swapchain horizontally, not the per-eye view).
        m_impl->pngAspectRatio = static_cast<float>(pngHeight) /
                                 static_cast<float>(pngWidth);

        const float halfFov = m_impl->config.horizontal_fov_deg * 0.5f * kPi / 180.0f;
        const float quadW = 2.0f * m_impl->config.distance_m * std::tan(halfFov);
        const float quadH = quadW * m_impl->pngAspectRatio;
        const float vOffsetY =
            m_impl->config.distance_m *
            std::tan(m_impl->config.vertical_offset_deg * kPi / 180.0f);

        // Shared layer fields. Both quads in SBS mode use the same pose,
        // same size, same swapchain, same blend flags — only
        // eyeVisibility and imageRect.offset differ.
        const auto fillCommon = [&](XrCompositionLayerQuad& q) {
            q.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            q.next = nullptr;
            q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                           XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
            q.space = m_impl->viewSpace;
            q.subImage.swapchain = m_impl->swapchain;
            q.subImage.imageRect.extent = {pngWidth, pngHeight};
            q.subImage.imageArrayIndex = 0;
            q.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
            q.pose.position = {0.0f, vOffsetY, -m_impl->config.distance_m};
            q.size = {quadW, quadH};
        };

        fillCommon(m_impl->quadLeft);
        if (sbs) {
            m_impl->quadLeft.eyeVisibility = XR_EYE_VISIBILITY_LEFT;
            m_impl->quadLeft.subImage.imageRect.offset = {0, 0};

            fillCommon(m_impl->quadRight);
            m_impl->quadRight.eyeVisibility = XR_EYE_VISIBILITY_RIGHT;
            m_impl->quadRight.subImage.imageRect.offset = {pngWidth, 0};

            m_impl->numQuads = 2;
        } else {
            m_impl->quadLeft.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            m_impl->quadLeft.subImage.imageRect.offset = {0, 0};
            m_impl->numQuads = 1;
        }

        return true;
    }

    // --- Per-frame entry point. -----------------------------------------

    size_t HelmetOverlay::appendLayers(XrTime /*displayTime*/,
                                       const XrCompositionLayerBaseHeader** outLayers) {
        if (!m_impl || !outLayers) return 0;
        if (m_impl->mode == HelmetOverlayMode::Disabled) return 0;
        if (m_impl->swapchain == XR_NULL_HANDLE) return 0;
        if (m_impl->numQuads == 0) return 0;

        // The static-image swapchain was filled once at init (see
        // createSwapchainFromPng). The runtime composites that single
        // image every frame on its own — we just hand back the layer
        // pointers. No acquire / wait / release / CopyResource here.
        outLayers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_impl->quadLeft);
        if (m_impl->numQuads >= 2) {
            outLayers[1] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&m_impl->quadRight);
        }
        return m_impl->numQuads;
    }

    void HelmetOverlay::updateLiveTunables(const HelmetOverlayConfig& newConfig) {
        if (!m_impl || m_impl->mode != HelmetOverlayMode::Quad) return;

        // Bail out cheaply if nothing changed (avoids spamming the log
        // when the file is touched but the helmet block is identical).
        const bool sameDistance =
            std::abs(m_impl->config.distance_m - newConfig.distance_m) < 1e-4f;
        const bool sameFov =
            std::abs(m_impl->config.horizontal_fov_deg - newConfig.horizontal_fov_deg) < 1e-3f;
        const bool sameVOffset =
            std::abs(m_impl->config.vertical_offset_deg - newConfig.vertical_offset_deg) < 1e-3f;
        const bool sameDepthAmp =
            std::abs(m_impl->config.stereo_depth_amplitude_m - newConfig.stereo_depth_amplitude_m) < 1e-4f;
        const bool sameBrightness =
            std::abs(m_impl->config.brightness - newConfig.brightness) < 1e-3f;
        const bool sameImage =
            (m_impl->config.imageRelativePath == newConfig.imageRelativePath);
        if (sameDistance && sameFov && sameVOffset && sameDepthAmp &&
            sameBrightness && sameImage) return;

        // ---- PNG path live-edit (stereo mode only). -----------------
        // Mono mode uses STATIC_IMAGE_BIT — the swapchain is locked
        // after init. Stereo mode keeps the mono pixels around and
        // can re-bake into the standby swapchain, but only if the
        // new PNG matches the existing dimensions (different dims
        // would require destroying and recreating both swapchains,
        // which is not implemented here).
        bool reloadedPng = false;
        if (!sameImage) {
            if (!m_impl->config.stereo_sbs) {
                Log(fmt::format("HelmetOverlay: image '{}' → '{}' requires a session "
                                "restart in mono mode (STATIC_IMAGE swapchain is "
                                "locked after init)\n",
                                m_impl->config.imageRelativePath,
                                newConfig.imageRelativePath));
            } else {
                const std::filesystem::path pngPath =
                    m_impl->helmetsDir / newConfig.imageRelativePath;
                if (!std::filesystem::exists(pngPath)) {
                    Log(fmt::format("HelmetOverlay: image '{}' not found at '{}', "
                                    "keeping previous\n",
                                    newConfig.imageRelativePath, pngPath.string()));
                } else {
                    uint8_t* newPixels = nullptr;
                    int newW = 0, newH = 0;
                    if (loadPngRgba8(pngPath, &newPixels, &newW, &newH)) {
                        if (newW != m_impl->monoPngWidth ||
                            newH != m_impl->monoPngHeight) {
                            Log(fmt::format("HelmetOverlay: image '{}' has different "
                                            "dimensions ({}x{}) than current ({}x{}); "
                                            "live-swap of different-sized PNGs is not "
                                            "supported, restart session to apply\n",
                                            newConfig.imageRelativePath, newW, newH,
                                            m_impl->monoPngWidth, m_impl->monoPngHeight));
                            stbi_image_free(newPixels);
                        } else {
                            const size_t newBytes =
                                static_cast<size_t>(newW) * newH * 4u;
                            m_impl->monoPixelsRgba8.assign(newPixels,
                                                           newPixels + newBytes);
                            stbi_image_free(newPixels);
                            m_impl->config.imageRelativePath =
                                newConfig.imageRelativePath;
                            reloadedPng = true;
                            Log(fmt::format("HelmetOverlay: live-loaded image '{}' "
                                            "({}x{})\n",
                                            newConfig.imageRelativePath, newW, newH));
                        }
                    }
                    // loadPngRgba8 logs its own failure reason on error
                }
            }
        }

        // ---- Brightness live-tunable (stereo mode only). -------------
        // Bakes into the disparity texture via applyBrightness during
        // re-bake. Mono mode can't apply brightness changes live — the
        // texture is STATIC_IMAGE — but we still update the config so
        // subsequent reloads don't trigger the warning twice.
        if (!sameBrightness && !m_impl->config.stereo_sbs) {
            Log(fmt::format("HelmetOverlay: brightness {} → {} requires a session "
                            "restart in mono mode (STATIC_IMAGE swapchain is "
                            "locked after init)\n",
                            m_impl->config.brightness, newConfig.brightness));
        }

        m_impl->config.distance_m                = newConfig.distance_m;
        m_impl->config.horizontal_fov_deg        = newConfig.horizontal_fov_deg;
        m_impl->config.vertical_offset_deg       = newConfig.vertical_offset_deg;
        m_impl->config.stereo_depth_amplitude_m  = newConfig.stereo_depth_amplitude_m;
        m_impl->config.brightness                = newConfig.brightness;

        constexpr float kPi = 3.14159265358979323846f;
        const float halfFov = newConfig.horizontal_fov_deg * 0.5f * kPi / 180.0f;
        const float quadW = 2.0f * newConfig.distance_m * std::tan(halfFov);
        const float quadH = quadW * m_impl->pngAspectRatio;
        const float vOffsetY = newConfig.distance_m *
                               std::tan(newConfig.vertical_offset_deg * kPi / 180.0f);

        // In SBS mode both quads share pose & size — only eyeVisibility
        // and imageRect.offset differ between them, and those are not
        // live-tunable.
        const XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f},
                           {0.0f, vOffsetY, -newConfig.distance_m}};
        const XrExtent2Df size{quadW, quadH};
        m_impl->quadLeft.pose = pose;
        m_impl->quadLeft.size = size;
        if (m_impl->numQuads >= 2) {
            m_impl->quadRight.pose = pose;
            m_impl->quadRight.size = size;
        }

        Log(fmt::format("HelmetOverlay: live-tuned distance={:.2f}m, fov={:.0f}°, "
                        "v_offset={:+.1f}°, size={:.2f}x{:.2f}m\n",
                        newConfig.distance_m, newConfig.horizontal_fov_deg,
                        newConfig.vertical_offset_deg, quadW, quadH));

        // Stereo SBS: re-bake the texture if any input to the bake
        // changed:
        //   - distance_m (in the disparity formula AND quadW)
        //   - horizontal_fov_deg (in quadW)
        //   - stereo_depth_amplitude_m (in Z(u))
        //   - brightness (applied to the bake buffer)
        //   - the source PNG itself (reloadedPng)
        // The rebake always reads the *current* m_impl->config and
        // m_impl->monoPixelsRgba8, both already updated above, so a
        // single rebake call covers any combination of changes.
        if (m_impl->config.stereo_sbs &&
            (!sameDistance || !sameFov || !sameDepthAmp ||
             !sameBrightness || reloadedPng)) {
            rebakeStereoTexture(newConfig.distance_m,
                                newConfig.stereo_depth_amplitude_m);
        }
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
            if (m_impl->swapchainStandby != XR_NULL_HANDLE) {
                m_impl->api->xrDestroySwapchain(m_impl->swapchainStandby);
                m_impl->swapchainStandby = XR_NULL_HANDLE;
            }
        }
        // Drop the retained mono pixels (~10 MB on a 2K helmet PNG)
        // — only relevant for stereo SBS sessions.
        m_impl->monoPixelsRgba8.clear();
        m_impl->monoPixelsRgba8.shrink_to_fit();
        m_impl->monoPngWidth = 0;
        m_impl->monoPngHeight = 0;
        m_impl->helmetsDir.clear();
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
