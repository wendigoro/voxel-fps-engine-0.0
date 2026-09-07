"""Projectile types and effect profiles (Python-authored)."""

from __future__ import annotations

from dataclasses import asdict, dataclass, field


# Must match C++ kWorldGravity scaling intent.
WORLD_GRAVITY = 9.81 * 0.35


@dataclass
class ProjectileType:
    id: str
    mass: float
    speed: float
    radius: float
    base_damage: float
    penetration: float
    gravity_scale: float = 1.0
    splash_radius: float = 0.0
    splash_falloff: float = 1.0
    effect: str = "kinetic"  # kinetic | explosive | shred | energy
    caliber: str = "medium"  # light | medium | heavy | energy
    hitscan: bool = False
    grain: float = 0.0
    tags: list[str] = field(default_factory=list)


# Radii/speeds tuned for VOXEL_SIZE=0.001 warehouse grid.
PROJECTILES: list[ProjectileType] = [
    ProjectileType(
        id="slug",
        mass=0.06,
        speed=3.2,
        radius=0.0025,
        base_damage=10.0,
        penetration=0.40,
        gravity_scale=1.0,
        effect="kinetic",
        tags=["default", "ballistic"],
    ),
    ProjectileType(
        id="spike",
        mass=0.04,
        speed=4.5,
        radius=0.0018,
        base_damage=8.0,
        penetration=0.65,
        gravity_scale=0.85,
        effect="kinetic",
        tags=["ap"],
    ),
    ProjectileType(
        id="shredder",
        mass=0.05,
        speed=2.8,
        radius=0.003,
        base_damage=7.0,
        penetration=0.25,
        gravity_scale=1.0,
        splash_radius=0.006,
        splash_falloff=1.4,
        effect="shred",
        tags=["anti_foliage"],
    ),
ProjectileType(
        id="impact_charge",
        mass=0.12,
        speed=2.0,
        radius=0.004,
        base_damage=18.0,
        penetration=0.15,
        gravity_scale=1.15,
        splash_radius=0.012,
        splash_falloff=1.1,
        effect="explosive",
        tags=["aoe"],
    ),
    # --- caliber class profiles (weapon system) ---
    ProjectileType(
        id="light_ball",
        mass=0.035,
        speed=4.2,
        radius=0.0016,
        base_damage=6.5,
        penetration=0.35,
        gravity_scale=0.85,
        effect="kinetic",
        tags=["caliber", "light", "ballistic"],
    ),
    ProjectileType(
        id="medium_ball",
        mass=0.06,
        speed=3.2,
        radius=0.0025,
        base_damage=10.0,
        penetration=0.40,
        gravity_scale=1.0,
        effect="kinetic",
        tags=["caliber", "medium", "ballistic"],
    ),
    ProjectileType(
        id="heavy_ball",
        mass=0.14,
        speed=2.1,
        radius=0.0035,
        base_damage=16.0,
        penetration=0.45,
        gravity_scale=1.25,
        splash_radius=0.004,
        splash_falloff=1.2,
        effect="kinetic",
        tags=["caliber", "heavy", "ballistic"],
    ),
    ProjectileType(
        id="energy_beam",
        mass=0.001,
        speed=50.0,  # unused when hitscan
        radius=0.0015,
        base_damage=12.0,
        penetration=0.7,
        gravity_scale=0.0,
        effect="energy",
        tags=["caliber", "energy", "hitscan"],
    ),
]


def projectiles_json() -> list[dict]:
    return [asdict(p) for p in PROJECTILES]


def get_projectile(pid: str) -> ProjectileType:
    for p in PROJECTILES:
        if p.id == pid:
            return p
    return PROJECTILES[0]
