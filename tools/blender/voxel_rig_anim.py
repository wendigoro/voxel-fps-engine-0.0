"""
Voxel FPS Engine - Blender Rigging, Animation, and Character Modeling Suite.

Features:
  - Canonical 20-bone humanoid skeleton generator with correct parenting, heads, tails, and rolls.
  - 3 weapon attachment bones (Root, Bolt, Mag) parented to Hand.R.
  - Optional two-bone IK setup for arms and legs with pole targets.
  - 16 mm sub-lattice character mesh generator with vertex groups and skinning weights.
  - Bi-directional .anim.json import and export with coordinate conversions:
      Blender (Z-Up, Y-Forward, X-Right) <-> Engine (Y-Up, Z-Forward, X-Right).
  - Can be run as a standalone headless CLI script or installed as a Blender Addon.
"""

bl_info = {
    "name": "Voxel FPS Rig & Animation Tooling",
    "author": "Voxel FPS Engine Team",
    "version": (1, 0, 0),
    "blender": (4, 0, 0),
    "location": "View3D > Sidebar > Voxel Engine",
    "description": "Rigging, animation exchange, and skinned character modeling for Voxel FPS Engine",
    "category": "Animation",
}

import sys
import os
import json
import math
from typing import Dict, List, Tuple, Optional, Any

try:
    import bpy
    import mathutils
except ImportError:
    bpy = None
    mathutils = None

# 16 mm sub-lattice cell size in meters
CELL_SIZE = 0.016

# Canonical bone definitions: (name, parent, head_engine, tail_engine, roll_deg)
# Engine coordinates: X-Right, Y-Up, Z-Forward
CANONICAL_BONES = [
    # Core spine chain
    ("Root", None, (0.0, 0.0, 0.0), (0.0, 0.2, 0.0), 0.0),
    ("Pelvis", "Root", (0.0, 56.0 * CELL_SIZE, 0.0), (0.0, 66.0 * CELL_SIZE, 0.0), 0.0),
    ("Spine", "Pelvis", (0.0, 66.0 * CELL_SIZE, 0.0), (0.0, 80.0 * CELL_SIZE, 0.0), 0.0),
    ("Chest", "Spine", (0.0, 80.0 * CELL_SIZE, 0.0), (0.0, 91.0 * CELL_SIZE, 0.0), 0.0),
    ("Neck", "Chest", (0.0, 91.0 * CELL_SIZE, 0.0), (0.0, 101.0 * CELL_SIZE, 0.0), 0.0),
    ("Head", "Neck", (0.0, 101.0 * CELL_SIZE, 0.0), (0.0, 110.0 * CELL_SIZE, 0.0), 0.0),

    # Left Arm Chain
    ("Shoulder.L", "Chest", (-9.0 * CELL_SIZE, 86.0 * CELL_SIZE, 0.0), (-12.0 * CELL_SIZE, 78.0 * CELL_SIZE, 0.0), 0.0),
    ("UpperArm.L", "Shoulder.L", (-12.0 * CELL_SIZE, 78.0 * CELL_SIZE, 0.0), (-12.0 * CELL_SIZE, 60.0 * CELL_SIZE, 0.0), 0.0),
    ("Forearm.L", "UpperArm.L", (-12.0 * CELL_SIZE, 60.0 * CELL_SIZE, 0.0), (-12.0 * CELL_SIZE, 45.0 * CELL_SIZE, 0.0), 0.0),
    ("Hand.L", "Forearm.L", (-12.0 * CELL_SIZE, 45.0 * CELL_SIZE, 0.0), (-12.0 * CELL_SIZE, 40.0 * CELL_SIZE, 0.0), 0.0),

    # Right Arm Chain
    ("Shoulder.R", "Chest", (9.0 * CELL_SIZE, 86.0 * CELL_SIZE, 0.0), (12.0 * CELL_SIZE, 78.0 * CELL_SIZE, 0.0), 0.0),
    ("UpperArm.R", "Shoulder.R", (12.0 * CELL_SIZE, 78.0 * CELL_SIZE, 0.0), (12.0 * CELL_SIZE, 60.0 * CELL_SIZE, 0.0), 0.0),
    ("Forearm.R", "UpperArm.R", (12.0 * CELL_SIZE, 60.0 * CELL_SIZE, 0.0), (12.0 * CELL_SIZE, 45.0 * CELL_SIZE, 0.0), 0.0),
    ("Hand.R", "Forearm.R", (12.0 * CELL_SIZE, 45.0 * CELL_SIZE, 0.0), (12.0 * CELL_SIZE, 40.0 * CELL_SIZE, 0.0), 0.0),

    # Left Leg Chain
    ("Thigh.L", "Pelvis", (-5.0 * CELL_SIZE, 40.0 * CELL_SIZE, 0.0), (-5.0 * CELL_SIZE, 17.0 * CELL_SIZE, 0.0), 0.0),
    ("Calf.L", "Thigh.L", (-5.0 * CELL_SIZE, 17.0 * CELL_SIZE, 0.0), (-5.0 * CELL_SIZE, 3.5 * CELL_SIZE, 1.5 * CELL_SIZE), 0.0),
    ("Foot.L", "Calf.L", (-5.0 * CELL_SIZE, 3.5 * CELL_SIZE, 1.5 * CELL_SIZE), (-5.0 * CELL_SIZE, 2.0 * CELL_SIZE, 5.0 * CELL_SIZE), 0.0),

    # Right Leg Chain
    ("Thigh.R", "Pelvis", (5.0 * CELL_SIZE, 40.0 * CELL_SIZE, 0.0), (5.0 * CELL_SIZE, 17.0 * CELL_SIZE, 0.0), 0.0),
    ("Calf.R", "Thigh.R", (5.0 * CELL_SIZE, 17.0 * CELL_SIZE, 0.0), (5.0 * CELL_SIZE, 3.5 * CELL_SIZE, 1.5 * CELL_SIZE), 0.0),
    ("Foot.R", "Calf.R", (5.0 * CELL_SIZE, 3.5 * CELL_SIZE, 1.5 * CELL_SIZE), (5.0 * CELL_SIZE, 2.0 * CELL_SIZE, 5.0 * CELL_SIZE), 0.0),
]

# Weapon accessory bones parented to Hand.R
WEAPON_BONES = [
    ("Weapon_Root", "Hand.R", (12.0 * CELL_SIZE, 40.0 * CELL_SIZE, 5.0 * CELL_SIZE), (12.0 * CELL_SIZE, 40.0 * CELL_SIZE, 15.0 * CELL_SIZE), 0.0),
    ("Weapon_Bolt", "Weapon_Root", (12.0 * CELL_SIZE, 43.0 * CELL_SIZE, 7.0 * CELL_SIZE), (12.0 * CELL_SIZE, 43.0 * CELL_SIZE, 4.0 * CELL_SIZE), 0.0),
    ("Weapon_Mag", "Weapon_Root", (12.0 * CELL_SIZE, 36.0 * CELL_SIZE, 8.0 * CELL_SIZE), (12.0 * CELL_SIZE, 28.0 * CELL_SIZE, 8.0 * CELL_SIZE), 0.0),
]

# Body regions matching char_model::BodyRegion
# (region_name, primary_bone, secondary_bone, blend_factor, [x0, x1, y0, y1, z0, z1] in cells)
# cx = 24, cz = 12
CX = 24
CZ = 12
BODY_REGIONS = [
    ("Region_LFoot", "Foot.L", "Calf.L", 0.3, [CX - 7, CX - 3, 2, 5, CZ - 2, CZ + 5]),
    ("Region_RFoot", "Foot.R", "Calf.R", 0.3, [CX + 3, CX + 7, 2, 5, CZ - 2, CZ + 5]),
    ("Region_LCalf", "Calf.L", "Thigh.L", 0.4, [CX - 7, CX - 3, 6, 28, CZ - 2, CZ + 2]),
    ("Region_RCalf", "Calf.R", "Thigh.R", 0.4, [CX + 3, CX + 7, 6, 28, CZ - 2, CZ + 2]),
    ("Region_LThigh", "Thigh.L", "Pelvis", 0.4, [CX - 7, CX - 2, 29, 52, CZ - 2, CZ + 3]),
    ("Region_RThigh", "Thigh.R", "Pelvis", 0.4, [CX + 2, CX + 7, 29, 52, CZ - 2, CZ + 3]),
    ("Region_Pelvis", "Pelvis", "Spine", 0.4, [CX - 8, CX + 8, 53, 60, CZ - 2, CZ + 3]),
    ("Region_Spine", "Spine", "Chest", 0.5, [CX - 7, CX + 7, 61, 72, CZ - 2, CZ + 2]),
    ("Region_Chest", "Chest", "Neck", 0.4, [CX - 9, CX + 9, 73, 88, CZ - 3, CZ + 4]),
    ("Region_Neck", "Neck", "Head", 0.5, [CX - 2, CX + 2, 89, 94, CZ - 2, CZ + 2]),
    ("Region_Head", "Head", "Neck", 0.2, [CX - 5, CX + 5, 95, 108, CZ - 4, CZ + 4]),
    ("Region_LUpperArm", "UpperArm.L", "Shoulder.L", 0.4, [CX - 14, CX - 10, 70, 86, CZ - 2, CZ + 2]),
    ("Region_LForeArm", "Forearm.L", "UpperArm.L", 0.4, [CX - 14, CX - 10, 50, 69, CZ - 2, CZ + 2]),
    ("Region_LHand", "Hand.L", "Forearm.L", 0.3, [CX - 14, CX - 10, 40, 49, CZ - 2, CZ + 1]),
    ("Region_RUpperArm", "UpperArm.R", "Shoulder.R", 0.4, [CX + 10, CX + 14, 70, 86, CZ - 2, CZ + 2]),
    ("Region_RForeArm", "Forearm.R", "UpperArm.R", 0.4, [CX + 10, CX + 14, 50, 69, CZ - 2, CZ + 2]),
    ("Region_RHand", "Hand.R", "Forearm.R", 0.3, [CX + 10, CX + 14, 40, 49, CZ - 2, CZ + 1]),
]


def engine_to_blender_pos(p: Tuple[float, float, float]) -> Tuple[float, float, float]:
    """Convert Engine position (X-Right, Y-Up, Z-Forward) to Blender (X-Right, Y-Forward, Z-Up)."""
    return (p[0], -p[2], p[1])


def blender_to_engine_pos(p: Tuple[float, float, float]) -> Tuple[float, float, float]:
    """Convert Blender position (X-Right, Y-Forward, Z-Up) to Engine (X-Right, Y-Up, Z-Forward)."""
    return (p[0], p[2], -p[1])


def engine_to_blender_quat(q: Tuple[float, float, float, float]) -> Tuple[float, float, float, float]:
    """
    Convert Engine quaternion (x, y, z, w) to Blender quaternion (w, x, y, z).
    Note: Blender mathutils.Quaternion is ordered (w, x, y, z).
    Engine quaternion is (x, y, z, w).
    """
    qx, qy, qz, qw = q
    # Invert mapping: x_b = x_e, y_b = -z_e, z_b = y_e, w_b = w_e
    return (qw, qx, -qz, qy)


def blender_to_engine_quat(q: Tuple[float, float, float, float]) -> Tuple[float, float, float, float]:
    """
    Convert Blender quaternion (w, x, y, z) to Engine quaternion (x, y, z, w).
    """
    qw, qx, qy, qz = q
    # Forward mapping: x_e = x_b, y_e = z_b, z_e = -y_b, w_e = w_b
    return (qx, qz, -qy, qw)


def normalize_bone_name(raw_name: str) -> str:
    """Normalize bone name for engine lookup."""
    s = raw_name.strip().lower()
    for prefix in ["mixamorig:", "def-", "def_", "mch-", "org-", "bip01_"]:
        if s.startswith(prefix):
            s = s[len(prefix):]
            break

    is_left = False
    is_right = False
    if s.startswith("left_"):
        is_left = True
        s = s[5:]
    elif s.startswith("left"):
        is_left = True
        s = s[4:]
    elif s.startswith("l_"):
        is_left = True
        s = s[2:]
    elif s.startswith("right_"):
        is_right = True
        s = s[6:]
    elif s.startswith("right"):
        is_right = True
        s = s[5:]
    elif s.startswith("r_"):
        is_right = True
        s = s[2:]

    if not is_left and not is_right:
        if s.endswith(".l") or s.endswith("_l") or s.endswith("-l"):
            is_left = True
            s = s[:-2]
        elif s.endswith(".r") or s.endswith("_r") or s.endswith("-r"):
            is_right = True
            s = s[:-2]
        elif s.endswith("_left"):
            is_left = True
            s = s[:-5]
        elif s.endswith("_right"):
            is_right = True
            s = s[:-6]

    s = s.strip("_-.")
    synonyms = {
        "hips": "pelvis", "hip": "pelvis",
        "spine1": "spine", "spine_01": "spine", "lower_spine": "spine",
        "spine2": "chest", "spine_02": "chest", "upper_spine": "chest", "torso": "chest",
        "arm": "upper_arm", "upperarm": "upper_arm",
        "forearm": "forearm", "lowerarm": "forearm", "lower_arm": "forearm",
        "hand": "hand", "wrist": "hand",
        "thigh": "thigh", "upleg": "thigh", "up_leg": "thigh", "upperleg": "thigh",
        "calf": "calf", "shin": "calf", "leg": "calf", "lowerleg": "calf",
        "foot": "foot", "ankle": "foot",
        "clavicle": "shoulder", "shoulder": "shoulder",
        "weapon": "weapon_root", "weaponroot": "weapon_root",
        "bolt": "weapon_bolt", "weaponbolt": "weapon_bolt",
        "mag": "weapon_mag", "magazine": "weapon_mag", "weaponmag": "weapon_mag",
    }
    s = synonyms.get(s, s)

    if is_left:
        return f"l_{s}"
    if is_right:
        return f"r_{s}"
    return s


def create_armature(add_weapon_bones: bool = True, setup_ik: bool = True) -> Any:
    """Generate the canonical 20-bone humanoid armature in Blender."""
    if bpy is None:
        raise RuntimeError("Blender (bpy) module is required to create armature.")

    armature_data = bpy.data.armatures.new("Voxel_Armature_Data")
    armature_data.display_type = 'OCTAHEDRAL'
    armature_obj = bpy.data.objects.new("Voxel_Armature", armature_data)
    bpy.context.collection.objects.link(armature_obj)

    bpy.context.view_layer.objects.active = armature_obj
    armature_obj.select_set(True)
    bpy.ops.object.mode_set(mode='EDIT')

    edit_bones = armature_data.edit_bones

    # 1. Create canonical bones
    for name, parent_name, head_eng, tail_eng, roll in CANONICAL_BONES:
        bone = edit_bones.new(name)
        bone.head = engine_to_blender_pos(head_eng)
        bone.tail = engine_to_blender_pos(tail_eng)
        bone.roll = math.radians(roll)
        if parent_name and parent_name in edit_bones:
            bone.parent = edit_bones[parent_name]

    # 2. Add weapon accessory bones
    if add_weapon_bones:
        for name, parent_name, head_eng, tail_eng, roll in WEAPON_BONES:
            bone = edit_bones.new(name)
            bone.head = engine_to_blender_pos(head_eng)
            bone.tail = engine_to_blender_pos(tail_eng)
            bone.roll = math.radians(roll)
            if parent_name and parent_name in edit_bones:
                bone.parent = edit_bones[parent_name]

    # 3. Setup IK Targets if requested
    if setup_ik:
        # Hands IK
        for side, hand_bone in [("L", "Hand.L"), ("R", "Hand.R")]:
            ik_target = edit_bones.new(f"IK_Hand.{side}")
            ik_target.head = edit_bones[hand_bone].head
            ik_target.tail = (ik_target.head[0], ik_target.head[1] - 0.1, ik_target.head[2])
            ik_target.parent = edit_bones["Root"]

            elbow_bone = f"Forearm.{side}"
            pole = edit_bones.new(f"Pole_Elbow.{side}")
            pole.head = (edit_bones[elbow_bone].head[0], edit_bones[elbow_bone].head[1] + 0.2, edit_bones[elbow_bone].head[2])
            pole.tail = (pole.head[0], pole.head[1], pole.head[2] + 0.05)
            pole.parent = edit_bones["Root"]

        # Feet IK
        for side, foot_bone in [("L", "Foot.L"), ("R", "Foot.R")]:
            ik_target = edit_bones.new(f"IK_Foot.{side}")
            ik_target.head = edit_bones[foot_bone].head
            ik_target.tail = (ik_target.head[0], ik_target.head[1] - 0.15, ik_target.head[2])
            ik_target.parent = edit_bones["Root"]

            knee_bone = f"Calf.{side}"
            pole = edit_bones.new(f"Pole_Knee.{side}")
            pole.head = (edit_bones[knee_bone].head[0], edit_bones[knee_bone].head[1] - 0.25, edit_bones[knee_bone].head[2])
            pole.tail = (pole.head[0], pole.head[1], pole.head[2] + 0.05)
            pole.parent = edit_bones["Root"]

    bpy.ops.object.mode_set(mode='POSE')

    # Add IK Constraints in pose mode
    if setup_ik:
        pose_bones = armature_obj.pose.bones
        for side in ["L", "R"]:
            forearm = pose_bones.get(f"Forearm.{side}")
            if forearm:
                ik = forearm.constraints.new(type='IK')
                ik.target = armature_obj
                ik.subtarget = f"IK_Hand.{side}"
                ik.pole_target = armature_obj
                ik.pole_subtarget = f"Pole_Elbow.{side}"
                ik.chain_count = 2

            calf = pose_bones.get(f"Calf.{side}")
            if calf:
                ik = calf.constraints.new(type='IK')
                ik.target = armature_obj
                ik.subtarget = f"IK_Foot.{side}"
                ik.pole_target = armature_obj
                ik.pole_subtarget = f"Pole_Knee.{side}"
                ik.chain_count = 2

    bpy.ops.object.mode_set(mode='OBJECT')
    return armature_obj


def create_character_mesh(armature_obj: Optional[Any] = None) -> Any:
    """Generate skinned 16 mm sub-lattice character mesh with vertex groups and weights."""
    if bpy is None:
        raise RuntimeError("Blender (bpy) module is required to create character mesh.")

    mesh_data = bpy.data.meshes.new("Voxel_Character_Mesh_Data")
    mesh_obj = bpy.data.objects.new("Voxel_Character_Mesh", mesh_data)
    bpy.context.collection.objects.link(mesh_obj)

    verts: List[Tuple[float, float, float]] = []
    faces: List[Tuple[int, int, int, int]] = []
    vert_weights: Dict[str, List[Tuple[int, float]]] = {}

    origin_eng = (-48 * 0.5 * CELL_SIZE, 0.0, -24 * 0.5 * CELL_SIZE)

    for reg_name, bone_primary, bone_secondary, blend_fac, (x0, x1, y0, y1, z0, z1) in BODY_REGIONS:
        # Box bounds in model space (meters)
        min_eng = (
            origin_eng[0] + x0 * CELL_SIZE,
            origin_eng[1] + y0 * CELL_SIZE,
            origin_eng[2] + z0 * CELL_SIZE,
        )
        max_eng = (
            origin_eng[0] + (x1 + 1) * CELL_SIZE,
            origin_eng[1] + (y1 + 1) * CELL_SIZE,
            origin_eng[2] + (z1 + 1) * CELL_SIZE,
        )

        # 8 corners in engine coordinates
        corners_eng = [
            (min_eng[0], min_eng[1], min_eng[2]),
            (max_eng[0], min_eng[1], min_eng[2]),
            (max_eng[0], max_eng[1], min_eng[2]),
            (min_eng[0], max_eng[1], min_eng[2]),
            (min_eng[0], min_eng[1], max_eng[2]),
            (max_eng[0], min_eng[1], max_eng[2]),
            (max_eng[0], max_eng[1], max_eng[2]),
            (min_eng[0], max_eng[1], max_eng[2]),
        ]

        base_idx = len(verts)
        for c in corners_eng:
            v_blender = engine_to_blender_pos(c)
            verts.append(v_blender)

            # Weight calculation matching char_mesh::computeBoneWeights
            v_idx = len(verts) - 1
            w_prim = 1.0 - blend_fac * 0.5
            w_sec = blend_fac * 0.5

            if bone_primary not in vert_weights:
                vert_weights[bone_primary] = []
            vert_weights[bone_primary].append((v_idx, w_prim))

            if bone_secondary and bone_secondary != bone_primary:
                if bone_secondary not in vert_weights:
                    vert_weights[bone_secondary] = []
                vert_weights[bone_secondary].append((v_idx, w_sec))

        # 6 quad faces per box
        box_quads = [
            (0, 1, 2, 3), # Back (-Z)
            (4, 7, 6, 5), # Front (+Z)
            (0, 4, 5, 1), # Bottom (-Y)
            (2, 6, 7, 3), # Top (+Y)
            (0, 3, 7, 4), # Left (-X)
            (1, 5, 6, 2), # Right (+X)
        ]
        for q in box_quads:
            faces.append((base_idx + q[0], base_idx + q[1], base_idx + q[2], base_idx + q[3]))

    mesh_data.from_pydata(verts, [], faces)
    mesh_data.update()

    # Create Vertex Groups and assign weights
    for bone_name, assignments in vert_weights.items():
        vgroup = mesh_obj.vertex_groups.new(name=bone_name)
        for v_idx, weight in assignments:
            vgroup.add([v_idx], weight, 'REPLACE')

    # Parent to armature with Armature modifier if provided
    if armature_obj:
        mesh_obj.parent = armature_obj
        mod = mesh_obj.modifiers.new(name="Armature", type='ARMATURE')
        mod.object = armature_obj
        mod.use_vertex_groups = True

    return mesh_obj


def export_action_to_anim_json(armature_obj: Any, action: Any, filepath: str, fps: float = 30.0) -> bool:
    """Export Blender Action to Voxel FPS Engine .anim.json format."""
    if bpy is None:
        raise RuntimeError("Blender (bpy) is required for animation export.")

    if not armature_obj or armature_obj.type != 'ARMATURE':
        raise ValueError("Must provide an Armature object.")

    if not action:
        raise ValueError("Must provide a valid Action.")

    frame_start = int(action.frame_range[0])
    frame_end = int(action.frame_range[1])
    duration_frames = max(1, frame_end - frame_start)
    duration_sec = duration_frames / fps

    anim_id = os.path.splitext(os.path.basename(filepath))[0].replace(".anim", "")

    tracks_data: List[Dict[str, Any]] = []

    # Map bone channels
    blender_to_engine_bone = {
        "Root": "root", "Pelvis": "pelvis", "Spine": "spine", "Chest": "chest",
        "Neck": "neck", "Head": "head",
        "Shoulder.L": "l_shoulder", "UpperArm.L": "l_upper_arm", "Forearm.L": "l_forearm", "Hand.L": "l_hand",
        "Shoulder.R": "r_shoulder", "UpperArm.R": "r_upper_arm", "Forearm.R": "r_forearm", "Hand.R": "r_hand",
        "Thigh.L": "l_thigh", "Calf.L": "l_calf", "Foot.L": "l_foot",
        "Thigh.R": "r_thigh", "Calf.R": "r_calf", "Foot.R": "r_foot",
        "Weapon_Root": "weapon_root", "Weapon_Bolt": "weapon_bolt", "Weapon_Mag": "weapon_mag"
    }

    armature_obj.animation_data_create()
    armature_obj.animation_data.action = action

    scene = bpy.context.scene
    orig_frame = scene.frame_current

    # Sample keyframes for each pose bone
    for pbone in armature_obj.pose.bones:
        eng_bone_name = blender_to_engine_bone.get(pbone.name, normalize_bone_name(pbone.name))
        frames_list = []

        for f in range(frame_start, frame_end + 1):
            scene.frame_set(f)
            t = (f - frame_start) / fps

            loc_b = pbone.location
            pos_eng = blender_to_engine_pos((loc_b.x, loc_b.y, loc_b.z))

            rot_mode = pbone.rotation_mode
            if rot_mode == 'QUATERNION':
                rot_b = pbone.rotation_quaternion
            else:
                rot_b = pbone.rotation_euler.to_quaternion()

            rot_eng = blender_to_engine_quat((rot_b.w, rot_b.x, rot_b.y, rot_b.z))

            frames_list.append({
                "time": round(t, 4),
                "pos": [round(p, 6) for p in pos_eng],
                "rot": [round(r, 6) for r in rot_eng]
            })

        # Keep track if there is any animated motion or non-identity transform
        tracks_data.append({
            "bone": eng_bone_name,
            "frames": frames_list
        })

    scene.frame_set(orig_frame)

    output = {
        "format_version": 1,
        "id": anim_id,
        "name": action.name,
        "duration_sec": round(duration_sec, 4),
        "loop": True,
        "frame_rate": round(fps, 1),
        "tracks": tracks_data
    }

    os.makedirs(os.path.dirname(os.path.abspath(filepath)), exist_ok=True)
    with open(filepath, 'w', encoding='utf-8') as f:
        json.dump(output, f, indent=2)

    return True


def import_anim_json_to_action(armature_obj: Any, filepath: str) -> Optional[Any]:
    """Import Voxel FPS Engine .anim.json into Blender Action on Armature."""
    if bpy is None:
        raise RuntimeError("Blender (bpy) is required for animation import.")

    if not armature_obj or armature_obj.type != 'ARMATURE':
        raise ValueError("Must provide an Armature object.")

    with open(filepath, 'r', encoding='utf-8') as f:
        data = json.load(f)

    action_name = data.get("name", os.path.basename(filepath))
    fps = data.get("frame_rate", 30.0)
    action = bpy.data.actions.new(name=action_name)

    # Engine to Blender bone mapping
    engine_to_blender = {
        "root": "Root", "pelvis": "Pelvis", "spine": "Spine", "chest": "Chest",
        "neck": "Neck", "head": "Head",
        "l_shoulder": "Shoulder.L", "l_upper_arm": "UpperArm.L", "l_forearm": "Forearm.L", "l_hand": "Hand.L",
        "r_shoulder": "Shoulder.R", "r_upper_arm": "UpperArm.R", "r_forearm": "Forearm.R", "r_hand": "Hand.R",
        "l_thigh": "Thigh.L", "l_calf": "Calf.L", "l_foot": "Foot.L",
        "r_thigh": "Thigh.R", "r_calf": "Calf.R", "r_foot": "Foot.R",
        "weapon_root": "Weapon_Root", "weapon_bolt": "Weapon_Bolt", "weapon_mag": "Weapon_Mag"
    }

    armature_obj.animation_data_create()
    armature_obj.animation_data.action = action

    for track in data.get("tracks", []):
        eng_bone = track.get("bone", "")
        blender_bone = engine_to_blender.get(eng_bone)
        if not blender_bone:
            # Fallback to normalized match
            for b in armature_obj.pose.bones:
                if normalize_bone_name(b.name) == eng_bone:
                    blender_bone = b.name
                    break

        if not blender_bone or blender_bone not in armature_obj.pose.bones:
            continue

        pbone = armature_obj.pose.bones[blender_bone]
        pbone.rotation_mode = 'QUATERNION'

        for frame in track.get("frames", []):
            t = frame.get("time", 0.0)
            f_num = t * fps

            pos_eng = frame.get("pos", [0.0, 0.0, 0.0])
            rot_eng = frame.get("rot", [0.0, 0.0, 0.0, 1.0])

            pos_b = engine_to_blender_pos((pos_eng[0], pos_eng[1], pos_eng[2]))
            rot_b = engine_to_blender_quat((rot_eng[0], rot_eng[1], rot_eng[2], rot_eng[3]))

            pbone.location = mathutils.Vector(pos_b)
            pbone.rotation_quaternion = mathutils.Quaternion(rot_b)

            pbone.keyframe_insert(data_path="location", frame=f_num)
            pbone.keyframe_insert(data_path="rotation_quaternion", frame=f_num)

    return action


def run_self_test() -> bool:
    """Run headless self-test verification inside Blender."""
    print("=== VOXEL FPS RIG & ANIMATION SELF-TEST ===")

    # 1. Coordinate Roundtrip Verification
    test_eng_pos = (1.5, 0.75, -2.25)
    b_pos = engine_to_blender_pos(test_eng_pos)
    roundtrip_eng_pos = blender_to_engine_pos(b_pos)
    assert max(abs(a - b) for a, b in zip(test_eng_pos, roundtrip_eng_pos)) < 1e-5, "Pos transform failed"

    test_eng_quat = (0.0, 0.70710678, 0.0, 0.70710678)
    b_quat = engine_to_blender_quat(test_eng_quat)
    roundtrip_eng_quat = blender_to_engine_quat(b_quat)
    assert max(abs(a - b) for a, b in zip(test_eng_quat, roundtrip_eng_quat)) < 1e-5, "Quat transform failed"
    print("Test 1: Coordinate transformation roundtrips OK.")

    # 2. Bone Normalizer Verification
    assert normalize_bone_name("UpperArm.L") == "l_upper_arm", "Normalize UpperArm.L failed"
    assert normalize_bone_name("DEF-forearm.R") == "r_forearm", "Normalize DEF-forearm.R failed"
    assert normalize_bone_name("mixamorig:LeftLeg") == "l_calf", "Normalize mixamorig:LeftLeg failed"
    assert normalize_bone_name("mixamorig:Hips") == "pelvis", "Normalize mixamorig:Hips failed"
    assert normalize_bone_name("Weapon_Root") == "weapon_root", "Normalize Weapon_Root failed"
    print("Test 2: Bone name normalizer & aliases OK.")

    # 3. Create Armature & Mesh Verification
    bpy.ops.wm.read_factory_settings(use_empty=True)
    arm_obj = create_armature(add_weapon_bones=True, setup_ik=True)
    assert arm_obj is not None, "Failed to create armature"
    assert len(arm_obj.data.bones) >= 23, f"Expected >= 23 bones, got {len(arm_obj.data.bones)}"
    print(f"Test 3: Armature created with {len(arm_obj.data.bones)} bones OK.")

    char_mesh = create_character_mesh(arm_obj)
    assert char_mesh is not None, "Failed to create character mesh"
    assert len(char_mesh.data.vertices) == 136, f"Expected 136 vertices, got {len(char_mesh.data.vertices)}"
    assert len(char_mesh.vertex_groups) == 19, f"Expected 19 vertex groups, got {len(char_mesh.vertex_groups)}"
    print(f"Test 4: Character mesh created with {len(char_mesh.data.vertices)} vertices & {len(char_mesh.vertex_groups)} vertex groups OK.")

    # 4. Action Export & Import Roundtrip
    action = bpy.data.actions.new("TestWalk")
    arm_obj.animation_data_create()
    arm_obj.animation_data.action = action

    pbone = arm_obj.pose.bones["UpperArm.L"]
    pbone.rotation_mode = 'QUATERNION'
    pbone.rotation_quaternion = mathutils.Quaternion((0.92388, 0.38268, 0.0, 0.0))
    pbone.keyframe_insert("rotation_quaternion", frame=1)

    pbone.rotation_quaternion = mathutils.Quaternion((0.92388, -0.38268, 0.0, 0.0))
    pbone.keyframe_insert("rotation_quaternion", frame=15)

    import tempfile
    test_anim_path = os.path.join(tempfile.gettempdir(), "test_walk.anim.json")
    export_action_to_anim_json(arm_obj, action, test_anim_path, fps=30.0)
    assert os.path.exists(test_anim_path), "Failed to export .anim.json"

    with open(test_anim_path, 'r', encoding='utf-8') as f:
        loaded = json.load(f)
    assert loaded["format_version"] == 1, "Invalid format version"
    assert len(loaded["tracks"]) > 0, "No tracks exported"
    print(f"Test 5: Exported .anim.json with {len(loaded['tracks'])} tracks OK.")

    # Re-import into a new action
    imported_action = import_anim_json_to_action(arm_obj, test_anim_path)
    assert imported_action is not None, "Failed to import .anim.json"
    print("Test 6: Imported .anim.json back into Blender Action OK.")

    print("ALL BLENDER RIGGING & ANIMATION TESTS PASSED (BLENDER_TEST_OK)")
    return True


# -----------------------------------------------------------------------------
# Blender Addon UI / Operators
# -----------------------------------------------------------------------------
if bpy is not None:
    class VOXEL_OT_CreateArmature(bpy.types.Operator):
        """Create canonical 20-bone humanoid rig + 3 weapon bones"""
        bl_idname = "voxel.create_armature"
        bl_label = "Create Humanoid Rig"
        bl_options = {'REGISTER', 'UNDO'}

        add_weapon_bones: bpy.props.BoolProperty(name="Add Weapon Bones", default=True) # type: ignore
        setup_ik: bpy.props.BoolProperty(name="Setup IK Constraints", default=True) # type: ignore

        def execute(self, context):
            create_armature(add_weapon_bones=self.add_weapon_bones, setup_ik=self.setup_ik)
            self.report({'INFO'}, "Created Voxel Humanoid Armature")
            return {'FINISHED'}

    class VOXEL_OT_CreateCharacterMesh(bpy.types.Operator):
        """Generate 16mm sub-lattice skinned character mesh"""
        bl_idname = "voxel.create_character_mesh"
        bl_label = "Generate Skinned Mesh"
        bl_options = {'REGISTER', 'UNDO'}

        def execute(self, context):
            arm_obj = context.active_object if context.active_object and context.active_object.type == 'ARMATURE' else None
            create_character_mesh(arm_obj)
            self.report({'INFO'}, "Generated Voxel Character Mesh")
            return {'FINISHED'}

    class VOXEL_PT_SidebarPanel(bpy.types.Panel):
        """Voxel FPS Engine Rigging & Animation Panel"""
        bl_label = "Voxel Rig & Animation"
        bl_idname = "VOXEL_PT_rig_anim"
        bl_space_type = 'VIEW_3D'
        bl_region_type = 'UI'
        bl_category = 'Voxel Engine'

        def draw(self, context):
            layout = self.layout
            col = layout.column(align=True)
            col.label(text="Rig & Modeling:", icon='ARMATURE_DATA')
            col.operator("voxel.create_armature", text="Create Humanoid Rig", icon='BONE_DATA')
            col.operator("voxel.create_character_mesh", text="Generate Skinned Mesh", icon='MESH_CUBE')

    classes = (
        VOXEL_OT_CreateArmature,
        VOXEL_OT_CreateCharacterMesh,
        VOXEL_PT_SidebarPanel,
    )

    def register():
        for cls in classes:
            bpy.utils.register_class(cls)

    def unregister():
        for cls in reversed(classes):
            bpy.utils.unregister_class(cls)


# -----------------------------------------------------------------------------
# CLI Entry Point
# -----------------------------------------------------------------------------
if __name__ == "__main__":
    if "--test" in sys.argv:
        success = run_self_test()
        sys.exit(0 if success else 1)
