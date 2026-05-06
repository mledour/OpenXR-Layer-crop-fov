#!/usr/bin/env python3
"""
cylinder_warp.py — pre-warp a flat helmet PNG so it renders on a flat
XrCompositionLayerQuad as if it had been projected onto a cylinder
(default) or a dome/sphere (--mode sphere).

Why this exists
---------------
The helmet overlay renders the bundled helmet PNG on a head-locked flat quad.
That works on every OpenXR runtime, but a real helmet's foam wraps
around the user's head — flat-quad rendering is geometrically wrong
at the edges. The "right" fix is XR_KHR_composition_layer_cylinder /
XR_KHR_composition_layer_equirect2, which Pimax OpenXR (official) and
PimaxXR (mbucchia) both *don't* expose, so we can't ship those paths
unconditionally.

This script does the curvature offline, in the asset itself: pre-warp
the PNG so that when the layer displays it on a flat quad, the
viewer's eye reconstructs an apparent cylindrical (or dome) mapping.
The DLL side stays a plain flat quad and runs identically on every
runtime.

Cylinder mode (default)
-----------------------
1-D horizontal warp. Source on a cylinder of central angle Θ, viewed
from the axis:
    at angle θ, eye sees source[θ/Θ + 0.5].

Same source warped, displayed on a flat quad spanning Θ horizontally:
    at angle θ, eye sees warped[(W/2)(1 + tan θ / tan(Θ/2))].

For the warped+flat display to match the cylinder display at every θ,
we sample the source at:
    u_src = atan((u_warped - 0.5) * 2 * tan(Θ/2)) / Θ + 0.5

This collapses to the identity at the center (u=0.5 ↔ u=0.5) and at
the edges (u=0 ↔ u=0, u=1 ↔ u=1); in between, source content is
pulled toward the center. Vertical axis is untouched — horizontal
source lines remain horizontal in the output.

Sphere / dome mode (--mode sphere)
----------------------------------
2-D warp combining the cylinder horizontal pass with a vertical
correction that depends on horizontal position. Goal: make a
rectangular visor opening appear cushion-shaped (smaller at the
corners than at the center) when displayed on the flat quad — the
"helmet wraps around the face at the temples" look.

For the desired cushion shape, the vertical sample at column u is
*expanded* by 1/cos(θ_h(u)), so the source's top/bottom rows fall
beyond [0, 1] at the edges and source-edge content is replicated
across the output's top/bottom corners:
    cos_th = cos(atan((u_warped − 0.5) × 2 × tan(Θ_h/2)))
    v_src = atan((v_warped − 0.5) × 2 × tan(Θ_v/2) / cos_th) / Θ_v + 0.5
    v_src = clip(v_src, 0, 1)

The clipping is generally invisible because helmet PNG corners are
uniform padding. If you have rich content near the source corners,
expect a faint banding artifact — tame it by lowering --vertical-angle
or post-feathering the corners of the source.

Usage
-----
    pip install pillow
    # Cylinder (default):
    python cylinder_warp.py <input.png> <output.png> [--angle 130]
    # Sphere/dome:
    python cylinder_warp.py <input.png> <output.png> --mode sphere \\
        --angle 130 [--vertical-angle 87]

    # batch a few angles for A/B comparison:
    for a in 90 110 130 160 180; do
        python cylinder_warp.py src.png "out_${a}.png" --angle "$a"
    done

Notes
-----
- RGBA inputs are preserved (alpha is warped along with RGB so a visor
  cutout in the source stays a visor cutout in the output).
- BILINEAR resampling is used because the visor edge in our PNGs is
  already feathered. BICUBIC can produce a faint halo on hard alpha
  boundaries — if the PNG has razor-sharp transparency, swap to
  BICUBIC and re-feather afterwards.
- Cylinder mode: 256 mesh strips on an 8K image gives strips of ~32 px —
  seam artifacts well below pixel level. Higher counts cost more time
  with no visible benefit.
- Sphere mode: defaults to a 2-D mesh of n_h × n_v cells, where n_v is
  picked from the PNG aspect to keep cell aspect ~1:1. ~40k–65k cells
  is normal; init takes a few seconds on a 6K image.
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
    """Build the PIL Image.MESH parameter list for the cylinder warp.

    A positive central_angle_deg pre-warps the source as if it lived on a
    *concave* cylinder seen from its axis (the standard helmet-from-inside
    case): source content is pulled toward the center.

    A negative central_angle_deg applies the *inverse* mapping at the same
    magnitude, producing a convex / barrel-style pre-warp (centre compressed,
    edges stretched). Only useful for testing — for a real helmet you almost
    always want the positive direction.
    """
    inverse = central_angle_deg < 0
    theta = math.radians(abs(central_angle_deg))
    half_tan = math.tan(theta / 2.0)

    if inverse:
        # Inverse mapping: u_src = tan((u - 0.5) * Θ) / (2 * tan(Θ/2)) + 0.5
        def src_u(u):
            return math.tan((u - 0.5) * theta) / (2.0 * half_tan) + 0.5
    else:
        # Forward mapping: u_src = atan((u - 0.5) * 2 * tan(Θ/2)) / Θ + 0.5
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


def build_mesh_sphere(width, height,
                      horizontal_angle_deg, vertical_angle_deg,
                      n_h, n_v):
    """Build the PIL Image.MESH parameter list for the sphere/dome warp.

    Combines the cylinder horizontal mapping with a vertical correction
    that depends on horizontal position:

    - Positive vertical_angle (cushion): vertical sample inflated by
      1/cos(θ_h) at the edges. Source horizontal lines bow toward
      v=0.5 at the output's edges → rectangular visor opening appears
      cushion-shaped (smaller at corners). Matches a real helmet seen
      from inside.

    - Negative vertical_angle (barrel/fish-eye): vertical sample
      compressed by cos(θ_h) at the edges. Source horizontal lines bow
      AWAY from v=0.5 at the output's edges → rectangular visor
      opening appears barrel-shaped (bigger at corners). Useful when
      you want a "porthole" feel where the opening bulges outward.

    Same convention as --angle for the horizontal: positive = forward,
    negative = inverse direction.

    The vertical sample is clamped to [0, 1] so the source's top and
    bottom rows are replicated across the output's corner bands when
    the warped v_src would otherwise leave the source.
    """
    inverse_v = vertical_angle_deg < 0
    theta_h = math.radians(horizontal_angle_deg)
    theta_v = math.radians(abs(vertical_angle_deg))
    half_tan_h = math.tan(theta_h / 2.0)
    half_tan_v = math.tan(theta_v / 2.0)

    def src_uv(u, v):
        # Horizontal: same as cylinder mode.
        th = math.atan((u - 0.5) * 2.0 * half_tan_h)
        cos_th = max(math.cos(th), 0.01)  # belt-and-braces against θ→90°.
        # Vertical inflation factor:
        #   cushion (positive vertical_angle): divide by cos_th
        #   barrel  (negative vertical_angle): multiply by cos_th
        scale = cos_th if inverse_v else (1.0 / cos_th)
        tv_arg = (v - 0.5) * 2.0 * half_tan_v * scale
        tv = math.atan(tv_arg)
        u_src = th / theta_h + 0.5
        v_src = tv / theta_v + 0.5
        # Clamp so the warp doesn't sample outside the source rect.
        return u_src, max(0.0, min(1.0, v_src))

    mesh = []
    for j in range(n_v):
        for i in range(n_h):
            u0, u1 = i / n_h, (i + 1) / n_h
            v0, v1 = j / n_v, (j + 1) / n_v
            target = (round(u0 * width), round(v0 * height),
                      round(u1 * width), round(v1 * height))
            su0t, sv0t = src_uv(u0, v0)  # TL
            su0b, sv0b = src_uv(u0, v1)  # BL
            su1b, sv1b = src_uv(u1, v1)  # BR
            su1t, sv1t = src_uv(u1, v0)  # TR
            quad = (su0t * width, sv0t * height,
                    su0b * width, sv0b * height,
                    su1b * width, sv1b * height,
                    su1t * width, sv1t * height)
            mesh.append((target, quad))
    return mesh


def warp_image(input_path, output_path,
               central_angle_deg=130.0, n_strips=256,
               mode="cylinder", vertical_angle_deg=None):
    img = Image.open(input_path)
    w, h = img.size
    print(f"input:  {input_path}: {w}x{h} {img.mode}")

    if mode == "sphere":
        # Default vertical angle = horizontal × pngH/pngW (positive,
        # cushion direction). Treats the source PNG as isotropic
        # angular sampling so cells stay ~square in mesh space.
        # User can override --vertical-angle, including with a
        # negative value to flip to the barrel/fish-eye direction.
        if vertical_angle_deg is None:
            vertical_angle_deg = abs(central_angle_deg) * h / w
        # Pick n_v so cells remain ~square (~6 px on a 1536×1024 PNG
        # with default n_h=256). Clamp to keep total cell count sane.
        n_v = max(16, min(n_strips, round(n_strips * h / w)))
        mesh = build_mesh_sphere(w, h,
                                 abs(central_angle_deg), vertical_angle_deg,
                                 n_strips, n_v)
    else:
        mesh = build_mesh(w, h, central_angle_deg, n_strips)

    warped = img.transform((w, h), Image.MESH, mesh, Image.BILINEAR)
    warped.save(output_path, "PNG", optimize=True)

    out_size_mb = os.path.getsize(output_path) / 1024 / 1024
    print(f"output: {output_path}: {w}x{h}, {out_size_mb:.1f} MB")
    if mode == "sphere":
        v_dir = "barrel/fish-eye (opening BIGGER at corners)" if vertical_angle_deg < 0 \
                else "cushion (opening SMALLER at corners)"
        print(f"mode:   sphere (h={abs(central_angle_deg):.1f}°, "
              f"v={vertical_angle_deg:+.1f}° → {v_dir}, mesh: {n_strips}×{n_v})")
    else:
        direction = "inverse (barrel/convex)" if central_angle_deg < 0 else "forward (concave)"
        print(f"mode:   cylinder, {central_angle_deg:.1f}° {direction} "
              f"(mesh strips: {n_strips})")

    # Sanity sample so the user can sanity-check the mapping
    if mode == "sphere":
        inverse_v = vertical_angle_deg < 0
        theta_h = math.radians(abs(central_angle_deg))
        theta_v = math.radians(abs(vertical_angle_deg))
        half_tan_h = math.tan(theta_h / 2.0)
        half_tan_v = math.tan(theta_v / 2.0)
        print("  sanity (warped (u, v) → source (u, v)):")
        for v in (0.0, 0.25, 0.5):
            row = []
            for u in (0.0, 0.5, 1.0):
                th = math.atan((u - 0.5) * 2.0 * half_tan_h)
                cos_th = max(math.cos(th), 0.01)
                scale = cos_th if inverse_v else (1.0 / cos_th)
                tv = math.atan((v - 0.5) * 2.0 * half_tan_v * scale)
                u_src = th / theta_h + 0.5
                v_src = max(0.0, min(1.0, tv / theta_v + 0.5))
                row.append(f"({u_src:.2f},{v_src:.2f})")
            print(f"    v_w={v:.2f}: " + " | ".join(row))
        if inverse_v:
            print(f"    (barrel: v_w=0 maps to v_src closer to 0.5 at edges)")
        else:
            print(f"    (cushion: top-row clamp at edges if source corners are not uniform)")
    else:
        inverse = central_angle_deg < 0
        theta = math.radians(abs(central_angle_deg))
        half_tan = math.tan(theta / 2.0)
        print("  sanity (warped u → source u):")
        for u in (0.0, 0.25, 0.5, 0.75, 1.0):
            if inverse:
                src = math.tan((u - 0.5) * theta) / (2.0 * half_tan) + 0.5
            else:
                src = math.atan((u - 0.5) * 2.0 * half_tan) / theta + 0.5
            print(f"    {u:.2f} ← {src:.3f}")


def main():
    p = argparse.ArgumentParser(description="Pre-warp a flat helmet PNG for cylinder-on-flat-quad or sphere/dome-on-flat-quad rendering.")
    p.add_argument("input", help="source PNG (RGB or RGBA)")
    p.add_argument("output", help="warped PNG output path")
    p.add_argument("--mode", choices=("cylinder", "sphere"), default="cylinder",
                   help="warp model. cylinder (default) = 1-D horizontal pre-warp, "
                        "horizontal source lines stay horizontal. "
                        "sphere = horizontal cylinder + vertical bow so the visor "
                        "opening looks cushion-shaped (smaller at the corners).")
    p.add_argument("--angle", type=float, default=130.0,
                   help="cylinder central angle (horizontal) in degrees (default 130; try 90 / 110 / 160 / 180 to taste). "
                        "Negative values apply the inverse (convex/barrel) warp at the same magnitude. "
                        "Sphere mode ignores the sign.")
    p.add_argument("--vertical-angle", type=float, default=None,
                   help="vertical central angle in degrees, sphere mode only "
                        "(default = horizontal_angle × pngH/pngW). "
                        "Positive = cushion direction (visor opening smaller at corners, real-helmet look). "
                        "Negative = barrel/fish-eye direction (visor opening bigger at corners, porthole look). "
                        "|value| controls strength: smaller = subtler, larger = more pronounced.")
    p.add_argument("--strips", type=int, default=256,
                   help="mesh subdivision count along width (default 256; lower = faster but visible seams). "
                        "Sphere mode picks n_vertical from the PNG aspect to keep cells ~square.")
    args = p.parse_args()

    if not os.path.isfile(args.input):
        print(f"ERROR: input not found: {args.input}", file=sys.stderr)
        sys.exit(1)
    if args.angle == 0 or abs(args.angle) >= 360:
        print(f"ERROR: angle must be in (-360, 0) ∪ (0, 360), got {args.angle}", file=sys.stderr)
        sys.exit(1)
    if args.mode == "sphere":
        if args.angle < 0:
            print("WARNING: sphere mode ignores the sign on --angle (no horizontal inverse mapping defined). "
                  "Use --vertical-angle <negative> if you want to flip the vertical bow direction.",
                  file=sys.stderr)
        if args.vertical_angle is not None and (args.vertical_angle == 0 or abs(args.vertical_angle) >= 180):
            print(f"ERROR: --vertical-angle must be in (-180, 0) ∪ (0, 180), got {args.vertical_angle}", file=sys.stderr)
            sys.exit(1)

    warp_image(args.input, args.output,
               args.angle, args.strips,
               mode=args.mode,
               vertical_angle_deg=args.vertical_angle)


if __name__ == "__main__":
    main()
