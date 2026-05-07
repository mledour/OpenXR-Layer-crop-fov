# Asset-prep tools

These are off-band utilities for preparing the helmet PNG that the
overlay loads at runtime. They are **not** part of the build — the
DLL and the layer's settings.json don't depend on them.

## `cylinder_warp.py`

Pre-warps a flat helmet PNG so a flat `XrCompositionLayerQuad` shows
it with the apparent geometry of a curved layer. The layer DLL only
ever renders a flat quad — runtime cylinder / equirect support was
removed because the runtimes we target (Pimax OpenXR, mbucchia's
PimaxXR) don't expose `XR_KHR_composition_layer_cylinder` /
`_equirect2` anyway. Instead, the curvature is baked into the asset
offline with this script.

```bash
pip install pillow
```

Two modes are supported.

### Cylinder mode (default) — 1-D horizontal warp

Bakes a vertical-axis cylinder projection: the source's vertical
lines bow toward the centre at the edges of the warped output.
Horizontal lines stay horizontal — no vertical correction.

```bash
python tools/cylinder_warp.py source.png helmet-F1_medium.png --angle 130
```

Iterate on `--angle` until the horizontal curvature feels right (`90`
is subtle, `130` is moderate, `180` is strong wraparound). Negative
values apply the inverse (convex/barrel) mapping at the same
magnitude — useful only for testing the direction; for a real helmet
you always want positive.

### Sphere mode (`--mode sphere`) — 2-D horizontal cylinder + vertical dome

Adds a vertical correction on top of the cylinder pass: the source's
horizontal lines bow AWAY from the centre at the edges, so a
rectangular visor opening appears wider at the corners than at the
centre — the "padding wraps around the temples and exposes more visor
at the periphery" look you actually see inside a real helmet.

```bash
python tools/cylinder_warp.py source.png helmet-F1_dome.png \
    --mode sphere --angle 100 --vertical-angle 80
```

`--vertical-angle` defaults to `horizontal_angle × pngH/pngW` if
omitted, which keeps the mesh cells roughly square.

**Sign convention** (matches `--angle` for the cylinder pass):

| Sign | Direction | Use case |
|---|---|---|
| `+` (default) | helmet-correct (concave-from-inside, opening WIDER at corners) | real helmet PNG |
| `-` | inverse / cushion (opening narrower at corners) | testing, non-helmet content |

Tuning rule of thumb: `--vertical-angle 60` is subtle, `80–100` is
moderate, `120+` starts to clip the source's top/bottom rows at the
output corners (see notes in the script's docstring).

### Where the output goes

The output PNG drops in next to `XR_APILAYER_MLEDOUR_fov_crop.dll`
as one of the bundled `helmet-F1_*.png` filenames (replacing the
default shipped asset). The DLL doesn't need to know — restart the
game and the new asset is picked up.

See the docstring at the top of `cylinder_warp.py` for the full math
derivation of both modes.
