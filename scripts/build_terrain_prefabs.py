"""Generate the terrain road and decoration prefabs (data/prefabs/*.vox.json).

    python scripts/build_terrain_prefabs.py [--out data/prefabs]

Why a script rather than hand-authored files: src/terrain.hpp decides a road
node's *shape* and its quarter turn, and a prefab that disagrees with that
decision is a road pointing at open terrain. The shape vocabulary here is
therefore derived from the same canonical masks terrain.hpp uses, so adding a
road kind forces an update in one place.

Every road tile is 64x64 (terrain::kRegionCells = 2x2 chunks) and 2 cells tall:

    y=0  base course   Dirt    — the embankment's own footing
    y=1  surface       Asphalt  — the drivable surface, Concrete along the edges

and the tile is stamped at (region origin, road level - 1), so local y=1 lands
on the road level the generator graded to, and local y=0 lands on the cell just
below it. That is exactly the two cells terrain::gradeRoads leaves open: it
fills to level - 2 and stops, so the tile's own two layers complete the column.

The height is not free. A third layer would push the surface to level + 1 and
open a one-cell air gap under the asphalt, because nothing else fills it. That
is not hypothetical: it is what a 3-cell tile actually did, and every gate still
passed, because the shape check only asked which SIDES the surface reached and
never which Y it sat at. terrain::kRoadLayers below is the single number, and
both the generator and the engine's altitude check read it.

Layout convention, matching the engine's grid: +X is east, +Z is north, and the
side bits in terrain.hpp are kSideW = -X, kSideE = +X, kSideS = -Z, kSideN = +Z.
A tile is authored in its canonical orientation (see CANONICAL below) and the
engine turns it; these files must never be pre-rotated to compensate.

Sides present means "the road continues off this tile's edge on that side". A
region at the edge of the world has no neighbour there, so it has no such side:
a dead end is kSideW alone, and the network must never present a stub.
"""

import argparse
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]

# terrain.hpp: kSideW=1, kSideE=2, kSideS=4, kSideN=8, and canonicalMask().
W, E, S, N = 1, 2, 4, 8
SIDES = ((W, -1, 0), (E, 1, 0), (S, 0, -1), (N, 0, 1))

TILE = 64          # terrain::kRegionCells
HALF = TILE // 2
ROAD_HALF = 8      # half the drivable width, in cells
KERB_HALF = ROAD_HALF + 1

# The tile's height, and the only place it is written. terrain::kRoadLayers
# says the same number for the engine, and the altitude gate compares the
# stamped surface against it, so the two cannot drift apart silently.
LAYERS = 2

# The canonical side mask per kind, copied from terrain::canonicalMask(). The
# engine picks the asset and the turn, so these are the orientations that get
# authored.
CANONICAL = {
    "road_end": W,
    "road_straight": W | E,
    "road_corner": W | N,
    "road_tee": W | E | N,
    "road_cross": W | E | S | N,
}

# The id in kClasses[].prop, and the block each is built from. Pines are the
# only prop with a trunk, so they carry a wood cell under the leaves.
PROPS = {
    "prop_shrub": {"dims": (3, 2, 3), "mat": "grass"},
    "prop_rock": {"dims": (3, 2, 3), "mat": "concrete"},
    "prop_pine": {"dims": (3, 5, 3), "mat": "snow", "trunk": "wood"},
}


def rle(cells):
    """Flat [y, z, x0, len, palette_index] runs along +X.

    Runs are cut wherever the material changes, so a single row may hold several
    materials. That is not an optimisation: with the kerb flush at the surface
    layer, one row of a road tile legitimately holds kerb, road, and kerb, and a
    per-row "one material" assumption lets the first cell's material swallow the
    whole row. It did exactly that, and every tile silently lost its north arm.
    """
    palette, index = [], {}

    def slot(mat):
        if mat not in index:
            index[mat] = len(palette)
            palette.append(mat)
        return index[mat]

    rows = {}
    for (y, z, x, mat) in cells:
        rows.setdefault((y, z), {})[x] = mat

    runs = []
    for (y, z) in sorted(rows):
        line = rows[(y, z)]
        xs = sorted(line)
        i = 0
        while i < len(xs):
            mat = line[xs[i]]
            j = i
            # Extend only while the cells are contiguous AND the same material:
            # a gap ends the run, and so does a change of material.
            while (j + 1 < len(xs) and xs[j + 1] == xs[j] + 1
                   and line[xs[j + 1]] == mat):
                j += 1
            runs.append([y, z, xs[i], xs[j] - xs[i] + 1, slot(mat)])
            i = j + 1
    return palette, runs


def road_cells(mask):
    """(y, z, x, material) cells for one 64x64 road tile.

    The road is the union of arms running from the tile centre out through each
    side the mask actually presents, half a road wide. Deriving it from the mask
    rather than from the kind is what makes the tile self-consistent: a dead end
    (W alone) stops at the centre and gets a kerb across it, instead of quietly
    running the full width and presenting a second side nobody asked for.

    The kerb is the 4-neighbourhood of the road that is not road, so it always
    follows the road it belongs to, including around a corner.
    """
    cells = []

    def arm(ox, oz):
        # ox/oz are cell offsets from the tile centre.
        if (mask & W) and ox < 0 and abs(oz) <= ROAD_HALF:
            return True
        if (mask & E) and ox > 0 and abs(oz) <= ROAD_HALF:
            return True
        if (mask & S) and oz < 0 and abs(ox) <= ROAD_HALF:
            return True
        if (mask & N) and oz > 0 and abs(ox) <= ROAD_HALF:
            return True
        return ox == 0 and oz == 0

    road = {(x, z) for z in range(TILE) for x in range(TILE)
            if arm(x - HALF, z - HALF)}
    kerb = set()
    for (x, z) in road:
        for (dx, dz) in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, nz = x + dx, z + dz
            # Only inside the tile: a neighbour outside belongs to the next
            # tile, and kerbing it here would build a wall across the seam.
            if 0 <= nx < TILE and 0 <= nz < TILE and (nx, nz) not in road:
                kerb.add((nx, nz))

    for z in range(TILE):
        for x in range(TILE):
            if (x, z) in road:
                cells.append((1, z, x, "asphalt"))
            elif (x, z) in kerb:
                # Flush with the surface, not a step above it: the kerb is the
                # edge of the drivable area, and a raised lip would have to be a
                # third layer, which is the height this tile no longer has.
                cells.append((1, z, x, "concrete"))
            # The base course spans the whole tile: it is what carries the
            # surface over a dip, and terrain::gradeRoads leaves exactly this
            # one cell under the surface for it.
            cells.append((0, z, x, "dirt"))
    return cells


def prop_cells(spec):
    sx, sy, sz = spec["dims"]
    trunk = spec.get("trunk")
    cells = []
    for y in range(sy):
        for z in range(sz):
            for x in range(sx):
                if trunk and y == 0:
                    cells.append((y, z, x, trunk))
                    continue
                cx, cz = x - sx // 2, z - sz // 2
                # A rounded blob, so a prop is not a hard cube at this scale.
                if abs(cx) + abs(cz) <= max(1, sx // 2):
                    cells.append((y, z, x, spec["mat"]))
    return cells


def document(cells, sx, sy, sz, mode, extra=None):
    palette, runs = rle(cells)
    doc = {
        "format_version": 2,
        "unit": 1,
        "voxel_size": 0.001,
        "mode": mode,
        "dims": [sx, sy, sz],
    }
    if extra:
        doc.update(extra)
    doc["cells_rle"] = {"palette": palette, "runs": runs}
    return doc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "data" / "prefabs"))
    args = ap.parse_args()
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    written = []
    for name, mask in CANONICAL.items():
        doc = document(road_cells(mask), TILE, LAYERS, TILE, "prefab")
        # A note in the file, so whoever opens a road tile in the painter can
        # see which orientation it is in and that the engine owns the rotation.
        doc["road"] = {"sides": mask, "canonical": True}
        (out / f"{name}.vox.json").write_text(
            json.dumps(doc, indent=1) + "\n", encoding="utf-8")
        written.append((name, len(doc["cells_rle"]["runs"])))

    for name, spec in PROPS.items():
        sx, sy, sz = spec["dims"]
        doc = document(prop_cells(spec), sx, sy, sz, "prefab")
        (out / f"{name}.vox.json").write_text(
            json.dumps(doc, indent=1) + "\n", encoding="utf-8")
        written.append((name, len(doc["cells_rle"]["runs"])))

    for name, runs in written:
        print(f"{name}: {runs} runs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
