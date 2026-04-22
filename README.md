# XR_APILAYER_MLEDOUR_fov_crop

An OpenXR API layer for Windows that reduces GPU load by narrowing the
effective field of view and swapchain resolution. Your game renders
fewer pixels per frame, so every frame is cheaper and the headroom goes
into higher FPS — at the cost of slightly narrower peripheral vision,
visible as black edges in the headset.

Works transparently with any OpenXR application and runtime. No game or
headset modification required.

> ⚠️ Release binaries are **not** code-signed yet. Anti-cheat systems
> may reject unsigned DLLs loaded into OpenXR games, and Windows
> SmartScreen will warn on the installer. A signed release via
> [SignPath Foundation](https://signpath.org/) is pending approval —
> see [docs/DEVELOPMENT.md](./docs/DEVELOPMENT.md#code-signing).

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
  "live_edit": false
}
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `enabled` | bool | `false` | Master switch — the layer is **opt-in** and a no-op until you flip this to `true`. |
| `crop_left_percent` | float | `10` | Percentage of the image covered by the black bar on the left edge (0-50). |
| `crop_right_percent` | float | `10` | Percentage of the image covered by the black bar on the right edge (0-50). |
| `crop_top_percent` | float | `15` | Percentage of the image covered by the black bar on the top edge (0-50). |
| `crop_bottom_percent` | float | `20` | Percentage of the image covered by the black bar on the bottom edge (0-50). |
| `live_edit` | bool | `false` | When true, the layer re-reads the config every ~1 second so you can tune values in-game. Set back to false once you're happy. |

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

**To activate the layer**, flip `"enabled": false` to `"enabled": true`
either:
- in `settings.json` — applies to every **future** game you launch, or
- in a specific `<app>_settings.json` — applies to that game only.

## License

MIT License — see [LICENSE](./LICENSE). Based on the
[OpenXR-Layer-Template](https://github.com/mbucchia/OpenXR-Layer-Template)
by Matthieu Bucchianeri (`mbucchia`).

## For developers

Build instructions, CI workflow, code signing details, and layer
internals are in [docs/DEVELOPMENT.md](./docs/DEVELOPMENT.md).
