# Development

Notes for contributors and anyone building the layer from source.
End-user install and configuration are in the [README](../README.md).

## What the layer does (internal view)

The layer intercepts a handful of OpenXR calls:

- [`xrEnumerateViewConfigurationViews`](https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrEnumerateViewConfigurationViews) —
  scales down the `recommendedImageRectWidth` / `recommendedImageRectHeight`
  returned to the application, so smaller swapchains are allocated.
- [`xrLocateViews`](https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrLocateViews) —
  narrows the `fov.angle{Left,Right,Up,Down}` values by the matching ratio so
  the application's projection stays pixel-consistent with the reduced target.
- [`xrEndFrame`](https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrEndFrame) —
  rewrites `XrCompositionLayerProjectionView::fov` and `subImage.imageRect` so
  the active runtime composites the image with the narrowed view frustum and
  sub-region.

Everything else passes through untouched.

## Prerequisites

- Visual Studio 2019 or newer (MSBuild, NuGet)
- Python 3 in `PATH` — required by `openxr-api-layer/framework/dispatch_generator.py`
- Populated OpenXR SDK sources under `external/` (see below)
- Inno Setup 6 (only needed if you want to build the installer locally)

## Relationship to the template

The project is a copy of [`mbucchia/OpenXR-Layer-Template`](https://github.com/mbucchia/OpenXR-Layer-Template)
with the layer-specific edits applied. Upstream changes are pulled into a
pristine vendored copy that lives outside this repository, so we can pick
up fixes without merge conflicts on our divergences.

## Building from source

Populate the `external/` submodules:

```bash
git submodule update --init --recursive
```

Then open `XR_APILAYER_MLEDOUR_fov_crop.sln` in Visual Studio and build
`Release|x64`. A pre-build step runs `openxr-api-layer/framework/dispatch_generator.py`
to regenerate `dispatch.gen.{h,cpp}` from `layer_apis.py`, and a second
pre-build script generates `openxr-api-layer.rc` from the `.rc.in`
template with the current version.

A post-build step runs `scripts\sed.exe` to substitute the `$(SolutionName)`
placeholder into `XR_APILAYER_MLEDOUR_fov_crop.json`, so the layer name
always tracks the `.sln` filename.

The test binary `openxr-api-layer-tests.exe` is built alongside the DLL
and runs unit tests (`crop_math`, `name_utils` helpers) plus integration
tests against an in-process mock OpenXR runtime.

## Releases

The GitHub Actions workflow ([`build-and-release.yml`](../.github/workflows/build-and-release.yml))
builds `Release` and `Debug` x64 on every push to `main` (as a sanity
check) and on every `v*.*.*` tag. On a tag push it additionally creates
a GitHub Release and attaches:

- `XR_APILAYER_MLEDOUR_fov_crop-<version>-x64-Setup.exe` — Inno Setup
  installer (recommended for end users).
- `XR_APILAYER_MLEDOUR_fov_crop-Release-x64.zip` — raw DLL + JSON +
  PowerShell scripts, for manual installation or development.
- `XR_APILAYER_MLEDOUR_fov_crop-Debug-x64.zip` — debug build with full
  symbols, for troubleshooting.

To publish a new release:

```bash
git tag v0.1.0
git push origin v0.1.0
```

The tag version is derived into the DLL's `VERSIONINFO` resource at
build time via [`scripts/Generate-VersionRc.ps1`](../scripts/Generate-VersionRc.ps1),
and into the installer's filename and `AppVersion` via the `/DMyAppVersion`
flag passed to ISCC.

## Code signing

Release binaries are **not** code-signed yet. Anti-cheat systems may
reject unsigned DLLs loaded into OpenXR games; Windows SmartScreen
may flag the installer.

Once approved by the [SignPath Foundation](https://signpath.org/)
open-source program, release builds of the DLL and the `Setup.exe`
installer will be automatically code-signed in CI. The certificate is
issued in the name of "SignPath Foundation" — the signature is what
matters for anti-cheat compatibility and Windows SmartScreen, not the
display name.

Once signing is active, builds triggered from a fork or from a pull
request will still not be signed (GitHub secrets are not exposed to
forked workflows), so only binaries from the [official GitHub Releases
page](../../../releases) of this repository should be trusted.

## License

MIT License — see [LICENSE](../LICENSE).

Based on the [OpenXR-Layer-Template](https://github.com/mbucchia/OpenXR-Layer-Template)
by Matthieu Bucchianeri (`mbucchia`), Copyright © 2022–2023. The
framework code (dispatch generator, entry point, logging, graphics
helpers) is his work; the `fov_crop` logic in `layer.cpp` / `layer.h`
is this project.
