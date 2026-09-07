#!/usr/bin/env python3
"""Export projectile/material defs for C++ and run a gravity physics demo."""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from effects.physics_loop import run_demo  # noqa: E402
from projectiles.ammo import ammo_json  # noqa: E402
from projectiles.defs import WORLD_GRAVITY, projectiles_json  # noqa: E402
from projectiles.materials import materials_json  # noqa: E402


def main() -> int:
    data_dir = ROOT / "data"
    data_dir.mkdir(parents=True, exist_ok=True)
    build_dir = ROOT / "build"
    build_dir.mkdir(parents=True, exist_ok=True)

    payload = {
        "gravity": WORLD_GRAVITY,
        "physics_engine": "pymunk",
        "materials": materials_json(),
        "projectiles": projectiles_json(),
        "ammo": ammo_json(),
    }

    out_data = data_dir / "projectiles.json"
    out_build = build_dir / "projectiles.json"
    text = json.dumps(payload, indent=2)
    out_data.write_text(text, encoding="utf-8")
    out_build.write_text(text, encoding="utf-8")
    print(f"wrote {out_data}")
    print(f"wrote {out_build}")

    demo = run_demo()
    demo_path = build_dir / "effects_demo.json"
    demo_path.write_text(json.dumps(demo, indent=2), encoding="utf-8")
    print(f"physics demo: {demo_path}")
    print(
        f"engine={demo['engine']} gravity={demo['gravity']:.3f} "
        f"destroys={demo['destroy_count']} debris={demo['debris_bodies']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
