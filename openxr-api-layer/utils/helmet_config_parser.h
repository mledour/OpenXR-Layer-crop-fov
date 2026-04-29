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

// Pure JSON-string → HelmetOverlayConfig parser. Header-only so the
// standalone test binary can exercise it without linking the full
// layer DLL — same pattern as crop_math.h and name_utils.h.
//
// loadHelmetConfig() in layer.cpp reads the per-app settings file
// from disk and delegates the parsing here. Tests call this function
// directly on JSON literals to verify defaults, type-coercion, and
// clamp boundaries.

#include "helmet_overlay.h"

#include <algorithm>
#include <rapidjson/document.h>
#include <string>

namespace openxr_api_layer {

    // Parses a settings.json content string and returns the
    // HelmetOverlayConfig captured by its "helmet_overlay" object.
    // Defaults match the documented values in helmet_overlay.h:
    //   enabled              = false
    //   image                = "helmet_visor.png"
    //   distance_m           = 0.5
    //   horizontal_fov_deg   = 130   (clamped to [10, 270])
    //   vertical_offset_deg  = 0     (clamped to [-30, +30])
    //   brightness           = 1.0   (clamped to [0, 1])
    //
    // Robustness contract:
    //   - Empty / malformed JSON → all defaults.
    //   - Missing "helmet_overlay" object → all defaults.
    //   - Wrong type on a field → that field falls back to its default,
    //     other fields are still parsed.
    //   - Integer JSON values where floats are expected are accepted
    //     (rapidjson's IsInt / GetDouble combo handles the conversion).
    inline HelmetOverlayConfig parseHelmetConfig(const std::string& jsonContent) {
        HelmetOverlayConfig hc;

        rapidjson::Document doc;
        doc.Parse(jsonContent.c_str(), jsonContent.size());
        if (doc.HasParseError() || !doc.IsObject()) return hc;
        if (!doc.HasMember("helmet_overlay") || !doc["helmet_overlay"].IsObject()) return hc;

        const auto& ho = doc["helmet_overlay"];

        const auto readBool = [&](const char* key, bool def) -> bool {
            if (!ho.HasMember(key)) return def;
            const auto& v = ho[key];
            return v.IsBool() ? v.GetBool() : def;
        };
        const auto readFloat = [&](const char* key, float def) -> float {
            if (!ho.HasMember(key)) return def;
            const auto& v = ho[key];
            if (v.IsFloat() || v.IsDouble() || v.IsInt() || v.IsUint()) {
                return static_cast<float>(v.GetDouble());
            }
            return def;
        };

        hc.enabled = readBool("enabled", false);
        hc.distance_m = readFloat("distance_m", 0.5f);

        // Clamp the angular FOV to a sane range. Below ~10° the quad
        // is a thin vertical strip; above ~270° tan() blows up and
        // the quad would extend past the user's actual visual field.
        hc.horizontal_fov_deg = std::max(10.0f, std::min(270.0f,
            readFloat("horizontal_fov_deg", 130.0f)));

        // Clamp the offset to ±30°. Beyond that the quad escapes the
        // user's FOV entirely and the overlay becomes invisible.
        hc.vertical_offset_deg = std::max(-30.0f, std::min(30.0f,
            readFloat("vertical_offset_deg", 0.0f)));

        // Clamp to [0, 1]. Above 1.0 would amplify highlights past the
        // original PNG values — never useful, only blows things out.
        hc.brightness = std::max(0.0f, std::min(1.0f,
            readFloat("brightness", 1.0f)));

        if (ho.HasMember("image") && ho["image"].IsString()) {
            hc.imageRelativePath = ho["image"].GetString();
        }

        return hc;
    }

} // namespace openxr_api_layer
