// MIT License
//
// Copyright (c) 2026 Michael Ledour
//
// Based on https://github.com/mbucchia/OpenXR-Layer-Template.
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright noticeand this permission notice shall be included in all
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

#include "layer.h"
#include <log.h>
#include <util.h>
#include <utils/crop_math.h>
#include <utils/helmet_config_parser.h>
#include <utils/helmet_overlay.h>
#include <utils/helmet_visibility_mask.h>
#include <utils/name_utils.h>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

namespace openxr_api_layer {

    using namespace log;

    // Our API layer implement these extensions, and their specified version.
    const std::vector<std::pair<std::string, uint32_t>> advertisedExtensions = {};

    // Initialize these vectors with arrays of extensions to block and implicitly request for the instance.
    const std::vector<std::string> blockedExtensions = {};
    // XR_KHR_visibility_mask: lets us augment the runtime's "hidden
    // triangle mesh" with the helmet's opaque silhouette. Apps that
    // consume xrGetVisibilityMaskKHR (e.g. for stencil rejection)
    // then skip shading on the masked pixels. Requested implicitly
    // so it works even on apps that don't enable the extension
    // themselves; if the runtime doesn't expose it, the framework
    // logs "Cannot satisfy implicit extension request" and our
    // override falls through to "no contribution".
    const std::vector<std::string> implicitExtensions = {
        XR_KHR_VISIBILITY_MASK_EXTENSION_NAME
    };

    // CropConfig, clampFactor, scaleSwapchainExtents, and narrowFov live in
    // <utils/crop_math.h> so they can be unit-tested from a standalone binary
    // without linking the layer DLL. (computeCroppedImageRect also lives there
    // for now — unused by the layer since we moved to single-application, but
    // kept as a reusable helper and covered by tests.)

    // Reads an optional float field from a rapidjson object. Returns defaultVal if
    // the field is absent or not a number. Accepts both integer and floating JSON numbers.
    static float readJsonFloat(const rapidjson::Value& obj, const char* key, float defaultVal) {
        if (!obj.HasMember(key)) return defaultVal;
        const auto& v = obj[key];
        if (v.IsFloat() || v.IsDouble() || v.IsInt() || v.IsUint()) {
            return static_cast<float>(v.GetDouble());
        }
        Log(fmt::format("settings.json: \"{}\" is not a number, using default {}\n", key, defaultVal));
        return defaultVal;
    }

    static bool readJsonBool(const rapidjson::Value& obj, const char* key, bool defaultVal) {
        if (!obj.HasMember(key)) return defaultVal;
        const auto& v = obj[key];
        if (v.IsBool()) return v.GetBool();
        Log(fmt::format("settings.json: \"{}\" is not a bool, using default {}\n", key, defaultVal));
        return defaultVal;
    }

    // sanitizeForFilename and resolvePerAppConfigPath live in
    // <utils/name_utils.h> so the test binary can unit-test them without
    // linking layer.cpp.

    // Writes a minimal default settings.json-style file at outputPath. If
    // appName is empty, the file is treated as the global template that
    // seeds per-app files on first run. Otherwise the file is a per-app
    // file and the comment embeds the application name.
    static bool writeDefaultConfig(const std::filesystem::path& outputPath, const std::string& appName) {
        std::ofstream out(outputPath);
        if (!out) return false;
        // The output here MUST stay byte-for-byte aligned with
        // installer/default_settings.json, which the Inno Setup script
        // drops into %LOCALAPPDATA%\XR_APILAYER_MLEDOUR_fov_crop\settings.json
        // at install time. writeDefaultConfig() is the runtime fallback
        // for ZIP / dev installs where the installer never ran. If the
        // two ever drift, installer users and ZIP users get different
        // out-of-the-box defaults.
        out << "{\n";
        if (appName.empty()) {
            out << "  \"_comment\": \"Default template. Each OpenXR application "
                <<                "gets a copy of this file the first time it runs. "
                <<                "Set \\\"enabled\\\" to true to activate the layer for that game "
                <<                "(or change the default here to affect every future game). "
                <<                "Edit crop percentages to taste.\",\n";
        } else {
            out << "  \"_comment\": \"Auto-generated per-app config for '" << appName
                <<                "'. Set \\\"enabled\\\" to true to activate the layer for this game.\",\n";
        }
        out << "  \"enabled\": false,\n"
            << "  \"crop_left_percent\": 0,\n"
            << "  \"crop_right_percent\": 0,\n"
            << "  \"crop_top_percent\": 40,\n"
            << "  \"crop_bottom_percent\": 30,\n"
            << "  \"live_edit\": true,\n"
            << "  \"helmet_overlay\": {\n"
            << "    \"enabled\": true,\n"
            << "    \"image\": \"helmet_visor.png\",\n"
            << "    \"distance_m\": 0.15,\n"
            << "    \"brightness\": 0.25,\n"
            << "    \"horizontal_fov_deg\": 120,\n"
            << "    \"vertical_offset_deg\": -10,\n"
            << "    \"use_visibility_mask\": true\n"
            << "  }\n"
            << "}\n";
        return out.good();
    }

    // Creates the global settings.json template (if absent) so the user has
    // a single file to edit to change the defaults applied to future games.
    // Silent no-op if the file already exists (never overwrites the user's
    // preferences).
    static void ensureTemplateConfig(const std::filesystem::path& configDir) {
        const std::filesystem::path templatePath = configDir / "settings.json";
        if (std::filesystem::exists(templatePath)) return;
        try {
            std::filesystem::create_directories(configDir);
            if (writeDefaultConfig(templatePath, "")) {
                Log(fmt::format("Created template {}\n", templatePath.string()));
            }
        } catch (const std::exception& e) {
            Log(fmt::format("Could not create template {}: {}\n",
                             templatePath.string(), e.what()));
        }
    }

    // Copies all *.png files from the build's bundled helmets directory
    // (next to the DLL) to the user's helmets directory under
    // localAppData. Existing files in the user dir are NEVER overwritten,
    // so any custom PNG the user dropped in keeps priority on subsequent
    // launches — same "bootstrap once, never touch user data" contract
    // as ensureTemplateConfig and the per-app settings flow.
    //
    // Silent no-op if the build directory is missing or empty (e.g. on
    // a manual install where the user only copied the DLL itself).
    static void ensureHelmetsBootstrapped(const std::filesystem::path& userHelmetsDir,
                                           const std::filesystem::path& bundledHelmetsDir) {
        try {
            std::filesystem::create_directories(userHelmetsDir);
        } catch (const std::exception& e) {
            Log(fmt::format("Could not create user helmets dir {}: {}\n",
                             userHelmetsDir.string(), e.what()));
            return;
        }

        std::error_code ec;
        if (!std::filesystem::is_directory(bundledHelmetsDir, ec)) {
            Log(fmt::format("Bundled helmets dir absent ({}), nothing to bootstrap\n",
                             bundledHelmetsDir.string()));
            return;
        }

        for (const auto& entry : std::filesystem::directory_iterator(bundledHelmetsDir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            const auto& src = entry.path();
            if (src.extension() != ".png") continue;

            const auto dst = userHelmetsDir / src.filename();
            if (std::filesystem::exists(dst)) continue;  // user file wins

            try {
                std::filesystem::copy_file(src, dst);
                Log(fmt::format("Bootstrapped helmet asset {} → {}\n",
                                 src.filename().string(), dst.string()));
            } catch (const std::exception& e) {
                Log(fmt::format("Failed to bootstrap helmet asset {}: {}\n",
                                 src.filename().string(), e.what()));
            }
        }
    }

    // Loads the crop config from the exact path `configPath` (not a directory).
    // If the file does not exist and `appName` is non-empty, the file is
    // bootstrapped: a sibling "settings.json" in the same directory is copied
    // in if it exists, otherwise a defaults file is written.
    static CropConfig loadConfig(const std::filesystem::path& configPath, const std::string& appName) {
        CropConfig config;
        const std::string configPathStr = configPath.string();

        // Bootstrap the per-app file the first time we see this application.
        if (!appName.empty() && !std::filesystem::exists(configPath)) {
            try {
                std::filesystem::create_directories(configPath.parent_path());
                const std::filesystem::path templatePath = configPath.parent_path() / "settings.json";
                if (std::filesystem::exists(templatePath) && templatePath != configPath) {
                    std::filesystem::copy_file(templatePath, configPath);
                    Log(fmt::format("Bootstrapped {} from settings.json template\n", configPathStr));
                } else if (writeDefaultConfig(configPath, appName)) {
                    Log(fmt::format("Bootstrapped {} with built-in defaults\n", configPathStr));
                } else {
                    Log(fmt::format("Could not create {}, falling back to defaults\n", configPathStr));
                    return config;
                }
            } catch (const std::exception& e) {
                Log(fmt::format("Error bootstrapping per-app config: {}, using defaults\n", e.what()));
                return config;
            }
        }

        Log(fmt::format("Looking for config at: {}\n", configPathStr));

        std::string fileContent;
        {
            std::ifstream file(configPathStr);
            if (!file.is_open()) {
                Log("Config file not found, using defaults\n");
                return config;
            }
            std::ostringstream ss;
            ss << file.rdbuf();
            fileContent = ss.str();
        }

        Log(fmt::format("Config file read OK ({} bytes)\n", fileContent.size()));

        rapidjson::Document doc;
        doc.Parse(fileContent.c_str(), fileContent.size());
        if (doc.HasParseError()) {
            Log(fmt::format("Config parse error at offset {}: {} — using defaults\n",
                             doc.GetErrorOffset(),
                             rapidjson::GetParseError_En(doc.GetParseError())));
            return config;
        }
        if (!doc.IsObject()) {
            Log("Config root is not an object — using defaults\n");
            return config;
        }

        const bool enabled = readJsonBool(doc, "enabled", false);
        const float leftPct = readJsonFloat(doc, "crop_left_percent", 10.0f);
        const float rightPct = readJsonFloat(doc, "crop_right_percent", 10.0f);
        const float topPct = readJsonFloat(doc, "crop_top_percent", 15.0f);
        const float bottomPct = readJsonFloat(doc, "crop_bottom_percent", 20.0f);
        const bool liveEdit = readJsonBool(doc, "live_edit", false);

        config.enabled = enabled;
        config.cropLeftFactor = clampFactor(leftPct);
        config.cropRightFactor = clampFactor(rightPct);
        config.cropTopFactor = clampFactor(topPct);
        config.cropBottomFactor = clampFactor(bottomPct);
        config.liveEdit = liveEdit;

        Log(fmt::format("Crop config: enabled={}, left={:.1f}, right={:.1f}, top={:.1f}, bottom={:.1f}, live_edit={}\n",
                         config.enabled, leftPct, rightPct, topPct, bottomPct, config.liveEdit));

        return config;
    }

    // Loads the helmet_overlay block from the same settings.json that
    // loadConfig() reads. Reads the file then delegates the actual
    // JSON parsing + clamping to parseHelmetConfig (utils/
    // helmet_config_parser.h), which is unit-tested in isolation.
    // Silently returns defaults (disabled) if the file is missing or
    // unreadable; parseHelmetConfig handles the rest of the
    // robustness contract (malformed JSON, wrong types, missing
    // fields, out-of-range values).
    static HelmetOverlayConfig loadHelmetConfig(const std::filesystem::path& configPath) {
        std::string fileContent;
        {
            std::ifstream file(configPath.string());
            if (!file.is_open()) return HelmetOverlayConfig{};
            std::ostringstream ss;
            ss << file.rdbuf();
            fileContent = ss.str();
        }

        const HelmetOverlayConfig hc = openxr_api_layer::parseHelmetConfig(fileContent);

        Log(fmt::format(
            "Helmet overlay config: enabled={}, distance={:.2f}m, fov={:.0f}°, "
            "v_offset={:+.1f}°, brightness={:.2f}, image={}\n",
            hc.enabled, hc.distance_m, hc.horizontal_fov_deg,
            hc.vertical_offset_deg, hc.brightness, hc.imageRelativePath));

        return hc;
    }

    // This class implements our API layer.
    class OpenXrLayer : public openxr_api_layer::OpenXrApi {
      public:
        OpenXrLayer() = default;
        ~OpenXrLayer() = default;

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetInstanceProcAddr
        XrResult xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) override {
            TraceLoggingWrite(g_traceProvider,
                              "xrGetInstanceProcAddr",
                              TLXArg(instance, "Instance"),
                              TLArg(name, "Name"),
                              TLArg(m_bypassApiLayer, "Bypass"));

            XrResult result = m_bypassApiLayer ? m_xrGetInstanceProcAddr(instance, name, function)
                                               : OpenXrApi::xrGetInstanceProcAddr(instance, name, function);

            TraceLoggingWrite(g_traceProvider, "xrGetInstanceProcAddr", TLPArg(*function, "Function"));

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrCreateInstance
        XrResult xrCreateInstance(const XrInstanceCreateInfo* createInfo) override {
            if (createInfo->type != XR_TYPE_INSTANCE_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            const XrResult result = OpenXrApi::xrCreateInstance(createInfo);
            if (XR_FAILED(result)) {
                TraceLoggingWrite(g_traceProvider,
                                  "xrCreateInstance_Failed",
                                  TLArg(xr::ToCString(result), "Result"),
                                  TLArg(createInfo->applicationInfo.applicationName, "ApplicationName"));
                Log(fmt::format("xrCreateInstance failed for {}: {}\n",
                                 createInfo->applicationInfo.applicationName,
                                 xr::ToCString(result)));
                return result;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrCreateInstance",
                              TLArg(xr::ToString(createInfo->applicationInfo.apiVersion).c_str(), "ApiVersion"),
                              TLArg(createInfo->applicationInfo.applicationName, "ApplicationName"),
                              TLArg(createInfo->applicationInfo.applicationVersion, "ApplicationVersion"),
                              TLArg(createInfo->applicationInfo.engineName, "EngineName"),
                              TLArg(createInfo->applicationInfo.engineVersion, "EngineVersion"),
                              TLArg(createInfo->createFlags, "CreateFlags"));
            Log(fmt::format("Application: {}\n", createInfo->applicationInfo.applicationName));

            // Ensure the global settings.json template exists. It seeds the
            // per-app file below, and gives the user a single place to edit
            // the defaults applied to future games.
            openxr_api_layer::ensureTemplateConfig(localAppData);

            // Bootstrap the helmets/ directory under localAppData on first
            // run. Copies the PNGs the build dropped next to the DLL into
            // the user's writable settings dir; existing user files are
            // never overwritten so custom PNGs stick around.
            openxr_api_layer::ensureHelmetsBootstrapped(
                localAppData / "helmets",
                dllHome / "helmets");

            // Per-app configuration: each OpenXR application gets its own
            // settings file, keyed by a sanitized version of the application
            // name. The first time a given application is seen, the file is
            // bootstrapped from the global settings.json template (always
            // present after ensureTemplateConfig). The user can then edit
            // that file per-app without affecting other games.
            m_appName = createInfo->applicationInfo.applicationName;
            m_configFilePath = openxr_api_layer::resolvePerAppConfigPath(localAppData, m_appName);

            // Route the file logger to a per-app file so debugging one
            // game doesn't mean sifting through messages from every other
            // game that ran on this machine. Messages before this point
            // (xrNegotiateLoaderApiLayerInterface's "layer is active")
            // stay in the default <LayerName>.log.
            const std::filesystem::path perAppLogPath =
                localAppData / (openxr_api_layer::sanitizeForFilename(m_appName) + ".log");
            openxr_api_layer::log::reopenLogFile(perAppLogPath);
            Log(fmt::format("Log routed to per-app file: {}\n", perAppLogPath.string()));

            m_config = openxr_api_layer::loadConfig(m_configFilePath, m_appName);
            m_helmetConfig = openxr_api_layer::loadHelmetConfig(m_configFilePath);
            ++m_configGen;
            if (!m_config.enabled) {
                Log(fmt::format("{} is disabled in {}\n", LayerName, m_configFilePath.string()));
                m_bypassApiLayer = true;
            }

            // If live_edit is on, record the per-app config's mtime so
            // xrLocateViews can detect changes without recomputing the path
            // each frame.
            if (m_config.liveEdit) {
                try {
                    m_configLastWriteTime = std::filesystem::last_write_time(m_configFilePath);
                } catch (...) {}
                Log(fmt::format("Live-edit enabled, watching {}\n", m_configFilePath.string()));
            }

            if (m_bypassApiLayer) {
                TraceLoggingWrite(g_traceProvider, "xrCreateInstance_Bypass");
                Log(fmt::format("{} layer will be bypassed\n", LayerName));
                return result;
            }

            for (uint32_t i = 0; i < createInfo->enabledApiLayerCount; i++) {
                TraceLoggingWrite(
                    g_traceProvider, "xrCreateInstance", TLArg(createInfo->enabledApiLayerNames[i], "ApiLayerName"));
            }
            for (uint32_t i = 0; i < createInfo->enabledExtensionCount; i++) {
                TraceLoggingWrite(
                    g_traceProvider, "xrCreateInstance", TLArg(createInfo->enabledExtensionNames[i], "ExtensionName"));
            }

            XrInstanceProperties instanceProperties = {XR_TYPE_INSTANCE_PROPERTIES};
            CHECK_XRCMD(OpenXrApi::xrGetInstanceProperties(GetXrInstance(), &instanceProperties));
            const auto runtimeName = fmt::format("{} {}.{}.{}",
                                                 instanceProperties.runtimeName,
                                                 XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
                                                 XR_VERSION_MINOR(instanceProperties.runtimeVersion),
                                                 XR_VERSION_PATCH(instanceProperties.runtimeVersion));
            TraceLoggingWrite(g_traceProvider, "xrCreateInstance", TLArg(runtimeName.c_str(), "RuntimeName"));
            Log(fmt::format("Using OpenXR runtime: {}\n", runtimeName));

            // Snapshot whether the framework managed to enable
            // XR_KHR_visibility_mask. The visibility-mask path no-ops
            // when this is false (no runtime function pointer to
            // forward to, and no app would call the function anyway
            // since the extension wasn't granted).
            for (const auto& ext : GetGrantedExtensions()) {
                if (ext == XR_KHR_VISIBILITY_MASK_EXTENSION_NAME) {
                    m_visibilityMaskExtensionGranted = true;
                    break;
                }
            }
            Log(fmt::format("XR_KHR_visibility_mask {}\n",
                             m_visibilityMaskExtensionGranted ? "granted" : "not granted"));

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetSystem
        XrResult xrGetSystem(XrInstance instance, const XrSystemGetInfo* getInfo, XrSystemId* systemId) override {
            if (getInfo->type != XR_TYPE_SYSTEM_GET_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrGetSystem",
                              TLXArg(instance, "Instance"),
                              TLArg(xr::ToCString(getInfo->formFactor), "FormFactor"));

            const XrResult result = OpenXrApi::xrGetSystem(instance, getInfo, systemId);
            if (XR_SUCCEEDED(result) && getInfo->formFactor == XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) {
                if (*systemId != m_systemId) {
                    XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
                    CHECK_XRCMD(OpenXrApi::xrGetSystemProperties(instance, *systemId, &systemProperties));
                    TraceLoggingWrite(g_traceProvider, "xrGetSystem", TLArg(systemProperties.systemName, "SystemName"));
                    Log(fmt::format("Using OpenXR system: {}\n", systemProperties.systemName));
                }

                m_systemId = *systemId;
            }

            TraceLoggingWrite(g_traceProvider, "xrGetSystem", TLArg((int)*systemId, "SystemId"));

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrCreateSession
        XrResult xrCreateSession(XrInstance instance,
                                 const XrSessionCreateInfo* createInfo,
                                 XrSession* session) override {
            if (createInfo->type != XR_TYPE_SESSION_CREATE_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrCreateSession",
                              TLXArg(instance, "Instance"),
                              TLArg((int)createInfo->systemId, "SystemId"),
                              TLArg(createInfo->createFlags, "CreateFlags"));

            const XrResult result = OpenXrApi::xrCreateSession(instance, createInfo, session);
            if (XR_SUCCEEDED(result)) {
                if (isSystemHandled(createInfo->systemId)) {
                    Log("Session created for handled system\n");
                }

                TraceLoggingWrite(g_traceProvider, "xrCreateSession", TLXArg(*session, "Session"));

                // Capture the session handle for the visibility-mask
                // event injection (xrPollEvent needs to attach the
                // session to each XR_TYPE_EVENT_DATA_VISIBILITY_MASK_
                // CHANGED_KHR it emits).
                m_session = *session;

                // Arm the helmet overlay. Best-practices: any failure
                // here must NEVER crash the host; the overlay degrades
                // to "not armed" and the rest of the layer keeps running.
                // We pass `this` so the overlay can call downstream PFNs
                // (xrCreateSwapchain, xrAcquireSwapchainImage, …) through
                // the layer's own dispatch.
                if (!m_bypassApiLayer) {
                    try {
                        // The overlay resolves config.imageRelativePath against
                        // the user-writable helmets/ folder under localAppData,
                        // which the bootstrap step in xrCreateInstance keeps
                        // populated with the build's bundled PNGs.
                        m_helmetOverlay.initialize(this, *session, createInfo->next,
                                                    m_helmetConfig,
                                                    localAppData / "helmets");
                    } catch (const std::exception& exc) {
                        ErrorLog(fmt::format("HelmetOverlay::initialize threw: {}\n", exc.what()));
                    }

                    // Visibility-mask path: only spin up if the runtime
                    // granted the extension. The mask itself is wired
                    // off the same PNG (re-loaded independently) and
                    // doesn't depend on D3D11; it just needs the file
                    // to be readable. Failure modes are silent —
                    // m_visibilityMask.isInitialized() stays false,
                    // and our xrGetVisibilityMaskKHR override falls
                    // through to pure pass-through.
                    if (m_visibilityMaskExtensionGranted) {
                        try {
                            m_visibilityMask.initialize(localAppData / "helmets",
                                                         m_helmetConfig);
                            // Companion XR_REFERENCE_SPACE_TYPE_VIEW handle
                            // for our own xrLocateViews calls. The helmet
                            // overlay creates its own viewSpace internally;
                            // we keep a dedicated one here so the two
                            // subsystems stay independent.
                            XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
                            rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
                            rci.poseInReferenceSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
                            rci.poseInReferenceSpace.position = {0.0f, 0.0f, 0.0f};
                            if (XR_FAILED(OpenXrApi::xrCreateReferenceSpace(*session, &rci, &m_maskViewSpace))) {
                                Log("VisibilityMask: xrCreateReferenceSpace(VIEW) failed, "
                                    "mask path will not arm\n");
                                m_maskViewSpace = XR_NULL_HANDLE;
                            }
                            Log(fmt::format(
                                "VisibilityMask: helmet mask {}, view space {}\n",
                                m_visibilityMask.isInitialized() ? "armed" : "inert",
                                m_maskViewSpace != XR_NULL_HANDLE ? "ready" : "absent"));
                        } catch (const std::exception& exc) {
                            ErrorLog(fmt::format("HelmetVisibilityMask::initialize threw: {}\n", exc.what()));
                        }
                    }
                }
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrDestroySession
        XrResult xrDestroySession(XrSession session) override {
            TraceLoggingWrite(g_traceProvider, "xrDestroySession", TLXArg(session, "Session"));

            // Release overlay + visibility-mask resources BEFORE
            // forwarding: once the runtime destroys the session, the
            // XrSwapchain / XrSpace handles are invalid and calling
            // destroy PFNs on them would be UB.
            if (m_maskViewSpace != XR_NULL_HANDLE) {
                OpenXrApi::xrDestroySpace(m_maskViewSpace);
                m_maskViewSpace = XR_NULL_HANDLE;
            }
            m_eyePoseCacheValid = false;
            m_visibilityMaskBuilt.fill(false);
            m_endFrameDiagLogged = false;       // re-arm one-shot for the next session
            m_strippedAppLayerLogged = false;   // ditto, layer-strip one-shot
            // Diagnostic: report whether the app actually queried
            // xrGetVisibilityMaskKHR during the session. Zero means
            // our mask contribution had no chance to show up in the
            // app's stencil and any perf impact attributed to the
            // visibility-mask path is environmental noise.
            if (m_visibilityMaskExtensionGranted) {
                Log(fmt::format(
                    "VisibilityMask: app queried xrGetVisibilityMaskKHR — "
                    "view 0: {} call(s), view 1: {} call(s) over the session\n",
                    m_visibilityMaskQueryCount[0], m_visibilityMaskQueryCount[1]));
            }
            m_visibilityMaskQueryCount.fill(0);
            // Drop any queued visibility-mask events tied to this
            // session — they reference m_session which is about to
            // become invalid.
            m_pendingVisibilityMaskEvents.clear();
            m_session = XR_NULL_HANDLE;
            try {
                m_helmetOverlay.shutdown();
            } catch (const std::exception& exc) {
                ErrorLog(fmt::format("HelmetOverlay::shutdown threw: {}\n", exc.what()));
            }

            return OpenXrApi::xrDestroySession(session);
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEndFrame
        XrResult xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) override {
            if (!frameEndInfo || frameEndInfo->type != XR_TYPE_FRAME_END_INFO) {
                return XR_ERROR_VALIDATION_FAILURE;
            }

            // Fast path: nothing to add — forward untouched so the original
            // layer array (which may live in the app's stack) is not copied.
            const XrCompositionLayerBaseHeader* helmetLayer = nullptr;
            const bool haveHelmet =
                !m_bypassApiLayer && m_helmetOverlay.isArmed() &&
                m_helmetOverlay.appendLayer(frameEndInfo->displayTime, &helmetLayer) &&
                helmetLayer != nullptr;

            // One-shot diagnostic: dump every composition layer being
            // submitted on the first frame of the session. Catches
            // XR_ERROR_SWAPCHAIN_RECT_INVALID and similar by showing
            // exactly which layer (the app's projection, the app's
            // own quads, or our helmet) has a suspect imageRect /
            // swapchain handle. Reset to false in xrDestroySession so
            // the next session also gets one snapshot.
            if (!m_endFrameDiagLogged) {
                m_endFrameDiagLogged = true;
                Log(fmt::format(
                    "xrEndFrame[first frame of session]: appLayerCount={}, helmetAppended={}\n",
                    frameEndInfo->layerCount, haveHelmet ? "yes" : "no"));
                for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i) {
                    const auto* layer = frameEndInfo->layers ? frameEndInfo->layers[i] : nullptr;
                    if (!layer) {
                        Log(fmt::format("  appLayer[{}] = NULL\n", i));
                        continue;
                    }
                    if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                        const auto* p =
                            reinterpret_cast<const XrCompositionLayerProjection*>(layer);
                        Log(fmt::format(
                            "  appLayer[{}] type=PROJECTION viewCount={} flags=0x{:x}\n",
                            i, p->viewCount, p->layerFlags));
                        for (uint32_t v = 0; v < p->viewCount && p->views; ++v) {
                            const auto& view = p->views[v];
                            Log(fmt::format(
                                "    view[{}] swap={:p} rect=offset({},{}) extent({}x{}) "
                                "arrayIdx={}\n",
                                v, (void*)view.subImage.swapchain,
                                view.subImage.imageRect.offset.x,
                                view.subImage.imageRect.offset.y,
                                view.subImage.imageRect.extent.width,
                                view.subImage.imageRect.extent.height,
                                view.subImage.imageArrayIndex));
                        }
                    } else if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
                        const auto* q =
                            reinterpret_cast<const XrCompositionLayerQuad*>(layer);
                        Log(fmt::format(
                            "  appLayer[{}] type=QUAD swap={:p} rect=offset({},{}) "
                            "extent({}x{}) arrayIdx={} flags=0x{:x}\n",
                            i, (void*)q->subImage.swapchain,
                            q->subImage.imageRect.offset.x,
                            q->subImage.imageRect.offset.y,
                            q->subImage.imageRect.extent.width,
                            q->subImage.imageRect.extent.height,
                            q->subImage.imageArrayIndex, q->layerFlags));
                    } else {
                        Log(fmt::format("  appLayer[{}] type={} (other, not inspected)\n",
                                        i, static_cast<int>(layer->type)));
                    }
                }
                if (haveHelmet) {
                    const auto* q =
                        reinterpret_cast<const XrCompositionLayerQuad*>(helmetLayer);
                    Log(fmt::format(
                        "  helmetLayer       type=QUAD swap={:p} rect=offset({},{}) "
                        "extent({}x{}) arrayIdx={} flags=0x{:x}\n",
                        (void*)q->subImage.swapchain,
                        q->subImage.imageRect.offset.x,
                        q->subImage.imageRect.offset.y,
                        q->subImage.imageRect.extent.width,
                        q->subImage.imageRect.extent.height,
                        q->subImage.imageArrayIndex, q->layerFlags));
                }
            }

            // Detect upstream-corrupt layers that would otherwise force
            // the runtime to fail the WHOLE submission with
            // XR_ERROR_SWAPCHAIN_RECT_INVALID, taking our helmet down
            // with it. Seen on LMU+OpenComposite: a QUAD with
            // extent(WxH) where H is negative — looks like an
            // OpenVR→OpenXR translation forgetting to absolute-value a
            // flipped-Y texture bound. We only flag QUADs here because
            // that's the only failure mode we've observed in the wild;
            // PROJECTION views with bad rects would need per-view
            // surgery (clone the views array) which we keep out for
            // now.
            const auto isQuadRectInvalid = [](const XrCompositionLayerBaseHeader* layer) {
                if (!layer || layer->type != XR_TYPE_COMPOSITION_LAYER_QUAD) return false;
                const auto* q = reinterpret_cast<const XrCompositionLayerQuad*>(layer);
                return q->subImage.imageRect.extent.width <= 0 ||
                       q->subImage.imageRect.extent.height <= 0;
            };

            bool anyStrippable = false;
            if (frameEndInfo->layers) {
                for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i) {
                    if (isQuadRectInvalid(frameEndInfo->layers[i])) {
                        anyStrippable = true;
                        break;
                    }
                }
            }

            // Fast path: nothing to strip and nothing to add — forward
            // untouched so the original layer array (which may live on
            // the app's stack) is not copied.
            if (!haveHelmet && !anyStrippable) {
                return OpenXrApi::xrEndFrame(session, frameEndInfo);
            }

            // Slow path: copy the app's layer pointers into our own
            // array, drop the invalid ones, append the helmet if armed.
            // OpenXR composition is back-to-front, so the helmet last
            // puts it in front of everything the game submitted.
            std::vector<const XrCompositionLayerBaseHeader*> patched;
            patched.reserve(frameEndInfo->layerCount + 1u);
            for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i) {
                const auto* layer = frameEndInfo->layers ? frameEndInfo->layers[i] : nullptr;
                if (isQuadRectInvalid(layer)) {
                    if (!m_strippedAppLayerLogged) {
                        m_strippedAppLayerLogged = true;
                        const auto* q =
                            reinterpret_cast<const XrCompositionLayerQuad*>(layer);
                        Log(fmt::format(
                            "xrEndFrame: stripped appLayer[{}] (QUAD) — invalid rect "
                            "extent({}x{}). Likely upstream submitter bug (e.g. "
                            "OpenComposite translating an OpenVR flipped-Y bound to "
                            "a negative XrExtent). The rest of the frame composites "
                            "normally so the helmet still renders.\n",
                            i, q->subImage.imageRect.extent.width,
                            q->subImage.imageRect.extent.height));
                    }
                    continue;
                }
                patched.push_back(layer);
            }
            if (haveHelmet) patched.push_back(helmetLayer);

            XrFrameEndInfo patchedInfo = *frameEndInfo;
            patchedInfo.layerCount = static_cast<uint32_t>(patched.size());
            patchedInfo.layers = patched.data();

            return OpenXrApi::xrEndFrame(session, &patchedInfo);
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrGetVisibilityMaskKHR
        XrResult xrGetVisibilityMaskKHR(XrSession session,
                                        XrViewConfigurationType viewConfigurationType,
                                        uint32_t viewIndex,
                                        XrVisibilityMaskTypeKHR visibilityMaskType,
                                        XrVisibilityMaskKHR* visibilityMask) override {
            // Tally inbound calls. Counted before the early bail-outs
            // below because the diagnostic is "did the app ask?",
            // independent of whether we contribute. Both probe and
            // fetch increment — a typical app shows a count of 2
            // per eye over the session lifetime.
            if (viewIndex < kMaxVisibilityViews) {
                ++m_visibilityMaskQueryCount[viewIndex];
            }

            // Always forward first — the runtime fills its own mesh
            // (lens occlusion / outside-FOV pixels) and crucially the
            // *Output count fields, which our append logic reads.
            const XrResult result = OpenXrApi::xrGetVisibilityMaskKHR(
                session, viewConfigurationType, viewIndex, visibilityMaskType, visibilityMask);
            if (XR_FAILED(result) || !visibilityMask) return result;

            // Bail out early on every situation where we have no
            // contribution to make. Each branch returns the runtime's
            // result unchanged.
            if (!m_visibilityMaskExtensionGranted) return result;
            if (!m_helmetConfig.use_visibility_mask) return result;
            if (!m_visibilityMask.isInitialized()) return result;
            if (!m_eyePoseCacheValid) return result;
            if (visibilityMaskType != XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR) return result;
            if (viewIndex >= kMaxVisibilityViews) return result;

            // Build the helmet NDC mesh for this eye if it hasn't
            // been built yet (or was invalidated by live-edit). The
            // built flag survives across xrGetVisibilityMaskKHR
            // calls until either xrDestroySession or a live-edit
            // geometry change clears it. Rebuild itself is cheap
            // (16 vertex projections) but going through it 2-4 times
            // per session for nothing was wasteful.
            if (!m_visibilityMaskBuilt[viewIndex]) {
                m_visibilityMask.rebuildForView(viewIndex,
                                                m_eyeInViewPoses[viewIndex],
                                                m_eyeFovs[viewIndex],
                                                m_helmetConfig);
                m_visibilityMaskBuilt[viewIndex] = true;
            }
            const auto& helmet = m_visibilityMask.meshForView(viewIndex);
            const uint32_t helmetVerts = static_cast<uint32_t>(helmet.vertices.size());
            const uint32_t helmetIdxs  = static_cast<uint32_t>(helmet.indices.size());
            if (helmetVerts == 0 || helmetIdxs == 0) return result;

            const uint32_t runtimeVerts = visibilityMask->vertexCountOutput;
            const uint32_t runtimeIdxs  = visibilityMask->indexCountOutput;
            const uint32_t totalVerts   = runtimeVerts + helmetVerts;
            const uint32_t totalIdxs    = runtimeIdxs  + helmetIdxs;

            // Two-call probe: app passes capacity 0 to learn the
            // required size. Report the merged size and stop.
            if (visibilityMask->vertexCapacityInput == 0 ||
                visibilityMask->indexCapacityInput == 0) {
                visibilityMask->vertexCountOutput = totalVerts;
                visibilityMask->indexCountOutput  = totalIdxs;
                return XR_SUCCESS;
            }

            // Fetch call: runtime already wrote its part into the
            // buffers. We append ours after it. Buffers must be sized
            // for the merged total — if not, surface the standard
            // SIZE_INSUFFICIENT and let the app re-probe.
            if (visibilityMask->vertexCapacityInput < totalVerts ||
                visibilityMask->indexCapacityInput  < totalIdxs) {
                visibilityMask->vertexCountOutput = totalVerts;
                visibilityMask->indexCountOutput  = totalIdxs;
                return XR_ERROR_SIZE_INSUFFICIENT;
            }

            if (visibilityMask->vertices && helmetVerts > 0) {
                std::memcpy(visibilityMask->vertices + runtimeVerts,
                            helmet.vertices.data(),
                            sizeof(XrVector2f) * helmetVerts);
            }
            if (visibilityMask->indices && helmetIdxs > 0) {
                // Helmet indices are local to its own vertex array —
                // shift them by the runtime's vertex count so they
                // address the appended block correctly.
                for (uint32_t i = 0; i < helmetIdxs; ++i) {
                    visibilityMask->indices[runtimeIdxs + i] =
                        helmet.indices[i] + runtimeVerts;
                }
            }
            visibilityMask->vertexCountOutput = totalVerts;
            visibilityMask->indexCountOutput  = totalIdxs;
            return XR_SUCCESS;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrPollEvent
        XrResult xrPollEvent(XrInstance instance, XrEventDataBuffer* eventData) override {
            // If we have a pending visibility-mask-changed event,
            // deliver it before forwarding to the runtime. xrPollEvent
            // returns one event per call — apps poll in a loop until
            // XR_EVENT_UNAVAILABLE, so they will pick up our event and
            // then go on to drain runtime events on subsequent calls.
            // Order between our events and runtime events doesn't carry
            // semantic meaning, only "every event eventually surfaces"
            // does, so always-front is fine.
            if (eventData && !m_pendingVisibilityMaskEvents.empty()) {
                const XrEventDataVisibilityMaskChangedKHR ev =
                    m_pendingVisibilityMaskEvents.front();
                m_pendingVisibilityMaskEvents.erase(m_pendingVisibilityMaskEvents.begin());
                // XrEventDataBuffer is the spec's polymorphic carrier
                // (256 bytes); writing our smaller struct via memcpy
                // is safe and matches how runtimes typically populate
                // it.
                std::memcpy(eventData, &ev, sizeof(ev));
                return XR_SUCCESS;
            }
            return OpenXrApi::xrPollEvent(instance, eventData);
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEnumerateViewConfigurationViews
        XrResult xrEnumerateViewConfigurationViews(XrInstance instance,
                                                   XrSystemId systemId,
                                                   XrViewConfigurationType viewConfigurationType,
                                                   uint32_t viewCapacityInput,
                                                   uint32_t* viewCountOutput,
                                                   XrViewConfigurationView* views) override {
            TraceLoggingWrite(g_traceProvider,
                              "xrEnumerateViewConfigurationViews",
                              TLXArg(instance, "Instance"),
                              TLArg((int)systemId, "SystemId"),
                              TLArg((int)viewConfigurationType, "ViewConfigurationType"),
                              TLArg(viewCapacityInput, "ViewCapacityInput"));

            const XrResult result = OpenXrApi::xrEnumerateViewConfigurationViews(
                instance, systemId, viewConfigurationType, viewCapacityInput, viewCountOutput, views);

            if (XR_SUCCEEDED(result) && m_config.enabled && views && viewCapacityInput > 0) {
                for (uint32_t i = 0; i < *viewCountOutput && i < viewCapacityInput; i++) {
                    const uint32_t origWidth = views[i].recommendedImageRectWidth;
                    const uint32_t origHeight = views[i].recommendedImageRectHeight;

                    const Extent2D scaled = scaleSwapchainExtents(origWidth, origHeight, m_config);

                    views[i].recommendedImageRectWidth = scaled.width;
                    views[i].recommendedImageRectHeight = scaled.height;

                    Log(fmt::format("View[{}] resolution: {}x{} -> {}x{}\n",
                                     i, origWidth, origHeight, scaled.width, scaled.height));
                    TraceLoggingWrite(g_traceProvider,
                                      "xrEnumerateViewConfigurationViews",
                                      TLArg(i, "ViewIndex"),
                                      TLArg(origWidth, "OrigWidth"),
                                      TLArg(origHeight, "OrigHeight"),
                                      TLArg(scaled.width, "NewWidth"),
                                      TLArg(scaled.height, "NewHeight"));
                }
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrLocateViews
        XrResult xrLocateViews(XrSession session,
                               const XrViewLocateInfo* viewLocateInfo,
                               XrViewState* viewState,
                               uint32_t viewCapacityInput,
                               uint32_t* viewCountOutput,
                               XrView* views) override {
            // ---- Live-edit: periodic config reload --------------------------
            // xrLocateViews is the hot path — called ~once per frame. Wrap the
            // poll here (rather than a dedicated xrEndFrame override) so the
            // user can tune the crop factors mid-session without restarting
            // the game. Swapchain dims stay fixed (allocated at session start)
            // so live_edit only affects the per-frame FOV narrowing.
            if (m_config.liveEdit && ++m_liveEditFrameCounter % kLiveEditCheckInterval == 0) {
                try {
                    const auto mtime = std::filesystem::last_write_time(m_configFilePath);
                    if (mtime != m_configLastWriteTime) {
                        m_configLastWriteTime = mtime;
                        m_config = openxr_api_layer::loadConfig(m_configFilePath, m_appName);

                        // Snapshot the helmet geometry + the
                        // use_visibility_mask flag before reload so we
                        // can decide whether the visibility-mask cache
                        // needs to be invalidated.
                        const float oldDistance = m_helmetConfig.distance_m;
                        const float oldFov      = m_helmetConfig.horizontal_fov_deg;
                        const float oldOffset   = m_helmetConfig.vertical_offset_deg;
                        const bool  oldUseMask  = m_helmetConfig.use_visibility_mask;

                        m_helmetConfig = openxr_api_layer::loadHelmetConfig(m_configFilePath);
                        // Push the new helmet tunables into the live
                        // overlay so distance_m / horizontal_fov_deg
                        // changes are visible without a session restart.
                        // Toggling enabled, replacing the PNG, or
                        // changing brightness still requires a restart —
                        // those would need swapchain / staging-texture
                        // reallocation (see HelmetOverlay::updateLiveTunables).
                        m_helmetOverlay.updateLiveTunables(m_helmetConfig);
                        ++m_configGen;

                        // Visibility-mask cache invalidation: any of the
                        // three geometry knobs changing OR a toggle of
                        // use_visibility_mask makes the app's stencil
                        // out-of-date. Clear the built flags so the
                        // next xrGetVisibilityMaskKHR recomputes;
                        // queue per-view CHANGED events so apps that
                        // listen on xrPollEvent know to re-query.
                        // Brightness / image / enabled don't affect
                        // mask geometry, so they don't trigger this.
                        const bool geomChanged =
                            std::abs(oldDistance - m_helmetConfig.distance_m) > 1e-4f ||
                            std::abs(oldFov - m_helmetConfig.horizontal_fov_deg) > 1e-3f ||
                            std::abs(oldOffset - m_helmetConfig.vertical_offset_deg) > 1e-3f;
                        const bool useMaskChanged =
                            oldUseMask != m_helmetConfig.use_visibility_mask;
                        if ((geomChanged || useMaskChanged) &&
                            m_visibilityMaskExtensionGranted &&
                            m_visibilityMask.isInitialized() &&
                            m_session != XR_NULL_HANDLE) {
                            m_visibilityMaskBuilt.fill(false);
                            for (uint32_t i = 0; i < kMaxVisibilityViews; ++i) {
                                XrEventDataVisibilityMaskChangedKHR ev{
                                    XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR};
                                ev.session = m_session;
                                ev.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                                ev.viewIndex = i;
                                // Note: XrEventDataVisibilityMaskChangedKHR
                                // does NOT carry a visibilityMaskType field —
                                // the spec is "this view's mask changed, re-
                                // query whichever types you care about". So
                                // apps that built a HIDDEN_TRIANGLE_MESH
                                // stencil will re-query that type on their
                                // own.
                                m_pendingVisibilityMaskEvents.push_back(ev);
                            }
                            Log(fmt::format(
                                "VisibilityMask: live-edit invalidation "
                                "(geom={}, useMaskToggle={}), queued mask-changed "
                                "events for both eyes\n",
                                geomChanged ? "yes" : "no",
                                useMaskChanged ? "yes" : "no"));
                        }
                    }
                } catch (...) {
                    // File mid-write, locked, or deleted. Skip, try next interval.
                }
            }

            TraceLoggingWrite(g_traceProvider,
                              "xrLocateViews",
                              TLXArg(session, "Session"),
                              TLArg(viewCapacityInput, "ViewCapacityInput"));

            const XrResult result = OpenXrApi::xrLocateViews(
                session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views);

            if (XR_SUCCEEDED(result) && m_config.enabled && views && viewCapacityInput > 0) {
                for (uint32_t i = 0; i < *viewCountOutput && i < viewCapacityInput; i++) {
                    const XrFovf origFov = views[i].fov;

                    // Cache the narrowed FOV per view. narrowFov() does 4 tan +
                    // 4 atan calls and the runtime returns an essentially
                    // constant FOV frame-to-frame (it tracks IPD and runtime
                    // config, not head pose), so this cache hits ~every frame
                    // after the first. Memcmp on the raw XrFovf is correct
                    // because identical bit patterns produce identical atan/tan
                    // outputs — we don't need IEEE equality semantics here,
                    // just "did the input change".
                    XrFovf narrowed;
                    if (i < kMaxCachedViews &&
                        m_fovCache[i].configGen == m_configGen &&
                        std::memcmp(&m_fovCache[i].origFov, &origFov, sizeof(XrFovf)) == 0) {
                        narrowed = m_fovCache[i].narrowedFov;
                    } else {
                        narrowed = narrowFov(origFov, m_config);
                        if (i < kMaxCachedViews) {
                            m_fovCache[i].origFov = origFov;
                            m_fovCache[i].narrowedFov = narrowed;
                            m_fovCache[i].configGen = m_configGen;
                        }
                    }
                    views[i].fov = narrowed;

                    if (!m_fovLogged) {
                        Log(fmt::format("View[{}] FOV: L={:.3f} R={:.3f} U={:.3f} D={:.3f} -> L={:.3f} R={:.3f} U={:.3f} D={:.3f}\n",
                                         i,
                                         origFov.angleLeft, origFov.angleRight, origFov.angleUp, origFov.angleDown,
                                         views[i].fov.angleLeft, views[i].fov.angleRight,
                                         views[i].fov.angleUp, views[i].fov.angleDown));
                    }

                    TraceLoggingWrite(g_traceProvider,
                                      "xrLocateViews",
                                      TLArg(i, "ViewIndex"),
                                      TLArg(views[i].fov.angleLeft, "FovLeft"),
                                      TLArg(views[i].fov.angleRight, "FovRight"),
                                      TLArg(views[i].fov.angleUp, "FovUp"),
                                      TLArg(views[i].fov.angleDown, "FovDown"));
                }
                m_fovLogged = true;
            }

            // Visibility-mask path: snapshot eye-in-view poses + FOVs
            // the first time xrLocateViews succeeds, by re-locating
            // against our own VIEW reference space. Done lazily because
            // we don't have a frame time at session creation; the eye
            // geometry relative to the head is static for a session,
            // so a single snapshot is enough until live-edit
            // invalidates it.
            if (XR_SUCCEEDED(result) &&
                !m_eyePoseCacheValid &&
                m_visibilityMaskExtensionGranted &&
                m_visibilityMask.isInitialized() &&
                m_maskViewSpace != XR_NULL_HANDLE) {
                XrViewLocateInfo myLocate = *viewLocateInfo;
                myLocate.space = m_maskViewSpace;
                XrViewState myState{XR_TYPE_VIEW_STATE};
                std::array<XrView, kMaxVisibilityViews> myViews{};
                for (auto& v : myViews) v.type = XR_TYPE_VIEW;
                uint32_t myCount = 0;
                const XrResult myResult = OpenXrApi::xrLocateViews(
                    session, &myLocate, &myState, kMaxVisibilityViews, &myCount, myViews.data());
                if (XR_SUCCEEDED(myResult) && myCount > 0) {
                    const uint32_t cap = std::min<uint32_t>(myCount, kMaxVisibilityViews);
                    for (uint32_t i = 0; i < cap; ++i) {
                        m_eyeInViewPoses[i] = myViews[i].pose;
                        m_eyeFovs[i] = myViews[i].fov;
                    }
                    m_eyePoseCacheValid = true;
                    Log(fmt::format("VisibilityMask: snapshot {} eye(s) in view space\n", cap));
                }
            }

            return result;
        }

      private:
        bool isSystemHandled(XrSystemId systemId) const {
            return systemId == m_systemId;
        }

        bool m_bypassApiLayer{false};
        XrSystemId m_systemId{XR_NULL_SYSTEM_ID};

        // Crop layer state.
        CropConfig m_config;
        bool m_fovLogged{false};

        // Live-edit: poll the config file for changes every kLiveEditCheckInterval
        // calls to xrLocateViews (~1 s at 90 Hz). Only active when m_config.liveEdit
        // is true. Swapchain dimensions stay fixed (xrEnumerateViewConfigurationViews
        // runs once at session start), so live_edit only picks up FOV factor changes.
        static constexpr uint32_t kLiveEditCheckInterval = 90u;
        uint32_t m_liveEditFrameCounter{0};
        std::filesystem::path m_configFilePath;
        std::filesystem::file_time_type m_configLastWriteTime{};
        std::string m_appName;

        // Per-view cache for narrowFov(). xrLocateViews is externally
        // synchronized (OpenXR spec), so no locking is needed. 4 covers every
        // XrViewConfigurationType currently defined (MONO, STEREO,
        // QUAD_VARJO, STEREO_WITH_FOVEATED_INSET); views beyond that bypass
        // the cache without breaking correctness.
        static constexpr uint32_t kMaxCachedViews = 4u;
        struct FovCacheEntry {
            XrFovf origFov{};
            XrFovf narrowedFov{};
            uint32_t configGen{UINT32_MAX}; // sentinel: never-written entry
        };
        std::array<FovCacheEntry, kMaxCachedViews> m_fovCache{};
        // Bumped every time m_config is (re)loaded, so a live-edit reload
        // forces a full recompute on the next xrLocateViews.
        uint32_t m_configGen{0};

        // Helmet overlay: default-constructed (disabled) until the
        // settings.json loader writes into m_helmetConfig. Owning the
        // overlay by value keeps its lifetime tied to the layer instance.
        HelmetOverlayConfig m_helmetConfig{};
        HelmetOverlay m_helmetOverlay{};

        // Visibility-mask path. m_visibilityMask is the helmet
        // contribution; the runtime's lens occlusion mesh comes from
        // forwarding to OpenXrApi::xrGetVisibilityMaskKHR. Cache of
        // eye poses in VIEW space + their FOV is filled lazily the
        // first time xrLocateViews is called after session start (we
        // need a frame time, which we don't have at session creation).
        HelmetVisibilityMask m_visibilityMask{};
        bool m_visibilityMaskExtensionGranted{false};
        XrSession m_session{XR_NULL_HANDLE};
        XrSpace m_maskViewSpace{XR_NULL_HANDLE};
        static constexpr uint32_t kMaxVisibilityViews = 2;  // stereo
        std::array<XrPosef, kMaxVisibilityViews> m_eyeInViewPoses{};
        std::array<XrFovf, kMaxVisibilityViews> m_eyeFovs{};
        bool m_eyePoseCacheValid{false};
        // Per-view "helmet mesh has been built since last
        // invalidation" flag. xrGetVisibilityMaskKHR rebuilds only
        // when false, then flips to true. Reset to all-false on
        // live-edit geometry changes.
        std::array<bool, kMaxVisibilityViews> m_visibilityMaskBuilt{};
        // One-shot guard for the xrEndFrame layer-dump diagnostic.
        // Set true after the first xrEndFrame of each session, reset
        // to false in xrDestroySession so the next session also gets
        // a single snapshot. Cheap to keep on permanently — the only
        // per-frame cost after the first frame is one bool check.
        bool m_endFrameDiagLogged{false};

        // Companion guard: log a single line per session the first
        // time we strip an invalid app layer in xrEndFrame. Avoids
        // flooding the log when a buggy submitter sends bad rects
        // every frame. Reset alongside m_endFrameDiagLogged.
        bool m_strippedAppLayerLogged{false};

        // Per-view tally of inbound xrGetVisibilityMaskKHR calls.
        // Diagnostic only: lets us tell at session-end whether the
        // app actually consumed our mask contribution. Typical apps
        // call it twice per eye at startup (one probe + one fetch);
        // some apps re-query on visibility-mask events. A zero count
        // means the app never asked — our mask was effectively
        // wallpaper, not stencil input — and the visibility-mask
        // path produces no measurable perf delta on that runtime/app
        // pair regardless of how good the mesh is.
        std::array<uint32_t, kMaxVisibilityViews> m_visibilityMaskQueryCount{};
        // Events queued for the next xrPollEvent calls. Each entry
        // is a XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR ready
        // to be memcpy'd into the app's eventData buffer. Pushed
        // when live-edit invalidates the mask geometry; popped one
        // per xrPollEvent call until empty.
        std::vector<XrEventDataVisibilityMaskChangedKHR> m_pendingVisibilityMaskEvents;
    };

    // This method is required by the framework to instantiate your OpenXrApi implementation.
    OpenXrApi* GetInstance() {
        if (!g_instance) {
            g_instance = std::make_unique<OpenXrLayer>();
        }
        return g_instance.get();
    }

} // namespace openxr_api_layer

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        TraceLoggingRegister(openxr_api_layer::log::g_traceProvider);
        break;

    case DLL_PROCESS_DETACH:
        TraceLoggingUnregister(openxr_api_layer::log::g_traceProvider);
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}
