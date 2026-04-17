# XR_APILAYER_MLEDOUR_fov_crop

An OpenXR API layer for Windows that reduces GPU / CPU load by narrowing the
rendered field of view and / or scaling the recommended swapchain dimensions.
The application submits fewer pixels per frame, so every frame is cheaper.

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
placeholder into `XR_APILAYER_MLEDOUR_fov_crop.json`, so the layer name always tracks the
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

Each release includes an Inno Setup **installer** (recommended) and raw
**ZIPs** (for manual install or development).

## Installing

### Installer (recommended)

Download `XR_APILAYER_MLEDOUR_fov_crop-<version>-x64-Setup.exe` from the
latest [GitHub Release](../../releases/latest) and run it. Admin elevation
is handled automatically.

The installer:
- Copies the DLL and JSON manifest to
  `C:\Program Files\OpenXR-Layer-fov-crop\`.
- Registers the layer in `HKLM\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit`.
- Creates an entry in Add/Remove Programs for clean uninstall.
- Program Files directory inherits the correct ACLs for sandboxed
  identities (WebXR in Chrome, OpenXR Tools for WMR).

### Manual (ZIP)

Download the `Release-x64` ZIP from the latest
[GitHub Release](../../releases/latest), unzip it to a **permanent**
location (the registry entry points at the DLL on disk, so the folder
must not move), and run `Install-Layer.ps1` from an elevated PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File .\Install-Layer.ps1
```

> ⚠️ Manual installs outside of `C:\Program Files\` may not be readable
> by sandboxed identities. The installer method avoids this issue.

### Uninstalling

- **Installer**: Settings → Apps → XR_APILAYER_MLEDOUR_fov_crop → Uninstall
- **Manual**: run `Uninstall-Layer.ps1` from an elevated PowerShell

> ⚠️ The DLL is **not code-signed**. Anti-cheat systems may reject unsigned
> DLLs loaded into OpenXR games. A signed release is planned.

## Disabling without uninstalling

Set the following environment variable to any value before launching the
OpenXR application:

```
DISABLE_XR_APILAYER_MLEDOUR_fov_crop=1
```

This is the standard OpenXR loader escape hatch. The layer will be skipped
by the loader for that process.

## Configuration

The layer keeps **a separate settings file per OpenXR application** inside
`%LOCALAPPDATA%\XR_APILAYER_MLEDOUR_fov_crop\`. Each file is named after
the application's OpenXR name, sanitized to lowercase + underscores:

| Application name (reported via OpenXR) | Settings file |
|-----------------------------------------|---------------|
| `DiRT Rally 2.0` | `dirt_rally_2_0_settings.json` |
| `Le Mans Ultimate` | `le_mans_ultimate_settings.json` |
| `iRacing Simulator` | `iracing_simulator_settings.json` |
| `hello_xr` | `hello_xr_settings.json` |

**First-run behavior** — when an application is seen for the first time, the
layer creates its per-app file automatically:

1. If `settings.json` (the optional template) exists in the same folder,
   its contents are copied into the new per-app file.
2. Otherwise, built-in defaults are written.

The layer never overwrites an existing per-app file, so once the file is
created you can edit it freely. Each game keeps its own crop values.

The global `settings.json` only acts as a **template** for future per-app
files. Editing it does not affect games that already have their own file.

### File format

```json
{
  "enabled": true,
  "crop_left_percent": 10,
  "crop_right_percent": 10,
  "crop_top_percent": 15,
  "crop_bottom_percent": 20,
  "live_edit": false
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `enabled` | bool | `true` | Master switch. `false` bypasses the layer entirely. |
| `crop_left_percent` | float | `10` | Percentage to crop from the left edge (0-50). |
| `crop_right_percent` | float | `10` | Percentage to crop from the right edge (0-50). |
| `crop_top_percent` | float | `15` | Percentage to crop from the top edge (0-50). |
| `crop_bottom_percent` | float | `20` | Percentage to crop from the bottom edge (0-50). |
| `live_edit` | bool | `false` | When true, the layer re-reads the config file every ~1 second. Set to true before launching the game to tune values in real time; set to false for normal use. |

## License and attribution

MIT License — see [`LICENSE`](./LICENSE).

Based on the [`OpenXR-Layer-Template`](https://github.com/mbucchia/OpenXR-Layer-Template)
by Matthieu Bucchianeri (`mbucchia`), Copyright © 2022–2023. The framework
code (dispatch generator, entry point, logging, graphics helpers) is his
work; the `fov_crop` logic in `layer.cpp` / `layer.h` is this project.
