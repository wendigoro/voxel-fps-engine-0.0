"""One-off migration (phase 3): move light sources out of map occupancy.

Before: data/maps/warehouse_v1.map.vox.json carried bulbs as `light_bulb`
cells and the moon as a `moon` cell, and the engine harvested lights by
scanning the grid. After: the map has a `lights` list and an `environment`
section, and neither block appears in `cells_rle`.

The lights are computed exactly as the deleted harvestBulbLights() did:
each vertical run of light_bulb cells is one light at the run's centre, with
colour (1.00, 0.75, 0.45), intensity min(1.8, 1.2 + 0.2 * cells) and radius
min(0.09, 0.06 + 0.01 * cells). Positions are written in cell units.

Kept for the record; running it again on a migrated map changes nothing.
"""

import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
MAP = ROOT / "data" / "maps" / "warehouse_v1.map.vox.json"
MOON_DIR = [0.32, 0.82, -0.48]  # the engine's fixed sky bearing (g_moonDirWorld)


def main() -> int:
    doc = json.loads(MAP.read_text(encoding="utf-8"))
    rle = doc["cells_rle"]
    pal = rle["palette"]
    bulb = pal.index("light_bulb")
    moon = pal.index("moon")
    runs = rle["runs"]
    quints = [runs[i:i + 5] for i in range(0, len(runs), 5)]

    bulb_cells = set()
    kept = []
    for y, z, x0, length, pi in quints:
        if pi == bulb:
            for x in range(x0, x0 + length):
                bulb_cells.add((x, y, z))
        elif pi != moon:
            kept.append([y, z, x0, length, pi])

    lights = []
    consumed = set()
    for x, y, z in sorted(bulb_cells, key=lambda c: (c[2], c[1], c[0])):
        if (x, y, z) in consumed:
            continue
        top = y
        cells = []
        while (x, top, z) in bulb_cells:
            consumed.add((x, top, z))
            cells.append(top)
            top += 1
        n = len(cells)
        lights.append({
            "kind": "bulb",
            "x": x + 0.5,
            "y": sum(c + 0.5 for c in cells) / n,
            "z": z + 0.5,
            "color": [1.00, 0.75, 0.45],
            "intensity": round(min(1.8, 1.2 + 0.2 * n), 4),
            "radius": round(min(0.09, 0.06 + 0.01 * n), 4),
        })

    rle["runs"] = [v for q in kept for v in q]
    if bulb_cells:  # an already-migrated map keeps the lights it has
        doc["lights"] = lights
    doc.setdefault("lights", [])
    doc.setdefault("environment", {"moon_dir": MOON_DIR})

    # Same layout the engine exporter writes: header, entities, then cells.
    order = ["format_version", "unit", "voxel_size", "mode", "dims", "player_spawn", "pickups",
             "lights", "environment", "cells_rle"]
    out = {k: doc[k] for k in order if k in doc}
    out.update({k: v for k, v in doc.items() if k not in out})
    lines = ["{"]
    keys = list(out)
    for i, k in enumerate(keys):
        sep = "," if i + 1 < len(keys) else ""
        if k in ("pickups", "lights"):
            items = ",\n".join("    " + json.dumps(e) for e in out[k])
            lines.append(f'  "{k}": [\n{items}\n  ]{sep}')
        elif k == "cells_rle":
            pal = json.dumps(out[k]["palette"], separators=(", ", ": "))
            runs = json.dumps(out[k]["runs"], separators=(",", ":"))
            lines.append(f'  "{k}": {{"palette": {pal}, "runs": {runs}}}{sep}')
        else:
            lines.append(f'  "{k}": {json.dumps(out[k], separators=(", ", ": "))}{sep}')
    lines.append("}")
    MAP.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"lights={len(out['lights'])} bulb_cells={len(bulb_cells)} "
          f"moon_cells_removed={sum(1 for q in quints if q[4] == moon)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
