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

// Self-sufficient — no pch, no D3D, no DirectXMath. Same hygiene as
// crop_math.h: the standalone test binary must be able to compile and
// link this without dragging the layer DLL.
//
// stb_image is loaded as the PNG decoder. STB_IMAGE_IMPLEMENTATION
// would conflict with the one already in helmet_overlay.cpp, so we
// declare ONLY the prototype here and rely on helmet_overlay's TU
// providing the symbol at link time. (Both TUs link into the same
// DLL; the test binary also links helmet_overlay.cpp, so the
// implementation is reachable in both contexts.)

#include "helmet_visibility_mask.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// Forward-declare stb_image's two functions we need. STB_IMAGE_IMPLEMENTATION
// in helmet_overlay.cpp provides them. We avoid including <stb_image.h>
// here so we don't accidentally double-define the implementation.
extern "C" unsigned char* stbi_load(const char* filename,
                                    int* x, int* y,
                                    int* channels_in_file,
                                    int desired_channels);
extern "C" void stbi_image_free(void* retval_from_stbi_load);

namespace openxr_api_layer {

    namespace {

        // Identity bbox sentinel: returned by detectVisorBbox when no
        // transparent pixel is found.
        constexpr float kPi = 3.14159265358979323846f;

        // Rotates a 3D vector by a unit quaternion using the optimized
        // form  v' = v + 2 * qv × (qv × v + w * v)  where qv = (x, y, z).
        // Equivalent to the textbook q * v * q⁻¹ for unit q, but cheaper
        // (no full quaternion-quaternion multiply).
        inline XrVector3f rotateVectorByQuat(const XrVector3f& v,
                                             const XrQuaternionf& q) {
            const float qx = q.x, qy = q.y, qz = q.z, qw = q.w;
            // t = 2 * (qv × v)
            const float tx = 2.0f * (qy * v.z - qz * v.y);
            const float ty = 2.0f * (qz * v.x - qx * v.z);
            const float tz = 2.0f * (qx * v.y - qy * v.x);
            // v' = v + qw * t + qv × t
            return XrVector3f{
                v.x + qw * tx + (qy * tz - qz * ty),
                v.y + qw * ty + (qz * tx - qx * tz),
                v.z + qw * tz + (qx * ty - qy * tx),
            };
        }

        // Inverse rotation = rotation by the conjugate quaternion.
        inline XrVector3f rotateVectorByInverseQuat(const XrVector3f& v,
                                                    const XrQuaternionf& q) {
            const XrQuaternionf qInv{-q.x, -q.y, -q.z, q.w};
            return rotateVectorByQuat(v, qInv);
        }

        // Quad-local 2D corner → view-space 3D point. Quad-local (0, 0)
        // is the quad center; +x right, +y up. The quad sits at view-
        // space (0, vOffsetY, -distance_m) facing -Z.
        inline XrVector3f quadLocalToView(float xLocal, float yLocal,
                                          float vOffsetY, float distance) {
            return XrVector3f{xLocal, yLocal + vOffsetY, -distance};
        }

        // UV coordinate (PNG, top-down) → quad-local 2D.
        // u=0 → x=-quadW/2, u=1 → x=+quadW/2
        // v=0 → y=+quadH/2 (top), v=1 → y=-quadH/2 (bottom)
        inline void uvToQuadLocal(float u, float v,
                                  float quadW, float quadH,
                                  float& outX, float& outY) {
            outX = (u - 0.5f) * quadW;
            outY = (0.5f - v) * quadH;
        }

        // Push a single rectangle (4 UV corners in CCW order) as 2
        // triangles into the mesh. Eye projection is applied on the
        // fly: each UV corner → quad-local → view-space → NDC.
        void appendRectAsTriangles(VisibilityMaskMesh& mesh,
                                   float u0, float v0, float u1, float v1,
                                   float quadW, float quadH,
                                   float vOffsetY, float distance,
                                   const XrPosef& eyePoseInView,
                                   const XrFovf& fov) {
            // Reject degenerate (zero-area) rects so we don't emit
            // collinear triangles that the runtime might choke on.
            if (u0 >= u1 || v0 >= v1) return;

            const auto projectUv = [&](float u, float v) {
                float xL, yL;
                uvToQuadLocal(u, v, quadW, quadH, xL, yL);
                const XrVector3f viewPt = quadLocalToView(xL, yL, vOffsetY, distance);
                return projectViewPointToNdc(viewPt, eyePoseInView, fov);
            };

            const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
            // CCW: TL, BL, BR, TR
            mesh.vertices.push_back(projectUv(u0, v0));
            mesh.vertices.push_back(projectUv(u0, v1));
            mesh.vertices.push_back(projectUv(u1, v1));
            mesh.vertices.push_back(projectUv(u1, v0));

            // Two triangles: (TL, BL, BR) and (TL, BR, TR)
            mesh.indices.push_back(base + 0);
            mesh.indices.push_back(base + 1);
            mesh.indices.push_back(base + 2);
            mesh.indices.push_back(base + 0);
            mesh.indices.push_back(base + 2);
            mesh.indices.push_back(base + 3);
        }

    } // namespace

    // ----- Pure helpers (also exported for tests) ----------------------

    bool detectVisorBbox(const uint8_t* rgba, int width, int height,
                         uint8_t alphaThreshold,
                         float& outU0, float& outV0,
                         float& outU1, float& outV1) {
        if (!rgba || width <= 0 || height <= 0) return false;

        int minX = width, maxX = -1;
        int minY = height, maxY = -1;

        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const uint8_t a = rgba[(y * width + x) * 4 + 3];
                if (a < alphaThreshold) {
                    if (x < minX) minX = x;
                    if (x > maxX) maxX = x;
                    if (y < minY) minY = y;
                    if (y > maxY) maxY = y;
                }
            }
        }

        if (maxX < 0) return false;  // no pixel under threshold

        // +1 on max so the bbox is exclusive at the high end (pixel
        // [maxX, maxY] is included; conversion to UV puts its right
        // edge at (maxX+1)/width).
        outU0 = static_cast<float>(minX) / static_cast<float>(width);
        outV0 = static_cast<float>(minY) / static_cast<float>(height);
        outU1 = static_cast<float>(maxX + 1) / static_cast<float>(width);
        outV1 = static_cast<float>(maxY + 1) / static_cast<float>(height);
        return true;
    }

    XrVector2f projectViewPointToNdc(const XrVector3f& viewPoint,
                                     const XrPosef& eyePoseInView,
                                     const XrFovf& fov) {
        // 1. View-space → eye-space.
        // P_eye = inverseRot(P_view - eye.position)
        const XrVector3f relative{
            viewPoint.x - eyePoseInView.position.x,
            viewPoint.y - eyePoseInView.position.y,
            viewPoint.z - eyePoseInView.position.z,
        };
        const XrVector3f eyePoint = rotateVectorByInverseQuat(relative,
                                                              eyePoseInView.orientation);

        // 2. Eye-space → NDC via asymmetric perspective. The eye looks
        // down -Z, so the homogeneous divide is by -ze.
        // ndc.x = (2*xe/-ze - (tanL+tanR)) / (tanR - tanL)
        // ndc.y = (2*ye/-ze - (tanU+tanD)) / (tanU - tanD)
        if (eyePoint.z >= 0.0f) {
            // Behind the eye plane — clamp to the far edge of NDC so
            // the resulting triangle is degenerate-but-bounded rather
            // than producing inf/NaN. For our use case (helmet at
            // distance > 0) this branch shouldn't fire.
            return XrVector2f{0.0f, 0.0f};
        }

        const float invNegZ = 1.0f / -eyePoint.z;
        const float xTan = eyePoint.x * invNegZ;
        const float yTan = eyePoint.y * invNegZ;

        const float tanL = std::tan(fov.angleLeft);
        const float tanR = std::tan(fov.angleRight);
        const float tanU = std::tan(fov.angleUp);
        const float tanD = std::tan(fov.angleDown);

        XrVector2f ndc;
        ndc.x = (2.0f * xTan - (tanL + tanR)) / (tanR - tanL);
        ndc.y = (2.0f * yTan - (tanU + tanD)) / (tanU - tanD);
        return ndc;
    }

    // ----- HelmetVisibilityMask methods --------------------------------

    bool HelmetVisibilityMask::initialize(const std::filesystem::path& helmetsDir,
                                          const HelmetOverlayConfig& config,
                                          uint8_t alphaThreshold) {
        m_initialized = false;
        m_bbox = VisorBbox{};
        m_pngAspectRatio = 1.0f;
        m_meshes.clear();

        if (!config.enabled) return false;

        const std::filesystem::path pngPath = helmetsDir / config.imageRelativePath;
        if (!std::filesystem::exists(pngPath)) return false;

        int w = 0, h = 0, channels = 0;
        unsigned char* pixels = stbi_load(pngPath.string().c_str(), &w, &h, &channels, 4);
        if (!pixels) return false;

        // RAII free.
        struct PixGuard {
            unsigned char* p;
            ~PixGuard() { if (p) stbi_image_free(p); }
        } guard{pixels};

        if (w <= 0 || h <= 0) return false;

        m_pngAspectRatio = static_cast<float>(h) / static_cast<float>(w);

        if (!detectVisorBbox(pixels, w, h, alphaThreshold,
                             m_bbox.u0, m_bbox.v0, m_bbox.u1, m_bbox.v1)) {
            // No transparent pixels → no visor cutout to model. Mask
            // contribution would be the full quad, which is what the
            // helmet_overlay quad covers anyway — adding it would mean
            // stencil-rejecting the visor opening too. Bail out.
            return false;
        }

        m_initialized = true;
        return true;
    }

    void HelmetVisibilityMask::rebuildForView(uint32_t viewIndex,
                                              const XrPosef& eyePoseInView,
                                              const XrFovf& fov,
                                              const HelmetOverlayConfig& config) {
        if (!m_initialized) return;

        // Grow the cache to cover this index.
        if (viewIndex >= m_meshes.size()) {
            m_meshes.resize(viewIndex + 1u);
        }
        VisibilityMaskMesh& mesh = m_meshes[viewIndex];
        mesh.vertices.clear();
        mesh.indices.clear();

        // Quad geometry, derived the same way helmet_overlay.cpp does it.
        const float halfFov = config.horizontal_fov_deg * 0.5f * kPi / 180.0f;
        const float quadW = 2.0f * config.distance_m * std::tan(halfFov);
        const float quadH = quadW * m_pngAspectRatio;
        const float vOffsetY = config.distance_m
                             * std::tan(config.vertical_offset_deg * kPi / 180.0f);

        // Visor bbox in UV; clamp to [0, 1] just in case detection
        // produced something past the edges (it shouldn't, but cheap
        // safety).
        const float u0 = std::max(0.0f, std::min(1.0f, m_bbox.u0));
        const float u1 = std::max(0.0f, std::min(1.0f, m_bbox.u1));
        const float v0 = std::max(0.0f, std::min(1.0f, m_bbox.v0));
        const float v1 = std::max(0.0f, std::min(1.0f, m_bbox.v1));

        // Reserve enough room: 4 strips × 4 vertices = 16, × 6 indices = 24
        mesh.vertices.reserve(16);
        mesh.indices.reserve(24);

        // 4 opaque strips around the visor bbox, in UV space.
        // Top   : u ∈ [0, 1],  v ∈ [0,  v0]
        // Bottom: u ∈ [0, 1],  v ∈ [v1, 1 ]
        // Left  : u ∈ [0, u0], v ∈ [v0, v1]
        // Right : u ∈ [u1, 1], v ∈ [v0, v1]
        appendRectAsTriangles(mesh, 0.0f, 0.0f, 1.0f, v0,
                              quadW, quadH, vOffsetY, config.distance_m,
                              eyePoseInView, fov);
        appendRectAsTriangles(mesh, 0.0f, v1, 1.0f, 1.0f,
                              quadW, quadH, vOffsetY, config.distance_m,
                              eyePoseInView, fov);
        appendRectAsTriangles(mesh, 0.0f, v0, u0, v1,
                              quadW, quadH, vOffsetY, config.distance_m,
                              eyePoseInView, fov);
        appendRectAsTriangles(mesh, u1, v0, 1.0f, v1,
                              quadW, quadH, vOffsetY, config.distance_m,
                              eyePoseInView, fov);
    }

    const VisibilityMaskMesh& HelmetVisibilityMask::meshForView(uint32_t viewIndex) const {
        if (!m_initialized || viewIndex >= m_meshes.size()) {
            return m_emptyMesh;
        }
        return m_meshes[viewIndex];
    }

} // namespace openxr_api_layer
