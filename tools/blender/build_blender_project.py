"""
Builds the master Blender project for Voxel FPS Engine containing:
  - Canonical 20-bone humanoid rig + 3 weapon bones + IK constraints
  - Skinned 16 mm sub-lattice character mesh with 19 vertex groups and weights
  - In-game weapon models from data/voxfmt/*.vox.json (starter_rifle, shotgun, etc.)
  - In-game animation Actions loaded from data/anims/*.anim.json
Saves to data/blender/voxel_rig_and_models.blend and local Blender AppData.
"""

import sys
import os
import json
import math
from typing import Dict, List, Tuple, Any, Optional

try:
    import bpy
    import mathutils
except ImportError:
    print("Error: bpy module must be run from inside Blender.")
    sys.exit(1)

# Add current directory to sys.path so we can import voxel_rig_anim
script_dir = os.path.dirname(os.path.abspath(__file__))
if script_dir not in sys.path:
    sys.path.append(script_dir)

import voxel_rig_anim


def decode_vox_cells(vox_data: Dict[str, Any]) -> List[Tuple[int, int, int, str]]:
    """Decode solid cells (x, y, z, material) from v1 or v2 voxfmt JSON."""
    cells = []
    if "cells_rle" in vox_data:
        rle = vox_data["cells_rle"]
        palette = rle.get("palette", ["concrete"])
        runs = rle.get("runs", [])
        # Each run is 5 ints: [y, z, x0, len, idx]
        for i in range(0, len(runs), 5):
            if i + 4 >= len(runs):
                break
            y = runs[i]
            z = runs[i + 1]
            x0 = runs[i + 2]
            length = runs[i + 3]
            idx = runs[i + 4]
            mat_name = palette[idx] if idx < len(palette) else "concrete"
            for x in range(x0, x0 + length):
                cells.append((x, y, z, mat_name))
    elif "voxels" in vox_data:
        for v in vox_data["voxels"]:
            cells.append((v["x"], v["y"], v["z"], v.get("mat", "concrete")))
    return cells


def create_weapon_mesh(name: str, vox_path: str, weapon_collection: Any) -> Optional[Any]:
    """Create a 3D mesh object from a weapon .vox.json file."""
    if not os.path.exists(vox_path):
        return None

    try:
        with open(vox_path, 'r', encoding='utf-8') as f:
            vox_data = json.load(f)
    except Exception as e:
        print(f"Warning: Failed to load {vox_path}: {e}")
        return None

    dims = vox_data.get("dims", [16, 16, 16])
    v_size = vox_data.get("voxel_size", 0.001) # Typically 1mm
    cells = decode_vox_cells(vox_data)
    if not cells:
        return None

    # Anchors for pivot centering
    anchors = vox_data.get("anchors", {})
    pivot = anchors.get("pivot", [dims[0] // 2, 0, dims[2] // 2])

    # Build box vertices and faces for solid cells
    verts = []
    faces = []

    # Unit cube offsets
    cube_verts = [
        (0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0),
        (0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)
    ]
    cube_faces = [
        (0, 1, 2, 3), (4, 7, 6, 5),
        (0, 4, 5, 1), (2, 6, 7, 3),
        (0, 3, 7, 4), (1, 5, 6, 2)
    ]

    cell_set = {(c[0], c[1], c[2]) for c in cells}

    for (x, y, z, mat) in cells:
        # Check exposed faces to keep mesh light
        neighbors = [
            (x, y, z - 1), (x, y, z + 1),
            (x, y - 1, z), (x, y + 1, z),
            (x - 1, y, z), (x + 1, y, z)
        ]

        # In engine space relative to pivot:
        ex = (x - pivot[0]) * v_size
        ey = (y - pivot[1]) * v_size
        ez = (z - pivot[2]) * v_size

        base_idx = len(verts)
        for vx, vy, vz in cube_verts:
            lx = ex + vx * v_size
            ly = ey + vy * v_size
            lz = ez + vz * v_size
            # Convert to Blender coords (X, -Z, Y)
            verts.append((lx, -lz, ly))

        for f_idx, (nx, ny, nz) in enumerate(neighbors):
            if (nx, ny, nz) not in cell_set:
                cf = cube_faces[f_idx]
                faces.append((base_idx + cf[0], base_idx + cf[1], base_idx + cf[2], base_idx + cf[3]))

    mesh_data = bpy.data.meshes.new(f"{name}_Mesh_Data")
    mesh_data.from_pydata(verts, [], faces)
    mesh_data.update()

    mesh_obj = bpy.data.objects.new(name, mesh_data)
    weapon_collection.objects.link(mesh_obj)
    return mesh_obj


def main():
    print("=== BUILDING VOXEL FPS ENGINE BLENDER PROJECT ===")
    repo_root = os.path.abspath(os.path.join(script_dir, "..", ".."))
    data_dir = os.path.join(repo_root, "data")
    voxfmt_dir = os.path.join(data_dir, "voxfmt")
    anims_dir = os.path.join(data_dir, "anims")
    blender_out_dir = os.path.join(data_dir, "blender")
    os.makedirs(blender_out_dir, exist_ok=True)

    # 1. Reset scene
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene

    # 2. Setup Collections
    col_char = bpy.data.collections.new("Character")
    col_weapons = bpy.data.collections.new("Weapons")
    scene.collection.children.link(col_char)
    scene.collection.children.link(col_weapons)

    # 3. Create Armature
    print("Creating canonical 20-bone humanoid rig + 3 weapon bones...")
    arm_obj = voxel_rig_anim.create_armature(add_weapon_bones=True, setup_ik=True)
    # Move to Character collection
    if arm_obj.name in scene.collection.objects:
        scene.collection.objects.unlink(arm_obj)
    col_char.objects.link(arm_obj)

    # 4. Create Skinned Character Mesh
    print("Creating skinned 16mm sub-lattice character mesh...")
    char_mesh = voxel_rig_anim.create_character_mesh(arm_obj)
    if char_mesh.name in scene.collection.objects:
        scene.collection.objects.unlink(char_mesh)
    col_char.objects.link(char_mesh)

    # 5. Load In-game Weapons
    print("Loading in-game weapon models from data/voxfmt/*.vox.json...")
    weapon_files = [
        ("starter_rifle", os.path.join(voxfmt_dir, "starter_rifle.vox.json")),
        ("shotgun_light", os.path.join(voxfmt_dir, "shotgun_light.vox.json")),
        ("carbine_energy", os.path.join(voxfmt_dir, "carbine_energy.vox.json")),
        ("sidearm_light", os.path.join(voxfmt_dir, "sidearm_light.vox.json")),
        ("smg_light", os.path.join(voxfmt_dir, "smg_light.vox.json")),
        ("marksman_rifle", os.path.join(voxfmt_dir, "marksman_rifle.vox.json")),
        ("lmg_medium", os.path.join(voxfmt_dir, "lmg_medium.vox.json")),
        ("sniper_heavy", os.path.join(voxfmt_dir, "sniper_heavy.vox.json")),
        ("cannon_heavy", os.path.join(voxfmt_dir, "cannon_heavy.vox.json")),
        ("railgun_energy", os.path.join(voxfmt_dir, "railgun_energy.vox.json")),
    ]

    loaded_weapons = {}
    for name, path in weapon_files:
        obj = create_weapon_mesh(name, path, col_weapons)
        if obj:
            loaded_weapons[name] = obj
            print(f"  Loaded weapon: {name} ({len(obj.data.vertices)} vertices)")

    # Parent starter_rifle to Weapon_Root bone
    if "starter_rifle" in loaded_weapons:
        sr = loaded_weapons["starter_rifle"]
        sr.parent = arm_obj
        sr.parent_type = 'BONE'
        sr.parent_bone = "Weapon_Root"
        # Position at bone origin
        sr.location = (0.0, 0.0, 0.0)

    # 6. Load in-game Animations
    print("Loading in-game animation cycles from data/anims/*.anim.json...")
    anim_files = [
        "rifle_idle.anim.json",
        "rifle_fire.anim.json",
        "rifle_reload.anim.json",
        "walk.anim.json",
    ]

    for af in anim_files:
        af_path = os.path.join(anims_dir, af)
        if os.path.exists(af_path):
            action = voxel_rig_anim.import_anim_json_to_action(arm_obj, af_path)
            if action:
                action.use_fake_user = True
                print(f"  Loaded action: {action.name}")

    # Set active action to rifle_idle
    idle_action = bpy.data.actions.get("rifle_idle") or bpy.data.actions.get("walk")
    if idle_action:
        arm_obj.animation_data.action = idle_action

    # 7. Save master .blend files
    # A: In repo
    repo_blend = os.path.join(blender_out_dir, "voxel_rig_and_models.blend")
    bpy.ops.wm.save_as_mainfile(filepath=repo_blend)
    print(f"Saved repository project: {repo_blend}")

    # B: In local Blender AppData
    appdata_52 = os.path.expandvars(r"%APPDATA%\Blender Foundation\Blender\5.2")
    if os.path.exists(appdata_52):
        appdata_blend = os.path.join(appdata_52, "voxel_rig_and_models.blend")
        bpy.ops.wm.save_as_mainfile(filepath=appdata_blend)
        print(f"Saved local Blender instance project: {appdata_blend}")

    print("=== BLENDER PROJECT BUILD COMPLETED SUCCESSFULLY ===")


if __name__ == "__main__":
    main()
