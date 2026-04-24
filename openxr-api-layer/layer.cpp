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
#include <utils/helmet_overlay.h>
#include <utils/name_utils.h>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

namespace openxr_api_layer {

    using namespace log;

    // Our API layer implement these extensions, and their specified version.
    const std::vector<std::pair<std::string, uint32_t>> advertisedExtensions = {};

    // Initialize these vectors with arrays of extensions to block and implicitly request for the instance.
    const std::vector<std::string> blockedExtensions = {};
    // XR_KHR_composition_layer_equirect2 is requested implicitly so the
    // helmet overlay can map a 360° equirectangular PNG onto a head-
    // locked sphere when the runtime supports it. If it does not, the
    // framework logs "Cannot satisfy implicit extension request" and
    // the overlay falls back to the quad + procedural mask path.
    const std::vector<std::string> implicitExtensions = {
        XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME
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
        out << "{\n";
        if (appName.empty()) {
            out << "  \"_comment\": \"Default template. Each OpenXR application "
                <<                "gets a copy of this file the first time it runs. "
                <<                "Set \\\"enabled\\\" to true to activate the layer for that game "
                <<                "(or flip the default here to affect every future game).\",\n";
        } else {
            out << "  \"_comment\": \"Auto-generated per-app config for '" << appName
                <<                "'. Set \\\"enabled\\\" to true to activate the layer for this game.\",\n";
        }
        out << "  \"enabled\": false,\n"
            << "  \"crop_left_percent\": 10,\n"
            << "  \"crop_right_percent\": 10,\n"
            << "  \"crop_top_percent\": 15,\n"
            << "  \"crop_bottom_percent\": 20,\n"
            << "  \"live_edit\": false,\n"
            << "  \"helmet_overlay\": {\n"
            << "    \"_comment\": \"Draws a motorcycle-helmet-interior mask on top of the game. Rendering backend not yet implemented; enabling this has no effect until a future layer version.\",\n"
            << "    \"enabled\": false,\n"
            << "    \"texture\": \"helmet_visor.png\",\n"
            << "    \"distance_m\": 0.5,\n"
            << "    \"width_m\": 0.6,\n"
            << "    \"height_m\": 0.4\n"
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

    // Parses the "helmet_overlay" object from the same settings.json that
    // loadConfig() reads. Kept as a standalone reader (rather than folded
    // into CropConfig) so the crop and overlay subsystems stay loosely
    // coupled — the overlay is opt-in, optional, and logically distinct
    // from the FOV narrowing. Silently returns defaults (disabled) if the
    // file, the block, or any field is missing or malformed.
    static HelmetOverlayConfig loadHelmetConfig(const std::filesystem::path& configPath) {
        HelmetOverlayConfig hc;

        std::string fileContent;
        {
            std::ifstream file(configPath.string());
            if (!file.is_open()) return hc;
            std::ostringstream ss;
            ss << file.rdbuf();
            fileContent = ss.str();
        }

        rapidjson::Document doc;
        doc.Parse(fileContent.c_str(), fileContent.size());
        if (doc.HasParseError() || !doc.IsObject()) return hc;
        if (!doc.HasMember("helmet_overlay") || !doc["helmet_overlay"].IsObject()) return hc;

        const auto& ho = doc["helmet_overlay"];
        hc.enabled = readJsonBool(ho, "enabled", false);
        hc.distance_m = readJsonFloat(ho, "distance_m", 0.5f);
        hc.width_m = readJsonFloat(ho, "width_m", 0.6f);
        hc.height_m = readJsonFloat(ho, "height_m", 0.4f);
        if (ho.HasMember("texture") && ho["texture"].IsString()) {
            hc.textureRelativePath = ho["texture"].GetString();
        }

        Log(fmt::format("Helmet overlay config: enabled={}, distance={:.2f}m, size={:.2f}x{:.2f}m, texture={}\n",
                         hc.enabled, hc.distance_m, hc.width_m, hc.height_m, hc.textureRelativePath));

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

                // Arm the helmet overlay. Best-practices: any failure
                // here must NEVER crash the host; the overlay degrades
                // to "not armed" and the rest of the layer keeps running.
                // We pass `this` so the overlay can call downstream PFNs
                // (xrCreateSwapchain, xrAcquireSwapchainImage, …) through
                // the layer's own dispatch.
                if (!m_bypassApiLayer) {
                    try {
                        m_helmetOverlay.initialize(this, *session, createInfo->next, m_helmetConfig, dllHome);
                    } catch (const std::exception& exc) {
                        ErrorLog(fmt::format("HelmetOverlay::initialize threw: {}\n", exc.what()));
                    }
                }
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrDestroySession
        XrResult xrDestroySession(XrSession session) override {
            TraceLoggingWrite(g_traceProvider, "xrDestroySession", TLXArg(session, "Session"));

            // Release overlay resources BEFORE forwarding: once the runtime
            // destroys the session, its XrSwapchain / XrSpace handles are
            // invalid and calling the destroy PFNs on them would be UB.
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

            if (!haveHelmet) {
                return OpenXrApi::xrEndFrame(session, frameEndInfo);
            }

            // Append the helmet visor on top of the app's layers. OpenXR
            // composition is strictly back-to-front, so placing our layer
            // last puts it in front of everything the game submitted.
            std::vector<const XrCompositionLayerBaseHeader*> patched;
            patched.reserve(frameEndInfo->layerCount + 1u);
            for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i) {
                patched.push_back(frameEndInfo->layers[i]);
            }
            patched.push_back(helmetLayer);

            XrFrameEndInfo patchedInfo = *frameEndInfo;
            patchedInfo.layerCount = static_cast<uint32_t>(patched.size());
            patchedInfo.layers = patched.data();

            return OpenXrApi::xrEndFrame(session, &patchedInfo);
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
                        // NB: live-edit refresh of the helmet block updates
                        // the cached config, but swapchain/space resources
                        // already created in xrCreateSession are NOT
                        // rebuilt. Toggling enabled, distance, or size will
                        // only fully apply after a session restart. The
                        // stub honours this: it doesn't own runtime
                        // resources yet, so the refresh is cheap here.
                        m_helmetConfig = openxr_api_layer::loadHelmetConfig(m_configFilePath);
                        ++m_configGen;
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
