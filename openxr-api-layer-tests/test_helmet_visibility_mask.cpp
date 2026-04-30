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

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include <utils/helmet_visibility_mask.h>

using openxr_api_layer::detectVisorBbox;
using openxr_api_layer::projectViewPointToNdc;

namespace {
    // Helper: build an opaque RGBA buffer of the given size, then set
    // a rectangle of pixels to alpha=0 (visor cutout). Returns the
    // raw byte buffer ready to feed into detectVisorBbox.
    std::vector<uint8_t> makeRgbaWithVisor(int w, int h,
                                           int x0, int y0,
                                           int x1, int y1,
                                           uint8_t visorAlpha = 0) {
        std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 4u, 0u);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t idx = (static_cast<size_t>(y) * w + x) * 4u;
                buf[idx + 0] = 0;
                buf[idx + 1] = 0;
                buf[idx + 2] = 0;
                if (x >= x0 && x <= x1 && y >= y0 && y <= y1) {
                    buf[idx + 3] = visorAlpha;
                } else {
                    buf[idx + 3] = 255;
                }
            }
        }
        return buf;
    }

    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kIdentityQuatW = 1.0f;

    XrQuaternionf identityQuat() { return XrQuaternionf{0.0f, 0.0f, 0.0f, kIdentityQuatW}; }

    // Yaw-only quaternion (rotation about the Y axis), useful for
    // simulating canted-display HMDs (Pimax Crystal eyes have a small
    // outward yaw). angle is in radians, sign positive = counter-
    // clockwise when looking down -Y.
    XrQuaternionf yawQuat(float radians) {
        return XrQuaternionf{0.0f, std::sin(radians * 0.5f), 0.0f, std::cos(radians * 0.5f)};
    }

    XrPosef makePose(const XrVector3f& pos, const XrQuaternionf& orient) {
        XrPosef p;
        p.position = pos;
        p.orientation = orient;
        return p;
    }

    // Symmetric FOV ±45° both axes — ergonomic for round-number sanity
    // checks (tan 45° = 1 on every edge).
    XrFovf fovSymmetric45() {
        return XrFovf{-kPi * 0.25f, kPi * 0.25f, kPi * 0.25f, -kPi * 0.25f};
    }
} // namespace

// ---------------------------------------------------------------------------
// detectVisorBbox
// ---------------------------------------------------------------------------

TEST_CASE("detectVisorBbox: all-opaque buffer returns false") {
    std::vector<uint8_t> buf(8 * 8 * 4, 255);
    float u0, v0, u1, v1;
    CHECK(!detectVisorBbox(buf.data(), 8, 8, /*threshold=*/16, u0, v0, u1, v1));
}

TEST_CASE("detectVisorBbox: null/zero-sized inputs return false") {
    float u0, v0, u1, v1;
    CHECK(!detectVisorBbox(nullptr, 8, 8, 16, u0, v0, u1, v1));

    std::vector<uint8_t> buf(8 * 8 * 4, 0);
    CHECK(!detectVisorBbox(buf.data(), 0, 8, 16, u0, v0, u1, v1));
    CHECK(!detectVisorBbox(buf.data(), 8, 0, 16, u0, v0, u1, v1));
    CHECK(!detectVisorBbox(buf.data(), -1, 8, 16, u0, v0, u1, v1));
}

TEST_CASE("detectVisorBbox: full-transparent buffer maps to (0,0,1,1)") {
    std::vector<uint8_t> buf(4 * 4 * 4, 0);
    float u0, v0, u1, v1;
    REQUIRE(detectVisorBbox(buf.data(), 4, 4, 16, u0, v0, u1, v1));
    CHECK(u0 == doctest::Approx(0.0f));
    CHECK(v0 == doctest::Approx(0.0f));
    CHECK(u1 == doctest::Approx(1.0f));
    CHECK(v1 == doctest::Approx(1.0f));
}

TEST_CASE("detectVisorBbox: single transparent pixel — bbox is one cell wide") {
    // 10×10 buffer, one transparent pixel at (5, 7). Expected bbox in
    // UV is the cell (5,7)→(6,8) — exclusive at the high end means
    // (0.5, 0.7, 0.6, 0.8).
    const auto buf = makeRgbaWithVisor(10, 10, 5, 7, 5, 7);
    float u0, v0, u1, v1;
    REQUIRE(detectVisorBbox(buf.data(), 10, 10, 16, u0, v0, u1, v1));
    CHECK(u0 == doctest::Approx(0.5f));
    CHECK(v0 == doctest::Approx(0.7f));
    CHECK(u1 == doctest::Approx(0.6f));
    CHECK(v1 == doctest::Approx(0.8f));
}

TEST_CASE("detectVisorBbox: rectangular visor in the middle of the image") {
    // 100×100, visor at (20, 30)→(60, 50) inclusive.
    const auto buf = makeRgbaWithVisor(100, 100, 20, 30, 60, 50);
    float u0, v0, u1, v1;
    REQUIRE(detectVisorBbox(buf.data(), 100, 100, 16, u0, v0, u1, v1));
    CHECK(u0 == doctest::Approx(0.20f));
    CHECK(v0 == doctest::Approx(0.30f));
    CHECK(u1 == doctest::Approx(0.61f));   // (60 + 1) / 100
    CHECK(v1 == doctest::Approx(0.51f));   // (50 + 1) / 100
}

TEST_CASE("detectVisorBbox: threshold is exclusive (alpha == threshold counts as opaque)") {
    // Set the visor's alpha to exactly the threshold. The function
    // checks `a < threshold`, so this should NOT trigger detection.
    const auto buf = makeRgbaWithVisor(10, 10, 3, 3, 5, 5, /*visorAlpha=*/16);
    float u0, v0, u1, v1;
    CHECK(!detectVisorBbox(buf.data(), 10, 10, /*threshold=*/16, u0, v0, u1, v1));

    // Drop the alpha just below the threshold — now it should detect.
    const auto buf2 = makeRgbaWithVisor(10, 10, 3, 3, 5, 5, /*visorAlpha=*/15);
    CHECK(detectVisorBbox(buf2.data(), 10, 10, /*threshold=*/16, u0, v0, u1, v1));
}

TEST_CASE("detectVisorBbox: scattered transparent pixels collapse to their AABB") {
    // 10×10, transparent at (1, 2) and (8, 7). Expected bbox is the
    // axis-aligned bounding box of both: (0.1, 0.2)→(0.9, 0.8).
    std::vector<uint8_t> buf(10 * 10 * 4, 0);
    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 10; ++x) {
            const size_t idx = (static_cast<size_t>(y) * 10 + x) * 4u;
            const bool transparent = (x == 1 && y == 2) || (x == 8 && y == 7);
            buf[idx + 3] = transparent ? 0 : 255;
        }
    }
    float u0, v0, u1, v1;
    REQUIRE(detectVisorBbox(buf.data(), 10, 10, 16, u0, v0, u1, v1));
    CHECK(u0 == doctest::Approx(0.1f));
    CHECK(v0 == doctest::Approx(0.2f));
    CHECK(u1 == doctest::Approx(0.9f));
    CHECK(v1 == doctest::Approx(0.8f));
}

// ---------------------------------------------------------------------------
// projectViewPointToNdc
// ---------------------------------------------------------------------------

TEST_CASE("projectViewPointToNdc: identity eye, center point lands at NDC origin") {
    const XrPosef eye = makePose({0.0f, 0.0f, 0.0f}, identityQuat());
    const XrFovf fov = fovSymmetric45();
    const auto ndc = projectViewPointToNdc({0.0f, 0.0f, -1.0f}, eye, fov);
    CHECK(ndc.x == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(ndc.y == doctest::Approx(0.0f).epsilon(1e-5));
}

TEST_CASE("projectViewPointToNdc: identity eye, FOV-edge points land at NDC ±1") {
    // Symmetric ±45° FOV: at z = -1, x = ±tan(45°) = ±1 maps to ±1
    // on the NDC x axis. Same for y.
    const XrPosef eye = makePose({0.0f, 0.0f, 0.0f}, identityQuat());
    const XrFovf fov = fovSymmetric45();

    SUBCASE("right edge") {
        const auto ndc = projectViewPointToNdc({1.0f, 0.0f, -1.0f}, eye, fov);
        CHECK(ndc.x == doctest::Approx(1.0f).epsilon(1e-5));
        CHECK(ndc.y == doctest::Approx(0.0f).epsilon(1e-5));
    }
    SUBCASE("left edge") {
        const auto ndc = projectViewPointToNdc({-1.0f, 0.0f, -1.0f}, eye, fov);
        CHECK(ndc.x == doctest::Approx(-1.0f).epsilon(1e-5));
        CHECK(ndc.y == doctest::Approx(0.0f).epsilon(1e-5));
    }
    SUBCASE("top edge") {
        const auto ndc = projectViewPointToNdc({0.0f, 1.0f, -1.0f}, eye, fov);
        CHECK(ndc.x == doctest::Approx(0.0f).epsilon(1e-5));
        CHECK(ndc.y == doctest::Approx(1.0f).epsilon(1e-5));
    }
    SUBCASE("bottom edge") {
        const auto ndc = projectViewPointToNdc({0.0f, -1.0f, -1.0f}, eye, fov);
        CHECK(ndc.x == doctest::Approx(0.0f).epsilon(1e-5));
        CHECK(ndc.y == doctest::Approx(-1.0f).epsilon(1e-5));
    }
}

TEST_CASE("projectViewPointToNdc: distance preserves angular position") {
    // Same direction at different distances should land at the same
    // NDC — perspective projection is invariant to depth along a ray.
    const XrPosef eye = makePose({0.0f, 0.0f, 0.0f}, identityQuat());
    const XrFovf fov = fovSymmetric45();
    const auto near = projectViewPointToNdc({0.5f, 0.0f, -1.0f}, eye, fov);
    const auto far  = projectViewPointToNdc({2.5f, 0.0f, -5.0f}, eye, fov);
    CHECK(near.x == doctest::Approx(far.x).epsilon(1e-5));
    CHECK(near.y == doctest::Approx(far.y).epsilon(1e-5));
}

TEST_CASE("projectViewPointToNdc: behind-eye point returns sentinel (0,0)") {
    // A point "behind" the eye plane (z >= 0) is not in the view
    // frustum — the implementation clamps to (0, 0) instead of
    // returning inf/NaN that downstream code would have to handle.
    const XrPosef eye = makePose({0.0f, 0.0f, 0.0f}, identityQuat());
    const XrFovf fov = fovSymmetric45();

    SUBCASE("behind") {
        const auto ndc = projectViewPointToNdc({1.0f, 1.0f, +1.0f}, eye, fov);
        CHECK(ndc.x == doctest::Approx(0.0f));
        CHECK(ndc.y == doctest::Approx(0.0f));
    }
    SUBCASE("on the plane") {
        const auto ndc = projectViewPointToNdc({1.0f, 1.0f, 0.0f}, eye, fov);
        CHECK(ndc.x == doctest::Approx(0.0f));
        CHECK(ndc.y == doctest::Approx(0.0f));
    }
}

TEST_CASE("projectViewPointToNdc: eye X offset shifts NDC stereoscopically") {
    // A point straight ahead of the head viewed by an eye offset by
    // +X (right eye, +32 mm IPD/2) should appear slightly to the
    // *left* of that eye's center, i.e. negative NDC x. This is the
    // basic stereo disparity that gives the brain depth perception.
    const XrPosef rightEye = makePose({0.032f, 0.0f, 0.0f}, identityQuat());
    const XrFovf fov = fovSymmetric45();
    const auto ndc = projectViewPointToNdc({0.0f, 0.0f, -1.0f}, rightEye, fov);
    // tan(angle) = -0.032, with symmetric ±45° FOV → ndc.x = -0.032
    CHECK(ndc.x == doctest::Approx(-0.032f).epsilon(1e-4));
    CHECK(ndc.y == doctest::Approx(0.0f).epsilon(1e-5));
}

TEST_CASE("projectViewPointToNdc: yaw-only eye rotation preserves on-axis points") {
    // An eye rotated by yaw (canted display) still projects a point
    // straight ahead of the *eye's local forward* to NDC center.
    // We pick a point on the rotated forward axis to exercise this.
    constexpr float yawRadians = 10.0f * kPi / 180.0f;
    const XrPosef yawedEye = makePose({0.0f, 0.0f, 0.0f}, yawQuat(yawRadians));
    const XrFovf fov = fovSymmetric45();

    // Eye looks along its rotated -Z. The standard yaw rotation
    // matrix around Y applied to the forward (0, 0, -1) yields
    // (-sin(yaw), 0, -cos(yaw)) — the X component is *negative* for
    // a positive yaw with this quaternion convention. A point at
    // distance 1 along that direction lands at NDC center.
    const float fx = -std::sin(yawRadians);
    const float fz = -std::cos(yawRadians);
    const auto ndc = projectViewPointToNdc({fx, 0.0f, fz}, yawedEye, fov);
    CHECK(ndc.x == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(ndc.y == doctest::Approx(0.0f).epsilon(1e-4));
}

TEST_CASE("projectViewPointToNdc: asymmetric FOV shifts NDC center off-axis") {
    // FOV with angleLeft = -30°, angleRight = +60° — the projection
    // center is not at the head-forward direction but at the angle
    // bisector ((tanL+tanR)/2 in the formula). A point straight
    // ahead at z=-1 should land at NDC x = (2*0 - (tanL+tanR)) /
    // (tanR - tanL).
    const float angleL = -30.0f * kPi / 180.0f;
    const float angleR = +60.0f * kPi / 180.0f;
    const XrFovf fov{angleL, angleR, kPi * 0.25f, -kPi * 0.25f};
    const XrPosef eye = makePose({0.0f, 0.0f, 0.0f}, identityQuat());

    const auto ndc = projectViewPointToNdc({0.0f, 0.0f, -1.0f}, eye, fov);
    const float tanL = std::tan(angleL);
    const float tanR = std::tan(angleR);
    const float expectedX = -(tanL + tanR) / (tanR - tanL);
    CHECK(ndc.x == doctest::Approx(expectedX).epsilon(1e-4));
}
