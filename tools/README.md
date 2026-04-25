# Asset-prep tools

These are off-band utilities for preparing the helmet PNG that the
overlay loads at runtime. They are **not** part of the build — the
DLL and the layer's settings.json don't depend on them.

## `cylinder_warp.py`

Pre-warps a flat helmet PNG so a flat `XrCompositionLayerQuad` shows
it with the apparent geometry of a cylindrical layer. Useful because
the OpenXR runtimes we test against (Pimax OpenXR, mbucchia's
PimaxXR) don't expose `XR_KHR_composition_layer_cylinder`, but the
visual effect of curvature can be baked into the asset itself with
zero changes on the DLL side.

```bash
pip install pillow
python tools/cylinder_warp.py path/to/source.png path/to/helmet_visor.png --angle 130
```

The output drops in next to `XR_APILAYER_MLEDOUR_fov_crop.dll` as
`helmet_visor.png` (replacing the default ellipse-shipped asset).

Iterate on `--angle` until the curvature feels right (`90` is subtle,
`130` is moderate, `180` is strong wraparound). The DLL doesn't need
to know — restart the game and the new asset is picked up.

See the docstring at the top of `cylinder_warp.py` for the full math
derivation.
