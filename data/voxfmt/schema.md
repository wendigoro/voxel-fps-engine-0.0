# vox.json schema

Cubic unit occupancy only. `unit` is always 1; `voxel_size` matches engine `0.001`.
Cell extent on Z always equals X and Y (no non-cubic voxels).

```json
{
  "unit": 1,
  "voxel_size": 0.001,
  "mode": "model|sky|character|weapon",
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
  "voxels": [
    {"x": 0, "y": 0, "z": 0, "mat": "concrete", "rgb": 6710886, "part": "barrel"}
  ]
}
```

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
