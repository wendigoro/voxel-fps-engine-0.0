"""
Blender Project Fetch Script:
Extracts 3D mesh objects from a .blend file into Voxel FPS Engine voxfmt JSON.
Can be invoked headlessly via:
  blender <project.blend> --background --python fetch_blend.py -- --object <ObjectName> --out <output.vox.json>
  blender <project.blend> --background --python fetch_blend.py -- --list --out <objects.json>
"""

import sys
import os
import json
import math
from typing import Dict, List, Tuple, Any

try:
    import bpy
    import mathutils
except ImportError:
    print("Error: bpy module must be run from inside Blender.")
    sys.exit(1)


def parse_args():
    args = sys.argv
    if "--" in args:
        args = args[args.index("--") + 1:]
    else:
        args = []

    parsed = {
        "object": None,
        "out": None,
        "list": False,
        "voxel_size": 0.001,
        "grid_size": 32,
    }

    i = 0
    while i < len(args):
        a = args[i]
        if a == "--object" and i + 1 < len(args):
            parsed["object"] = args[i + 1]
            i += 2
        elif a == "--out" and i + 1 < len(args):
            parsed["out"] = args[i + 1]
            i += 2
        elif a == "--list":
            parsed["list"] = True
            i += 1
        elif a == "--voxel-size" and i + 1 < len(args):
            parsed["voxel_size"] = float(args[i + 1])
            i += 2
        else:
            i += 1
    return parsed


def list_objects(out_path: str):
    """List all mesh objects in the opened .blend file."""
    mesh_objs = [obj.name for obj in bpy.data.objects if obj.type == 'MESH']
    result = {"mesh_objects": mesh_objs}
    if out_path:
        with open(out_path, 'w', encoding='utf-8') as f:
            json.dump(result, f, indent=2)
    print(f"MESH_OBJECTS: {','.join(mesh_objs)}")


def extract_object_voxels(obj: Any, voxel_size: float = 0.001) -> Dict[str, Any]:
    """
    Extract voxel cells from a Blender mesh object.
    Coordinates converted from Blender (X, Y, Z) to Engine (X, Z, -Y).
    """
    mesh = obj.data

    # Bounding box in object local space
    bbox = [obj.matrix_world @ mathutils.Vector(corner) for corner in obj.bound_box]
    min_x = min(v.x for v in bbox)
    max_x = max(v.x for v in bbox)
    min_y = min(v.y for v in bbox)
    max_y = max(v.y for v in bbox)
    min_z = min(v.z for v in bbox)
    max_z = max(v.z for v in bbox)

    # In engine space:
    # x_e = x_b
    # y_e = z_b
    # z_e = -y_b
    # Cell grid dimensions
    dx = max(1, int(math.ceil((max_x - min_x) / voxel_size)) + 1)
    dy = max(1, int(math.ceil((max_z - min_z) / voxel_size)) + 1)
    dz = max(1, int(math.ceil((max_y - min_y) / voxel_size)) + 1)

    # Cap to reasonable dimensions if huge
    if dx > 128 or dy > 128 or dz > 128:
        # Scale to fit in 64 max dim
        max_d = max(dx, dy, dz)
        scale = 64.0 / max_d
        voxel_size /= scale
        dx = max(1, int(math.ceil((max_x - min_x) / voxel_size)) + 1)
        dy = max(1, int(math.ceil((max_z - min_z) / voxel_size)) + 1)
        dz = max(1, int(math.ceil((max_y - min_y) / voxel_size)) + 1)

    # Sample solid cells from vertices and faces
    occupied = set()

    for poly in mesh.polygons:
        # Centroid of polygon
        c = poly.center
        world_c = obj.matrix_world @ c
        # Convert to grid indices
        gx = int(round((world_c.x - min_x) / voxel_size))
        gy = int(round((world_c.z - min_z) / voxel_size))
        gz = int(round((-world_c.y - (-max_y)) / voxel_size))
        if 0 <= gx < dx and 0 <= gy < dy and 0 <= gz < dz:
            occupied.add((gx, gy, gz))

    for vert in mesh.vertices:
        wv = obj.matrix_world @ vert.co
        gx = int(round((wv.x - min_x) / voxel_size))
        gy = int(round((wv.z - min_z) / voxel_size))
        gz = int(round((-wv.y - (-max_y)) / voxel_size))
        if 0 <= gx < dx and 0 <= gy < dy and 0 <= gz < dz:
            occupied.add((gx, gy, gz))

    # Fallback if empty (e.g. point mesh)
    if not occupied:
        occupied.add((0, 0, 0))

    # Encode into cells_rle
    palette = ["sheet_metal"]
    runs = []

    for y in range(dy):
        for z in range(dz):
            x = 0
            while x < dx:
                if (x, y, z) in occupied:
                    x0 = x
                    length = 0
                    while x < dx and (x, y, z) in occupied:
                        length += 1
                        x += 1
                    runs.extend([y, z, x0, length, 0])
                else:
                    x += 1

    mode = "weapon" if "rifle" in obj.name.lower() or "gun" in obj.name.lower() else "model"
    if "character" in obj.name.lower():
        mode = "character"

    return {
        "format_version": 2,
        "unit": 1,
        "voxel_size": voxel_size,
        "mode": mode,
        "dims": [dx, dy, dz],
        "anchors": {
            "pivot": [dx // 2, 0, dz // 2]
        },
        "cells_rle": {
            "palette": palette,
            "runs": runs
        }
    }


def main():
    parsed = parse_args()

    if parsed["list"]:
        list_objects(parsed["out"])
        return

    target_name = parsed["object"]
    target_obj = None

    if target_name:
        target_obj = bpy.data.objects.get(target_name)
    else:
        # Default to first mesh object
        for obj in bpy.data.objects:
            if obj.type == 'MESH':
                target_obj = obj
                break

    if not target_obj:
        print(f"Error: Target object '{target_name}' not found.")
        sys.exit(1)

    print(f"Extracting voxels from Blender object: {target_obj.name}...")
    vox_data = extract_object_voxels(target_obj, voxel_size=parsed["voxel_size"])

    out_path = parsed["out"]
    if out_path:
        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
        with open(out_path, 'w', encoding='utf-8') as f:
            json.dump(vox_data, f, indent=2)
        print(f"Saved voxfmt JSON to {out_path} (cells={len(vox_data['cells_rle']['runs']) // 5} runs)")
    else:
        print(json.dumps(vox_data, indent=2))

    print("FETCH_BLEND_OK")


if __name__ == "__main__":
    main()
