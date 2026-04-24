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
#
# NOTE: the helmet-overlay feature will eventually need xrCreateSwapchain,
# xrEnumerateSwapchainImages, xrAcquire/Wait/ReleaseSwapchainImage,
# xrCreateReferenceSpace, xrDestroySpace, xrDestroySwapchain. They are
# deliberately NOT requested yet — adding them before the backend is
# implemented makes layer init fail against runtimes / test mocks that
# don't expose them. They will be added alongside the D3D11 renderer.
requested_functions = [
    "xrGetInstanceProperties",
    "xrGetSystemProperties"
]

# The list of OpenXR extensions our layer will either override or use.
extensions = []
