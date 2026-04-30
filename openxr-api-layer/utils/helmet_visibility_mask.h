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

// Builds a per-eye triangle mesh in NDC that covers the helmet's
// opaque silhouette so apps consuming XR_KHR_visibility_mask can
// stencil-reject those pixels and skip shading them. Decoupled from
// the rendering side (helmet_overlay.cpp) so the visibility-mask
// path can ship independently and be unit-tested without D3D11
// dependencies.
//
// v1 strategy: bbox-of-visor. Find the rectangular bounding box of
// transparent pixels (alpha < threshold) in the helmet PNG; the four
// strips around that bbox are the opaque foam region. Each strip is
// a flat rectangle in the quad's local plane; perspective-projected
// per eye, it stays roughly rectangular in NDC (no distortion since
// all corners share the same view-space depth, modulo eye orientation).
//
// Output meshes use OpenXR's XrVector2f (x, y in NDC, [-1, +1]) and
// uint32_t indices, ready to be appended to whatever the runtime
// returns from xrGetVisibilityMaskKHR.

#include <filesystem>
#include <vector>

#include <openxr/openxr.h>

#include "helmet_overlay.h"  // HelmetOverlayConfig

namespace openxr_api_layer {

    // A flat triangle mesh in NDC ([-1, +1] both axes). Pure data,
    // no GPU resources — ready to be copied into the
    // XrVisibilityMaskKHR buffers.
    struct VisibilityMaskMesh {
        std::vector<XrVector2f> vertices;
        std::vector<uint32_t> indices;  // triangle list
    };

    // Helmet contribution to xrGetVisibilityMaskKHR.
    //
    // Lifecycle:
    //   1. Construct.
    //   2. initialize(helmetsDir, config) — loads the PNG, detects the
    //      visor bbox in UV space. Independent of view geometry; runs
    //      once per session.
    //   3. rebuildForView(viewIndex, eyePoseInView, fov, config) — for
    //      each eye, builds the cached NDC mesh from the bbox + the
    //      eye's projection. Called by the layer the first time
    //      xrLocateViews has populated FOVs, and again whenever
    //      live-edit invalidates the cache.
    //   4. meshForView(viewIndex) — read-only access to the cached
    //      mesh; what xrGetVisibilityMaskKHR override appends to the
    //      runtime's reply.
    class HelmetVisibilityMask {
    public:
        HelmetVisibilityMask() = default;
        ~HelmetVisibilityMask() = default;

        HelmetVisibilityMask(const HelmetVisibilityMask&) = delete;
        HelmetVisibilityMask& operator=(const HelmetVisibilityMask&) = delete;

        // Returns true iff a valid bbox was found (PNG decoded OK and
        // contained at least one transparent pixel under
        // alphaThreshold). On false, no mask contribution will be
        // produced and the layer should fall back to pure pass-through.
        bool initialize(const std::filesystem::path& helmetsDir,
                        const HelmetOverlayConfig& config,
                        uint8_t alphaThreshold = 16);

        // Rebuilds the cached NDC mesh for one eye. eyePoseInView is
        // the eye's pose relative to the viewer's head — typically
        // obtained via xrLocateViews + a transform into VIEW space.
        // fov is the eye's asymmetric perspective frustum.
        // viewIndex grows the cache vector as needed (so callers can
        // push view 0 then view 1 without pre-sizing).
        void rebuildForView(uint32_t viewIndex,
                            const XrPosef& eyePoseInView,
                            const XrFovf& fov,
                            const HelmetOverlayConfig& config);

        // Returns an empty mesh if viewIndex is out of bounds or the
        // mask was never initialized — i.e. always safe to read.
        const VisibilityMaskMesh& meshForView(uint32_t viewIndex) const;

        // True iff initialize() succeeded and the bbox is non-empty.
        bool isInitialized() const { return m_initialized; }

        // Detected visor bbox in UV space ([0, 1] for both axes,
        // (0, 0) = top-left of the texture). Exposed for testing and
        // for diagnostic logs; callers should not need to consume it
        // directly.
        struct VisorBbox {
            float u0 = 0.0f, v0 = 0.0f;
            float u1 = 0.0f, v1 = 0.0f;
        };
        const VisorBbox& visorBbox() const { return m_bbox; }

    private:
        bool m_initialized = false;
        VisorBbox m_bbox;
        // Aspect ratio of the loaded PNG (height/width). Drives the
        // quad's physical height in tryInitQuad — captured here too
        // so the projection math knows the quad's vertical extent.
        float m_pngAspectRatio = 1.0f;
        std::vector<VisibilityMaskMesh> m_meshes;
        // Used when meshForView() is asked for an out-of-bounds index;
        // returning a const ref forces us to keep something valid alive.
        VisibilityMaskMesh m_emptyMesh;
    };

    // ---- Pure helpers exposed for unit tests. -------------------------

    // Find the axis-aligned bbox of pixels with alpha < threshold in
    // an RGBA8 buffer. Outputs UV-space corners ([0, 1] both axes).
    // Returns false if no pixel met the threshold (e.g. fully opaque
    // PNG → no visor cutout).
    bool detectVisorBbox(const uint8_t* rgba, int width, int height,
                         uint8_t alphaThreshold,
                         float& outU0, float& outV0,
                         float& outU1, float& outV1);

    // Project a view-space point through an eye's pose+FOV to NDC.
    // viewPoint is in the head-locked VIEW reference space. The eye
    // pose describes where the eye sits in that same space (position
    // for IPD; orientation for canted displays like Pimax Crystal).
    XrVector2f projectViewPointToNdc(const XrVector3f& viewPoint,
                                     const XrPosef& eyePoseInView,
                                     const XrFovf& fov);

} // namespace openxr_api_layer
