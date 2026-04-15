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

            // Needed to resolve the requested function pointers.
            OpenXrApi::xrCreateInstance(createInfo);

            // Dump the application name, OpenXR runtime information and other useful things for debugging.
            TraceLoggingWrite(g_traceProvider,
                              "xrCreateInstance",
                              TLArg(xr::ToString(createInfo->applicationInfo.apiVersion).c_str(), "ApiVersion"),
                              TLArg(createInfo->applicationInfo.applicationName, "ApplicationName"),
                              TLArg(createInfo->applicationInfo.applicationVersion, "ApplicationVersion"),
                              TLArg(createInfo->applicationInfo.engineName, "EngineName"),
                              TLArg(createInfo->applicationInfo.engineVersion, "EngineVersion"),
                              TLArg(createInfo->createFlags, "CreateFlags"));
            Log(fmt::format("Application: {}\n", createInfo->applicationInfo.applicationName));

            // Here there can be rules to disable the API layer entirely (based on applicationName for example).
            // m_bypassApiLayer = ...

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

                // Remember the XrSystemId to use.
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
                    // Do something useful here...
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
                              TLArg((uint32_t)viewConfigurationType, "ViewConfigurationType"),
                              TLArg(viewCapacityInput, "ViewCapacityInput"));

            const XrResult result = OpenXrApi::xrEnumerateViewConfigurationViews(
                instance, systemId, viewConfigurationType, viewCapacityInput, viewCountOutput, views);

            // Scale only on the data-returning call (viewCapacityInput > 0), for the handled
            // system, and only while the layer is active. This shrinks the swapchains the
            // application will allocate — fewer pixels shaded per frame.
            if (XR_SUCCEEDED(result) && !m_bypassApiLayer && viewCapacityInput > 0 && views != nullptr &&
                viewCountOutput != nullptr && isSystemHandled(systemId)) {
                for (uint32_t i = 0; i < *viewCountOutput; i++) {
                    const uint32_t origW = views[i].recommendedImageRectWidth;
                    const uint32_t origH = views[i].recommendedImageRectHeight;

                    // Apply H and V scales independently, floor to even for stereo/chroma-friendly alignment.
                    uint32_t newW = static_cast<uint32_t>(origW * m_fovScaleH) & ~1u;
                    uint32_t newH = static_cast<uint32_t>(origH * m_fovScaleV) & ~1u;
                    if (newW < 2) newW = 2;
                    if (newH < 2) newH = 2;

                    views[i].recommendedImageRectWidth = newW;
                    views[i].recommendedImageRectHeight = newH;

                    TraceLoggingWrite(g_traceProvider,
                                      "xrEnumerateViewConfigurationViews_Scaled",
                                      TLArg(i, "ViewIndex"),
                                      TLArg(origW, "OriginalWidth"),
                                      TLArg(origH, "OriginalHeight"),
                                      TLArg(newW, "ScaledWidth"),
                                      TLArg(newH, "ScaledHeight"));
                    Log(fmt::format("View {}: {}x{} -> {}x{} (H={:.2f} V={:.2f})\n",
                                    i,
                                    origW,
                                    origH,
                                    newW,
                                    newH,
                                    m_fovScaleH,
                                    m_fovScaleV));
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

            // Oculus-style FovTangentMultiplier: narrow the frustum by scaling the
            // tangents of the FOV half-angles. The app's projection becomes tighter
            // around the center, and the runtime composites black outside the
            // narrowed cone — visible as black bars around the rendered image in
            // the HMD. Called once per eye per frame; keep this path cheap (no Log()).
            if (XR_SUCCEEDED(result) && !m_bypassApiLayer && viewCapacityInput > 0 && views != nullptr &&
                viewCountOutput != nullptr) {
                const float kH = m_fovScaleH;
                const float kV = m_fovScaleV;
                for (uint32_t i = 0; i < *viewCountOutput; i++) {
                    XrFovf& fov = views[i].fov;
                    fov.angleLeft = std::atan(std::tan(fov.angleLeft) * kH);
                    fov.angleRight = std::atan(std::tan(fov.angleRight) * kH);
                    fov.angleUp = std::atan(std::tan(fov.angleUp) * kV);
                    fov.angleDown = std::atan(std::tan(fov.angleDown) * kV);

                    TraceLoggingWrite(g_traceProvider,
                                      "xrLocateViews_FovScaled",
                                      TLArg(i, "ViewIndex"),
                                      TLArg(fov.angleLeft, "AngleLeft"),
                                      TLArg(fov.angleRight, "AngleRight"),
                                      TLArg(fov.angleUp, "AngleUp"),
                                      TLArg(fov.angleDown, "AngleDown"));
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

        // FOV tangent multipliers, split per axis (Oculus-style FovTangentMultiplier).
        // 1.0f = no change on that axis; < 1.0f narrows the FOV returned from xrLocateViews
        // on that axis AND proportionally shrinks the recommendedImageRect on the same axis.
        // Current defaults: letterbox — full horizontal FOV, 30% vertical tangent crop
        // (top+bottom black bars, ~30% GPU saving from smaller swapchain height).
        // TODO(phase-2.5): load from %LOCALAPPDATA%\XR_APILAYER_MLEDOUR_fov_crop\config.json
        float m_fovScaleH{1.0f};
        float m_fovScaleV{0.70f};
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
