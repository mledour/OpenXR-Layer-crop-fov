# Authoring custom helmet PNGs

Notes for users who want to swap the bundled `helmet-F1_*.png` assets
for their own helmet shape — Stilo, Bell, Arai, an open-face karting
lid, a fighter pilot helmet, anything with an alpha channel.

End-user installation, settings, and the swap procedure live in the
[README](../README.md). This doc is the *content authoring* guide:
what makes a usable PNG, the GIMP recipe to cut a visor, export
settings that avoid common artifacts, and the optional pre-warp step
that bakes apparent curvature into a flat-quad render.

## What makes a usable helmet PNG

A drop-in replacement for a bundled `helmet-F1_*.png` must be:

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

## GIMP procedure

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
   → my_helmet.png`. The default PNG settings work but are not
   optimal for our pipeline — see [GIMP export settings](#gimp-export-settings)
   below for the right checkboxes.

Drop the resulting PNG into
`%LOCALAPPDATA%\XR_APILAYER_MLEDOUR_fov_crop\helmets\` (overwriting
the bundled `helmet-F1_medium.png` to replace the default, or saving
under another name to add a new skin you select via the `image`
field) and restart the game.

## Optional: smoother alpha transition

The simple "Feather Selection → Clear" approach in step 4 above
gives a symmetric fade where the gradient bites into both the
visor center *and* the surrounding foam. If you want a fade that
**only** extends outward into the foam (visor stays sharp at its
original boundary, foam edge softens):

1. Right-click the layer → `Add Layer Mask…` → check **"Transfer
   layer's alpha channel"** → OK. The current alpha becomes an
   editable mask.
2. Click the **mask thumbnail** (right of the layer thumbnail) so
   it's selected for editing.
3. `Filters → Blur → Gaussian Blur` → **8-12 px** → OK. The mask
   edge is now a symmetric gradient on both sides.
4. `Colors → Curves`. Drag the bottom-left point of the curve right
   from `x=0` to `x≈50-80` (keep `y=0`). This crushes the
   "visor side" of the gradient back to fully transparent, leaving
   only the "foam side" of the fade.
5. `Layer → Mask → Apply Layer Mask` to bake the result into alpha.
6. Export as PNG using the settings below.

Tune `Gaussian Blur` (step 3) for fade width, and the curve point
(step 4) for fade asymmetry. Larger blur + smaller curve point =
wider, softer fade.

## GIMP export settings

The PNG export dialog has several checkboxes. The right setup for
this layer:

| Option | Setting | Why |
|---|---|---|
| Interlacing (Adam7) | ❌ off | Progressive decode is useless when the layer waits for the full PNG before uploading to GPU. Adds 5-15% file size and 10-20% decode time. |
| Save background color (`bKGD` chunk) | ❌ off | The compositor blends on the live game scene, not on a static color. The chunk is ignored. |
| Save gamma | ❌ off | stb_image doesn't apply gamma metadata. We rely on the implicit "PNG = sRGB" convention. |
| Save layer offset | ❌ off | Single-layer image. |
| Save resolution (`pHYs` chunk) | ❌ off | DPI is irrelevant in VR — the texture is sampled in texel space, not projected at a physical print size. |
| Save creation time | indifferent | A few bytes either way. |
| **Save color values from transparent pixels** | ✓ **on** | **Important.** Without this, GIMP zeroes the RGB of fully-transparent pixels. Bilinear filtering at the visor edge then averages opaque foam color with black, producing a visible **dark halo** around the visor in VR. With it on, transparent pixels keep colors that match the adjacent foam, and the halo disappears. |
| Save thumbnail | ❌ off | No-one is browsing the file in Explorer. |
| Save Exif / XMP / IPTC | ❌ off | Not consumed by the layer. |
| **Save color profile (ICC)** | ❌ off | stb_image ignores the embedded profile. The pipeline assumes sRGB-encoded bytes implicitly. **Make sure the source image is actually in sRGB before exporting** — `Image → Image Properties → Color Profile`. "GIMP built-in sRGB" is correct. If it's anything else (AdobeRGB, ProPhoto, etc.), use `Image → Color Management → Convert to Color Profile` to convert to sRGB first; otherwise the layer will display slightly desaturated colors. |
| Compression level | **9** | Slowest compression, smallest file. The cost (a few hundred ms at export time) is paid once. |

In short: **only "Save color values from transparent pixels" needs
to be checked**; everything else off. The default PNG settings are
not catastrophic but produce a slightly larger file with a halo
risk on the visor edge.

## Optional: apparent curvature via `tools/cylinder_warp.py`

A real helmet's foam shell wraps around your face — flat-quad
rendering is geometrically wrong at the edges of the texture. The
"right" fix is `XR_KHR_composition_layer_cylinder`, but Pimax
OpenXR (official) and PimaxXR (mbucchia) both don't expose it, so
the layer always renders a flat quad.

The work-around is to bake the cylinder projection into the asset
itself. The repo ships [`tools/cylinder_warp.py`](../tools/cylinder_warp.py)
for that:

```bash
pip install pillow
python tools/cylinder_warp.py source.png helmet-F1_medium.png --angle 130
```

The output drops in at the layer's install path the same way as a
hand-cut PNG. Try `--angle 90` for subtle curvature, `130` for
moderate (default), `180` for strong wraparound. The DLL renders
the result on a flat quad and the user's eye reconstructs the
apparent cylindrical mapping. Works on every OpenXR runtime since
no extension is needed.

A second mode, `--mode sphere`, adds a vertical-dome correction on
top of the cylinder pass — use it for content where you also want
the visor opening to widen at the corners (matches what you actually
see inside a real helmet). See [`tools/README.md`](../tools/README.md)
for the full options, sign convention, and math derivation.
