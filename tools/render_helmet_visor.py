#!/usr/bin/env python3
"""
render_helmet_visor.py — render a helmet 3D model from inside the rider's
POV via Blender, producing an RGBA PNG with the visor area transparent.

Run via Blender (NOT directly with python3 — needs `bpy`):

    blender --background --python render_helmet_visor.py -- \
        --input-dir "/Users/michaelledour/Desktop/Motorcycle Helmet" \
        --output    "/Users/michaelledour/Desktop/visor/helmet_preview.png" \
        --fov       100

First pass: omit --hide. The script writes a preview PNG and prints a table
of every imported mesh with its world-space bbox center, so you can pick
which model_N.obj is the visor (typically the foremost mesh — largest
forward coordinate along the helmet's "look" axis). Then re-run with
--hide on the right files; those meshes are skipped from rendering and
their area becomes alpha=0 in the output PNG.

    blender --background --python render_helmet_visor.py -- \
        --input-dir "/Users/michaelledour/Desktop/Motorcycle Helmet" \
        --output    "/Users/michaelledour/Desktop/visor/helmet_visor.png" \
        --fov       100 \
        --hide      model_5.obj model_8.obj

Notes
-----
- The downloaded folder has 26 OBJ parts and a few PNG textures but no
  visible MTL files — Blender will fall back to default materials. The
  preview will look monochrome; that's fine for identifying parts.
- Default camera looks along +Y (Sketchfab "forward"). If your model
  is oriented differently, override with --look-axis.
- All materials get use_backface_culling=False so the camera inside
  the shell renders the inner face of the polygons (otherwise you'd
  see solid black walls).
- For a quick first pass use Eevee (default). Toggle --cycles for a
  cleaner render once you've nailed the framing.
"""

import argparse
import math
import os
import sys
from glob import glob

import bpy
from mathutils import Vector


# ─── argv parsing (only what's after "--") ───────────────────────────────
def parse_args():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--input-dir", required=True,
                   help="folder containing model_*.obj")
    p.add_argument("--output", required=True,
                   help="output PNG path (RGBA, transparent background)")
    p.add_argument("--fov", type=float, default=100.0,
                   help="horizontal FOV in degrees (default 100; should match "
                        "your layer's horizontal_fov_deg)")
    p.add_argument("--hide", nargs="*", default=[],
                   help="OBJ filenames to hide from render, e.g. --hide model_5.obj")
    p.add_argument("--res-w", type=int, default=2048)
    p.add_argument("--res-h", type=int, default=1024)
    p.add_argument("--cam-back", type=float, default=0.10,
                   help="fraction of helmet's largest dim to push the camera "
                        "back from bbox center (default 0.10 = 10%%; tune 0.05–0.20)")
    p.add_argument("--cam-up", type=float, default=0.05,
                   help="fraction of helmet's largest dim to push the camera "
                        "up from bbox center (default 0.05 = 5%%; eye line above chin)")
    p.add_argument("--look-axis", choices=["+X", "-X", "+Y", "-Y", "+Z", "-Z"],
                   default="+Y",
                   help="world axis the camera looks along (= helmet 'forward'). "
                        "Try -Y if the visor is at -Y instead of +Y.")
    p.add_argument("--cycles", action="store_true",
                   help="render with Cycles (slower, better quality) instead of Eevee")
    return p.parse_args(argv)


# ─── scene reset ─────────────────────────────────────────────────────────
def reset_scene():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    for m in list(bpy.data.meshes):
        bpy.data.meshes.remove(m)
    for mat in list(bpy.data.materials):
        bpy.data.materials.remove(mat)


# ─── OBJ import (Blender 4.x first, fallback to old name) ────────────────
def import_obj(filepath):
    before = set(bpy.data.objects)
    if hasattr(bpy.ops.wm, "obj_import"):  # Blender 4.x
        bpy.ops.wm.obj_import(filepath=filepath)
    else:                                  # Blender ≤ 3.x
        bpy.ops.import_scene.obj(filepath=filepath)
    return [o for o in bpy.data.objects if o not in before]


# ─── helpers ─────────────────────────────────────────────────────────────
def bbox_center_world(obj):
    cs = [obj.matrix_world @ Vector(c) for c in obj.bound_box]
    return sum(cs, Vector()) / 8.0


def force_double_sided(obj):
    for slot in obj.material_slots:
        if slot.material:
            slot.material.use_backface_culling = False


def pick_render_engine(prefer_cycles):
    """Pick a real engine name across Blender versions."""
    items = bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items.keys()
    if prefer_cycles and "CYCLES" in items:
        return "CYCLES"
    for name in ("BLENDER_EEVEE_NEXT", "BLENDER_EEVEE"):  # 4.2+ first, then 3.x/4.0
        if name in items:
            return name
    return list(items)[0]


# ─── main ────────────────────────────────────────────────────────────────
def main():
    args = parse_args()
    reset_scene()

    obj_files = sorted(glob(os.path.join(args.input_dir, "model_*.obj")))
    if not obj_files:
        print(f"ERROR: no model_*.obj found in {args.input_dir}", file=sys.stderr)
        sys.exit(1)

    print(f"importing {len(obj_files)} OBJ files from {args.input_dir} …")
    name_to_objs = {}
    for path in obj_files:
        fname = os.path.basename(path)
        new = import_obj(path)
        name_to_objs[fname] = new
        for o in new:
            force_double_sided(o)

    # Per-mesh bbox table (helps identify which part is the visor)
    print()
    print(f"{'file':<18} {'mesh':<24} {'bbox center (x, y, z)':<32}  size")
    print("─" * 90)
    all_objs = []
    for fname, objs in name_to_objs.items():
        for o in objs:
            c = bbox_center_world(o)
            corners = [o.matrix_world @ Vector(v) for v in o.bound_box]
            mins = Vector((min(v.x for v in corners),
                           min(v.y for v in corners),
                           min(v.z for v in corners)))
            maxs = Vector((max(v.x for v in corners),
                           max(v.y for v in corners),
                           max(v.z for v in corners)))
            size = maxs - mins
            print(f"{fname:<18} {o.name:<24} "
                  f"({c.x:+.3f}, {c.y:+.3f}, {c.z:+.3f})  "
                  f"{size.x:.2f}×{size.y:.2f}×{size.z:.2f}")
            all_objs.append(o)

    # Hide whatever the user asked for
    for fname in args.hide:
        if fname not in name_to_objs:
            print(f"WARNING: --hide {fname} not found among imports", file=sys.stderr)
            continue
        for o in name_to_objs[fname]:
            o.hide_render = True
            print(f"  hidden for render: {o.name} (from {fname})")

    visible_objs = [o for o in all_objs if not o.hide_render]
    if not visible_objs:
        print("ERROR: nothing left to render after --hide", file=sys.stderr)
        sys.exit(1)

    # Global bbox over visible meshes for camera placement
    centers = [bbox_center_world(o) for o in visible_objs]
    cx = sum(c.x for c in centers) / len(centers)
    cy = sum(c.y for c in centers) / len(centers)
    cz = sum(c.z for c in centers) / len(centers)
    center = Vector((cx, cy, cz))

    # Compute global bbox extent so we can scale --cam-back/--cam-up if the
    # model is in non-metric units (Sketchfab often exports in cm or mm).
    # We treat --cam-back / --cam-up as a *fraction* of the largest helmet
    # dimension so the same numbers work whatever the model's unit scale.
    all_corners = [o.matrix_world @ Vector(v)
                   for o in visible_objs for v in o.bound_box]
    bb_min = Vector((min(c.x for c in all_corners),
                     min(c.y for c in all_corners),
                     min(c.z for c in all_corners)))
    bb_max = Vector((max(c.x for c in all_corners),
                     max(c.y for c in all_corners),
                     max(c.z for c in all_corners)))
    helmet_size = max((bb_max - bb_min).x,
                      (bb_max - bb_min).y,
                      (bb_max - bb_min).z)
    print(f"\nvisible bbox center: ({cx:+.3f}, {cy:+.3f}, {cz:+.3f})  "
          f"size: {helmet_size:.2f} units (largest dim)")

    # Forward axis the camera looks along
    axis_vec = {
        "+X": Vector(( 1, 0, 0)), "-X": Vector((-1, 0, 0)),
        "+Y": Vector(( 0, 1, 0)), "-Y": Vector(( 0,-1, 0)),
        "+Z": Vector(( 0, 0, 1)), "-Z": Vector(( 0, 0,-1)),
    }
    forward = axis_vec[args.look_axis]
    world_up = Vector((0, 0, 1)) if args.look_axis not in ("+Z", "-Z") \
                                  else Vector((0, 1, 0))

    # Interpret --cam-back / --cam-up as fractions of the largest helmet
    # dimension. Default 0.10 ≈ "10% of the helmet's biggest extent backward
    # from the bbox center", which works whether the model is in m, cm or mm.
    cam_back_units = args.cam_back * helmet_size
    cam_up_units = args.cam_up * helmet_size
    cam_pos = center - forward * cam_back_units + world_up * cam_up_units
    look_target = center + forward  # arbitrary point ahead
    print(f"  cam offsets in model units: "
          f"back={cam_back_units:.3f}, up={cam_up_units:.3f}")

    # Camera object
    cam_data = bpy.data.cameras.new("HelmetCam")
    cam_data.lens_unit = "FOV"
    cam_data.angle = math.radians(args.fov)
    # Clip planes scaled to the model's unit system: very near (we're inside
    # the shell) but the far plane large enough to not eat the back wall.
    cam_data.clip_start = max(helmet_size * 0.001, 1e-4)
    cam_data.clip_end = helmet_size * 100.0
    cam_obj = bpy.data.objects.new("HelmetCam", cam_data)
    bpy.context.scene.collection.objects.link(cam_obj)
    cam_obj.location = cam_pos
    direction = (look_target - cam_pos).normalized()
    cam_obj.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
    bpy.context.scene.camera = cam_obj
    print(f"camera at:           ({cam_pos.x:+.3f}, {cam_pos.y:+.3f}, {cam_pos.z:+.3f}) "
          f"looking {args.look_axis} (FOV {args.fov}°)")

    # World: flat dim grey ambient (helmet interior is dark; bright bg
    # would bleed through alpha)
    world = bpy.data.worlds.get("World") or bpy.data.worlds.new("World")
    bpy.context.scene.world = world
    world.use_nodes = True
    bg = world.node_tree.nodes.get("Background")
    if bg is not None:
        bg.inputs["Color"].default_value = (0.05, 0.05, 0.05, 1.0)
        bg.inputs["Strength"].default_value = 1.0

    # Interior fill: a SUN lamp aimed forward through the visor opening.
    # Sun lamps have no distance falloff so the light works the same
    # whether the model is in metres, cm or mm. We aim it slightly
    # downward from behind the rider's head — that's roughly how daylight
    # bleeds into a real helmet.
    fill_data = bpy.data.lights.new("InteriorFill", type="SUN")
    fill_data.energy = 2.0
    fill_data.angle = math.radians(20.0)  # softer shadows
    fill_obj = bpy.data.objects.new("InteriorFill", fill_data)
    fill_obj.location = cam_pos
    # Point the sun forward + slightly down by reusing the camera's rotation
    # but tilted 15° toward the floor.
    sun_dir = (forward - world_up * 0.3).normalized()
    fill_obj.rotation_euler = sun_dir.to_track_quat("-Z", "Y").to_euler()
    bpy.context.scene.collection.objects.link(fill_obj)

    # Render settings
    scene = bpy.context.scene
    scene.render.engine = pick_render_engine(prefer_cycles=args.cycles)
    scene.render.resolution_x = args.res_w
    scene.render.resolution_y = args.res_h
    scene.render.film_transparent = True
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.image_settings.color_depth = "8"
    scene.render.filepath = os.path.abspath(args.output)

    out_dir = os.path.dirname(scene.render.filepath)
    if out_dir and not os.path.isdir(out_dir):
        os.makedirs(out_dir, exist_ok=True)

    print(f"\nrendering {args.res_w}×{args.res_h}, engine={scene.render.engine} …")
    bpy.ops.render.render(write_still=True)
    print(f"output: {scene.render.filepath}")


if __name__ == "__main__":
    main()
