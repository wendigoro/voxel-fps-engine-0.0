"""Convert voxfmt v1 documents (a "voxels" list) to v2 (run-length layers).

    python scripts/migrate_voxfmt_v2.py <file-or-dir> [...] [--dry-run]

v2 is what the painter and engine write now (data/voxfmt/schema.md, "Format
v2"). Both still read v1, so migrating is optional, but v2 files are far
smaller (the 32x16x32 map fixture went from tens of KB to 1.5 KB) and are the
only form the engine's prefab loader is designed around.

Conversion, cell for cell:
  - each voxel's material becomes a cells_rle run (unknown names are kept;
    the engine drops and counts painter-only materials, as with v1),
  - its rgb becomes paint in "appearance" only when it differs from the
    material's own colour, read from MaterialPalette.java so the rule cannot
    drift from the painter's,
  - its weapon part becomes a parts_rle run,
  - "feet" moves into "anchors" next to a "pivot",
  - every other key is carried over unchanged.
Files already at v2 are skipped. The original is overwritten unless --dry-run.
"""

import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
PALETTE_JAVA = ROOT / "java" / "painter" / "src" / "voxel" / "painter" / "grid" / "MaterialPalette.java"


def material_defaults() -> dict:
    text = PALETTE_JAVA.read_text(encoding="utf-8")
    return {name: int(rgb, 16) for name, rgb in re.findall(r'new Entry\(\w+, "(\w+)", 0x([0-9A-Fa-f]{6})\)', text)}


def runs(dims, value_of):
    """Runs along +X of equal non-None values: flat [y, z, x0, len, value]."""
    sx, sy, sz = dims
    out = []
    for y in range(sy):
        for z in range(sz):
            x = 0
            while x < sx:
                v = value_of(x, y, z)
                n = 1
                while x + n < sx and value_of(x + n, y, z) == v:
                    n += 1
                if v is not None:
                    out.append((y, z, x, n, v))
                x += n
    return out


def convert(doc: dict, defaults: dict) -> dict:
    dims = doc["dims"]
    cells = {}
    for v in doc.get("voxels", []):
        cells[(v["x"], v["y"], v["z"])] = v
    mat = lambda x, y, z: cells.get((x, y, z), {}).get("mat") if (x, y, z) in cells and cells[(x, y, z)].get("mat") != "air" else None

    def paint(x, y, z):
        v = cells.get((x, y, z))
        if not v or v.get("mat") == "air":
            return None
        rgb = int(v.get("rgb", 0)) & 0xFFFFFF
        return None if rgb == defaults.get(v["mat"]) else rgb

    def part(x, y, z):
        v = cells.get((x, y, z))
        return v.get("part") if v and v.get("mat") != "air" and v.get("part") else None

    out = {k: v for k, v in doc.items() if k not in ("voxels", "feet", "format_version")}
    out = {"format_version": 2, **out}
    anchors = {"pivot": [dims[0] // 2, 0, dims[2] // 2]}
    if "feet" in doc:
        anchors["feet"] = doc["feet"]
    out["anchors"] = anchors

    mat_runs = runs(dims, mat)
    names = sorted({r[4] for r in mat_runs})
    paint_runs = runs(dims, paint)
    colors = []
    for r in paint_runs:
        if r[4] not in colors:
            colors.append(r[4])
    if len(colors) > 255:
        raise SystemExit("more than 255 distinct paint colours; v2 palettes hold 255")
    part_runs = runs(dims, part)
    parts = sorted({r[4] for r in part_runs})
    if paint_runs:
        out["appearance"] = {
            "palette": [[(c >> 16) & 255, (c >> 8) & 255, c & 255] for c in colors],
            "runs": [v for r in paint_runs for v in (*r[:4], colors.index(r[4]) + 1)],
        }
    if part_runs:
        out["parts_rle"] = {"palette": parts, "runs": [v for r in part_runs for v in (*r[:4], parts.index(r[4]))]}
    out["cells_rle"] = {"palette": names, "runs": [v for r in mat_runs for v in (*r[:4], names.index(r[4]))]}
    return out


def main(argv) -> int:
    dry = "--dry-run" in argv
    targets = [pathlib.Path(a) for a in argv if a != "--dry-run"]
    if not targets:
        print(__doc__)
        return 2
    defaults = material_defaults()
    files = []
    for t in targets:
        files += sorted(t.rglob("*.vox.json")) if t.is_dir() else [t]
    for f in files:
        doc = json.loads(f.read_text(encoding="utf-8"))
        if int(doc.get("format_version", 1)) >= 2:
            print(f"skip (already v2) {f}")
            continue
        new = convert(doc, defaults)
        cells = sum(r for r in new["cells_rle"]["runs"][3::5])
        print(f"{'would convert' if dry else 'converted'} {f}: {len(doc.get('voxels', []))} voxels -> "
              f"{len(new['cells_rle']['runs']) // 5} runs ({cells} cells)")
        if not dry:
            f.write_text(json.dumps(new, separators=(", ", ": ")) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
