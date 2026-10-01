# vox.json schema

Cubic unit occupancy only. `unit` is always 1; `voxel_size` matches engine `0.001`.
Cell extent on Z always equals X and Y (no non-cubic voxels).

## Format v2 (current)

Every kind of asset (model/prop, character, weapon, item, sky, map) uses the same layers. The
painter writes v2 and reads v1 and v2; the engine reads both and refuses anything newer.
`scripts/migrate_voxfmt_v2.py` converts v1 files cell for cell.

```json
{
  "format_version": 2, "unit": 1, "voxel_size": 0.001, "mode": "model", "dims": [sx, sy, sz],
  "anchors": {"pivot": [x, y, z], "feet": [x, y, z]},
  "appearance": {"palette": [[r, g, b], ...], "runs": [y, z, x0, length, paletteIndex, ...]},
  "parts_rle": {"palette": ["barrel", ...], "runs": [y, z, x0, length, index, ...]},
  "cells_rle": {"palette": ["concrete", "wood", ...], "runs": [y, z, x0, length, index, ...]}
}
```

- All three layers are runs along +X within a (y, z) row; cells no run covers are Air / unpainted
  / no part.
- `cells_rle` names are painter material names. The engine maps them to blocks; a painter-only
  material (`bush_leaves`, `plexiglass`, `carbon_fiber`, `treated_wood`, `custom`) is dropped and
  counted, never approximated.
- `appearance` is paint that differs from the material's own colour; a cell in its material's own
  colour is not painted. Palette indices start at 1 (0 means "material colour"); at most 255.
- `anchors` are named cells: `pivot` (bottom centre) on everything, `feet` on characters.
- The v1 voxel list below is still read, so older files keep working.

### Prefabs

A map may list prefab instances, stamped at load from `data/prefabs/<id>.vox.json` (any v2
asset) with the asset's local origin at the given cell and `rot` quarter turns about +Y within
its own footprint. A quarter turn permutes cells and their paint; it never resamples them.
Prefabs are stamped in list order after the map's own cells, so a later one overwrites an earlier
one. A missing or invalid prefab refuses the map.

```json
"prefabs": [{"id": "crate_8", "x": 30, "y": 3, "z": 34, "rot": 1}]
```

## Format v1 (still read)

```json
{
  "format_version": 1,
  "unit": 1,
  "voxel_size": 0.001,
  "mode": "model|sky|character|weapon|item|map",
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

## Map mode (`"mode": "map"`)

Map mode is a `vox.json` carrying a cubic unit voxel grid **plus** authored entities. Entity
coordinates are integer cell coordinates on that same grid, so a map rescales with
`VOXEL_SIZE` alone — no separate map scale exists. All three sections are optional and are
omitted entirely when empty, so "no section" means "none authored", never "empty list".

```json
{
  "mode": "map",
  "id_counters": {"evt": 4, "npc": 3, "route": 2},
  "scripted_events": [
    {"x":10,"y":1,"z":10,"id":"evt_1","name":"Test \"Event\"","script":"open_door.ps1",
     "trigger":"on_signal","radius":2.000,"cooldown":20,"required_signal":"key_found",
     "emit_signal":"door_open","condition":"count(\"kills\") > 0","repeat":3,"enabled":false}
  ],
  "npcs": [
    {"x":15,"y":1,"z":15,"id":"npc_1","name":"Guard","type":"guard","ai_profile":"patrol",
     "patrol_route":"route_1","health":120,"max_health":150,"speed":0.075,"view_dist":24.500,
     "view_angle":110.0,"faction":"hostile","dialogue":"guard_taunt","inventory":"",
     "static":true,"spawn_tick":120,"spawn_condition":"wave_2","enabled":true}
  ],
  "patrol_routes": [
    {"id":"route_1","name":"Guard Patrol","loop":false,
     "nodes":[{"x":15,"y":1,"z":15,"wait":2.00,"action":"idle"}]}
  ]
}
```

| Field | Notes |
|-------|-------|
| `trigger` | `on_enter`, `on_interact`, `on_timer`, `on_signal` |
| `cooldown` | ticks, since `sim_hz` is fixed at 120 |
| `repeat` | fire limit; absent means unlimited |
| `ai_profile` | `static` or `patrol` |
| `static` | NPC never moves regardless of `patrol_route` |
| `speed`/`view_dist`/`view_angle` | authoring hints in cells and degrees |
| `loop` | whether a patrol route wraps back to its first node |

Entity `id`s are assigned per document in authoring order (`evt_1`, `npc_1`, `route_1`, …) so
the same authoring order always produces the same ids. The writer escapes `"` and `\` in every
string, and the reader unescapes them, so a quote in an event name survives a round trip.

`id_counters` is the next id to issue for each family. It round-trips so reloading a map and
placing one more entity never re-issues an id that already exists. On load the reader takes the
maximum of the persisted counter and `max(numeric suffix of existing ids) + 1`, so a document
written before counters existed, or one hand-edited to add a high-numbered entity, still cannot
collide with an existing id.

`format_version` is the document layout version. Omitted files default to `1`. Writers always
emit the current version; readers refuse a `format_version` **greater** than the version they
understand.

The engine reads map mode through `src/map_vox.hpp`, which applies the same refusals (version,
`unit`, `voxel_size`, mode) and stamps the voxel grid into authoritative occupancy.

### Whole-map sections (engine maps, e.g. `data/maps/warehouse_v1.map.vox.json`)

```json
{
  "player_spawn": {"x": 96, "y": 3, "z": 132, "yaw": 0.0, "pitch": -0.08},
  "pickups": [{"item": "supply_grenade", "x": 93, "y": 3, "z": 126, "rot": 0}],
  "cells_rle": {
    "palette": ["air", "dirt", "concrete", "..."],
    "runs": [0, 0, 0, 192, 1, "..."]
  }
}
```

- `player_spawn` is the cell the player's feet occupy, plus facing. It is used for the first
  spawn and every respawn. An engine map without one is refused.
- `pickups` are world items resting on exact cells; `item` is an id from `data/items`. An
  unknown id is skipped, not faked. `rot` is quarter turns about +Y.
- `cells_rle` is run-length occupancy for maps too large to list voxel by voxel. `runs` is a
  flat list of `[y, z, x0, length, paletteIndex]` quintuples along +X; cells no run covers are
  Air. Palette names are the painter material names that map to engine blocks, plus three
  engine-only blocks the painter cannot author: `water_current`, `light_bulb` and `moon`.
  An unknown palette name, a non-positive length, or an out-of-range index refuses the map.
- `lights` are light sources (environment layer), never occupancy: `{"kind": "bulb", "x", "y",
  "z" (cell units, floats), "color": [r, g, b], "intensity", "radius" (world units)}`. The
  fixture model drawn for each light comes from `kind`.
- `environment` holds map-wide sky data: `{"moon_dir": [x, y, z]}`.
- `light_bulb` and `moon` are refused in `cells_rle` and `voxels`: lights are not occupancy.
- `appearance` is the appearance layer: `{"palette": [[r, g, b], ...], "runs": [y, z, x0,
  length, paletteIndex, ...]}` with palette indices starting at 1 (0 means "the material's own
  colour" and is never written). At most 255 colours. A `voxels` entry's `rgb` also paints its
  cell. Appearance is display data: it is sent to the view and never read by the simulation, and
  breaking a cell clears its paint.
- The painter does not read or write these sections yet.
- `dims` must equal the engine world size (currently 192×64×160) for an engine map.

### `"terrain"`: generated ground (optional)

A map with no `terrain` section is loaded exactly as authored — no cells are generated, and the
section is absent from `--export-map` output. A map *with* one has its ground generated at load time
from that section, after the map's own cells and prefabs are stamped and before the player spawns.

```json
{
  "terrain": {
    "class": "valley",
    "seed": 20260930,
    "base": 0,
    "props": true,
    "roads": true,
    "road_x": 0
  }
}
```

| key | required | default | meaning |
|-----|----------|---------|---------|
| `class` | no | `"valley"` | chunk class: `valley`, `dunes`, `tundra` or `parkland` |
| `seed` | **yes** | — | integer seed. A missing seed is a refusal |
| `base` | no | the class's own base cell | ground base height override |
| `props` | no | `true` | place decoration prefabs on the surface |
| `roads` | no | `true` | plan and stamp the road network |
| `road_x` | no | derived from `seed` | which region column carries the road |

- The seed must be in the file. The generator substitutes no clock and no random source, so the same
  map and seed produce the same world on every machine.
- An unknown `class` refuses the map, as does a `base` that leaves under 8 cells of headroom.
- `base` and `road_x` distinguish *absent* from *zero*. Omitting them is not the same as setting
  them to 0: absent means "use the class default" and "derive from the seed", and `--export-map`
  preserves the distinction, so a map round-trips to the same world.
- **The generator only fills Air.** It never clears a cell the map already filled, so a terrain
  section can be added to a hand-authored map without demolishing it. A road meeting high ground is
  raised onto an embankment rather than cut through it. This covers the road and decoration prefabs
  too, not just the ground: a generated prefab that lands on an authored cell declines it.
- Terrain is generated in 2×2-chunk regions (64 cells across). The road base unit is one region, and
  roads are stamped as prefabs from `data/prefabs/road_*.vox.json` (generated by
  `scripts/build_terrain_prefabs.py`). A missing road prefab is a refusal, not a silent omission.
- A road tile is **two cells tall**: a `dirt` base course and the drivable `asphalt` surface, with
  `concrete` kerbs along the road's edges. It is stamped at the network's level minus one, so its
  surface lands exactly on that level and the graded embankment underneath it has no gap. The load
  path measures each road prefab and **refuses** one that is not exactly two cells tall, so a tile
  authored at the wrong height is rejected rather than producing a road floating above a hole. The
  terrain gate checks the same contract over the generated network, but the refusal is at load time:
  a gate only runs when someone runs it, a stale or hand-edited prefab loads every time.
- Everything the generator writes is ordinary occupancy on the authoritative grid, so the mesher,
  physics and the sim fingerprint see nothing special. It stays unit-cubic; a terrain cell is no
  exception to the 1 mm grid rule.
- A map with a terrain section and no `player_spawn` spawns the player on the generated ground.
  Without a terrain section, a map with no spawn is still refused.
- Load one with `voxel_engine.exe --map data/maps/valley_demo.map.vox.json`, or generate a variant
  with `python scripts/build_terrain_map.py --class dunes --seed 7`.

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
