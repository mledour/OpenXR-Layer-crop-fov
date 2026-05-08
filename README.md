[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/G2G21Z5SOM)
# XR_APILAYER_MLEDOUR_fov_crop

An OpenXR API layer for Windows with two features, both **enabled
by default** in the shipped template:

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

> ⚠️ Release binaries are signed with a **Certum Open Source Code
> Signing Cloud** certificate. SmartScreen may still warn the first
> few times the installer is downloaded (publisher reputation builds
> with download volume); anti-cheat systems may flag any OpenXR layer
> DLL — including a signed one — when loaded into a hooked game. See
> [docs/DEVELOPMENT.md](./docs/DEVELOPMENT.md#code-signing) for the
> CI flow and how to verify the signature.

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
   enabled by default** for every new game — see
   [Configuration](#configuration) below to tune the crop ratios or
   disable it on a per-game basis.

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
  "enabled": true,
  "crop_left_percent": 6,
  "crop_right_percent": 6,
  "crop_top_percent": 40,
  "crop_bottom_percent": 32,
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
| `enabled` | bool | `true` | Master switch for the FOV crop. The shipped default is `true`, so the crop is active out of the box for any new game; flip to `false` to disable it for that game. The helmet overlay below has its own `enabled` flag and runs independently. |
| `crop_left_percent` | float | `6` | Percentage of the image covered by the black bar on the left edge (0-50). |
| `crop_right_percent` | float | `6` | Percentage of the image covered by the black bar on the right edge (0-50). |
| `crop_top_percent` | float | `40` | Percentage of the image covered by the black bar on the top edge (0-50). |
| `crop_bottom_percent` | float | `32` | Percentage of the image covered by the black bar on the bottom edge (0-50). |
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

**The FOV crop is on by default** in the shipped template. To
**disable** it (or re-enable it after disabling), flip the
`"enabled"` field:
- in `settings.json` — applies to every **future** game you launch
  (existing per-app files are not touched), or
- in a specific `<app>_settings.json` — applies to that game only.

The helmet overlay is independent and follows the same rule with
`helmet_overlay.enabled`.

## Helmet overlay

Composites a head-locked PNG of a helmet interior on top of the
game's image. The PNG's alpha channel decides what the player sees
through and what they don't — the typical content is "opaque foam
all around, transparent rectangle in front of the eyes for the
visor", which gives the perception of being inside a real helmet.

Three starter helmet PNGs (`helmet-F1_thin.png`,
`helmet-F1_medium.png`, `helmet-F1_large.png`) ship with the
installer. The overlay loads PNGs from the user-writable directory

```
%LOCALAPPDATA%\XR_APILAYER_MLEDOUR_fov_crop\helmets\
```

next to the per-app `*_settings.json` files — no admin elevation
needed to add or swap helmets.

### How that directory is populated

| Install path | When the directory is created | What gets dropped in |
|---|---|---|
| **Installer (`Setup.exe`)** | At install time, before any game runs | All bundled PNGs from the build |
| **Manual ZIP + `Install-Layer.ps1`** | First launch of any OpenXR application after install | All bundled PNGs found at `<unzip dir>\helmets\`, copied by the layer's first-run logic |

### Upgrade policy

- **Bundled filenames are canonical.** The installer **overwrites**
  same-named PNGs on every upgrade, so a fix or refresh of any
  bundled `helmet-F1_*.png` in the build propagates automatically.
  If you edit a bundled PNG in place, your changes will be wiped on
  the next install.
- **User-added PNGs (different filenames) are never touched.**
  Drop `arai_full_face.png`, `kart_open.png`, anything you like —
  installer upgrades and the runtime first-run logic both leave
  them alone, same contract as `<app>_settings.json`.
- **Uninstall leaves the entire helmets directory in place.**
  Cleanup is the user's responsibility (the per-app config files
  live alongside it and would be orphaned otherwise).

### Switching helmet skin

1. Drop your PNG in `%LOCALAPPDATA%\XR_APILAYER_MLEDOUR_fov_crop\helmets\` —
   e.g. `arai_full_face.png`. Use a name **different** from any
   bundled PNG so the installer never overwrites it on upgrade.
2. Edit the per-app `<app>_settings.json` and set
   `"image": "arai_full_face.png"` in the `helmet_overlay` block.
3. Restart the game.

You can keep multiple PNGs side by side and switch between them
just by changing the `image` field.

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
| `image` | string | `helmet-F1_medium.png` | Filename of the PNG to load, resolved relative to `%LOCALAPPDATA%\XR_APILAYER_MLEDOUR_fov_crop\helmets\`. Change this to switch between several PNGs you keep in that folder side by side. |
| `distance_m` | float | `0.25` | **Depth-feel knob**: distance from the eye to the quad's plane, in meters. Controls the stereo disparity, i.e. how "close to your face" the helmet feels. Try `0.15` for "right against the face" (real helmet feel), `0.25` for "close but not claustrophobic" (default), `0.5` for "TV-in-front-of-you". Live-tunable. |
| `horizontal_fov_deg` | float | `115` | **Coverage knob**: angular width of the quad in your view, in degrees. Clamped to `[10°, 270°]`. The physical quad width is derived as `2 × distance_m × tan(fov/2)`, so changing `distance_m` no longer also changes coverage — these two parameters are orthogonal and can be tuned independently. Try `90°` for "tight visor", `115°` for "moderate wraparound" (default), `180°` for "ear-to-ear". Quad height follows the PNG aspect ratio so the image is never stretched. Live-tunable. |
| `vertical_offset_deg` | float | `-8` | **Position knob**: shifts the quad up (`+`) or down (`-`) by an angle in your view, in degrees. Clamped to `[-30°, +30°]`. Decoupled from `distance_m` — at any distance, "+5°" always shifts the helmet up by 5° in your FOV. Useful when the helmet sits slightly above or below your gaze line because of HMD lens placement or asymmetric `crop_top` / `crop_bottom`. The default `-8°` drops the helmet slightly below the gaze line to clear the cockpit horizon; tune in 1° steps from there. Live-tunable. |
| `brightness` | float | `0.25` | RGB multiplier applied at load time, clamped to `[0.0, 1.0]`. `1.0` = pristine PNG, `0.5` = half luminance, `0.0` = pure black. The default `0.25` keeps the F1 cockpit photo readable but dim enough to not wash out the game in a bright HMD; raise toward `0.5` if your HMD or cockpit is darker. Alpha is never multiplied so the visor cutout stays transparent at any value. **Not** live-tunable — changing it requires a session restart (the texture is uploaded once at session start). |

### Creating your own helmet PNG

The bundled `helmet-F1_*.png` files are starting points — you can
swap in any helmet shape (Stilo, Bell, Arai, an open-face karting
lid, a fighter pilot helmet, anything with an alpha channel) by
authoring your own PNG.

The full authoring guide lives in
[**docs/HELMET_AUTHORING.md**](./docs/HELMET_AUTHORING.md):

- PNG format requirements (alpha channel, resolution sweet spot,
  aspect ratio guidance)
- Step-by-step GIMP recipe for cutting a visor from a helmet photo
- An optional "smoother alpha fade" recipe for nicer foam edges
- The exact GIMP export settings to avoid the dark-halo artifact
  around the visor in VR
- An optional pre-warp step (`tools/cylinder_warp.py`) that bakes
  apparent cylindrical / spherical curvature into the flat-quad
  asset

## License

MIT License — see [LICENSE](./LICENSE). Based on the
[OpenXR-Layer-Template](https://github.com/mbucchia/OpenXR-Layer-Template)
by Matthieu Bucchianeri (`mbucchia`).

## For developers

Build instructions, CI workflow, code signing details, and layer
internals are in [docs/DEVELOPMENT.md](./docs/DEVELOPMENT.md).
