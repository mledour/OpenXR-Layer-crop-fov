# XR_APILAYER_MLEDOUR_fov_crop

An OpenXR API layer for Windows with two features, both **enabled by
default** in the shipped template:

1. **FOV crop** — narrows the effective field of view and swapchain
   resolution. Your game renders fewer pixels per frame; the saved
   GPU goes into FPS, super-sampling, or higher graphics settings.
   Cost: slightly narrower peripheral vision, visible as black edges.
2. **Helmet overlay** — composites a head-locked PNG of a helmet
   interior on top of the game, with a transparent visor cutout.
   Adds the "looking through a helmet" feel to sims that don't model
   one. ~+1 % GPU when enabled, strictly 0 when disabled.

Works with any OpenXR application and runtime. No game or headset
modification required.

> Release binaries are signed with a **Certum Open Source Code Signing
> Cloud** certificate. SmartScreen may still warn on early downloads
> until publisher reputation builds. Anti-cheat can flag any layer DLL
> — even a signed one — when loaded into a hooked game. CI flow and
> verification commands: [docs/DEVELOPMENT.md](./docs/DEVELOPMENT.md#code-signing).

## Tested on

End-to-end on real games and runtimes:

| Game | Renderer | HMD | Runtime | Notes |
|------|----------|-----|---------|-------|
| Le Mans Ultimate | D3D11 | Pimax Crystal Light | Pimax OpenXR | Online tested, no anti-cheat issues |
| DiRT Rally 2.0 | D3D11 | Pimax Crystal Light | Pimax OpenXR | |
| Star Wars Squadrons | D3D11 | Pimax Crystal Light | Pimax OpenXR | |
| Assetto Corsa Rally | D3D12 | Pimax Crystal Light | Pimax OpenXR | UE5; helmet routed through the D3D11On12 bridge |

Plus the conformance bar:

- `hello_xr -G D3D11` and `hello_xr -G D3D12` from the OpenXR-SDK
  samples — both pass with the layer loaded.
- OpenXR Conformance Test Suite (CTS) green on Pimax OpenXR and
  SteamVR.

The FOV crop is graphics-API-agnostic and the helmet overlay
supports both D3D11 and D3D12 hosts, so combinations not in the
table above are likely to work too. Looking for testers on other
Pimax models (full Crystal, 8KX, 5K Super) and other runtimes
(SteamVR, WMR, Oculus, Virtual Desktop) — feedback either way at
[GitHub Issues](../../issues).

## Compatibility notes

**Incompatible with any other layer that modifies the per-eye FOV**,
including the "OpenXR Toolkit crop2vr" mod by Ohne Speed or any
custom layer that rewrites `xrLocateViews`. Running two FOV-modifying
layers at once chains the transforms, which produces visible
breakage:

- **UI / HUD displaced.** The game uses the FOV the runtime reports
  to place its in-game menus and HUD elements. After two
  consecutive transforms the FOV no longer matches reality, so menus
  and overlays drift — typical symptom in LMU: menus and in-game
  HUD elements pushed several meters in front of the centered view
  point.
- **Image deformation.** The projection ends up applied through two
  different FOV maps in sequence; straight lines look bent or
  the world appears stretched, especially toward the edges of the
  cropped area.

If you have any FOV-modifying layer installed, uninstall it before
using `fov_crop` — this layer covers the same use case, is signed,
and is the maintained alternative.

**Layer load order matters for overlay layers.** OpenXR layers that
add their own composition overlays — OpenXR Toolkit's FPS counter,
Racelab Overlay, OpenKneeBoard, … — must be loaded **after**
`fov_crop` in the chain. If they load before, they end up calling
`xrLocateViews` through `fov_crop` and see the narrowed FOV, so
their overlays get sized and positioned inside the cropped zone
rather than extending across the full HMD view. Loaded after, they
see the runtime's raw FOV and their overlays show up cleanly
outside the crop bars.

The OpenXR loader orders implicit layers by alphabetical sort of
their registry value, so the practical fix is one of:

- Use [`fredemmott/OpenXR-API-Layers-GUI`](https://github.com/fredemmott/OpenXR-API-Layers-GUI)
  — a small GUI tool that lists and reorders registered OpenXR
  layers without you having to edit `HKLM` by hand. Recommended.
- Or manually rename the conflicting layer's registry entry so it
  sorts alphabetically after `XR_APILAYER_MLEDOUR_fov_crop.json`.

## Installing

### Installer (recommended)

1. Download `XR_APILAYER_MLEDOUR_fov_crop-<version>-x64-Setup.exe` from
   the latest [GitHub Release](../../releases/latest).
2. Run it (admin elevation handled automatically). Installs to
   `C:\Program Files\OpenXR-Layer-fov-crop\` and registers with the
   OpenXR loader.
3. A default `settings.json` is dropped in
   `%LOCALAPPDATA%\XR_APILAYER_MLEDOUR_fov_crop\`. **The layer is
   active out of the box** — see [Configuration](#configuration) to
   tune or disable per-game.

### Manual (ZIP)

Download `XR_APILAYER_MLEDOUR_fov_crop-Release-x64.zip`, unzip to a
**permanent** location (the registry entry points at the DLL on disk),
and run `Install-Layer.ps1` from an elevated PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File .\Install-Layer.ps1
```

> Manual installs outside `C:\Program Files\` may not be readable by
> sandboxed identities (WebXR in Chrome, OpenXR Tools for WMR). The
> installer method handles this automatically.

### Uninstalling

- **Installer**: Settings → Apps → `XR_APILAYER_MLEDOUR_fov_crop` →
  Uninstall.
- **Manual**: run `Uninstall-Layer.ps1` from an elevated PowerShell.

### Disabling without uninstalling

Set `DISABLE_XR_APILAYER_MLEDOUR_fov_crop=1` as an environment
variable on the target process. Standard OpenXR loader escape hatch —
the layer is skipped entirely for that process.

## Configuration

Settings live in `%LOCALAPPDATA%\XR_APILAYER_MLEDOUR_fov_crop\`:

- **`settings.json`** — the template. Edit to change defaults for
  every **future** game launched. Existing per-app files are not
  touched.
- **`<app>_settings.json`** — per-game overrides, created
  automatically on first launch by copying `settings.json`. Edit
  per game without affecting others.

Filenames follow the OpenXR application name lowercased with
underscores — `Le Mans Ultimate` → `le_mans_ultimate_settings.json`,
`DiRT Rally 2.0` → `dirt_rally_2_0_settings.json`.

### File format

```json
{
  "enabled": true,
  "crop_left_percent": 6,
  "crop_right_percent": 6,
  "crop_top_percent": 40,
  "crop_bottom_percent": 32,
  "crop_left_right_percent": 0,
  "crop_right_left_percent": 0,
  "live_edit": false,
  "helmet_overlay": {
    "enabled": true,
    "image": "helmet-F1_medium.png",
    "distance_m": 0.25,
    "horizontal_fov_deg": 115,
    "vertical_offset_deg": -8,
    "brightness": 0.25
  }
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `enabled` | bool | `true` | Master switch for the FOV crop. |
| `crop_left_percent` | float | `6` | Black bar width on the **left eye's left** (outer) edge, as % of image (0–50). |
| `crop_right_percent` | float | `6` | Same, on the **right eye's right** (outer) edge. |
| `crop_top_percent` | float | `40` | Top edge, applied uniformly to both eyes. |
| `crop_bottom_percent` | float | `32` | Bottom edge, applied uniformly to both eyes. |
| `crop_left_right_percent` | float | `0` | **Left eye's right** (inner / nose-side) edge. Combined with `crop_right_left_percent` below, this trims the binocular-overlap zone — the central area both eyes redundantly render — to claw back GPU on wide-FOV HMDs. Ships at `0` (no extra crop) so first-install behavior is unchanged; bump both to `~30` on a Pimax to recover ~25-30 % GPU on a wide-FOV cockpit game. If the field is absent from the JSON entirely (pre-feature settings file) the loader falls back to `crop_right_percent`, preserving the pre-feature symmetric behavior. |
| `crop_right_left_percent` | float | `0` | **Right eye's left** (inner / nose-side) edge. Symmetric counterpart to `crop_left_right_percent`. Ships at `0`. Falls back to `crop_left_percent` if absent. |
| `live_edit` | bool | `false` | Re-read this file every ~1 s so you can tune in-game. Crop, binocular-overlap and helmet `distance_m`/`horizontal_fov_deg`/`vertical_offset_deg` pick up changes; turn back off when satisfied (see [Live-edit cost](#live-edit-cost) below). |
| `helmet_overlay` | object | see below | Helmet overlay block — see [Helmet overlay](#helmet-overlay). |

Each `crop_*_percent` is the fraction of the image covered by a black
bar on that edge. `crop_bottom_percent: 25` covers the bottom 25 %;
the maximum is 50 (bar reaches image center). The math is done in
tan-space so the bar lands at the configured percentage to the pixel,
regardless of HMD or eye offset.

To disable a feature without uninstalling, set `"enabled": false`
either in `settings.json` (affects future games) or in the per-app
file (one game). The helmet overlay has its own independent
`enabled` flag.

### Live-edit cost

`live_edit: true` spawns a background watcher thread that polls the
config file at 1 Hz on `BELOW_NORMAL` priority. The frame thread does
zero filesystem I/O — it just consumes a pre-parsed config the
watcher prepares off-thread.

Measured on Pimax Crystal Light + Pimax OpenXR + LMU at 90 Hz, six
runs of ~76 s each, `live_edit` ON vs OFF on the same binary
(interleaved):

| Metric | OFF | ON | Delta |
|---|---:|---:|---|
| Frame time median | 11.13 ms | 11.12 ms | identical |
| GPU median | 5.56 ms | 5.55 ms | identical |
| Stutter rate | 0.05 % | 0.06 % | noise |
| Jitter rate | 0.41 % | 0.41 % | identical |
| Jitter P99 | 1.71 ms | 1.71 ms | identical |
| Missed frames | 0.16 % | 0.23 % | +0.07 % abs |

User-visible smoothness (stutter, jitter) is statistically
indistinguishable. The only metric that ticks up is the
"missed frames" counter, by ~0.07 % absolute — about one frame
every 17 seconds — which is below the VR perception threshold and
does not translate into a stutter event.

Bottom line: leaving `live_edit` on is fine for day-to-day driving
if you want the option to tweak mid-session. Flip it to `false` for
benchmarking, replays, or any session where you've already settled
on your numbers and don't want any extra threads in the process.

## Helmet overlay

A head-locked PNG of a helmet interior composited on top of the
game's image. The PNG's alpha channel decides what's visible
through — typically opaque foam everywhere with a transparent visor
rectangle in front of the eyes.

### Helmet PNG directory

PNGs are loaded from
`%LOCALAPPDATA%\XR_APILAYER_MLEDOUR_fov_crop\helmets\`. Three starter
PNGs ship with the build: `helmet-F1_thin.png`, `helmet-F1_medium.png`,
`helmet-F1_large.png`.

Populated:
- **By the installer** at install time.
- **By the layer itself** on first launch when installed via ZIP
  (looking next to the unzipped DLL for a `helmets\` directory).

Upgrade contract:
- **Bundled filenames are overwritten on upgrade.** Fixes to bundled
  PNGs propagate; edits you make to bundled PNGs in place are wiped
  on the next install.
- **User-added PNGs (any other filename) are never touched** —
  drop `arai_full_face.png` and it survives every upgrade.
- **Uninstall leaves the directory** alone (per-app configs live
  there too; cleanup is manual).

### Switching helmet skin

1. Drop your PNG into the `helmets\` directory above. Use a name
   **different** from any bundled PNG so upgrades don't overwrite it.
2. Edit the per-app `<app>_settings.json` and set
   `"image": "your_file.png"` in `helmet_overlay`.
3. Restart the game.

You can keep multiple PNGs side by side and switch via the `image`
field.

### Parameters

```json
"helmet_overlay": {
  "enabled": true,
  "image": "helmet-F1_medium.png",
  "distance_m": 0.25,
  "horizontal_fov_deg": 115,
  "vertical_offset_deg": -8,
  "brightness": 0.25
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `enabled` | bool | `true` | Master switch for the helmet overlay. |
| `image` | string | `helmet-F1_medium.png` | PNG filename in `helmets\`. |
| `distance_m` | float | `0.25` | Distance from eye to quad, in meters. Lower = closer to face. Live-tunable. |
| `horizontal_fov_deg` | float | `115` | Quad's apparent angular width, clamped `[10°, 270°]`. Quad height follows PNG aspect ratio. Live-tunable. |
| `vertical_offset_deg` | float | `-8` | Shifts the quad up (`+`) / down (`-`) by an angle, clamped `[-30°, +30°]`. Live-tunable. |
| `brightness` | float | `0.25` | RGB multiplier `[0.0, 1.0]`. Alpha untouched so the visor stays transparent. Session restart required to apply changes. |

**Tuning recipes** for `distance_m` × `horizontal_fov_deg`:

| Feel | `distance_m` | `horizontal_fov_deg` |
|------|--------------|----------------------|
| Real helmet (right against the face) | `0.15` | `100` |
| Default cinematic | `0.25` | `115` |
| TV-in-front-of-you | `0.5` | `90` |

`vertical_offset_deg` defaults to `-8°` (drops the helmet slightly
below the gaze line to clear the cockpit horizon); tune in 1° steps
from there to taste.

### Creating your own helmet PNG

The bundled `helmet-F1_*.png` files are starting points — you can
swap in any helmet shape (Stilo, Bell, Arai, an open-face karting
lid, a fighter pilot helmet, anything with an alpha channel) by
authoring your own.

The full authoring guide lives in
[**docs/HELMET_AUTHORING.md**](./docs/HELMET_AUTHORING.md):
PNG format requirements, a step-by-step GIMP recipe for cutting a
visor, an optional asymmetric alpha-fade variant for nicer foam
edges, the exact GIMP export settings to avoid the dark-halo
artifact around the visor in VR, and an optional pre-warp step
(`tools/cylinder_warp.py`) that bakes apparent cylindrical /
spherical curvature into a flat-quad asset.

## License

MIT License — see [LICENSE](./LICENSE). Based on the
[OpenXR-Layer-Template](https://github.com/mbucchia/OpenXR-Layer-Template)
by Matthieu Bucchianeri (`mbucchia`).

## For developers

Build instructions, CI workflow, code signing details, and layer
internals are in [docs/DEVELOPMENT.md](./docs/DEVELOPMENT.md).
