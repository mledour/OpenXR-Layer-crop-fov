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

namespace openxr_api_layer {

    using namespace log;

    // Our API layer implement these extensions, and their specified version.
    const std::vector<std::pair<std::string, uint32_t>> advertisedExtensions = {};

    // Initialize these vectors with arrays of extensions to block and implicitly request for the instance.
    const std::vector<std::string> blockedExtensions = {};
    const std::vector<std::string> implicitExtensions = {};

    // Crop configuration loaded from settings.json.
    struct CropConfig {
        bool enabled = true;
        float cropLeftFactor = 0.90f;   // 1.0 - (10 / 100)
        float cropRightFactor = 0.90f;  // 1.0 - (10 / 100)
        float cropTopFactor = 0.85f;    // 1.0 - (15 / 100)
        float cropBottomFactor = 0.80f; // 1.0 - (20 / 100)
    };

    static float clampFactor(float percent) {
        if (percent < 0.0f) percent = 0.0f;
        if (percent > 50.0f) percent = 50.0f;
        return 1.0f - (percent / 100.0f);
    }

    static float parseFloat(const std::string& line, const std::string& key, float defaultVal) {
        auto pos = line.find(key);
        if (pos == std::string::npos) return defaultVal;
        auto colon = line.find(':', pos + key.size());
        if (colon == std::string::npos) return defaultVal;
        std::string rest = line.substr(colon + 1);
        float val = defaultVal;
        if (sscanf(rest.c_str(), " %f", &val) == 1) {
            return val;
        }
        return defaultVal;
    }

    static bool parseBool(const std::string& line, const std::string& key, bool defaultVal) {
        auto pos = line.find(key);
        if (pos == std::string::npos) return defaultVal;
        auto colon = line.find(':', pos + key.size());
        if (colon == std::string::npos) return defaultVal;
        std::string rest = line.substr(colon + 1);
        return rest.find("true") != std::string::npos;
    }

    static CropConfig loadConfig(const std::filesystem::path& configDir) {
        CropConfig config;
        std::string configPathStr = (configDir / "settings.json").string();

        Log(fmt::format("Looking for config at: {}\n", configPathStr));

        std::string fileContent;
        {
            std::ifstream file(configPathStr);
            if (!file.is_open()) {
                Log("No settings.json found, using defaults\n");
                return config;
            }
            std::ostringstream ss;
            ss << file.rdbuf();
            fileContent = ss.str();
            file.close();
        }

        Log(fmt::format("Config file read OK ({} bytes)\n", fileContent.size()));

        try {
            float leftPct = 10.0f, rightPct = 10.0f, topPct = 15.0f, bottomPct = 20.0f;
            bool enabled = true;

            std::istringstream stream(fileContent);
            std::string line;
            while (std::getline(stream, line)) {
                if (line.find("\"enabled\"") != std::string::npos) {
                    enabled = parseBool(line, "\"enabled\"", true);
                } else if (line.find("\"crop_left_percent\"") != std::string::npos) {
                    leftPct = parseFloat(line, "\"crop_left_percent\"", 10.0f);
                } else if (line.find("\"crop_right_percent\"") != std::string::npos) {
                    rightPct = parseFloat(line, "\"crop_right_percent\"", 10.0f);
                } else if (line.find("\"crop_top_percent\"") != std::string::npos) {
                    topPct = parseFloat(line, "\"crop_top_percent\"", 15.0f);
                } else if (line.find("\"crop_bottom_percent\"") != std::string::npos) {
                    bottomPct = parseFloat(line, "\"crop_bottom_percent\"", 20.0f);
                }
            }

            config.enabled = enabled;
            config.cropLeftFactor = clampFactor(leftPct);
            config.cropRightFactor = clampFactor(rightPct);
            config.cropTopFactor = clampFactor(topPct);
            config.cropBottomFactor = clampFactor(bottomPct);

            Log(fmt::format("Crop config: enabled={}, left={:.1f}, right={:.1f}, top={:.1f}, bottom={:.1f}\n",
                             config.enabled, leftPct, rightPct, topPct, bottomPct));
        } catch (const std::exception& e) {
            Log(fmt::format("Error parsing config: {}, using defaults\n", e.what()));
        } catch (...) {
            Log("Unknown error parsing config, using defaults\n");
        }

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

            OpenXrApi::xrCreateInstance(createInfo);

            TraceLoggingWrite(g_traceProvider,
                              "xrCreateInstance",
                              TLArg(xr::ToString(createInfo->applicationInfo.apiVersion).c_str(), "ApiVersion"),
                              TLArg(createInfo->applicationInfo.applicationName, "ApplicationName"),
                              TLArg(createInfo->applicationInfo.applicationVersion, "ApplicationVersion"),
                              TLArg(createInfo->applicationInfo.engineName, "EngineName"),
                              TLArg(createInfo->applicationInfo.engineVersion, "EngineVersion"),
                              TLArg(createInfo->createFlags, "CreateFlags"));
            Log(fmt::format("Application: {}\n", createInfo->applicationInfo.applicationName));

            if (m_bypassApiLayer) {
                Log(fmt::format("{} layer will be bypassed\n", LayerName));
                return XR_SUCCESS;
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

            // Load crop configuration.
            m_config = openxr_api_layer::loadConfig(localAppData);

            return XR_SUCCESS;
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
                const float widthFactor = std::min(m_config.cropLeftFactor, m_config.cropRightFactor);
                const float heightFactor = std::min(m_config.cropTopFactor, m_config.cropBottomFactor);

                for (uint32_t i = 0; i < *viewCountOutput && i < viewCapacityInput; i++) {
                    const uint32_t origWidth = views[i].recommendedImageRectWidth;
                    const uint32_t origHeight = views[i].recommendedImageRectHeight;

                    uint32_t newWidth = static_cast<uint32_t>(origWidth * widthFactor);
                    uint32_t newHeight = static_cast<uint32_t>(origHeight * heightFactor);
                    newWidth = std::max(newWidth & ~1u, 2u);
                    newHeight = std::max(newHeight & ~1u, 2u);

                    views[i].recommendedImageRectWidth = newWidth;
                    views[i].recommendedImageRectHeight = newHeight;

                    Log(fmt::format("View[{}] resolution: {}x{} -> {}x{}\n", i, origWidth, origHeight, newWidth, newHeight));
                    TraceLoggingWrite(g_traceProvider,
                                      "xrEnumerateViewConfigurationViews",
                                      TLArg(i, "ViewIndex"),
                                      TLArg(origWidth, "OrigWidth"),
                                      TLArg(origHeight, "OrigHeight"),
                                      TLArg(newWidth, "NewWidth"),
                                      TLArg(newHeight, "NewHeight"));
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

                    views[i].fov.angleLeft *= m_config.cropLeftFactor;
                    views[i].fov.angleRight *= m_config.cropRightFactor;
                    views[i].fov.angleUp *= m_config.cropTopFactor;
                    views[i].fov.angleDown *= m_config.cropBottomFactor;

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
                std::lock_guard<std::mutex> lock(m_swapchainMapMutex);
                m_swapchainInfoMap[*swapchain] = *createInfo;

                Log(fmt::format("Swapchain created: {}x{} format={}\n",
                                 createInfo->width, createInfo->height, createInfo->format));
                TraceLoggingWrite(g_traceProvider,
                                  "xrCreateSwapchain",
                                  TLXArg(*swapchain, "Swapchain"));
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
                        // Look up swapchain dimensions.
                        XrSwapchainCreateInfo swapInfo{};
                        {
                            std::lock_guard<std::mutex> lock(m_swapchainMapMutex);
                            auto it = m_swapchainInfoMap.find(view.subImage.swapchain);
                            if (it != m_swapchainInfoMap.end()) {
                                swapInfo = it->second;
                            }
                        }

                        if (swapInfo.width > 0 && swapInfo.height > 0) {
                            const float leftCropPixels = swapInfo.width * (1.0f - m_config.cropLeftFactor);
                            const float rightCropPixels = swapInfo.width * (1.0f - m_config.cropRightFactor);
                            const float topCropPixels = swapInfo.height * (1.0f - m_config.cropTopFactor);
                            const float bottomCropPixels = swapInfo.height * (1.0f - m_config.cropBottomFactor);

                            const int32_t newOffsetX = static_cast<int32_t>(leftCropPixels * 0.5f);
                            const int32_t newOffsetY = static_cast<int32_t>(topCropPixels * 0.5f);
                            const int32_t newWidth = static_cast<int32_t>(swapInfo.width - leftCropPixels * 0.5f - rightCropPixels * 0.5f);
                            const int32_t newHeight = static_cast<int32_t>(swapInfo.height - topCropPixels * 0.5f - bottomCropPixels * 0.5f);

                            if (newWidth > 0 && newHeight > 0) {
                                view.subImage.imageRect.offset.x = newOffsetX;
                                view.subImage.imageRect.offset.y = newOffsetY;
                                view.subImage.imageRect.extent.width = newWidth;
                                view.subImage.imageRect.extent.height = newHeight;
                            }
                        }

                        // Adjust the FOV in the projection view to match the crop.
                        view.fov.angleLeft *= m_config.cropLeftFactor;
                        view.fov.angleRight *= m_config.cropRightFactor;
                        view.fov.angleUp *= m_config.cropTopFactor;
                        view.fov.angleDown *= m_config.cropBottomFactor;
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
        std::unordered_map<XrSwapchain, XrSwapchainCreateInfo> m_swapchainInfoMap;
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
