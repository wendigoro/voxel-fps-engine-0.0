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
    tags: list[str] = field(default_factory=list)
    caliber: str = ""  # light | medium | heavy | energy
    hitscan: bool = False
    ammo_id: str = ""  # optional link into ammo table


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
    # Caliber ballistic profiles (gravity projectiles).
    ProjectileType(
        id="light_ball",
        mass=0.03,
        speed=4.0,
        radius=0.0015,
        base_damage=6.0,
        penetration=0.30,
        gravity_scale=1.0,
        effect="kinetic",
        tags=["ballistic", "caliber"],
        caliber="light",
    ),
    ProjectileType(
        id="medium_ball",
        mass=0.06,
        speed=3.4,
        radius=0.0022,
        base_damage=11.0,
        penetration=0.42,
        gravity_scale=1.0,
        effect="kinetic",
        tags=["ballistic", "caliber"],
        caliber="medium",
    ),
    ProjectileType(
        id="heavy_ball",
        mass=0.11,
        speed=2.6,
        radius=0.0032,
        base_damage=16.0,
        penetration=0.55,
        gravity_scale=1.1,
        effect="kinetic",
        tags=["ballistic", "caliber"],
        caliber="heavy",
    ),
    # Energy hitscan: no gravity, instant ray flag for engine-fire child.
    ProjectileType(
        id="energy_beam",
        mass=0.01,
        speed=80.0,
        radius=0.0012,
        base_damage=14.0,
        penetration=0.70,
        gravity_scale=0.0,
        effect="energy",
        tags=["energy", "hitscan"],
        caliber="energy",
        hitscan=True,
    ),
]


def projectiles_json() -> list[dict]:
    return [asdict(p) for p in PROJECTILES]


def get_projectile(pid: str) -> ProjectileType:
    for p in PROJECTILES:
        if p.id == pid:
            return p
    return PROJECTILES[0]
