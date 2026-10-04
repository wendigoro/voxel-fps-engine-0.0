"""Convert engine capture shots to PNG and compare them against a golden set.

The engine's --capture <dir> writes one binary PPM per fixed camera shot
(see kCaptureShots in src/main.cpp). This script:

  capture_compare.py convert <dir>
      writes <shot>.png next to every <shot>.ppm (for looking at them).

  capture_compare.py compare <golden_dir> <new_dir> [--tol N] [--max-frac F]
      compares every golden <shot>.png with the new capture of the same shot.
      A pixel counts as changed when any channel differs by more than --tol
      (default 2, to absorb driver-level rounding). The shot fails when more
      than --max-frac of its pixels changed (default 0.0005). Writes
      <new_dir>/<shot>.diff.png highlighting changed pixels. Exit code 1 if any
      shot fails or is missing.

Golden images are only meaningful on the machine and driver that produced
them; regenerate them there when a change is intentionally visible.
"""

import argparse
import pathlib
import sys

import numpy as np
from PIL import Image


def convert(directory: pathlib.Path) -> int:
    count = 0
    for ppm in sorted(directory.glob("*.ppm")):
        Image.open(ppm).convert("RGB").save(ppm.with_suffix(".png"))
        count += 1
    print(f"converted {count} shot(s) in {directory}")
    return 0 if count else 1


def load(path: pathlib.Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def compare(golden: pathlib.Path, new: pathlib.Path, tol: int, max_frac: float) -> int:
    convert(new)
    failed = 0
    goldens = sorted(golden.glob("*.png"))
    goldens = [g for g in goldens if not g.name.endswith(".diff.png")]
    if not goldens:
        print(f"no golden shots in {golden}")
        return 1
    for g in goldens:
        n = new / g.name
        if not n.exists():
            print(f"FAIL {g.stem}: missing from {new}")
            failed += 1
            continue
        a, b = load(g), load(n)
        if a.shape != b.shape:
            print(f"FAIL {g.stem}: size {a.shape[1]}x{a.shape[0]} vs {b.shape[1]}x{b.shape[0]}")
            failed += 1
            continue
        delta = np.abs(a - b).max(axis=2)
        changed = delta > tol
        frac = float(changed.mean())
        verdict = "ok  " if frac <= max_frac else "FAIL"
        if frac > max_frac:
            failed += 1
        print(f"{verdict} {g.stem}: changed={frac * 100:.4f}% max_delta={int(delta.max())}")
        mask = np.zeros(a.shape, dtype=np.uint8)
        mask[..., 0] = np.where(changed, 255, (b[..., 0] // 4).astype(np.uint8))
        mask[..., 1] = (b[..., 1] // 4).astype(np.uint8)
        mask[..., 2] = (b[..., 2] // 4).astype(np.uint8)
        Image.fromarray(mask).save(new / f"{g.stem}.diff.png")
    print("CAPTURE_COMPARE_OK" if failed == 0 else f"CAPTURE_COMPARE_FAIL shots={failed}")
    return 0 if failed == 0 else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("convert")
    c.add_argument("dir", type=pathlib.Path)
    m = sub.add_parser("compare")
    m.add_argument("golden", type=pathlib.Path)
    m.add_argument("new", type=pathlib.Path)
    m.add_argument("--tol", type=int, default=2)
    m.add_argument("--max-frac", type=float, default=0.0005)
    args = ap.parse_args()
    if args.cmd == "convert":
        return convert(args.dir)
    return compare(args.golden, args.new, args.tol, args.max_frac)


if __name__ == "__main__":
    sys.exit(main())
