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

#include <utils/helmet_config_parser.h>

using openxr_api_layer::HelmetOverlayConfig;
using openxr_api_layer::parseHelmetConfig;

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

TEST_CASE("parseHelmetConfig: empty JSON object returns disabled defaults") {
    const auto hc = parseHelmetConfig("{}");
    CHECK(hc.enabled == false);
    CHECK(hc.imageRelativePath == "helmet_visor.png");
    CHECK(hc.distance_m == doctest::Approx(0.5f));
    CHECK(hc.horizontal_fov_deg == doctest::Approx(130.0f));
    CHECK(hc.vertical_offset_deg == doctest::Approx(0.0f));
    CHECK(hc.brightness == doctest::Approx(1.0f));
}

TEST_CASE("parseHelmetConfig: missing helmet_overlay block returns defaults") {
    // Top-level "enabled" belongs to the crop layer, not us — we should
    // not pick it up by accident.
    const auto hc = parseHelmetConfig(R"({"enabled": true, "crop_top_percent": 30})");
    CHECK(hc.enabled == false);
    CHECK(hc.distance_m == doctest::Approx(0.5f));
}

TEST_CASE("parseHelmetConfig: empty helmet_overlay object returns defaults") {
    const auto hc = parseHelmetConfig(R"({"helmet_overlay": {}})");
    CHECK(hc.enabled == false);
    CHECK(hc.brightness == doctest::Approx(1.0f));
}

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

TEST_CASE("parseHelmetConfig: full config is parsed verbatim within clamps") {
    const auto hc = parseHelmetConfig(R"({
        "helmet_overlay": {
            "enabled": true,
            "image": "stilo_full_face.png",
            "distance_m": 0.30,
            "horizontal_fov_deg": 160,
            "vertical_offset_deg": 5.0,
            "brightness": 0.5
        }
    })");
    CHECK(hc.enabled == true);
    CHECK(hc.imageRelativePath == "stilo_full_face.png");
    CHECK(hc.distance_m == doctest::Approx(0.30f));
    CHECK(hc.horizontal_fov_deg == doctest::Approx(160.0f));
    CHECK(hc.vertical_offset_deg == doctest::Approx(5.0f));
    CHECK(hc.brightness == doctest::Approx(0.5f));
}

TEST_CASE("parseHelmetConfig: integer values for float fields are accepted") {
    // Settings written by hand commonly use bare integers (no trailing .0)
    // for whole values like 130. rapidjson reports those as IsInt(), which
    // our IsInt|IsUint|IsFloat|IsDouble check covers.
    const auto hc = parseHelmetConfig(R"({
        "helmet_overlay": {
            "distance_m": 1,
            "horizontal_fov_deg": 90,
            "vertical_offset_deg": 0,
            "brightness": 1
        }
    })");
    CHECK(hc.distance_m == doctest::Approx(1.0f));
    CHECK(hc.horizontal_fov_deg == doctest::Approx(90.0f));
    CHECK(hc.vertical_offset_deg == doctest::Approx(0.0f));
    CHECK(hc.brightness == doctest::Approx(1.0f));
}

// ---------------------------------------------------------------------------
// Clamps — preserves the orthogonal-tuning contract by refusing degenerate
// values that would make the overlay invisible or break the math.
// ---------------------------------------------------------------------------

TEST_CASE("parseHelmetConfig: brightness clamps to [0.0, 1.0]") {
    SUBCASE("negative brightness clamps up to 0.0") {
        const auto hc = parseHelmetConfig(R"({"helmet_overlay": {"brightness": -0.5}})");
        CHECK(hc.brightness == doctest::Approx(0.0f));
    }
    SUBCASE("brightness above 1.0 clamps down") {
        const auto hc = parseHelmetConfig(R"({"helmet_overlay": {"brightness": 2.5}})");
        CHECK(hc.brightness == doctest::Approx(1.0f));
    }
    SUBCASE("boundary values are preserved exactly") {
        CHECK(parseHelmetConfig(R"({"helmet_overlay": {"brightness": 0.0}})").brightness
              == doctest::Approx(0.0f));
        CHECK(parseHelmetConfig(R"({"helmet_overlay": {"brightness": 1.0}})").brightness
              == doctest::Approx(1.0f));
    }
}

TEST_CASE("parseHelmetConfig: horizontal_fov_deg clamps to [10, 270]") {
    SUBCASE("below 10 clamps up") {
        CHECK(parseHelmetConfig(R"({"helmet_overlay": {"horizontal_fov_deg": 5}})")
                  .horizontal_fov_deg == doctest::Approx(10.0f));
    }
    SUBCASE("above 270 clamps down") {
        CHECK(parseHelmetConfig(R"({"helmet_overlay": {"horizontal_fov_deg": 350}})")
                  .horizontal_fov_deg == doctest::Approx(270.0f));
    }
    SUBCASE("zero clamps to 10 (avoids tan(0) degeneracy)") {
        CHECK(parseHelmetConfig(R"({"helmet_overlay": {"horizontal_fov_deg": 0}})")
                  .horizontal_fov_deg == doctest::Approx(10.0f));
    }
}

TEST_CASE("parseHelmetConfig: vertical_offset_deg clamps to [-30, +30]") {
    SUBCASE("below -30 clamps up") {
        CHECK(parseHelmetConfig(R"({"helmet_overlay": {"vertical_offset_deg": -50}})")
                  .vertical_offset_deg == doctest::Approx(-30.0f));
    }
    SUBCASE("above +30 clamps down") {
        CHECK(parseHelmetConfig(R"({"helmet_overlay": {"vertical_offset_deg": 50}})")
                  .vertical_offset_deg == doctest::Approx(30.0f));
    }
    SUBCASE("zero is preserved") {
        CHECK(parseHelmetConfig(R"({"helmet_overlay": {"vertical_offset_deg": 0}})")
                  .vertical_offset_deg == doctest::Approx(0.0f));
    }
}

// ---------------------------------------------------------------------------
// Robustness — caller is the user editing settings.json by hand. The parser
// must never throw or return garbage. Worst case = silently fall back to
// defaults.
// ---------------------------------------------------------------------------

TEST_CASE("parseHelmetConfig: malformed JSON returns defaults") {
    const auto hc = parseHelmetConfig("{ this is not json");
    CHECK(hc.enabled == false);
    CHECK(hc.brightness == doctest::Approx(1.0f));
}

TEST_CASE("parseHelmetConfig: completely empty input returns defaults") {
    const auto hc = parseHelmetConfig("");
    CHECK(hc.enabled == false);
}

TEST_CASE("parseHelmetConfig: non-object root returns defaults") {
    // E.g. the user accidentally wrapped the config in an array.
    const auto hc = parseHelmetConfig(R"([{"helmet_overlay": {"enabled": true}}])");
    CHECK(hc.enabled == false);
}

TEST_CASE("parseHelmetConfig: non-object helmet_overlay returns defaults") {
    const auto hc = parseHelmetConfig(R"({"helmet_overlay": "yes please"})");
    CHECK(hc.enabled == false);
}

TEST_CASE("parseHelmetConfig: wrong type on a field falls back per-field") {
    // The bad type on enabled / distance_m / horizontal_fov_deg should not
    // poison the whole config — image (which is the right type) should
    // still be picked up.
    const auto hc = parseHelmetConfig(R"({
        "helmet_overlay": {
            "enabled": "yes",
            "distance_m": "0.3",
            "horizontal_fov_deg": [130],
            "vertical_offset_deg": null,
            "brightness": {"value": 0.5},
            "image": "ok.png"
        }
    })");
    CHECK(hc.enabled == false);                     // default
    CHECK(hc.distance_m == doctest::Approx(0.5f));  // default
    CHECK(hc.horizontal_fov_deg == doctest::Approx(130.0f));   // default
    CHECK(hc.vertical_offset_deg == doctest::Approx(0.0f));    // default
    CHECK(hc.brightness == doctest::Approx(1.0f));             // default
    CHECK(hc.imageRelativePath == "ok.png");      // string → kept
}

TEST_CASE("parseHelmetConfig: non-string image leaves default name in place") {
    const auto hc = parseHelmetConfig(R"({
        "helmet_overlay": {"image": 123}
    })");
    CHECK(hc.imageRelativePath == "helmet_visor.png");
}

TEST_CASE("parseHelmetConfig: extra unknown fields are ignored gracefully") {
    // Forward-compatibility with future additions.
    const auto hc = parseHelmetConfig(R"({
        "helmet_overlay": {
            "enabled": true,
            "future_knob": 42,
            "_comment": "tooling note"
        }
    })");
    CHECK(hc.enabled == true);
    CHECK(hc.brightness == doctest::Approx(1.0f));  // still defaulted
}
