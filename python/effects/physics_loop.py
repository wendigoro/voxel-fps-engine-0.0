"""
Effects physics loop powered by Pymunk (Chipmunk2D).

Gravity is applied every step. Used to:
  - simulate ballistic projectile arcs
  - spawn debris fragments on break with material mass
  - emit destruction events the C++ engine can consume

Coordinate mapping for the side-view solver:
  x -> world X, y -> world Y (up). Z is carried as metadata.
"""

from __future__ import annotations

import json
import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import pymunk
from pymunk import Vec2d

import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python"))

from projectiles.defs import WORLD_GRAVITY, get_projectile  # noqa: E402
from projectiles.materials import MATERIALS  # noqa: E402


@dataclass
class DestroyEvent:
    x: int
    y: int
    z: int
    material: str
    energy: float
    effect: str


@dataclass
class SimResult:
    destroys: list[DestroyEvent] = field(default_factory=list)
    impact_pos: tuple[float, float, float] | None = None
    flight_time: float = 0.0
    debris_settled: int = 0


def _voxel_mass(material: str) -> float:
    m = MATERIALS[material]
    vol = 0.001**3
    return m.density * m.weight * vol * 1000.0


def _break_threshold(material: str) -> float:
    m = MATERIALS[material]
    frag = max(0.05, m.fragility)
    return m.toughness * _voxel_mass(material) / frag


def _effect_mul(effect: str, material: str) -> float:
    if effect == "shred":
        if material in ("bush_leaves", "bush_branch"):
            return 2.4
        if material == "wood":
            return 1.3
        return 0.85
    if effect == "explosive":
        if material in ("dirt", "bush_leaves"):
            return 1.6
        if material == "concrete":
            return 0.75
        return 1.2
    return 1.0


class EffectsPhysicsLoop:
    """Pymunk space with gravity for projectile + debris effects."""

    def __init__(self, gravity: float = WORLD_GRAVITY) -> None:
        self.gravity = gravity
        self.space = pymunk.Space()
        self.space.gravity = (0.0, -abs(gravity))
        self.space.damping = 0.98

    def reset(self) -> None:
        self.space = pymunk.Space()
        self.space.gravity = (0.0, -abs(self.gravity))
        self.space.damping = 0.98

    def simulate_shot(
        self,
        projectile_id: str,
        origin: tuple[float, float, float],
        direction: tuple[float, float, float],
        samples: list[dict[str, Any]],
        dt: float = 1.0 / 120.0,
        max_time: float = 2.5,
    ) -> SimResult:
        """
        samples: list of solid voxel proxies along a coarse column/occupancy query
                 each: {x,y,z, wx,wy,wz, material}
        """
        self.reset()
        proj = get_projectile(projectile_id)
        result = SimResult()

        # Normalize direction
        dx, dy, dz = direction
        length = math.sqrt(dx * dx + dy * dy + dz * dz) or 1.0
        dx, dy, dz = dx / length, dy / length, dz / length

        body = pymunk.Body(mass=max(proj.mass, 1e-4), moment=pymunk.moment_for_circle(proj.mass, 0, proj.radius))
        body.position = (origin[0], origin[1])
        body.velocity = Vec2d(dx * proj.speed, dy * proj.speed)
        shape = pymunk.Circle(body, max(proj.radius, 0.005))
        shape.elasticity = 0.05
        shape.friction = 0.4
        shape.collision_type = 1
        self.space.add(body, shape)

        # Static segment blockers from sample voxels projected on XY
        static_body = self.space.static_body
        voxel_lookup: dict[tuple[int, int], dict[str, Any]] = {}
        for s in samples:
            key = (int(s["x"]), int(s["y"]))
            voxel_lookup[key] = s
            hx = float(s["wx"])
            hy = float(s["wy"])
            # small box edge as segment pair
            r = 0.005
            seg = pymunk.Segment(static_body, (hx - r, hy - r), (hx + r, hy + r), r)
            seg.sensor = True
            seg.collision_type = 2
            self.space.add(seg)

        energy = 0.5 * proj.mass * proj.speed * proj.speed * 100.0 * proj.base_damage / 10.0
        energy *= max(0.1, proj.gravity_scale)
        t = 0.0
        hit = False
        last_pos = (origin[0], origin[1], origin[2])

        while t < max_time and energy > 0.05:
            # gravity already in space; step physics
            self.space.step(dt)
            t += dt
            px, py = body.position.x, body.position.y
            # recover z from ray param
            traveled = math.sqrt((px - origin[0]) ** 2 + (py - origin[1]) ** 2)
            pz = origin[2] + dz * traveled
            last_pos = (px, py, pz)

            # occupancy test against samples (grid)
            gx = int(px / 0.001)
            gy = int(py / 0.001)
            gz = int(pz / 0.001)
            key = (gx, gy)
            solid = voxel_lookup.get(key)
            if solid is None:
                # also try nearest sample by world distance
                best = None
                best_d = 1e9
                for s in samples:
                    d = (s["wx"] - px) ** 2 + (s["wy"] - py) ** 2
                    if d < best_d:
                        best_d = d
                        best = s
                if best is not None and best_d < (0.02**2):
                    solid = best

            if solid is None:
                continue

            mat = solid["material"]
            if mat not in MATERIALS:
                continue

            mul = _effect_mul(proj.effect, mat)
            local_e = energy * mul
            thr = _break_threshold(mat)
            if local_e < thr:
                energy *= 1.0 - MATERIALS[mat].damping
                # bounce lightly
                body.velocity *= 0.2
                continue

            # destroy primary voxel
            result.destroys.append(
                DestroyEvent(
                    x=int(solid["x"]),
                    y=int(solid["y"]),
                    z=int(solid["z"]),
                    material=mat,
                    energy=local_e,
                    effect=proj.effect,
                )
            )
            hit = True
            result.impact_pos = (px, py, pz)

            # splash
            if proj.splash_radius > 0:
                for s in samples:
                    d = math.sqrt(
                        (s["wx"] - px) ** 2 + (s["wy"] - py) ** 2 + (s["wz"] - pz) ** 2
                    )
                    if d > proj.splash_radius:
                        continue
                    fall = max(0.0, 1.0 - d / max(proj.splash_radius, 1e-6)) ** proj.splash_falloff
                    se = local_e * fall * 0.65
                    sm = s["material"]
                    if sm in MATERIALS and se >= _break_threshold(sm) * 0.8:
                        result.destroys.append(
                            DestroyEvent(
                                x=int(s["x"]),
                                y=int(s["y"]),
                                z=int(s["z"]),
                                material=sm,
                                energy=se,
                                effect=proj.effect,
                            )
                        )
                        self._spawn_debris(s["wx"], s["wy"], sm, se)

            self._spawn_debris(px, py, mat, local_e)
            energy *= proj.penetration * (1.0 - MATERIALS[mat].damping * 0.5)

            if proj.effect != "explosive" and energy < thr * 0.5:
                break
            if proj.effect == "explosive":
                break

        # let debris settle briefly under gravity
        for _ in range(30):
            self.space.step(dt)
        result.debris_settled = sum(1 for b in self.space.bodies if not b.body_type == pymunk.Body.STATIC)
        result.flight_time = t
        if not hit:
            result.impact_pos = last_pos

        # unique voxel destroys
        uniq: dict[tuple[int, int, int], DestroyEvent] = {}
        for d in result.destroys:
            uniq[(d.x, d.y, d.z)] = d
        result.destroys = list(uniq.values())
        return result

    def _spawn_debris(self, x: float, y: float, material: str, energy: float) -> None:
        mass = max(_voxel_mass(material), 1e-4)
        moment = pymunk.moment_for_circle(mass, 0, 0.006)
        body = pymunk.Body(mass=mass, moment=moment)
        body.position = (x, y)
        # kick outward/up with gravity taking over in subsequent steps
        ang = (hash((int(x * 1000), int(y * 1000), material)) % 360) * math.pi / 180.0
        kick = min(4.0, 0.5 + energy * 0.02)
        body.velocity = Vec2d(math.cos(ang) * kick, abs(math.sin(ang)) * kick + 1.5)
        shape = pymunk.Circle(body, 0.006)
        shape.elasticity = 0.1
        shape.friction = 0.8
        self.space.add(body, shape)


def run_demo() -> dict[str, Any]:
    """Standalone demo shot into a vertical stack of starter materials."""
    loop = EffectsPhysicsLoop()
    samples = []
    # Dense wall of materials ahead of the muzzle so gravity arc still intersects.
    mats = ["bush_leaves", "bush_branch", "wood", "dirt", "concrete"]
    for i, mat in enumerate(mats):
        for dy in range(6):
            for dx in range(-2, 3):
                y = 0.18 + i * 0.04 + dy * 0.01
                x = 0.35 + dx * 0.01
                samples.append(
                    {
                        "x": int(x / 0.001),
                        "y": int(y / 0.001),
                        "z": 40,
                        "wx": x,
                        "wy": y,
                        "wz": 0.40,
                        "material": mat,
                    }
                )

    res = loop.simulate_shot(
        "impact_charge",
        origin=(0.05, 0.30, 0.40),
        direction=(1.0, -0.15, 0.0),
        samples=samples,
        max_time=1.5,
    )
    return {
        "flight_time": res.flight_time,
        "impact_pos": res.impact_pos,
        "destroy_count": len(res.destroys),
        "destroys": [d.__dict__ for d in res.destroys[:32]],
        "debris_bodies": res.debris_settled,
        "gravity": WORLD_GRAVITY,
        "engine": f"pymunk {pymunk.version}",
    }


if __name__ == "__main__":
    print(json.dumps(run_demo(), indent=2))
