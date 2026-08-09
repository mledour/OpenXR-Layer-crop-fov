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

// Shared test fixture for the integration + microbench TUs. Both file types
// boot the real OpenXrLayer against the in-process mock_runtime, isolate
// %LOCALAPPDATA% to a temp dir per test, and use the same handful of
// resolved entry points. Keeping this in one header keeps the boot recipe
// in one place.

#include "mock_runtime.h"

#include <layer.h>

// REQUIRE() in boot() expands to a doctest assertion, so the fixture
// header pulls doctest in itself rather than relying on the including
// TU to do it in the right order. The header is heavy (~6000 lines)
// but every consumer of this fixture is a doctest TU that would
// include doctest anyway, so this is an order-fix, not a new cost.
#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace test_fixture {

// Every TEST_CASE creates a fresh layer singleton + mock state, so tests are
// order-independent. We also isolate %LOCALAPPDATA% to a unique temp dir per
// test so settings.json handling is deterministic on CI where the real
// %LOCALAPPDATA%\XR_APILAYER_MLEDOUR_fov_crop\ might exist with user data.
struct LayerFixture {
    std::filesystem::path configDir;

    LayerFixture() {
        mock::reset();
        openxr_api_layer::ResetInstance();

        // Unique temp dir avoids clashes if tests run in parallel one day.
        std::random_device rd;
        auto suffix = std::to_string(rd());
        configDir = std::filesystem::temp_directory_path() /
                    ("openxr-layer-test-" + suffix);
        std::filesystem::create_directories(configDir);
        openxr_api_layer::localAppData = configDir;
        openxr_api_layer::dllHome = configDir;
    }

    ~LayerFixture() {
        openxr_api_layer::ResetInstance();
        std::error_code ec;
        std::filesystem::remove_all(configDir, ec);
    }

    // Writes the global settings.json template into the fixture's
    // localAppData. On the next boot(), the layer bootstraps the per-app
    // config (test_settings.json) from this file. Pass empty string to skip
    // (layer will use defaults or whatever a previous writePerAppSettings
    // put in place).
    void writeSettings(const std::string& json) {
        if (json.empty()) return;
        std::ofstream f(configDir / "settings.json");
        f << json;
    }

    // Writes directly to the per-app config file (test_settings.json) that
    // the fixture's boot() will use (applicationName = "test" -> slug
    // "test"). Use this for post-boot mutations that need the live-edit
    // watcher to observe the change — writeSettings() targets the template,
    // which the watcher does not monitor.
    void writePerAppSettings(const std::string& json) {
        if (json.empty()) return;
        std::ofstream f(configDir / "test_settings.json");
        f << json;
    }

    // Brings the layer up the same way xrCreateApiLayerInstance would:
    // wire GIPA -> call xrCreateInstance -> resolve every function we plan to
    // exercise (so the m_xrFoo pointers in OpenXrApi are populated via the
    // generated xrGetInstanceProcAddrInternal path).
    openxr_api_layer::OpenXrApi* boot() {
        openxr_api_layer::OpenXrApi* layer = openxr_api_layer::GetInstance();
        REQUIRE(layer != nullptr);

        // XrInstance here is a fake handle — the layer never dereferences it,
        // just threads it back to the mock via m_xrGetInstanceProcAddr.
        XrInstance fakeInstance = reinterpret_cast<XrInstance>(static_cast<uintptr_t>(0xBEEF));
        layer->SetGetInstanceProcAddr(&mock::xrGetInstanceProcAddr, fakeInstance);

        XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
        std::strncpy(ici.applicationInfo.applicationName, "test", XR_MAX_APPLICATION_NAME_SIZE - 1);
        ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

        REQUIRE(layer->xrCreateInstance(&ici) == XR_SUCCESS);

        // Resolve the entry points the tests will call. This is what the
        // loader does after xrCreateApiLayerInstance: each of these calls
        // populates m_xrFoo inside OpenXrApi via the generated
        // xrGetInstanceProcAddrInternal. We never use the returned function
        // pointers — we go through the virtual methods instead.
        //
        // We explicitly target OpenXrApi::xrGetInstanceProcAddr (the base,
        // non-bypassed implementation). The OpenXrLayer override short-circuits
        // to raw runtime pointers when m_bypassApiLayer is true, which skips
        // the m_xrFoo population and would leave the test calling null in
        // bypass-enabled test cases. Going through the base always populates
        // the dispatch table so subsequent virtual calls are safe to make;
        // the override's enabled-check still governs whether the layer
        // mutates results.
        auto resolve = [&](const char* name) {
            PFN_xrVoidFunction fn = nullptr;
            REQUIRE(layer->OpenXrApi::xrGetInstanceProcAddr(fakeInstance, name, &fn) == XR_SUCCESS);
        };
        resolve("xrGetSystem");
        resolve("xrCreateSession");
        resolve("xrEnumerateViewConfigurationViews");
        resolve("xrLocateViews");
        resolve("xrEndFrame");

        return layer;
    }
};

constexpr float kDefaultLeftAngle = -0.90f;
constexpr float kDefaultRightAngle = 0.90f;
constexpr float kDefaultUpAngle = 0.70f;
constexpr float kDefaultDownAngle = -0.70f;

inline XrFovf defaultFov() {
    return XrFovf{kDefaultLeftAngle, kDefaultRightAngle, kDefaultUpAngle, kDefaultDownAngle};
}

} // namespace test_fixture
