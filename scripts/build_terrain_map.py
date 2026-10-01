"""Write the terrain demo map (data/maps/valley_demo.map.vox.json).

    python scripts/build_terrain_map.py [--out data/maps/valley_demo.map.vox.json]

The document is deliberately almost empty: no cells, no prefabs, no spawn, no
lights. Everything visible is produced by the engine's terrain generator from
the "terrain" section, which is the point of the demo -- it shows the generator
working with nothing of its own to hide behind.

The seed is a literal here rather than a clock or a random draw, so regenerating
this file always produces the same world (RULES.md, determinism). Change it by
hand to explore other seeds:

    voxel_engine.exe --map data/maps/valley_demo.map.vox.json
"""

import argparse
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]

# sim::kWorldW / kWorldH / kWorldD. The engine refuses a map whose dims differ
# from the world, so these are not free.
DIMS = (192, 64, 160)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "data" / "maps" / "valley_demo.map.vox.json"))
    ap.add_argument("--class", dest="family", default="valley",
                    choices=["valley", "dunes", "tundra", "parkland"])
    ap.add_argument("--seed", type=int, default=20260930)
    ap.add_argument("--base", type=int, default=None,
                    help="ground base cell; omitted means the class default")
    ap.add_argument("--no-roads", action="store_true")
    ap.add_argument("--no-props", action="store_true")
    ap.add_argument("--road-x", type=int, default=None,
                    help="which region column carries the road; omitted means seeded")
    args = ap.parse_args()

    terrain = {
        "class": args.family,
        "seed": args.seed,
        "props": not args.no_props,
        "roads": not args.no_roads,
    }
    if args.base is not None:
        terrain["base"] = args.base
    if args.road_x is not None:
        terrain["road_x"] = args.road_x

    doc = {
        "format_version": 2,
        "unit": 1,
        "voxel_size": 0.001,
        "mode": "map",
        "dims": list(DIMS),
        "terrain": terrain,
        "cells_rle": {"palette": [], "runs": []},
    }
    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(doc, indent=1) + "\n", encoding="utf-8")
    print(f"{out} class={args.family} seed={args.seed} roads={terrain['roads']} "
          f"props={terrain['props']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
