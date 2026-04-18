// MIT License
//
// Copyright (c) 2026 mledour
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
#include <utils/name_utils.h>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

namespace openxr_api_layer {

    using namespace log;

    // Our API layer implement these extensions, and their specified version.
    const std::vector<std::pair<std::string, uint32_t>> advertisedExtensions = {};

    // Initialize these vectors with arrays of extensions to block and implicitly request for the instance.
    const std::vector<std::string> blockedExtensions = {};
    const std::vector<std::string> implicitExtensions = {};

    // CropConfig, clampFactor, scaleSwapchainExtents, computeCroppedImageRect,
    // and narrowFov live in <utils/crop_math.h> so they can be unit-tested
    // from a standalone binary without linking the layer DLL.

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
            << "  \"live_edit\": false\n"
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

            // The singleton OpenXrLayer outlives a single XrInstance in some
            // scenarios (notably the CTS, which creates and destroys instances
            // back to back). If the previous instance left entries behind
            // (e.g. app skipped xrDestroySession, or the runtime tore things
            // down out of band) a handle value could be reused by the runtime
            // for this new instance and collide. Start every instance with a
            // clean map.
            {
                std::lock_guard<std::mutex> lock(m_swapchainMapMutex);
                m_swapchainInfoMap.clear();
            }

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

            m_config = openxr_api_layer::loadConfig(m_configFilePath, m_appName);
            if (!m_config.enabled) {
                Log(fmt::format("{} is disabled in {}\n", LayerName, m_configFilePath.string()));
                m_bypassApiLayer = true;
            }

            // If live_edit is on, record the per-app config's mtime so
            // xrEndFrame can detect changes without recomputing the path
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
            }

            return result;
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
            TraceLoggingWrite(g_traceProvider,
                              "xrLocateViews",
                              TLXArg(session, "Session"),
                              TLArg(viewCapacityInput, "ViewCapacityInput"));

            const XrResult result = OpenXrApi::xrLocateViews(
                session, viewLocateInfo, viewState, viewCapacityInput, viewCountOutput, views);

            if (XR_SUCCEEDED(result) && m_config.enabled && views && viewCapacityInput > 0) {
                for (uint32_t i = 0; i < *viewCountOutput && i < viewCapacityInput; i++) {
                    const XrFovf origFov = views[i].fov;

                    views[i].fov = narrowFov(origFov, m_config);

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

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrCreateSwapchain
        XrResult xrCreateSwapchain(XrSession session,
                                   const XrSwapchainCreateInfo* createInfo,
                                   XrSwapchain* swapchain) override {
            TraceLoggingWrite(g_traceProvider,
                              "xrCreateSwapchain",
                              TLXArg(session, "Session"),
                              TLArg(createInfo->width, "Width"),
                              TLArg(createInfo->height, "Height"),
                              TLArg(createInfo->format, "Format"),
                              TLArg(createInfo->usageFlags, "UsageFlags"),
                              TLArg(createInfo->sampleCount, "SampleCount"),
                              TLArg(createInfo->arraySize, "ArraySize"));

            const XrResult result = OpenXrApi::xrCreateSwapchain(session, createInfo, swapchain);
            if (XR_SUCCEEDED(result)) {
                // The caller's `next` chain points to extension structs that are
                // typically on the caller's stack: after xrCreateSwapchain returns
                // the runtime has consumed them and those pointers may be
                // reused/freed. We only need width/height/format later, so null
                // `next` defensively before we persist the struct.
                XrSwapchainCreateInfo snapshot = *createInfo;
                snapshot.next = nullptr;

                std::lock_guard<std::mutex> lock(m_swapchainMapMutex);
                m_swapchainInfoMap[*swapchain] = SwapchainEntry{session, snapshot};

                Log(fmt::format("Swapchain created: {}x{} format={}\n",
                                 createInfo->width, createInfo->height, createInfo->format));
                TraceLoggingWrite(g_traceProvider,
                                  "xrCreateSwapchain",
                                  TLXArg(*swapchain, "Swapchain"));
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrDestroySession
        XrResult xrDestroySession(XrSession session) override {
            TraceLoggingWrite(g_traceProvider, "xrDestroySession", TLXArg(session, "Session"));

            const XrResult result = OpenXrApi::xrDestroySession(session);
            if (XR_SUCCEEDED(result)) {
                // Per the spec, the app is supposed to destroy every swapchain
                // before destroying the session, but buggy apps skip this and
                // some runtimes also invalidate swapchains implicitly without
                // routing xrDestroySwapchain through the layer chain. Purge any
                // entries we still hold for this session so a handle reused by
                // the runtime for a later session cannot collide.
                std::lock_guard<std::mutex> lock(m_swapchainMapMutex);
                size_t erased = 0;
                for (auto it = m_swapchainInfoMap.begin(); it != m_swapchainInfoMap.end(); ) {
                    if (it->second.session == session) {
                        it = m_swapchainInfoMap.erase(it);
                        ++erased;
                    } else {
                        ++it;
                    }
                }
                if (erased > 0) {
                    Log(fmt::format("xrDestroySession: purged {} orphan swapchain entr{}\n",
                                     erased, erased == 1 ? "y" : "ies"));
                }
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrDestroySwapchain
        XrResult xrDestroySwapchain(XrSwapchain swapchain) override {
            TraceLoggingWrite(g_traceProvider, "xrDestroySwapchain", TLXArg(swapchain, "Swapchain"));

            const XrResult result = OpenXrApi::xrDestroySwapchain(swapchain);
            if (XR_SUCCEEDED(result)) {
                std::lock_guard<std::mutex> lock(m_swapchainMapMutex);
                m_swapchainInfoMap.erase(swapchain);
            }

            return result;
        }

        // https://www.khronos.org/registry/OpenXR/specs/1.0/html/xrspec.html#xrEndFrame
        XrResult xrEndFrame(XrSession session, const XrFrameEndInfo* frameEndInfo) override {
            // ---- Live-edit: periodic config reload --------------------------
            // Runs before the enabled check so the user can re-enable the
            // layer mid-session (though swapchain dims stay fixed — see README).
            if (m_config.liveEdit && ++m_liveEditFrameCounter % kLiveEditCheckInterval == 0) {
                try {
                    const auto mtime = std::filesystem::last_write_time(m_configFilePath);
                    if (mtime != m_configLastWriteTime) {
                        m_configLastWriteTime = mtime;
                        m_config = openxr_api_layer::loadConfig(m_configFilePath, m_appName);
                        // If the reloaded config turned live_edit off, this is
                        // the last reload — the next frame skips the check.
                    }
                } catch (...) {
                    // File mid-write, locked, or deleted. Skip, try next interval.
                }
            }

            if (!m_config.enabled || !frameEndInfo || frameEndInfo->layerCount == 0) {
                return OpenXrApi::xrEndFrame(session, frameEndInfo);
            }

            XrFrameEndInfo modifiedFrameEndInfo = *frameEndInfo;

            std::vector<const XrCompositionLayerBaseHeader*> modifiedLayerPointers;
            std::vector<XrCompositionLayerProjection> modifiedProjectionLayers;
            std::vector<std::vector<XrCompositionLayerProjectionView>> modifiedViewsArrays;

            modifiedProjectionLayers.reserve(frameEndInfo->layerCount);
            modifiedViewsArrays.reserve(frameEndInfo->layerCount);

            for (uint32_t i = 0; i < frameEndInfo->layerCount; i++) {
                const auto* layer = frameEndInfo->layers[i];

                if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                    const auto* projLayer = reinterpret_cast<const XrCompositionLayerProjection*>(layer);

                    std::vector<XrCompositionLayerProjectionView> views(
                        projLayer->views, projLayer->views + projLayer->viewCount);

                    for (auto& view : views) {
                        // Capture the app's rendered FOV BEFORE we narrow it.
                        // computeCroppedImageRect needs the rendered FOV (not
                        // the soon-to-be-submitted one) to know how pixels
                        // map to tan-space on the existing swapchain.
                        const XrFovf renderedFov = view.fov;

                        // Look up swapchain dimensions.
                        XrSwapchainCreateInfo swapInfo{};
                        {
                            std::lock_guard<std::mutex> lock(m_swapchainMapMutex);
                            auto it = m_swapchainInfoMap.find(view.subImage.swapchain);
                            if (it != m_swapchainInfoMap.end()) {
                                swapInfo = it->second.createInfo;
                            }
                        }

                        if (swapInfo.width > 0 && swapInfo.height > 0) {
                            const XrRect2Di cropped = computeCroppedImageRect(
                                swapInfo.width, swapInfo.height, renderedFov, m_config);
                            if (cropped.extent.width > 0 && cropped.extent.height > 0) {
                                view.subImage.imageRect = cropped;
                            }
                        }

                        // Adjust the FOV in the projection view to match the crop.
                        view.fov = narrowFov(renderedFov, m_config);
                    }

                    modifiedViewsArrays.push_back(std::move(views));

                    XrCompositionLayerProjection modifiedProjLayer = *projLayer;
                    modifiedProjLayer.views = modifiedViewsArrays.back().data();
                    modifiedProjLayer.viewCount = static_cast<uint32_t>(modifiedViewsArrays.back().size());
                    modifiedProjectionLayers.push_back(modifiedProjLayer);

                    modifiedLayerPointers.push_back(
                        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&modifiedProjectionLayers.back()));
                } else {
                    modifiedLayerPointers.push_back(layer);
                }
            }

            modifiedFrameEndInfo.layers = modifiedLayerPointers.data();
            modifiedFrameEndInfo.layerCount = static_cast<uint32_t>(modifiedLayerPointers.size());

            return OpenXrApi::xrEndFrame(session, &modifiedFrameEndInfo);
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
        // frames (~1 s at 90 Hz). Only active when m_config.liveEdit is true.
        static constexpr uint32_t kLiveEditCheckInterval = 90u;
        uint32_t m_liveEditFrameCounter{0};
        std::filesystem::path m_configFilePath;
        std::filesystem::file_time_type m_configLastWriteTime{};
        std::string m_appName;

        // We keep the owning session alongside the createInfo so that when a
        // session is destroyed we can purge its swapchains without walking
        // every app handle.
        struct SwapchainEntry {
            XrSession session;
            XrSwapchainCreateInfo createInfo;
        };
        std::unordered_map<XrSwapchain, SwapchainEntry> m_swapchainInfoMap;
        std::mutex m_swapchainMapMutex;
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
