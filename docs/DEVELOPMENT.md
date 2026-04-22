# Development

Notes for contributors and anyone building the layer from source.
End-user install and configuration are in the [README](../README.md).

## What the layer does (internal view)

The layer intercepts exactly two OpenXR calls:

- [`xrEnumerateViewConfigurationViews`](https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrEnumerateViewConfigurationViews) —
  scales down the `recommendedImageRectWidth` / `recommendedImageRectHeight`
  returned to the application, so smaller swapchains are allocated and the
  app renders fewer pixels per frame.
- [`xrLocateViews`](https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrLocateViews) —
  narrows the `fov.angle{Left,Right,Up,Down}` values the app uses to build
  its projection matrix. The app forwards this narrowed fov to
  `xrEndFrame` naturally (via `XrCompositionLayerProjectionView::fov`), so
  the active runtime composites the image with the same frustum the app
  rendered — the black bars around the content come from the compositor
  placing that narrower frustum inside the HMD's native FOV.

Everything else (including `xrEndFrame`) passes through untouched. An
earlier design also rewrote `xrEndFrame` and tracked swapchains — we
dropped it when we confirmed the single-application path (see below)
produces correct geometry on reprojection and saves ~20pp more GPU for
the same visible crop.

## The crop math (why tan, why average)

**Factors, not percents.** A `CropConfig` stores four per-edge factors
in `[0, 1]` (one per edge). `clampFactor` converts the user-facing
`crop_*_percent` (0-50, "fraction of the image covered by the bar on
that edge") to a factor via `1 - percent/50`: 0% → factor 1.0 (no
crop), 50% → factor 0.0 (bar reaches the image center).

**Narrowing is done in tan-space, not angle-space.**
`narrowFov(fov, cfg)` computes each output edge as:

```
out.angleX = atan( tan(in.angleX) * cfg.cropXFactor )
```

The `tan` is there because an OpenXR projection is a perspective
projection: swapchain pixels are uniform in `tan(angle)`, not in angle
itself. Scaling `tan(angle)` directly by the factor is what makes a
config of `50%` land the bar exactly at the image center — if we
scaled the raw angle instead, the bar would drift off-center as the
factor gets smaller (non-linearly, because `tan` is super-linear near
π/2).

Both `tan` and `atan` are odd functions, so the OpenXR sign convention
(`angleLeft`/`angleDown` negative, `angleRight`/`angleUp` positive) is
preserved automatically. At the boundary factor = 0 the result is
exactly 0 for every edge, which is the "bar at middle" configuration
for that side.

**Swapchain scaling uses the per-axis average.**
`scaleSwapchainExtents` picks a single width factor and a single height
factor out of the four per-edge ones:

```
widthFactor  = (cropLeftFactor + cropRightFactor) / 2
heightFactor = (cropTopFactor  + cropBottomFactor) / 2
```

Average (not min, not max) is the best simple choice here:

- For a symmetric HMD FOV the average matches the tan-extent of the
  narrowed frustum exactly, so pixel density stays native (1:1).
- For strongly asymmetric FOVs (Pimax canted, WMR off-axis) the
  average can under- or over-provision by up to ~25% — acceptable as a
  conservative default; the alternative is plumbing the raw fov into
  this pure helper, which we explicitly avoid to keep `crop_math.h`
  dependency-free and trivially unit-testable.
- Using `min` would collapse the swapchain to zero as soon as one
  edge hits factor 0 (e.g. `crop_bottom_percent: 50`), which crashed
  the Pimax runtime on Le Mans Ultimate — the regression is pinned
  by the `scaleSwapchainExtents: factor 0 on a single edge` unit test
  and the `crop_bottom_percent 50 ... end-to-end` integration test.

The result is always aligned down to a multiple of 8 pixels with 8 as a
floor, so BC-compressed formats, tiled memory layouts, and the usual
DLSS/FSR tile sizes stay well-behaved, and no downstream runtime ever
receives a zero-dimension swapchain.

**Unused-but-kept helper: `computeCroppedImageRect`.**
`crop_math.h` still exposes `computeCroppedImageRect`, which maps the
narrowed tan-fov onto a `XrRect2Di` inside the swapchain. The layer no
longer uses it (that was the `xrEndFrame` sub-image path we removed),
but it's kept as a reusable utility with full unit-test coverage in
case we ever need a defensive `xrEndFrame` override for apps that
don't forward the `xrLocateViews` fov.

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
