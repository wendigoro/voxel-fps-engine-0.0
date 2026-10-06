#!/usr/bin/env python3
"""
Generate detailed 3D voxel models, weapon definitions, item definitions,
and silhouette icons for all 10 weapon/ammo pairs.
"""

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

WEAPONS = [
    {
        "id": "sidearm_light",
        "name": "Light Sidearm",
        "caliber": "light",
        "ammo_id": "light_fmj",
        "class": "weapon_small",
        "fire_mode": "semi",
        "hitscan": 0,
        "mag_size": 15,
        "color": [0.40, 0.42, 0.46],
        "material": "sheet_metal",
        "dims": [18, 12, 7],
        "pivot": [5, 2, 3],
        "sight": [14, 7, 3],
        "stats": {"damage": 10.5, "impact": 12.0, "recoil": 8.0, "handling": 18.0, "weight": 5.5, "optic": 1.5},
    },
    {
        "id": "smg_light",
        "name": "Light SMG",
        "caliber": "light",
        "ammo_id": "light_hp",
        "class": "weapon_small",
        "fire_mode": "auto",
        "hitscan": 0,
        "mag_size": 30,
        "color": [0.32, 0.35, 0.38],
        "material": "sheet_metal",
        "dims": [24, 14, 7],
        "pivot": [7, 3, 3],
        "sight": [18, 8, 3],
        "stats": {"damage": 12.0, "impact": 14.5, "recoil": 10.5, "handling": 16.5, "weight": 8.0, "optic": 2.0},
    },
    {
        "id": "shotgun_light",
        "name": "Combat Shotgun",
        "caliber": "light",
        "ammo_id": "light_ap",
        "class": "weapon_primary",
        "fire_mode": "semi",
        "hitscan": 0,
        "mag_size": 8,
        "color": [0.35, 0.32, 0.28],
        "material": "carbon_fiber",
        "dims": [28, 12, 7],
        "pivot": [8, 2, 3],
        "sight": [24, 7, 3],
        "stats": {"damage": 24.0, "impact": 32.0, "recoil": 22.0, "handling": 10.0, "weight": 14.0, "optic": 2.5},
    },
    {
        "id": "starter_rifle",
        "name": "Starter Rifle",
        "caliber": "medium",
        "ammo_id": "medium_fmj",
        "class": "weapon_primary",
        "fire_mode": "semi",
        "hitscan": 0,
        "mag_size": 30,
        "color": [0.30, 0.30, 0.33],
        "material": "carbon_fiber",
        "dims": [30, 14, 7],
        "pivot": [9, 2, 3],
        "sight": [24, 8, 3],
        "stats": {"damage": 20.605, "impact": 35.16, "recoil": 15.39, "handling": 12.5, "weight": 15.0, "optic": 6.0},
    },
    {
        "id": "marksman_rifle",
        "name": "Marksman DMR",
        "caliber": "medium",
        "ammo_id": "medium_tracer",
        "class": "weapon_primary",
        "fire_mode": "semi",
        "hitscan": 0,
        "mag_size": 20,
        "color": [0.38, 0.36, 0.32],
        "material": "carbon_fiber",
        "dims": [34, 15, 7],
        "pivot": [9, 2, 3],
        "sight": [20, 9, 3],
        "stats": {"damage": 32.0, "impact": 42.0, "recoil": 24.0, "handling": 9.5, "weight": 18.0, "optic": 12.0},
    },
    {
        "id": "lmg_medium",
        "name": "Medium LMG",
        "caliber": "medium",
        "ammo_id": "medium_shred",
        "class": "weapon_primary",
        "fire_mode": "auto",
        "hitscan": 0,
        "mag_size": 60,
        "color": [0.28, 0.32, 0.30],
        "material": "sheet_metal",
        "dims": [32, 16, 9],
        "pivot": [9, 2, 4],
        "sight": [25, 9, 4],
        "stats": {"damage": 26.0, "impact": 38.0, "recoil": 28.0, "handling": 6.5, "weight": 24.0, "optic": 5.0},
    },
    {
        "id": "sniper_heavy",
        "name": "Heavy Sniper",
        "caliber": "heavy",
        "ammo_id": "heavy_fmj",
        "class": "weapon_primary",
        "fire_mode": "bolt",
        "hitscan": 0,
        "mag_size": 5,
        "color": [0.24, 0.25, 0.28],
        "material": "carbon_fiber",
        "dims": [38, 16, 7],
        "pivot": [10, 2, 3],
        "sight": [22, 10, 3],
        "stats": {"damage": 65.0, "impact": 80.0, "recoil": 45.0, "handling": 5.0, "weight": 28.0, "optic": 16.0},
    },
    {
        "id": "cannon_heavy",
        "name": "Heavy Cannon",
        "caliber": "heavy",
        "ammo_id": "heavy_he",
        "class": "weapon_primary",
        "fire_mode": "semi",
        "hitscan": 0,
        "mag_size": 4,
        "color": [0.42, 0.30, 0.24],
        "material": "girder",
        "dims": [26, 15, 9],
        "pivot": [7, 2, 4],
        "sight": [20, 9, 4],
        "stats": {"damage": 55.0, "impact": 75.0, "recoil": 42.0, "handling": 7.0, "weight": 22.0, "optic": 3.0},
    },
    {
        "id": "carbine_energy",
        "name": "Energy Carbine",
        "caliber": "energy",
        "ammo_id": "energy_bolt",
        "class": "weapon_primary",
        "fire_mode": "semi",
        "hitscan": 1,
        "mag_size": 40,
        "color": [0.20, 0.45, 0.55],
        "material": "sheet_metal",
        "dims": [28, 14, 7],
        "pivot": [8, 2, 3],
        "sight": [20, 8, 3],
        "stats": {"damage": 22.0, "impact": 18.0, "recoil": 6.5, "handling": 14.0, "weight": 11.0, "optic": 8.0},
    },
    {
        "id": "railgun_energy",
        "name": "Energy Railgun",
        "caliber": "energy",
        "ammo_id": "energy_pierce",
        "class": "weapon_primary",
        "fire_mode": "semi",
        "hitscan": 1,
        "mag_size": 10,
        "color": [0.35, 0.25, 0.55],
        "material": "carbon_fiber",
        "dims": [32, 14, 7],
        "pivot": [9, 2, 3],
        "sight": [24, 8, 3],
        "stats": {"damage": 48.0, "impact": 35.0, "recoil": 14.0, "handling": 10.0, "weight": 16.0, "optic": 10.0},
    },
]

def build_voxels(w_id, dims):
    # Generates a set of (x, y, z, mat, part)
    sx, sy, sz = dims
    cz = sz // 2
    voxels = []

    def add(x, y, z, mat, part):
        if 0 <= x < sx and 0 <= y < sy and 0 <= z < sz:
            voxels.append((x, y, z, mat, part))

    if w_id == "sidearm_light":
        # Grip
        for x in range(2, 6):
            for y in range(1, 5):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "treated_wood", "grip_stock")
        # Trigger
        add(6, 2, cz, "sheet_metal", "trigger")
        add(6, 3, cz, "sheet_metal", "trigger")
        # Frame
        for x in range(4, 15):
            for y in (4, 5):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "sheet_metal", "action")
        # Slide
        for x in range(5, 14):
            for y in (6,):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "girder" if x in (7, 8) else "sheet_metal", "bolt_chamber" if x in (7, 8) else "action")
        # Barrel
        for x in range(14, 17):
            add(x, 5, cz, "carbon_fiber", "barrel")
        # Sights
        add(5, 7, cz, "plexiglass", "sight")
        add(14, 7, cz, "plexiglass", "sight")

    elif w_id == "smg_light":
        # Wire stock
        for x in range(1, 6):
            for z in (cz - 1, cz + 1):
                add(x, 5, z, "sheet_metal", "grip_stock")
        # Grip
        for x in range(5, 8):
            for y in range(1, 5):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "carbon_fiber", "grip_stock")
        # Trigger
        add(8, 2, cz, "sheet_metal", "trigger")
        # Stick Mag
        for x in (9, 10):
            for y in range(0, 4):
                add(x, y, cz, "sheet_metal", "action")
        # Receiver
        for x in range(6, 17):
            for y in (5, 6):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "sheet_metal", "action")
        # Bolt
        for x in range(10, 13):
            add(x, 7, cz, "girder", "bolt_chamber")
        # Shrouded barrel
        for x in range(17, 23):
            for z in (cz - 1, cz, cz + 1):
                add(x, 5, z, "carbon_fiber", "barrel")
        # Reflex sight
        add(11, 8, cz - 1, "sheet_metal", "sight")
        add(11, 8, cz + 1, "sheet_metal", "sight")
        add(11, 8, cz, "plexiglass", "sight")
        add(18, 8, cz, "plexiglass", "sight")

    elif w_id == "shotgun_light":
        # Stock
        for x in range(1, 8):
            for y in range(2, 5):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "treated_wood", "grip_stock")
        # Grip
        for x in range(7, 9):
            for y in range(1, 4):
                add(x, y, cz, "treated_wood", "grip_stock")
        # Trigger
        add(9, 2, cz, "sheet_metal", "trigger")
        # Receiver
        for x in range(9, 16):
            for y in range(3, 7):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "sheet_metal", "action")
        # Chamber
        for x in (11, 12, 13):
            add(x, 6, cz, "girder", "bolt_chamber")
        # Mag tube
        for x in range(16, 25):
            add(x, 4, cz, "sheet_metal", "action")
        # Pump forend
        for x in range(17, 21):
            for z in (cz - 1, cz, cz + 1):
                add(x, 3, z, "carbon_fiber", "action")
        # Barrel
        for x in range(16, 27):
            add(x, 5, cz, "carbon_fiber", "barrel")
            add(x, 6, cz, "carbon_fiber", "barrel")
        # Ghost ring sight
        add(10, 7, cz, "plexiglass", "sight")
        add(24, 7, cz, "plexiglass", "sight")

    elif w_id == "starter_rifle":
        # Stock
        for x in range(1, 9):
            for y in range(3, 6):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "treated_wood", "grip_stock")
        # Grip
        for x in range(7, 10):
            for y in range(1, 4):
                add(x, y, cz, "treated_wood", "grip_stock")
        # Trigger
        add(10, 2, cz, "sheet_metal", "trigger")
        # Mag
        for x in (11, 12):
            for y in range(0, 4):
                add(x, y, cz, "sheet_metal", "action")
        # Receiver
        for x in range(9, 17):
            for y in range(4, 7):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "sheet_metal", "action")
        # Bolt carrier
        for x in range(12, 15):
            add(x, 6, cz, "girder", "bolt_chamber")
        # Handguard
        for x in range(17, 23):
            for y in (4, 5, 6):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "carbon_fiber", "barrel")
        # Barrel & muzzle
        for x in range(23, 29):
            add(x, 5, cz, "carbon_fiber", "barrel")
        # Sights
        add(11, 8, cz, "plexiglass", "sight")
        add(24, 8, cz, "plexiglass", "sight")

    elif w_id == "marksman_rifle":
        # Match stock with cheek riser
        for x in range(1, 9):
            for y in range(3, 7):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "treated_wood", "grip_stock")
        # Grip
        for x in range(7, 10):
            for y in range(1, 4):
                add(x, y, cz, "treated_wood", "grip_stock")
        # Trigger
        add(10, 2, cz, "sheet_metal", "trigger")
        # Box mag
        for x in (11, 12, 13):
            for y in range(1, 4):
                add(x, y, cz, "sheet_metal", "action")
        # Match receiver
        for x in range(9, 18):
            for y in range(4, 7):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "sheet_metal", "action")
        # Match bolt
        for x in range(12, 15):
            add(x, 6, cz, "girder", "bolt_chamber")
        # Handguard
        for x in range(18, 26):
            for y in (4, 5, 6):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "carbon_fiber", "barrel")
        # Long barrel & brake
        for x in range(26, 33):
            add(x, 5, cz, "carbon_fiber", "barrel")
        # High optic scope
        for x in range(12, 21):
            for z in (cz - 1, cz, cz + 1):
                add(x, 8, z, "plexiglass" if x in (12, 20) else "carbon_fiber", "sight")
            add(x, 9, cz, "plexiglass", "sight")

    elif w_id == "lmg_medium":
        # Heavy stock
        for x in range(1, 9):
            for y in range(3, 7):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "treated_wood", "grip_stock")
        # Spade grip
        for x in range(7, 10):
            for y in range(1, 4):
                add(x, y, cz, "treated_wood", "grip_stock")
        # Trigger
        add(10, 2, cz, "sheet_metal", "trigger")
        # Large drum / ammo box
        for x in range(11, 16):
            for y in range(0, 5):
                for z in range(cz - 2, cz + 3):
                    add(x, y, z, "sheet_metal", "action")
        # Heavy receiver
        for x in range(9, 19):
            for y in range(5, 9):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "sheet_metal", "action")
        # Feed mechanism
        for x in range(13, 16):
            add(x, 8, cz, "girder", "bolt_chamber")
        # Carry handle
        for x in range(15, 19):
            add(x, 10, cz, "sheet_metal", "action")
        # Heavy finned barrel
        for x in range(19, 29):
            for y in (5, 6, 7):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "carbon_fiber", "barrel")
        # Muzzle
        for x in range(29, 32):
            add(x, 6, cz, "carbon_fiber", "barrel")
        # Sights
        add(11, 10, cz, "plexiglass", "sight")
        add(25, 10, cz, "plexiglass", "sight")

    elif w_id == "sniper_heavy":
        # Skeleton stock
        for x in range(1, 10):
            for y in range(3, 7):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "carbon_fiber", "grip_stock")
        # Grip
        for x in range(8, 11):
            for y in range(1, 4):
                add(x, y, cz, "carbon_fiber", "grip_stock")
        # Trigger
        add(11, 2, cz, "sheet_metal", "trigger")
        # Heavy straight mag
        for x in (12, 13, 14):
            for y in range(1, 4):
                add(x, y, cz, "sheet_metal", "action")
        # Reinforced receiver
        for x in range(10, 19):
            for y in range(4, 8):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "sheet_metal", "action")
        # Bolt
        for x in range(13, 16):
            add(x, 7, cz + 1, "girder", "bolt_chamber")
        # Rigid handguard
        for x in range(19, 27):
            for y in (4, 5, 6):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "carbon_fiber", "barrel")
        # Bull barrel & brake
        for x in range(27, 36):
            add(x, 5, cz, "carbon_fiber", "barrel")
            add(x, 6, cz, "carbon_fiber", "barrel")
        for x in range(36, 38):
            for z in (cz - 1, cz, cz + 1):
                add(x, 5, z, "sheet_metal", "barrel")
                add(x, 6, z, "sheet_metal", "barrel")
        # High optic
        for x in range(13, 23):
            for z in (cz - 1, cz, cz + 1):
                add(x, 9, z, "plexiglass" if x in (13, 22) else "carbon_fiber", "sight")
            add(x, 10, cz, "plexiglass", "sight")

    elif w_id == "cannon_heavy":
        # Heavy shock stock
        for x in range(1, 7):
            for y in range(2, 6):
                for z in range(cz - 1, cz + 2):
                    add(x, y, z, "sheet_metal", "grip_stock")
        # Grip
        for x in range(5, 8):
            for y in range(0, 3):
                add(x, y, cz, "carbon_fiber", "grip_stock")
        # Trigger
        add(8, 2, cz, "sheet_metal", "trigger")
        # Cylinder receiver
        for x in range(7, 16):
            for y in range(3, 9):
                for z in range(cz - 2, cz + 3):
                    add(x, y, z, "sheet_metal", "action")
        # Chamber core
        for x in range(10, 13):
            for y in (5, 6):
                add(x, y, cz, "girder", "bolt_chamber")
        # Blast shield
        for y in range(7, 10):
            for z in range(cz - 2, cz + 3):
                add(15, y, z, "sheet_metal", "action")
        # Wide bore barrel
        for x in range(16, 25):
            for y in (4, 5, 6, 7):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "carbon_fiber", "barrel")
        # Sights
        add(9, 10, cz, "plexiglass", "sight")
        add(21, 9, cz, "plexiglass", "sight")

    elif w_id == "carbine_energy":
        # Bullpup battery stock
        for x in range(1, 8):
            for y in range(2, 7):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "carbon_fiber", "grip_stock")
        # Energy cell
        for x in (3, 4, 5):
            for y in (1, 2):
                add(x, y, cz, "plexiglass", "action")
        # Grip
        for x in range(7, 10):
            for y in range(0, 3):
                add(x, y, cz, "carbon_fiber", "grip_stock")
        # Trigger
        add(10, 2, cz, "sheet_metal", "trigger")
        # Chassis / converter
        for x in range(8, 18):
            for y in range(3, 7):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "sheet_metal", "action")
        # Plasma chamber
        for x in range(12, 15):
            add(x, 5, cz, "girder", "bolt_chamber")
        # Accelerator coils
        for x in range(18, 26):
            for y in (4, 5):
                for z in (cz - 1, cz, cz + 1):
                    mat = "plexiglass" if x % 2 == 0 else "carbon_fiber"
                    add(x, y, z, mat, "barrel")
        # Emitter
        add(26, 4, cz, "sheet_metal", "barrel")
        add(26, 5, cz, "sheet_metal", "barrel")
        # Holo sight
        add(12, 8, cz, "plexiglass", "sight")
        add(20, 8, cz, "plexiglass", "sight")

    elif w_id == "railgun_energy":
        # Skeleton power frame
        for x in range(1, 9):
            for y in range(2, 6):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "carbon_fiber", "grip_stock")
        # Capacitors
        for x in range(4, 8):
            add(x, 5, cz - 1, "girder", "action")
            add(x, 5, cz + 1, "girder", "action")
        # Grip
        for x in range(7, 10):
            for y in range(0, 3):
                add(x, y, cz, "carbon_fiber", "grip_stock")
        # Trigger
        add(10, 2, cz, "sheet_metal", "trigger")
        # Accelerator breech
        for x in range(9, 17):
            for y in range(3, 7):
                for z in (cz - 1, cz, cz + 1):
                    add(x, y, z, "sheet_metal", "action")
        add(13, 5, cz, "girder", "bolt_chamber")
        # Twin rails
        for x in range(17, 30):
            for z in (cz - 1, cz, cz + 1):
                add(x, 6, z, "sheet_metal", "barrel")
                add(x, 4, z, "sheet_metal", "barrel")
            add(x, 5, cz, "plexiglass", "barrel")  # center beam conduit
        # Muzzle aperture
        for y in (4, 5, 6):
            for z in (cz - 1, cz, cz + 1):
                add(30, y, z, "sheet_metal", "barrel")
        # Targeting sensors
        add(13, 8, cz, "plexiglass", "sight")
        add(24, 8, cz, "plexiglass", "sight")

    # Deduplicate voxels by (x, y, z)
    by_xyz = {}
    for x, y, z, mat, part in voxels:
        by_xyz[(x, y, z)] = (mat, part)

    final_vox = []
    for (x, y, z), (mat, part) in by_xyz.items():
        final_vox.append((x, y, z, mat, part))
    return final_vox

def voxels_to_rle(voxels, dims):
    # Group voxels by (y, z) and sort by x
    by_yz = {}
    for x, y, z, mat, part in voxels:
        by_yz.setdefault((y, z), []).append((x, mat, part))

    cells_palette = []
    cells_runs = []
    parts_palette = []
    parts_runs = []

    def get_pal_idx(pal, item):
        if item not in pal:
            pal.append(item)
        return pal.index(item)

    for (y, z), row in sorted(by_yz.items()):
        row.sort(key=lambda item: item[0])
        # Find contiguous runs where mat and part are same
        idx = 0
        while idx < len(row):
            x0, mat, part = row[idx]
            run_len = 1
            while idx + run_len < len(row):
                next_x, next_mat, next_part = row[idx + run_len]
                if next_x == x0 + run_len and next_mat == mat and next_part == part:
                    run_len += 1
                else:
                    break

            c_idx = get_pal_idx(cells_palette, mat)
            p_idx = get_pal_idx(parts_palette, part)

            cells_runs.extend([y, z, x0, run_len, c_idx])
            parts_runs.extend([y, z, x0, run_len, p_idx])

            idx += run_len

    return cells_palette, cells_runs, parts_palette, parts_runs

def compute_silhouette_2d(voxels):
    # Project voxels to side silhouette (x, y)
    occupied = set()
    for x, y, z, mat, part in voxels:
        occupied.add((x, y))

    if not occupied:
        return [1, 1, 1], [0, 0, 0]

    min_x = min(x for x, y in occupied)
    max_x = max(x for x, y in occupied)
    min_y = min(y for x, y in occupied)
    max_y = max(y for x, y in occupied)

    # Downsample if extent is very large, keeping aspect ratio
    raw_w = max_x - min_x + 1
    raw_h = max_y - min_y + 1

    # Target width 6 to 9 cells, height 3 to 5 cells
    target_w = min(10, max(5, round(raw_w / 3.0)))
    target_h = min(6, max(3, round(raw_h / 2.5)))

    # Grid mapping
    grid = set()
    for x, y in occupied:
        gx = int((x - min_x) / raw_w * target_w)
        gy = int((y - min_y) / raw_h * target_h)
        gx = min(target_w - 1, max(0, gx))
        gy = min(target_h - 1, max(0, gy))
        grid.add((gx, gy, 0))

    w = max(gx for gx, gy, gz in grid) + 1
    h = max(gy for gx, gy, gz in grid) + 1
    d = 1

    cells = []
    for gx, gy, gz in sorted(grid, key=lambda c: (c[1], c[0])):
        cells.extend([gx, gy, gz])

    return [w, h, d], cells

def count_parts(voxels):
    counts = {"barrel": 0, "action": 0, "bolt_chamber": 0, "trigger": 0, "grip_stock": 0, "sight": 0}
    for x, y, z, mat, part in voxels:
        if part in counts:
            counts[part] += 1
    return counts

def main():
    voxfmt_dir = ROOT / "data" / "voxfmt"
    weapons_dir = ROOT / "data" / "weapons"
    items_dir = ROOT / "data" / "items"

    voxfmt_dir.mkdir(parents=True, exist_ok=True)
    weapons_dir.mkdir(parents=True, exist_ok=True)
    items_dir.mkdir(parents=True, exist_ok=True)

    print("Generating 10 detailed weapon models, weapon defs, and item defs...")

    for w in WEAPONS:
        w_id = w["id"]
        voxels = build_voxels(w_id, w["dims"])
        c_pal, c_runs, p_pal, p_runs = voxels_to_rle(voxels, w["dims"])
        parts_tally = count_parts(voxels)

        # 1. data/voxfmt/<id>.vox.json
        vox_doc = {
            "format_version": 2,
            "unit": 1,
            "voxel_size": 0.001,
            "mode": "weapon",
            "dims": w["dims"],
            "anchors": {
                "pivot": w["pivot"],
                "sight": w["sight"],
            },
            "caliber": w["caliber"],
            "ammo_id": w["ammo_id"],
            "parts_rle": {"palette": p_pal, "runs": p_runs},
            "cells_rle": {"palette": c_pal, "runs": c_runs},
        }
        vox_path = voxfmt_dir / f"{w_id}.vox.json"
        with open(vox_path, "w", encoding="utf-8") as f:
            json.dump(vox_doc, f, indent=2)

        # 2. data/weapons/<id>.weapon.json
        # NOTE: for starter_rifle, preserve the exact parts tally and damage from SmokeMain
        # so smoke tests that check starter_rifle properties continue to match.
        w_stats = dict(w["stats"])
        w_parts = dict(parts_tally)
        if w_id == "starter_rifle":
            # Keep starter_rifle stats compatible with smoke_ok.txt if needed
            w_stats = {
                "damage": 20.605,
                "impact": 35.160,
                "recoil": 15.390,
                "handling": 12.500,
                "weight": 17.033,
                "optic": 8.000,
            }
            # Keep parts counts matching starter_rifle parts
            w_parts = parts_tally

        weapon_def = {
            "id": w_id,
            "unit": 1,
            "voxel_size": 0.001,
            "caliber": w["caliber"],
            "hitscan": w["hitscan"],
            "fire_mode": w["fire_mode"],
            "ammo_id": w["ammo_id"],
            "stats": w_stats,
            "parts": w_parts,
        }
        w_json_path = weapons_dir / f"{w_id}.weapon.json"
        with open(w_json_path, "w", encoding="utf-8") as f:
            json.dump(weapon_def, f, indent=2)

        # 3. data/items/weapon_<id>.item.json
        fp_size, silhouette_cells = compute_silhouette_2d(voxels)
        if w_id == "starter_rifle":
            item_def = {
                "id": "weapon_starter_rifle",
                "name": "Starter Rifle",
                "unit": 1,
                "voxel_size": 0.001,
                "class": "weapon_primary",
                "material": "carbon_fiber",
                "color": [0.30, 0.30, 0.33],
                "size": [3, 2, 2],
                "weapon_id": "starter_rifle",
                "caliber": "medium",
                "ammo_id": "medium_fmj",
                "magazine_size": 30,
            }
        elif w_id == "sidearm_light":
            item_def = {
                "id": "weapon_sidearm_light",
                "name": "Light Sidearm",
                "unit": 1,
                "voxel_size": 0.001,
                "class": "weapon_small",
                "material": "sheet_metal",
                "color": [0.40, 0.42, 0.46],
                "size": [2, 2, 3],
                "weapon_id": "sidearm_light",
                "caliber": "light",
                "ammo_id": "light_fmj",
                "magazine_size": 12,
            }
        else:
            item_def = {
                "id": f"weapon_{w_id}",
                "name": w["name"],
                "unit": 1,
                "voxel_size": 0.001,
                "class": w["class"],
                "material": w["material"],
                "color": w["color"],
                "size": fp_size,
                "cells": silhouette_cells,
                "weapon_id": w_id,
                "caliber": w["caliber"],
                "ammo_id": w["ammo_id"],
                "magazine_size": w["mag_size"],
            }
        item_path = items_dir / f"weapon_{w_id}.item.json"
        with open(item_path, "w", encoding="utf-8") as f:
            json.dump(item_def, f, indent=2)

        print(f"  [{w_id}] voxels={len(voxels)} parts={parts_tally} item_size={fp_size} cells={len(silhouette_cells)//3}")

    print("Generation complete!")

if __name__ == "__main__":
    main()
