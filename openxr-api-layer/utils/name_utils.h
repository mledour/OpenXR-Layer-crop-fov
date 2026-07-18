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

// Pure string-to-filename helpers shared between the layer DLL and the
// standalone test binary. Header-only so neither side needs to link the
// other.

#include <filesystem>
#include <string>

namespace openxr_api_layer {

    // Turns a free-form OpenXR application name (e.g. "DiRT Rally 2.0",
    // "Le Mans Ultimate", "hello_xr") into a lowercase, filesystem-safe
    // slug suitable as a filename prefix. Rules:
    //   - uppercase ASCII is lowercased
    //   - any non-[a-z0-9] char becomes '_'
    //   - consecutive '_' are collapsed to one
    //   - trailing '_' is trimmed
    //   - empty or all-non-alphanumeric input yields "unknown_app"
    inline std::string sanitizeForFilename(const std::string& raw) {
        std::string out;
        out.reserve(raw.size());
        bool lastWasSep = true; // treat start as separator to strip leading '_'
        for (char c : raw) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            const bool isAlnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
            if (isAlnum) {
                out.push_back(c);
                lastWasSep = false;
            } else if (!lastWasSep) {
                out.push_back('_');
                lastWasSep = true;
            }
        }
        while (!out.empty() && out.back() == '_') out.pop_back();
        if (out.empty()) out = "unknown_app";
        return out;
    }

    // Escapes a raw string so it can be embedded safely inside a JSON string
    // literal. Without this, an OpenXR application name containing a double
    // quote, backslash, control character, or invalid UTF-8 byte would
    // corrupt a config file we generate with the name inlined — RapidJSON
    // then rejects the whole file ("Invalid encoding in string" / "Invalid
    // escape character") and the layer silently falls back to defaults and
    // disables itself for that game.
    //
    // Rules:
    //   - '"' and '\' are backslash-escaped
    //   - \b \f \n \r \t use their short escapes; other C0 controls become \u00XX
    //   - well-formed UTF-8 multi-byte sequences pass through unchanged
    //   - malformed UTF-8 bytes are replaced with '?' so the output is always
    //     valid UTF-8 (and therefore valid inside a JSON string)
    // Header-only and dependency-free (only <string>) to match the "test
    // binary needn't link the layer" contract of this file.
    inline std::string jsonEscape(const std::string& raw) {
        static const char* kHex = "0123456789abcdef";
        std::string out;
        out.reserve(raw.size() + 8);
        const size_t n = raw.size();
        size_t i = 0;
        while (i < n) {
            const unsigned char c = static_cast<unsigned char>(raw[i]);
            if (c == '"') {
                out += "\\\"";
                ++i;
            } else if (c == '\\') {
                out += "\\\\";
                ++i;
            } else if (c < 0x20) {
                switch (c) {
                    case '\b': out += "\\b"; break;
                    case '\f': out += "\\f"; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default:
                        out += "\\u00";
                        out.push_back(kHex[(c >> 4) & 0xF]);
                        out.push_back(kHex[c & 0xF]);
                        break;
                }
                ++i;
            } else if (c < 0x80) {
                out.push_back(static_cast<char>(c));
                ++i;
            } else {
                // Multi-byte UTF-8. Validate strictly against the Unicode
                // well-formed byte-sequence table (Unicode 15 Table 3-7): this
                // rejects invalid lead bytes (0x80-0xC1, 0xF5-0xFF), overlong
                // encodings, and UTF-16 surrogates (U+D800-U+DFFF), not just
                // "does every trailing byte look like a continuation". The
                // range of the FIRST continuation byte depends on the lead.
                size_t len = 0;           // total bytes in the sequence
                unsigned char lo = 0x80;  // valid range of the 1st continuation
                unsigned char hi = 0xBF;
                if (c >= 0xC2 && c <= 0xDF)      { len = 2; }
                else if (c == 0xE0)              { len = 3; lo = 0xA0; }
                else if (c >= 0xE1 && c <= 0xEC) { len = 3; }
                else if (c == 0xED)              { len = 3; hi = 0x9F; } // no surrogates
                else if (c >= 0xEE && c <= 0xEF) { len = 3; }
                else if (c == 0xF0)              { len = 4; lo = 0x90; } // no overlong
                else if (c >= 0xF1 && c <= 0xF3) { len = 4; }
                else if (c == 0xF4)              { len = 4; hi = 0x8F; } // <= U+10FFFF
                // else: invalid lead byte -> len stays 0

                bool valid = (len != 0) && (i + len <= n);
                if (valid) {
                    const unsigned char b1 = static_cast<unsigned char>(raw[i + 1]);
                    if (b1 < lo || b1 > hi) valid = false;
                    for (size_t k = 2; valid && k < len; ++k) {
                        const unsigned char bk = static_cast<unsigned char>(raw[i + k]);
                        if (bk < 0x80 || bk > 0xBF) valid = false;
                    }
                }
                if (valid) {
                    out.append(raw, i, len);
                    i += len;
                } else {
                    out.push_back('?');
                    ++i;
                }
            }
        }
        return out;
    }

    // Resolves the path to the per-app settings file for the given raw
    // application name. Puts it alongside the global settings.json template
    // in the given config directory (typically localAppData\<layer-name>\).
    inline std::filesystem::path resolvePerAppConfigPath(const std::filesystem::path& configDir,
                                                         const std::string& appName) {
        return configDir / (sanitizeForFilename(appName) + "_settings.json");
    }

} // namespace openxr_api_layer
