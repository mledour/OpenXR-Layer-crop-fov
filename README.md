# XR_APILAYER_MLEDOUR_fov_crop

An OpenXR API layer for Windows with two opt-in features:

1. **FOV crop** — narrows the effective field of view and swapchain
   resolution. Your game renders fewer pixels per frame, every frame
   is cheaper and the headroom goes into higher FPS — at the cost of
   slightly narrower peripheral vision, visible as black edges in
   the headset.
2. **Helmet overlay** — composites a head-locked PNG (motorcycle /
   karting / racing helmet interior, your choice) on top of the
   game's image, with a transparent visor cutout so the user looks
   "out" through it. Adds immersion to sims that don't model a full
   helmet themselves. Optimised to ~+1 % GPU cost when enabled,
   strictly 0 cost when disabled.

Works transparently with any OpenXR application and runtime. No game or
headset modification required.

> ⚠️ Release binaries are **not** code-signed yet. Anti-cheat systems
> may reject unsigned DLLs loaded into OpenXR games, and Windows
> SmartScreen will warn on the installer. A signed release is
> planned — see [docs/DEVELOPMENT.md](./docs/DEVELOPMENT.md#code-signing).

## Installing

### Installer (recommended)

1. Download `XR_APILAYER_MLEDOUR_fov_crop-<version>-x64-Setup.exe` from
   the latest [GitHub Release](../../releases/latest).
2. Run it. Admin elevation is handled automatically.
3. The layer is installed under
   `C:\Program Files\OpenXR-Layer-fov-crop\` and registered with the
   OpenXR loader.
4. A default `settings.json` is dropped in
   `%LOCALAPPDATA%\XR_APILAYER_MLEDOUR_fov_crop\`. **The layer is
   disabled by default** — see [Configuration](#configuration) below
   to turn it on.

### Manual (ZIP)

Download `XR_APILAYER_MLEDOUR_fov_crop-Release-x64.zip`, unzip it to a
**permanent** location (the registry entry points at the DLL on disk,
so the folder must not move), and run `Install-Layer.ps1` from an
elevated PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File .\Install-Layer.ps1
```

> ⚠️ Manual installs outside `C:\Program Files\` may not be readable by
> sandboxed identities (WebXR in Chrome, OpenXR Tools for WMR). The
> installer method handles this automatically.

### Uninstalling

- **Installer**: Settings → Apps → `XR_APILAYER_MLEDOUR_fov_crop` →
  Uninstall.
- **Manual**: run `Uninstall-Layer.ps1` from an elevated PowerShell.

### Disabling without uninstalling

Set the environment variable `DISABLE_XR_APILAYER_MLEDOUR_fov_crop=1`
for the target process. This is the standard OpenXR loader escape
hatch — the layer is skipped entirely for that process.

## Configuration

The layer keeps **one settings file per OpenXR application** inside
`%LOCALAPPDATA%\XR_APILAYER_MLEDOUR_fov_crop\`. Each file is named
after the application's OpenXR name, sanitized to lowercase with
underscores:

| Application name (reported via OpenXR) | Settings file |
|-----------------------------------------|---------------|
| `DiRT Rally 2.0` | `dirt_rally_2_0_settings.json` |
| `Le Mans Ultimate` | `le_mans_ultimate_settings.json` |
| `iRacing Simulator` | `iracing_simulator_settings.json` |
| `hello_xr` | `hello_xr_settings.json` |

### The two kinds of file

- **`settings.json`** — the template, dropped in by the installer (or
  auto-created by the layer on manual install). Edit this file to
  change the defaults that every **future** game will start with. Your
  edits survive reinstalls.
- **`<app>_settings.json`** — the per-app file for one specific game.
  Created automatically the first time that game runs, by copying
  `settings.json`. Edit this to tune crop values for that game without
  affecting others.

An existing `<app>_settings.json` is never touched on subsequent runs.
Editing `settings.json` only affects new games, not games that already
have their own file.

### File format

```json
{
  "enabled": false,
  "crop_left_percent": 10,
  "crop_right_percent": 10,
  "crop_top_percent": 15,
  "crop_bottom_percent": 20,
  "live_edit": false,
  "helmet_overlay": {
    "enabled": false,
    "texture": "helmet_visor.png",
    "distance_m": 0.5,
    "horizontal_fov_deg": 130,
    "vertical_offset_deg": 0.0,
    "brightness": 1.0
  }
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `enabled` | bool | `false` | Master switch for the FOV crop — the crop is **opt-in** and a no-op until you flip this to `true`. The helmet overlay below has its own `enabled` flag and runs independently. |
| `crop_left_percent` | float | `10` | Percentage of the image covered by the black bar on the left edge (0-50). |
| `crop_right_percent` | float | `10` | Percentage of the image covered by the black bar on the right edge (0-50). |
| `crop_top_percent` | float | `15` | Percentage of the image covered by the black bar on the top edge (0-50). |
| `crop_bottom_percent` | float | `20` | Percentage of the image covered by the black bar on the bottom edge (0-50). |
| `live_edit` | bool | `false` | When true, the layer re-reads the config every ~1 second so you can tune values in-game. Picks up changes to crop percentages and to `helmet_overlay.distance_m` / `helmet_overlay.horizontal_fov_deg` / `helmet_overlay.vertical_offset_deg`. Set back to false once you're happy. |
| `helmet_overlay` | object | (see below) | Helmet overlay configuration. See [Helmet overlay](#helmet-overlay). |

### How the percentages are interpreted

Each `crop_*_percent` is the **fraction of the image covered by the black
bar on that edge**. The mapping is linear in pixel space:

- `crop_bottom_percent: 10` → the bottom bar covers the bottom 10% of
  the image.
- `crop_bottom_percent: 25` → the bottom bar covers the bottom 25%.
- `crop_bottom_percent: 50` → the bottom bar reaches the image center
  (covers the bottom 50%). This is the maximum: values above 50 are
  clamped because a single edge cannot physically cover more than half
  the image without overlapping the opposite bar.

The same rule applies independently on each of the four edges, so
`10/10/15/20` leaves a central content area spanning 80% of the width
and 65% of the height of the full image, centered.

The layer does the math in tan-space (the pixel ↔ angle mapping of
perspective projection), so the bar lands at the configured percentage
to the pixel — regardless of the HMD's native FOV or eye offset.

**To activate the FOV crop**, flip `"enabled": false` to `"enabled": true`
either:
- in `settings.json` — applies to every **future** game you launch, or
- in a specific `<app>_settings.json` — applies to that game only.

The helmet overlay is independent and follows the same rule with
`helmet_overlay.enabled`.

## Helmet overlay

Composites a head-locked PNG of a helmet interior on top of the
game's image. The PNG's alpha channel decides what the player sees
through and what they don't — the typical content is "opaque foam
all around, transparent rectangle in front of the eyes for the
visor", which gives the perception of being inside a real helmet.

A starter `helmet_visor.png` (4:3, 2048×1536) ships with the
installer next to the DLL. Replace it with your own image to use a
different helmet skin (Stilo, Arai, karting, etc.) — the layer
picks up whichever PNG is at the path on next session start.

### Parameters

```json
"helmet_overlay": {
  "enabled": false,
  "texture": "helmet_visor.png",
  "distance_m": 0.5,
  "horizontal_fov_deg": 130,
  "vertical_offset_deg": 0.0,
  "brightness": 1.0
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `enabled` | bool | `false` | Master switch for the helmet overlay. |
| `texture` | string | `helmet_visor.png` | Filename of the PNG to load, resolved relative to the DLL's install directory. Change this only if you want to switch between several PNGs you keep side by side. |
| `distance_m` | float | `0.5` | **Depth-feel knob**: distance from the eye to the quad's plane, in meters. Controls the stereo disparity, i.e. how "close to your face" the helmet feels. Try `0.15` for "right against the face" (real helmet feel), `0.3` for "close but not claustrophobic", `0.5` for "TV-in-front-of-you". Live-tunable. |
| `horizontal_fov_deg` | float | `130` | **Coverage knob**: angular width of the quad in your view, in degrees. Clamped to `[10°, 270°]`. The physical quad width is derived as `2 × distance_m × tan(fov/2)`, so changing `distance_m` no longer also changes coverage — these two parameters are orthogonal and can be tuned independently. Try `90°` for "tight visor", `130°` for "moderate wraparound", `180°` for "ear-to-ear". Quad height follows the PNG aspect ratio so the image is never stretched. Live-tunable. |
| `vertical_offset_deg` | float | `0.0` | **Position knob**: shifts the quad up (`+`) or down (`-`) by an angle in your view, in degrees. Clamped to `[-30°, +30°]`. Decoupled from `distance_m` — at any distance, "+5°" always shifts the helmet up by 5° in your FOV. Useful when the helmet sits slightly above or below your gaze line because of HMD lens placement or asymmetric `crop_top` / `crop_bottom`. Try `+2°` (helmet up) or `-2°` (helmet down) and adjust to taste. Live-tunable. |
| `brightness` | float | `1.0` | RGB multiplier applied at load time, clamped to `[0.0, 1.0]`. `1.0` = pristine PNG, `0.5` = half luminance, `0.0` = pure black. Useful when studio-lit photos look cramée on a bright VR HMD in a dim cockpit. Alpha is never multiplied so the visor cutout stays transparent at any value. **Not** live-tunable — changing it requires a session restart (the texture is uploaded once at session start). |

### Custom PNG: requirements

A drop-in replacement for `helmet_visor.png` must be:

- **PNG with an alpha channel (RGBA)**. JPG won't work — no alpha.
  Indexed-color PNGs are converted on load but it's safer to stay in
  full RGBA.
- **Visor opening at `alpha = 0`** (fully transparent). Anywhere else
  the player will see the texture instead of the game world.
- **Foam / structure at `alpha = 255`** (fully opaque) so the
  periphery occludes the game.
- **Aspect ratio that matches your content goal.** The layer derives
  the quad's height from the PNG aspect, so a 3:2 PNG keeps the
  quad 1.5× wider than tall, a 2:1 PNG twice as wide as tall, etc.
  3:2 / 4:3 fit a "natural helmet visor" shape; 2:1 to 3:1 fit
  better when you want to use every visible pixel of an HMD with
  aggressive top/bottom cropping.
- **Resolution sweet spot is ~2K wide.** 1024 wide → visible
  pixelation on dense HMDs (Crystal / Quest 3). 2048-3072 wide →
  optimal. 4K+ → measurable VRAM and GPU bandwidth cost without
  visible benefit (we benched a 13× regression at 8K vs 2K with the
  same PNG content).

### Custom PNG: GIMP procedure

The fastest way to produce a working PNG from any helmet photo /
render:

1. **Open** the source in GIMP. `File → Open`.
2. **Add an alpha channel**: `Layer → Transparency → Add Alpha
   Channel`. (If the menu entry is greyed out, the image already
   has alpha — proceed.) Confirm `Image → Mode → RGB`.
3. **Cut the visor**:
   - **Quick path**: `Tools → Selection Tools → By Color` (Shift+O).
     Click in the visor. Adjust `Threshold` until the marching ants
     hug the visor edge cleanly.
   - **Precise path**: `Tools → Paths` (B). Click 8-12 anchor points
     around the visor boundary, drag at each click to shape Bézier
     handles for the curves. Close the path with Ctrl+click on the
     first anchor. In the Paths tab on the right, click the "Path to
     Selection" button.
4. **Soften the edge**: `Select → Feather → 6 px → OK`. (Adjust to
   3 px for a sharper cut, 10 px for a softer one.)
5. **Cut**: `Edit → Clear` (or just hit Delete). The visor area
   becomes a checkerboard.
6. **Deselect**: `Select → None` (`Shift+Ctrl+A`).
7. **Export** (not Save, which produces a `.xcf`): `File → Export As
   → helmet_visor.png`. Default PNG settings are fine.

Drop the resulting `helmet_visor.png` into the layer's install
directory (overwriting the starter one) and restart the game.

### Optional: apparent curvature via `tools/cylinder_warp.py`

A real helmet's foam shell wraps around your face — flat-quad
rendering is geometrically wrong at the edges of the texture. The
"right" fix is `XR_KHR_composition_layer_cylinder`, but Pimax
OpenXR (official) and PimaxXR (mbucchia) both don't expose it, so
the layer always renders a flat quad.

The work-around is to bake the cylinder projection into the asset
itself. The repo ships [`tools/cylinder_warp.py`](./tools/README.md)
for that:

```bash
pip install pillow
python tools/cylinder_warp.py source.png helmet_visor.png --angle 130
```

The output drops in at the layer's install path the same way as a
hand-cut PNG. Try `--angle 90` for subtle curvature, `130` for
moderate (default), `180` for strong wraparound. The DLL renders
the result on a flat quad and the user's eye reconstructs the
apparent cylindrical mapping. Works on every OpenXR runtime since
no extension is needed.

## License

MIT License — see [LICENSE](./LICENSE). Based on the
[OpenXR-Layer-Template](https://github.com/mbucchia/OpenXR-Layer-Template)
by Matthieu Bucchianeri (`mbucchia`).

## For developers

Build instructions, CI workflow, code signing details, and layer
internals are in [docs/DEVELOPMENT.md](./docs/DEVELOPMENT.md).
