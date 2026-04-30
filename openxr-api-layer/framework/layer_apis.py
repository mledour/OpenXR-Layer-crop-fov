# The list of OpenXR functions our layer will override.
override_functions = [
    "xrGetSystem",
    "xrCreateSession",
    "xrDestroySession",
    "xrEnumerateViewConfigurationViews",
    "xrLocateViews",
    "xrEndFrame",
    # The visibility-mask path augments the runtime-supplied "hidden
    # triangle mesh" with the helmet's opaque silhouette so apps that
    # consume XR_KHR_visibility_mask stencil-reject those pixels and
    # save shading work. xrPollEvent is overridden so we can inject
    # XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR when live-edit
    # changes the mask geometry.
    "xrGetVisibilityMaskKHR",
    "xrPollEvent"
]

# The list of OpenXR functions our layer will use from the runtime.
# Might repeat entries from override_functions above.
requested_functions = [
    "xrGetInstanceProperties",
    "xrGetSystemProperties",
    # Helmet overlay needs its own swapchain + head-locked reference space
    # and acquires/releases images per frame. Requesting them at layer
    # init is safe against any conformant runtime; the mock runtime in
    # openxr-api-layer-tests stubs them out.
    "xrCreateReferenceSpace",
    "xrDestroySpace",
    "xrEnumerateSwapchainFormats",
    "xrCreateSwapchain",
    "xrDestroySwapchain",
    "xrEnumerateSwapchainImages",
    "xrAcquireSwapchainImage",
    "xrWaitSwapchainImage",
    "xrReleaseSwapchainImage",
    # Even though we override these, request them from the runtime so the
    # generated dispatch table has m_xrFoo populated for the forward path
    # (we always call OpenXrApi::xrFoo to get the runtime's data first,
    # then merge our own contribution).
    "xrGetVisibilityMaskKHR",
    "xrPollEvent"
]

# The list of OpenXR extensions our layer will either override or use.
# XR_KHR_visibility_mask: required for xrGetVisibilityMaskKHR symbol
# resolution. Listed here so the auto-generated dispatch knows it's an
# extension function (rather than a core one). Run-time enabling is
# handled separately via implicitExtensions in layer.cpp.
extensions = ["XR_KHR_visibility_mask"]
