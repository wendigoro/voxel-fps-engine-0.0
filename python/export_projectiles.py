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
from projectiles.defs import WORLD_GRAVITY, PROJECTILES, projectiles_json  # noqa: E402
from projectiles.materials import materials_json  # noqa: E402


def _enrich_projectiles() -> list[dict]:
    """Attach caliber/hitscan/grain defaults derived from tags for C++."""
    out = []
    for p in PROJECTILES:
        d = dict(
            id=p.id,
            mass=p.mass,
            speed=p.speed,
            radius=p.radius,
            base_damage=p.base_damage,
            penetration=p.penetration,
            gravity_scale=p.gravity_scale,
            splash_radius=p.splash_radius,
            splash_falloff=p.splash_falloff,
            effect=p.effect,
            tags=list(p.tags),
        )
        cal = p.caliber
        if "light" in p.tags:
            cal = "light"
        elif "heavy" in p.tags:
            cal = "heavy"
        elif "energy" in p.tags or p.effect == "energy":
            cal = "energy"
        elif "medium" in p.tags:
            cal = "medium"
        hitscan = p.hitscan or cal == "energy" or p.effect == "energy" or "hitscan" in p.tags
        d["caliber"] = cal
        d["hitscan"] = 1 if hitscan else 0
        d["grain"] = float(p.grain)
        d["pellets"] = int(getattr(p, "pellets", 1) or 1)
        d["spread_deg"] = float(getattr(p, "spread_deg", 0.0) or 0.0)
        if hitscan:
            d["gravity_scale"] = 0.0
        out.append(d)
    return out


def main() -> int:
    data_dir = ROOT / "data"
    data_dir.mkdir(parents=True, exist_ok=True)
    build_dir = ROOT / "build"
    build_dir.mkdir(parents=True, exist_ok=True)

    payload = {
        "gravity": WORLD_GRAVITY,
        "physics_engine": "pymunk",
        "materials": materials_json(),
        "projectiles": _enrich_projectiles(),
        "ammo": ammo_json(),
        "calibers": ["light", "medium", "heavy", "energy"],
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
