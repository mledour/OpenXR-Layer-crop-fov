# The list of OpenXR functions our layer will override.
override_functions = [
    "xrGetSystem",
    "xrCreateSession",
    "xrDestroySession",
    "xrEnumerateViewConfigurationViews",
    "xrLocateViews",
    "xrEndFrame"
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
    "xrReleaseSwapchainImage"
]

# The list of OpenXR extensions our layer will either override or use.
extensions = []
