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

// Single source of truth for the settings.help.txt documentation shipped
// next to the config files. The layer DLL writes this verbatim on first run
// (writeHelpFile in layer.cpp); the Inno installer ships a byte-identical
// copy at installer/settings.help.txt so installed users have the docs before
// they ever launch a game. test_crop_math.cpp asserts the on-disk installer
// copy matches this constant (line-ending-insensitive), so the two can never
// drift silently — edit the text HERE and update installer/settings.help.txt
// to match, and CI enforces it.
//
// Header-only, dependency-free. Plain ASCII, LF line endings.

namespace openxr_api_layer {

    inline constexpr char kSettingsHelpText[] =
        "XR_APILAYER_MLEDOUR_fov_crop - settings reference\n"
        "=================================================\n"
        "\n"
        "Edit settings.json (global default for every game) or a per-app\n"
        "file such as myGame_settings.json (created the first time that game\n"
        "runs). A per-app file overrides the global default for that game.\n"
        "\n"
        "IMPORTANT: settings.json is strict JSON. Edit it with a plain-text\n"
        "editor and save as UTF-8. Do not add comments or trailing commas -\n"
        "if the file fails to parse the layer runs with built-in defaults\n"
        "and writes a *.PARSE_ERROR.txt file next to it explaining why.\n"
        "\n"
        "Fields\n"
        "------\n"
        "enabled                   true/false. Master switch for this game.\n"
        "crop_left_percent         % cropped from the LEFT outer edge (0-100).\n"
        "crop_right_percent        % cropped from the RIGHT outer edge.\n"
        "crop_top_percent          % cropped from the TOP edge.\n"
        "crop_bottom_percent       % cropped from the BOTTOM edge.\n"
        "crop_left_right_percent   % cropped from the LEFT eye's INNER edge\n"
        "                          (binocular-overlap zone). 0 = use the\n"
        "                          matching outer value.\n"
        "crop_right_left_percent   % cropped from the RIGHT eye's INNER edge.\n"
        "live_edit                 true reloads this file every frame so you\n"
        "                          can tune crop values with the headset on.\n"
        "                          Leave false for normal play.\n"
        "\n"
        "helmet_overlay (object)\n"
        "  enabled                 true/false. Draw a helmet visor overlay.\n"
        "  image                   PNG filename in the helmets/ subfolder.\n"
        "  distance_m              Overlay distance in metres.\n"
        "  brightness              0.0 (invisible) to 1.0 (opaque).\n"
        "  horizontal_fov_deg      Angular width of the overlay in degrees.\n"
        "  vertical_offset_deg     Vertical placement in degrees (negative =\n"
        "                          lower).\n"
        "\n"
        "To disable the layer entirely without uninstalling, set the\n"
        "environment variable named in the layer's JSON manifest\n"
        "(disable_environment) - see the project README.\n";

} // namespace openxr_api_layer
