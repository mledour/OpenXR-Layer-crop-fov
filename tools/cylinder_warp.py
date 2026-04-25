#!/usr/bin/env python3
"""
cylinder_warp.py — pre-warp a flat helmet PNG so it renders on a flat
XrCompositionLayerQuad as if it had been projected onto a cylinder.

Why this exists
---------------
The helmet overlay renders helmet_visor.png on a head-locked flat quad.
That works on every OpenXR runtime, but a real helmet's foam wraps
around the user's head — flat-quad rendering is geometrically wrong
at the edges. The "right" fix is XR_KHR_composition_layer_cylinder,
which Pimax OpenXR (official) and PimaxXR (mbucchia) both *don't*
expose, so we can't ship that path unconditionally.

This script does the curvature offline, in the asset itself: pre-warp
the PNG so that when the layer displays it on a flat quad, the
viewer's eye reconstructs an apparent cylindrical mapping. The DLL
side stays a plain flat quad and runs identically on every runtime.

Math (see helmet_overlay.cpp file-level comment for the dual)
-------------------------------------------------------------
Source on a cylinder of central angle Θ, viewed from the axis:
    at angle θ, eye sees source[θ/Θ + 0.5].

Same source warped, displayed on a flat quad spanning Θ horizontally:
    at angle θ, eye sees warped[(W/2)(1 + tan θ / tan(Θ/2))].

For the warped+flat display to match the cylinder display at every θ,
we sample the source at:
    u_src = atan((u_warped - 0.5) * 2 * tan(Θ/2)) / Θ + 0.5

This collapses to the identity at the center (u=0.5 ↔ u=0.5) and at
the edges (u=0 ↔ u=0, u=1 ↔ u=1); in between, source content is
pulled toward the center, which is the visual signature of a cylinder
viewed from its axis.

Usage
-----
    pip install pillow
    python cylinder_warp.py <input.png> <output.png> [--angle 130]

    # batch a few angles for A/B comparison:
    for a in 90 110 130 160 180; do
        python cylinder_warp.py src.png "out_${a}.png" --angle "$a"
    done

Notes
-----
- RGBA inputs are preserved (alpha is warped along with RGB so a visor
  cutout in the source stays a visor cutout in the output).
- Vertical axis is untouched. Cylinder layers in the OpenXR spec only
  curve horizontally; matching that here keeps the math 1-D.
- BILINEAR resampling is used because the visor edge in our PNGs is
  already feathered. BICUBIC can produce a faint halo on hard alpha
  boundaries — if the PNG has razor-sharp transparency, swap to
  BICUBIC and re-feather afterwards.
- 256 mesh strips on an 8K image gives strips of ~32 px — seam
  artifacts well below pixel level. Higher counts cost more time
  with no visible benefit.
"""

import argparse
import math
import os
import sys

try:
    from PIL import Image
except ImportError:
    print("ERROR: pillow is not installed. Run: pip install pillow", file=sys.stderr)
    sys.exit(1)


def build_mesh(width, height, central_angle_deg, n_strips):
    """Build the PIL Image.MESH parameter list for the cylinder warp."""
    theta = math.radians(central_angle_deg)
    half_tan = math.tan(theta / 2.0)

    def src_u(u):
        return math.atan((u - 0.5) * 2.0 * half_tan) / theta + 0.5

    mesh = []
    for i in range(n_strips):
        u0, u1 = i / n_strips, (i + 1) / n_strips
        target = (round(u0 * width), 0, round(u1 * width), height)
        x0, x1 = src_u(u0) * width, src_u(u1) * width
        # Source quadrilateral corners: TL, BL, BR, TR (CCW from spec).
        # Vertical untouched, so top/bottom y are 0/H.
        quad = (x0, 0,
                x0, height,
                x1, height,
                x1, 0)
        mesh.append((target, quad))
    return mesh


def warp_image(input_path, output_path, central_angle_deg=130.0, n_strips=256):
    img = Image.open(input_path)
    w, h = img.size
    print(f"input:  {input_path}: {w}x{h} {img.mode}")

    mesh = build_mesh(w, h, central_angle_deg, n_strips)
    warped = img.transform((w, h), Image.MESH, mesh, Image.BILINEAR)
    warped.save(output_path, "PNG", optimize=True)

    out_size_mb = os.path.getsize(output_path) / 1024 / 1024
    print(f"output: {output_path}: {w}x{h}, {out_size_mb:.1f} MB")
    print(f"angle:  {central_angle_deg:.1f}° (mesh strips: {n_strips})")

    # Sanity sample so the user can sanity-check the mapping
    theta = math.radians(central_angle_deg)
    half_tan = math.tan(theta / 2.0)
    print("  sanity (warped u → source u):")
    for u in (0.0, 0.25, 0.5, 0.75, 1.0):
        src = math.atan((u - 0.5) * 2.0 * half_tan) / theta + 0.5
        print(f"    {u:.2f} ← {src:.3f}")


def main():
    p = argparse.ArgumentParser(description="Pre-warp a flat helmet PNG for cylinder-on-flat-quad rendering.")
    p.add_argument("input", help="source PNG (RGB or RGBA)")
    p.add_argument("output", help="warped PNG output path")
    p.add_argument("--angle", type=float, default=130.0,
                   help="cylinder central angle in degrees (default 130; try 90 / 110 / 160 / 180 to taste)")
    p.add_argument("--strips", type=int, default=256,
                   help="mesh subdivision count (default 256; lower = faster but visible seams)")
    args = p.parse_args()

    if not os.path.isfile(args.input):
        print(f"ERROR: input not found: {args.input}", file=sys.stderr)
        sys.exit(1)
    if args.angle <= 0 or args.angle >= 360:
        print(f"ERROR: angle must be in (0, 360), got {args.angle}", file=sys.stderr)
        sys.exit(1)

    warp_image(args.input, args.output, args.angle, args.strips)


if __name__ == "__main__":
    main()
