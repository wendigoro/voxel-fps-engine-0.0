# vox.json schema

Cubic unit occupancy only. `unit` is always 1; `voxel_size` matches engine `0.001`.
Cell extent on Z always equals X and Y (no non-cubic voxels).

```json
{
  "unit": 1,
  "voxel_size": 0.001,
  "mode": "model|sky|character|weapon|item",
  "dims": [sx, sy, sz],
  "seg_u": 28,
  "seg_v": 14,
  "moon_dir": [x, y, z],
  "moon_intensity": 0.95,
  "feet": [x, y, z],
  "weapon_name": "starter_rifle",
  "caliber": "light|medium|heavy|energy",
  "fire_mode": "semi|auto|bolt",
  "ammo_id": "medium_fmj",
  "active_part": "barrel",
  "item_id": "ammo_pouch_medium",
  "item_name": "Medium Ammo Pouch",
  "item_class": "weapon_primary|weapon_small|ammo_pouch|armor|backpack|misc",
  "armor_zone": "head|chest|arms|legs",
  "pack_size": [sx, sy, sz],
  "voxels": [
    {"x": 0, "y": 0, "z": 0, "mat": "concrete", "rgb": 6710886, "part": "barrel"}
  ]
}
```

`item_*`/`armor_zone`/`pack_size` are item-mode only. `dims` is the painter work area; it is
**not** the item footprint (see the item export below).

## Weapon parts (per-voxel `part` field)

| id | name | primary stat |
|----|------|--------------|
| 0 | none | — |
| 1 | barrel | damage |
| 2 | action | impact |
| 3 | bolt_chamber | recoil |
| 4 | trigger | handling |
| 5 | grip_stock | weight |
| 6 | sight | optic |

Materials after water: `plexiglass` (9), `carbon_fiber` (10), `treated_wood` (11); painter `custom` is 12.

## Weapon export (`data/weapons/*.weapon.json`)

Composed from painted part volumes + materials via `WeaponParts.compose`. Fields include
`id`, `unit`, `voxel_size`, `caliber`, `hitscan` (0|1), `fire_mode` (semi|auto|bolt), `ammo_id`
(Python ids: `light_fmj`, `medium_fmj`, …), nested `stats` (damage/impact/recoil/handling/weight/optic),
`parts` tallies. Engine loads ammo scales from `data/projectiles.json` `ammo[]` and applies them on fire.

Bitcrush 32× is a display/export filter only — it must not rewrite occupancy.

## Item export (`data/items/*.item.json`)

Written by painter `Mode.ITEM` via `Items.exportItemJson`. The grid the artist paints is a work
area; the item's footprint is the **tight bounding box of the solid cells**:

```json
{
  "id": "ammo_pouch_medium",
  "name": "Medium Ammo Pouch",
  "unit": 1,
  "voxel_size": 0.001,
  "class": "ammo_pouch",
  "material": "sheet_metal",
  "color": [0.52, 0.47, 0.33],
  "size": [2, 2, 2],
  "cells": [0, 0, 0, 0, 0, 1, ...],
  "caliber": "medium",
  "ammo_id": "medium_fmj",
  "rounds": 60
}
```

- `size` is the tight box of `cells`, and `cells` is a flat list of `[x, y, z]` triples rebased
  to that origin — so empty canvas around the item is never charged as storage.
- `cells` is optional: without it the engine treats the item as a solid box of `size`, which is
  how the hand-authored base items are written.
- `armor_zone` is emitted for `armor`; `pack_size` for `backpack` (granted on equip, removed on
  unequip — never falling back to the 3×3×4 base pack). `weapon_id`/`caliber`/`ammo_id` come from
  the weapon classes, `caliber`/`ammo_id` from pouches.
- Magazine/round counts are metadata only until firing and reload consume them.
