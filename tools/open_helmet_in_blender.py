#!/usr/bin/env python3
"""
open_helmet_in_blender.py — interactively inspect a multi-OBJ helmet
download in Blender's GUI.

What it does at startup
-----------------------
1. Wipes the default cube/light/camera.
2. Imports every model_*.obj from --input-dir.
3. Gives each mesh a random flat colour so parts are trivially
   distinguishable even when the .mtl files are missing (Sketchfab
   downloads often ship without them).
4. Parks the 3D viewport at the rider's eye position, oriented along
   the helmet's "forward" axis. X-Ray mode is on so you can see
   through the outer shell immediately.

Usage (NOT --background — this is interactive):

    /Applications/Blender.app/Contents/MacOS/Blender \\
        --python "/Users/michaelledour/Documents/Claude/Projects/OpenXR Layers/XR_APILAYER_MLEDOUR_fov_crop/tools/open_helmet_in_blender.py" -- \\
        --input-dir "/Users/michaelledour/Desktop/Motorcycle Helmet"

In the GUI
----------
- Middle-click + drag (or two-finger drag on a touchpad) to orbit
  around the eye position.
- Click any model_N in the outliner to highlight it; press '/' on the
  numpad to isolate it (Local View). Press '/' again to come back.
- Once you've identified the visor and any meshes that block the eye
  cavity, use them as `--hide` arguments to render_helmet_visor.py.

Capturing the current view as a clean PNG
-----------------------------------------
Pass --capture-output and the script registers a "Capture Helmet View"
operator bound to Ctrl+Shift+P. When you press it: Blender aligns a
hidden camera onto whatever the viewport currently shows, renders it
through Eevee with a transparent background, and writes the PNG to the
path you gave. You can press it again after moving the view — each
press overwrites the same PNG (or appends a counter if --capture-uniq).

Also accessible from F3 search → "Capture Helmet View".

Tuning
------
- Helmet not pointing along world +Y? Pass --look-axis=-Y / +X / -X.
  (Note: argparse needs `=` syntax for negative-axis values.)
- Eye position too far back / too high? Tune --cam-back and --cam-up
  (fractions of the helmet's largest dimension; 0 = bbox center).
"""

import argparse
import math
import os
import random
import sys
from colorsys import hsv_to_rgb
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
    p.add_argument("--look-axis", choices=["+X", "-X", "+Y", "-Y", "+Z", "-Z"],
                   default="+Y",
                   help="world axis the viewport looks along (= helmet 'forward')")
    p.add_argument("--cam-back", type=float, default=0.10,
                   help="fraction of helmet's largest dim to push the eye "
                        "back from bbox center (default 0.10)")
    p.add_argument("--cam-up", type=float, default=0.05,
                   help="fraction of helmet's largest dim to push the eye "
                        "up from bbox center (default 0.05)")
    p.add_argument("--capture-output", default=None,
                   help="if set, registers a 'Capture Helmet View' operator "
                        "bound to Ctrl+Shift+P that renders the current "
                        "viewport view through a real camera and writes a "
                        "transparent-background PNG to this path")
    p.add_argument("--capture-fov", type=float, default=100.0,
                   help="horizontal FOV (deg) for the capture camera "
                        "(should match your layer's horizontal_fov_deg)")
    p.add_argument("--capture-res-x", type=int, default=2048)
    p.add_argument("--capture-res-y", type=int, default=1024)
    p.add_argument("--capture-uniq", action="store_true",
                   help="append a counter to the PNG name on each capture "
                        "(helmet_capture_001.png, _002.png, …) so you can "
                        "snapshot multiple angles without overwriting")
    p.add_argument("--use-textures", action="store_true",
                   help="load Layers.png/metal6.png/n.png from --input-dir "
                        "as a single PBR atlas material (Principled BSDF) "
                        "applied to every mesh; viewport switches to "
                        "Material Preview so the textures render. The OBJ's "
                        "own UV coords route the atlas to the right spots.")
    p.add_argument("--hide", nargs="*", default=[],
                   help="OBJ filenames to hide from viewport AND capture, "
                        "e.g. --hide model_22.obj for the visor. Hidden "
                        "meshes leave alpha=0 in the captured PNG where "
                        "they would have been — exactly what you want for "
                        "the visor cutout in the final layer asset.")
    p.add_argument("--no-xray", action="store_true",
                   help="force X-Ray off in the viewport. Default keeps "
                        "X-Ray on for SOLID/inspection mode (so you can see "
                        "through outer layers to find parts) and off for "
                        "--use-textures mode (so PBR renders cleanly with "
                        "alpha=255 in the captured PNG). Pass this when you "
                        "want a final asset capture: meshes opaque on the "
                        "periphery, alpha=0 only where you --hide them.")
    p.add_argument("--ai-prep", action="store_true",
                   help="capture mode tuned for img2img / ControlNet feeding: "
                        "(1) clean clay matcap shading regardless of "
                        "--use-textures (no PBR atlas the AI would try to "
                        "preserve), (2) cavity overlay on for sharp edges "
                        "the model can pick up, (3) mid-grey background "
                        "instead of transparent so the visor zone doesn't "
                        "look like a hole the AI feels compelled to fill "
                        "with eyes/face, (4) writes a sidecar "
                        "<output>_mask.png — pure B&W silhouette of the "
                        "real helmet area — to re-apply as a layer mask in "
                        "GIMP/Photoshop after img2img.")
    return p.parse_args(argv)


# ─── scene reset ─────────────────────────────────────────────────────────
def reset_scene():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    for m in list(bpy.data.meshes):
        bpy.data.meshes.remove(m)
    for mat in list(bpy.data.materials):
        bpy.data.materials.remove(mat)


# ─── OBJ import (Blender 4.x first, fallback) ────────────────────────────
def import_obj(filepath):
    before = set(bpy.data.objects)
    if hasattr(bpy.ops.wm, "obj_import"):
        bpy.ops.wm.obj_import(filepath=filepath)
    else:
        bpy.ops.import_scene.obj(filepath=filepath)
    return [o for o in bpy.data.objects if o not in before]


# ─── helpers ─────────────────────────────────────────────────────────────
def bbox_center_world(obj):
    cs = [obj.matrix_world @ Vector(c) for c in obj.bound_box]
    return sum(cs, Vector()) / 8.0


def colour_objects(objs):
    """Give each object a unique saturated flat-shaded material so parts
    are easy to pick out in the viewport with default shading."""
    rng = random.Random(42)  # reproducible palette across runs
    for o in objs:
        h = rng.random()
        s = 0.65 + rng.random() * 0.30
        v = 0.70 + rng.random() * 0.25
        r, g, b = hsv_to_rgb(h, s, v)
        mat = bpy.data.materials.new(name=f"M_{o.name}")
        mat.use_nodes = False
        mat.diffuse_color = (r, g, b, 1.0)
        # Replace any imported material slots with the new flat colour
        o.data.materials.clear()
        o.data.materials.append(mat)


def _find_texture(input_dir, candidates):
    """Return the first existing path among <input_dir>/<candidate> or None."""
    for name in candidates:
        path = os.path.join(input_dir, name)
        if os.path.isfile(path):
            return path
    return None


def apply_textures(objs, input_dir):
    """Build a single Principled BSDF material from the PNGs in input_dir
    (Sketchfab atlas convention: Layers/metal/n) and assign it to every
    mesh. The OBJ's own UVs route into the atlas — no per-mesh setup
    needed, but if a mesh's UVs are wrong it'll show stretched/garbage
    texels (worth knowing if the helmet looks weird in some spots).

    Returns True if at least the albedo was found and applied; False if
    no albedo PNG exists (caller falls back to flat colours)."""
    albedo_path = _find_texture(input_dir,
        ["Layers.png", "albedo.png", "diffuse.png", "BaseColor.png"])
    metal_path  = _find_texture(input_dir,
        ["metal6.png", "metallic.png", "roughness.png", "MetallicRoughness.png"])
    normal_path = _find_texture(input_dir,
        ["n.png", "normal.png", "Normal.png", "NormalGL.png"])

    if not albedo_path:
        print("WARNING: --use-textures requested but no albedo PNG found in "
              f"{input_dir} (looked for Layers.png / albedo.png / diffuse.png)")
        return False

    print(f"  textures: albedo={os.path.basename(albedo_path)}"
          f"  metal/rough={os.path.basename(metal_path) if metal_path else '-'}"
          f"  normal={os.path.basename(normal_path) if normal_path else '-'}")

    mat = bpy.data.materials.new("HelmetAtlas")
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()

    out_node = nt.nodes.new("ShaderNodeOutputMaterial")
    out_node.location = (700, 0)
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    bsdf.location = (350, 0)
    nt.links.new(bsdf.outputs["BSDF"], out_node.inputs["Surface"])

    # Albedo → Base Color
    img_a = bpy.data.images.load(albedo_path, check_existing=True)
    tex_a = nt.nodes.new("ShaderNodeTexImage")
    tex_a.image = img_a
    tex_a.location = (-200, 200)
    nt.links.new(tex_a.outputs["Color"], bsdf.inputs["Base Color"])

    # Metal/roughness — Sketchfab convention varies; we assume the texture
    # is a packed RGB where R≈metallic and G≈roughness. If it looks
    # inverted, swap the R/G connections in the node tree manually.
    if metal_path:
        img_m = bpy.data.images.load(metal_path, check_existing=True)
        img_m.colorspace_settings.name = "Non-Color"
        tex_m = nt.nodes.new("ShaderNodeTexImage")
        tex_m.image = img_m
        tex_m.location = (-200, -50)
        # ShaderNodeSeparateColor (RGB) is the modern (Blender 3.3+) name;
        # ShaderNodeSeparateRGB still works on older versions.
        sep_type = "ShaderNodeSeparateColor" if hasattr(
            bpy.types, "ShaderNodeSeparateColor") else "ShaderNodeSeparateRGB"
        sep = nt.nodes.new(sep_type)
        sep.location = (100, -50)
        # Some Blender 4.x branches expose 'Color' input, older 'Image'
        if "Color" in sep.inputs:
            nt.links.new(tex_m.outputs["Color"], sep.inputs["Color"])
        else:
            nt.links.new(tex_m.outputs["Color"], sep.inputs["Image"])
        # Output sockets named "Red"/"Green" in SeparateColor, "R"/"G"
        # in legacy SeparateRGB.
        red_out  = sep.outputs.get("Red")  or sep.outputs.get("R")
        green_out = sep.outputs.get("Green") or sep.outputs.get("G")
        if "Metallic" in bsdf.inputs and red_out:
            nt.links.new(red_out, bsdf.inputs["Metallic"])
        if "Roughness" in bsdf.inputs and green_out:
            nt.links.new(green_out, bsdf.inputs["Roughness"])

    # Normal map
    if normal_path:
        img_n = bpy.data.images.load(normal_path, check_existing=True)
        img_n.colorspace_settings.name = "Non-Color"
        tex_n = nt.nodes.new("ShaderNodeTexImage")
        tex_n.image = img_n
        tex_n.location = (-200, -300)
        nm = nt.nodes.new("ShaderNodeNormalMap")
        nm.location = (100, -300)
        nt.links.new(tex_n.outputs["Color"], nm.inputs["Color"])
        if "Normal" in bsdf.inputs:
            nt.links.new(nm.outputs["Normal"], bsdf.inputs["Normal"])

    # Apply the single shared atlas material to every mesh, replacing the
    # random flat materials assigned by colour_objects.
    n_applied = 0
    for o in objs:
        if o.type != "MESH":
            continue
        o.data.materials.clear()
        o.data.materials.append(mat)
        n_applied += 1
    print(f"  atlas material applied to {n_applied} mesh(es)")
    return True


def park_viewport_inside(center, helmet_size, look_axis, cam_back, cam_up,
                          textured=False, no_xray=False):
    """Aim every 3D View region at the rider's eye position, looking along
    the helmet's 'forward' axis. X-Ray on so the outer shell doesn't
    occlude the interior."""
    axis_vec = {
        "+X": Vector(( 1, 0, 0)), "-X": Vector((-1, 0, 0)),
        "+Y": Vector(( 0, 1, 0)), "-Y": Vector(( 0,-1, 0)),
        "+Z": Vector(( 0, 0, 1)), "-Z": Vector(( 0, 0,-1)),
    }
    forward = axis_vec[look_axis]
    world_up = Vector((0, 0, 1)) if look_axis not in ("+Z", "-Z") \
                                  else Vector((0, 1, 0))

    eye = center - forward * (cam_back * helmet_size) \
                 + world_up * (cam_up   * helmet_size)
    rot = forward.to_track_quat("-Z", "Y")

    found_view = False
    for area in bpy.context.screen.areas:
        if area.type != "VIEW_3D":
            continue
        for space in area.spaces:
            if space.type != "VIEW_3D":
                continue
            found_view = True
            r3d = space.region_3d
            r3d.view_perspective = "PERSP"
            r3d.view_location = eye
            # Tiny view_distance so the orbit centre IS the eye position;
            # rotating the view feels like turning your head.
            r3d.view_distance = max(helmet_size * 0.001, 1e-4)
            r3d.view_rotation = rot

            # Sensible clip planes for the model's unit system.
            space.clip_start = max(helmet_size * 0.0005, 1e-5)
            space.clip_end = helmet_size * 100.0
            space.lens = 22  # ~92° horizontal — close to a real visor opening

            # X-Ray + cavity shading so the inner geometry is immediately
            # visible without manually toggling Alt+Z.
            space.shading.type = "SOLID"
            space.shading.show_xray = True
            space.shading.xray_alpha = 0.5
            if textured:
                # Material Preview shows the PBR atlas (albedo + metal +
                # normal) on a default HDRI. The capture operator
                # (render.opengl, view_context) picks this up and writes
                # a PNG that does show the textures.
                space.shading.type = "MATERIAL"
                space.shading.use_scene_world = False
            else:
                # Solid + matcap: random flat colours per mesh, easy
                # part identification. Viewport stays dirt-cheap.
                space.shading.type = "SOLID"
                space.shading.light = "MATCAP"
                for name in ("basic_grey.exr", "clay_studio.exr",
                             "hard_surface_grey.exr", "pearl.exr"):
                    try:
                        space.shading.studio_light = name
                        break
                    except (TypeError, ValueError):
                        continue
                space.shading.show_cavity = True

            # X-Ray: ON by default for SOLID/inspection (lets you see
            # through outer layers to identify parts), OFF for textured
            # mode (so the PBR atlas shows opaque, with alpha=255 in the
            # captured PNG where meshes are). --no-xray forces off
            # everywhere — what you want for final asset captures.
            if no_xray:
                space.shading.show_xray = False
            elif textured:
                space.shading.show_xray = False
            else:
                space.shading.show_xray = True
                space.shading.xray_alpha = 0.5

    return found_view, eye


# ─── capture operator ────────────────────────────────────────────────────
# Module-level state so the operator can read what main() set up at start.
_CAPTURE_STATE = {
    "output_path": None,   # str, where to write PNG
    "fov_deg":     100.0,
    "res_x":       2048,
    "res_y":       1024,
    "uniq":        False,  # if True, suffix _001, _002, …
    "counter":     0,
    "helmet_size": 1.0,
    "ai_prep":     False,  # if True, override shading to clay matcap +
                           # write sidecar mask; see --ai-prep flag
}


def _next_capture_path():
    """Resolve the actual output path for this press, respecting --capture-uniq."""
    base = _CAPTURE_STATE["output_path"]
    if not _CAPTURE_STATE["uniq"]:
        return base
    _CAPTURE_STATE["counter"] += 1
    root, ext = os.path.splitext(base)
    return f"{root}_{_CAPTURE_STATE['counter']:03d}{ext or '.png'}"


def _pick_render_engine():
    items = bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items.keys()
    for name in ("BLENDER_EEVEE_NEXT", "BLENDER_EEVEE"):
        if name in items:
            return name
    return "CYCLES" if "CYCLES" in items else list(items)[0]


def _derive_mask_path(color_path):
    """For 'helmet_capture_001.png' → 'helmet_capture_001_mask.png'."""
    root, ext = os.path.splitext(color_path)
    return f"{root}_mask{ext or '.png'}"


def _write_mask_from_alpha(src_png, mask_png):
    """Read src_png (RGBA), copy its alpha into RGB to produce a B&W
    layer mask, write mask_png. Soft alpha preserved (good for AA edges
    when re-applied as a layer mask in GIMP/Photoshop)."""
    src = bpy.data.images.load(src_png, check_existing=False)
    try:
        w, h = src.size
        if w == 0 or h == 0:
            print(f"  WARNING: mask source has zero dimensions: {src_png}")
            return
        src_px = list(src.pixels)
        n = w * h
        mask_px = [0.0] * (n * 4)
        # Walk the flat RGBA buffer once: for each pixel, copy alpha into
        # R/G/B and force alpha=1 in the output.
        for i in range(n):
            a = src_px[i * 4 + 3]
            j = i * 4
            mask_px[j]     = a
            mask_px[j + 1] = a
            mask_px[j + 2] = a
            mask_px[j + 3] = 1.0
        mask_img = bpy.data.images.new("HelmetCaptureMask",
                                       width=w, height=h, alpha=True)
        try:
            mask_img.pixels = mask_px
            mask_img.filepath_raw = os.path.abspath(mask_png)
            mask_img.file_format = "PNG"
            mask_img.save()
        finally:
            bpy.data.images.remove(mask_img)
    finally:
        bpy.data.images.remove(src)


class HELMET_OT_capture_view(bpy.types.Operator):
    """Render the current 3D viewport view through a real camera and save as PNG.

    The active 3D viewport is sampled, a hidden camera is parked at the
    same pose with --capture-fov FOV, render settings are configured for
    a transparent-background RGBA PNG, and the result is written to
    --capture-output.
    """
    bl_idname = "helmet.capture_view"
    bl_label = "Capture Helmet View as PNG"
    bl_options = {"REGISTER"}

    def execute(self, context):
        if not _CAPTURE_STATE.get("output_path"):
            self.report({"ERROR"},
                        "Capture not configured: pass --capture-output at startup.")
            return {"CANCELLED"}

        # Snapshot prior render settings so we can restore after.
        scene = context.scene
        prev = {
            "filepath":    scene.render.filepath,
            "fmt":         scene.render.image_settings.file_format,
            "color_mode":  scene.render.image_settings.color_mode,
            "transparent": scene.render.film_transparent,
            "res_x":       scene.render.resolution_x,
            "res_y":       scene.render.resolution_y,
        }

        out_path = _next_capture_path()
        out_dir = os.path.dirname(os.path.abspath(out_path))
        if out_dir and not os.path.isdir(out_dir):
            os.makedirs(out_dir, exist_ok=True)

        # Find the active 3D viewport space + region for the temp_override.
        target_area = None
        target_region = None
        target_space = None
        for area in context.screen.areas:
            if area.type != "VIEW_3D":
                continue
            target_area = area
            for space in area.spaces:
                if space.type == "VIEW_3D":
                    target_space = space
                    break
            for region in area.regions:
                if region.type == "WINDOW":
                    target_region = region
                    break
            break
        if target_area is None or target_region is None or target_space is None:
            self.report({"ERROR"}, "No 3D viewport found to capture.")
            return {"CANCELLED"}

        ai_prep = bool(_CAPTURE_STATE.get("ai_prep"))

        # Snapshot the viewport state we mutate (overlays + maybe shading
        # in --ai-prep mode) so we can restore on the way out.
        prev_overlays = target_space.overlay.show_overlays
        prev_shading = {
            "type":             target_space.shading.type,
            "light":            target_space.shading.light,
            "studio_light":     target_space.shading.studio_light,
            "show_xray":        target_space.shading.show_xray,
            "show_cavity":      target_space.shading.show_cavity,
            "background_type":  target_space.shading.background_type,
            "background_color": tuple(target_space.shading.background_color),
        }

        # Common: kill overlays so the floor grid / axes / selection
        # outlines don't bake into the PNG.
        target_space.overlay.show_overlays = False

        # Common render output config
        scene.render.resolution_x = _CAPTURE_STATE["res_x"]
        scene.render.resolution_y = _CAPTURE_STATE["res_y"]
        scene.render.image_settings.file_format = "PNG"
        scene.render.image_settings.color_mode = "RGBA"

        try:
            if ai_prep:
                # Override shading to clean clay matcap, no PBR atlas, no
                # X-Ray, cavity on for sharp edges. ControlNet/img2img
                # consume this best when there's strong shape signal and
                # no misleading "texture detail" the model would try to
                # preserve.
                target_space.shading.type = "SOLID"
                target_space.shading.light = "MATCAP"
                for name in ("clay_studio.exr", "basic_grey.exr",
                             "hard_surface_grey.exr", "pearl.exr"):
                    try:
                        target_space.shading.studio_light = name
                        break
                    except (TypeError, ValueError):
                        continue
                target_space.shading.show_xray = False
                target_space.shading.show_cavity = True
                # Mid-grey background fills the visor cutout in the
                # SHADED render — the AI sees a continuous neutral
                # surround rather than a gaping black hole that begs to
                # be filled with eyes/face.
                target_space.shading.background_type = "VIEWPORT"
                target_space.shading.background_color = (0.5, 0.5, 0.5)

                # ── Pass 1: AI-input PNG (grey BG, NO transparency) ─────
                scene.render.film_transparent = False
                scene.render.filepath = os.path.abspath(out_path)
                with context.temp_override(area=target_area, region=target_region):
                    bpy.ops.render.opengl(write_still=True, view_context=True)

                # ── Pass 2: alpha pass for the sidecar mask ─────────────
                # Same shading, same camera, but transparent BG so the
                # alpha channel encodes "where is helmet". We then walk
                # that alpha into a B&W mask PNG and discard the temp.
                mask_path = _derive_mask_path(out_path)
                tmp_path = mask_path + ".tmp.png"
                scene.render.film_transparent = True
                scene.render.filepath = os.path.abspath(tmp_path)
                with context.temp_override(area=target_area, region=target_region):
                    bpy.ops.render.opengl(write_still=True, view_context=True)
                _write_mask_from_alpha(tmp_path, mask_path)
                try:
                    os.remove(tmp_path)
                except OSError:
                    pass  # leave it; user can clean by hand
                print(f"  captured (ai-prep) → {os.path.abspath(out_path)}")
                print(f"  sidecar mask       → {os.path.abspath(mask_path)}")
                self.report({"INFO"}, f"AI-prep capture: {out_path} (+ mask)")
            else:
                # ── Default single-pass capture (current behaviour) ─────
                # Transparent BG, current viewport shading, alpha=0 in
                # hidden zones — the WYSIWYG path the rest of the script
                # was already producing.
                scene.render.film_transparent = True
                scene.render.filepath = os.path.abspath(out_path)
                with context.temp_override(area=target_area, region=target_region):
                    bpy.ops.render.opengl(write_still=True, view_context=True)
                print(f"  captured → {os.path.abspath(out_path)}")
                self.report({"INFO"}, f"Captured: {out_path}")
        finally:
            # Restore everything
            target_space.overlay.show_overlays = prev_overlays
            target_space.shading.type             = prev_shading["type"]
            target_space.shading.light            = prev_shading["light"]
            try:
                target_space.shading.studio_light = prev_shading["studio_light"]
            except (TypeError, ValueError):
                pass
            target_space.shading.show_xray        = prev_shading["show_xray"]
            target_space.shading.show_cavity      = prev_shading["show_cavity"]
            target_space.shading.background_type  = prev_shading["background_type"]
            target_space.shading.background_color = prev_shading["background_color"]
            scene.render.filepath               = prev["filepath"]
            scene.render.image_settings.file_format = prev["fmt"]
            scene.render.image_settings.color_mode  = prev["color_mode"]
            scene.render.film_transparent       = prev["transparent"]
            scene.render.resolution_x           = prev["res_x"]
            scene.render.resolution_y           = prev["res_y"]

        return {"FINISHED"}


# Track the keymap entry we add so a re-run of the script could clean up
# (not strictly needed for one-shot interactive sessions but tidy).
_REGISTERED_KEYMAP = []


def register_capture(args, helmet_size):
    """Register the operator + Ctrl+Shift+P keymap. Idempotent-ish."""
    _CAPTURE_STATE["output_path"] = os.path.abspath(args.capture_output)
    _CAPTURE_STATE["fov_deg"] = args.capture_fov
    _CAPTURE_STATE["res_x"] = args.capture_res_x
    _CAPTURE_STATE["res_y"] = args.capture_res_y
    _CAPTURE_STATE["uniq"] = args.capture_uniq
    _CAPTURE_STATE["counter"] = 0
    _CAPTURE_STATE["helmet_size"] = helmet_size
    _CAPTURE_STATE["ai_prep"] = bool(getattr(args, "ai_prep", False))

    # Operator (skip if already registered, e.g. re-run inside the same session)
    if not hasattr(bpy.types, HELMET_OT_capture_view.bl_idname.replace(".", "_").upper()):
        try:
            bpy.utils.register_class(HELMET_OT_capture_view)
        except ValueError:
            pass  # already registered

    # Keymap: Ctrl+Shift+P in the 3D View. (We avoid F-keys because on
    # macOS they default to system media controls — Cmd+Shift+F12 just
    # raises the volume.)
    wm = bpy.context.window_manager
    kc = wm.keyconfigs.user or wm.keyconfigs.default
    if kc:
        km = kc.keymaps.get("3D View") or kc.keymaps.new(
            name="3D View", space_type="VIEW_3D")
        kmi = km.keymap_items.new(
            "helmet.capture_view", type="P",
            value="PRESS", ctrl=True, shift=True)
        _REGISTERED_KEYMAP.append((km, kmi))


# ─── main ────────────────────────────────────────────────────────────────
def main():
    args = parse_args()
    reset_scene()

    obj_files = sorted(glob(os.path.join(args.input_dir, "model_*.obj")))
    if not obj_files:
        print(f"ERROR: no model_*.obj in {args.input_dir}", file=sys.stderr)
        sys.exit(1)

    print(f"importing {len(obj_files)} OBJ files…")
    all_objs = []
    name_to_objs = {}  # basename ("model_22.obj") → [imported objects]
    for path in obj_files:
        new = import_obj(path)
        all_objs.extend(new)
        name_to_objs[os.path.basename(path)] = new

    # Apply --hide: viewport AND render hidden, so they leave alpha=0 in
    # the captured PNG (Ctrl+Shift+P uses render.opengl which honours
    # viewport visibility).
    for fname in args.hide:
        targets = name_to_objs.get(fname)
        if not targets:
            print(f"WARNING: --hide {fname} not found among imports",
                  file=sys.stderr)
            continue
        for o in targets:
            o.hide_set(True)
            o.hide_render = True
            print(f"  hidden: {o.name} (from {fname})")

    textured = False
    if args.use_textures:
        print(f"loading PBR atlas textures from {args.input_dir} …")
        textured = apply_textures(all_objs, args.input_dir)
    if not textured:
        print(f"colouring {len(all_objs)} parts with random palette…")
        colour_objects(all_objs)

    # Global bbox over everything for camera placement
    centers = [bbox_center_world(o) for o in all_objs]
    cx = sum(c.x for c in centers) / len(centers)
    cy = sum(c.y for c in centers) / len(centers)
    cz = sum(c.z for c in centers) / len(centers)
    center = Vector((cx, cy, cz))

    corners = [o.matrix_world @ Vector(v)
               for o in all_objs for v in o.bound_box]
    bb_min = Vector((min(c.x for c in corners),
                     min(c.y for c in corners),
                     min(c.z for c in corners)))
    bb_max = Vector((max(c.x for c in corners),
                     max(c.y for c in corners),
                     max(c.z for c in corners)))
    helmet_size = max((bb_max - bb_min).x,
                      (bb_max - bb_min).y,
                      (bb_max - bb_min).z)

    found, eye = park_viewport_inside(center, helmet_size,
                                       args.look_axis,
                                       args.cam_back, args.cam_up,
                                       textured=textured,
                                       no_xray=args.no_xray)
    if not found:
        print("WARNING: no VIEW_3D area found — viewport not parked. "
              "Did you launch Blender without --background?")

    if args.capture_output:
        register_capture(args, helmet_size)

    print()
    print(f"  {len(all_objs)} parts loaded.")
    print(f"  helmet size:        {helmet_size:.2f} units (largest dim)")
    print(f"  bbox center:        ({center.x:+.2f}, {center.y:+.2f}, {center.z:+.2f})")
    print(f"  viewport eye at:    ({eye.x:+.2f}, {eye.y:+.2f}, {eye.z:+.2f})")
    print(f"  looking along:      {args.look_axis}")
    print()
    print("Orbit with middle-click + drag. '/' on numpad to isolate the")
    print("selected mesh. Each mesh has a random flat colour to tell parts apart.")
    if args.capture_output:
        print()
        mode_label = "AI-prep (matcap + grey BG + sidecar mask)" \
                     if args.ai_prep else "WYSIWYG (viewport shading, transparent BG)"
        print(f"  Capture armed: press Ctrl+Shift+P in the 3D View to render")
        print(f"  the current view — mode: {mode_label}")
        print(f"  Resolution: {args.capture_res_x}×{args.capture_res_y}")
        print(f"  Color → {os.path.abspath(args.capture_output)}"
              f"{' (suffixed _001/_002/…)' if args.capture_uniq else ''}")
        if args.ai_prep:
            print(f"  Mask  → <same path>_mask.png "
                  f"(re-apply as layer mask in GIMP after img2img)")
        print(f"  Also accessible via F3 search → 'Capture Helmet View'")


if __name__ == "__main__":
    main()
