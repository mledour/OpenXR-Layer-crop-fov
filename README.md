# XR_APILAYER_MLEDOUR_fov_crop

An OpenXR API layer for Windows that reduces GPU / CPU load by narrowing the
rendered field of view and / or scaling the recommended swapchain dimensions.
The application submits fewer pixels per frame, so every frame is cheaper.

> Status: **work in progress** — see [`../CLAUDE.md`](../CLAUDE.md) for the
> development plan and project-wide rules.

## What it does

The layer intercepts two OpenXR calls:

- [`xrEnumerateViewConfigurationViews`](https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrEnumerateViewConfigurationViews) —
  scales down the `recommendedImageRectWidth` / `recommendedImageRectHeight`
  returned to the application, so smaller swapchains are allocated.
- [`xrLocateViews`](https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrLocateViews) —
  narrows the `fov.angle{Left,Right,Up,Down}` values by the matching ratio so
  the application's projection stays pixel-consistent with the reduced target.

Everything else passes through untouched, and
[`xrEndFrame`](https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#xrEndFrame)
rewrites `XrCompositionLayerProjectionView::fov` on the way out so the active
runtime composites the image with the narrowed view frustum.

## Prerequisites

- Visual Studio 2019 or newer
- NuGet package manager (installed via Visual Studio Installer)
- Python 3 in `PATH` — required by `framework/dispatch_generator.py`
- Populated OpenXR SDK sources under `external/` (see below)

## Relationship to the template

This folder is a copy of [`../OpenXR-Layer-Template/`](../OpenXR-Layer-Template)
(upstream: [`mbucchia/OpenXR-Layer-Template`](https://github.com/mbucchia/OpenXR-Layer-Template)),
with the layer-specific edits applied. The upstream template is kept pristine
so we can pull its updates cleanly — this folder holds our divergences.

## Building from source

This folder is not a git repo by design. To populate `external/`, either
`git init` here and add the three submodules from `.gitmodules`, or clone
the upstream template sources manually into `external/OpenXR-SDK/`,
`external/OpenXR-SDK-Source/`, and `external/OpenXR-MixedReality/`.

```bat
:: once external/ is populated, on Windows
:: open XR_APILAYER_MLEDOUR_fov_crop.sln in Visual Studio and build Release|x64
```

The post-build step runs `scripts\sed.exe` to substitute the `$(SolutionName)`
placeholder into `openxr-api-layer.json`, so the layer name always tracks the
`.sln` filename.

## Releases

A GitHub Actions workflow ([`build-and-release.yml`](./.github/workflows/build-and-release.yml))
builds x64 `Release` and `Debug` on every push to `main` (as a sanity check)
and on every `v*.*.*` tag (which additionally creates a GitHub Release and
attaches the two ZIPs).

To publish a new release:

```bash
git tag v0.1.0
git push origin v0.1.0
```

Each release ZIP contains the DLL, its PDB, the processed
`openxr-api-layer.json`, and the install / uninstall PowerShell scripts.

## Installing

Either download the `Release-x64` ZIP from the latest
[GitHub Release](../../releases/latest) and unzip it to a permanent location,
or build locally and use `bin\x64\Release\`. Then run `Install-Layer.ps1`
from that directory.

The script writes the layer manifest path under
`HKLM\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit` as required by the
[OpenXR Loader spec](https://registry.khronos.org/OpenXR/specs/1.0/loader.html).

> ⚠️ The current `Install-Layer.ps1` does **not** yet set ACLs for sandboxed
> identities (All Packages / All Restricted Packages) and does **not** sign
> the DLL. Both are acceptance criteria before this layer is ready for
> real users — see rules 3 and 5 in
> [`../docs/openxr_api_layers_best_practices.md`](../docs/openxr_api_layers_best_practices.md).

## Disabling without uninstalling

Set the following environment variable to any value before launching the
OpenXR application:

```
DISABLE_XR_APILAYER_MLEDOUR_fov_crop=1
```

This is the standard OpenXR loader escape hatch. The layer will be skipped
by the loader for that process.

## Configuration

> _Planned:_ a JSON config file under
> `%LOCALAPPDATA%\XR_APILAYER_MLEDOUR_fov_crop\config.json` with fields like
> `resolution_scale` (0.5 – 1.0) and `fov_scale`. Not implemented yet; the
> layer currently ships no user-facing knobs.

## License and attribution

MIT License — see [`LICENSE`](./LICENSE).

Based on the [`OpenXR-Layer-Template`](https://github.com/mbucchia/OpenXR-Layer-Template)
by Matthieu Bucchianeri (`mbucchia`), Copyright © 2022–2023. The framework
code (dispatch generator, entry point, logging, graphics helpers) is his
work; the `fov_crop` logic in `layer.cpp` / `layer.h` is this project.
