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
    pellets: int = 1  # >1 = shotgun-style multi-spawn
    spread_deg: float = 0.0  # cone half-angle for multi-pellet
    tags: list[str] = field(default_factory=list)


# Bullet size is one 8x8x8 subunit of a unit voxel (VOXEL_SIZE=0.001).
# subunit edge = 0.001/8; projectile radius = half-edge (fits one subunit cube).
VOXEL_SIZE = 0.001
SUB_DIV = 8
SUB_EDGE = VOXEL_SIZE / SUB_DIV  # 0.000125
SUB_RADIUS = SUB_EDGE * 0.5  # one subunit projectile

# Caliber area scales (multiples of one subunit radius).
CAL_R = {
    "light": 1.0,
    "medium": 1.6,
    "heavy": 2.4,
    "energy": 1.2,
}


def _r(cal: str, mult: float = 1.0) -> float:
    return SUB_RADIUS * CAL_R.get(cal, 1.6) * mult


def _splash(cal: str, damage: float) -> float:
    """AOE radius from caliber + damage (world units); density applied at impact."""
    base = _r(cal) * 2.0
    return base * (0.85 + damage / 20.0)


# Radii are subunit-based; splash is caliber/damage scaled (density at impact).
PROJECTILES: list[ProjectileType] = [
    ProjectileType(
        id="slug",
        mass=0.06,
        speed=3.2,
        radius=_r("medium"),
        base_damage=10.0,
        penetration=0.40,
        gravity_scale=1.0,
        splash_radius=_splash("medium", 10.0),
        effect="kinetic",
        caliber="medium",
        tags=["default", "ballistic"],
    ),
    ProjectileType(
        id="spike",
        mass=0.04,
        speed=4.5,
        radius=_r("medium", 0.85),
        base_damage=8.0,
        penetration=0.65,
        gravity_scale=0.85,
        effect="kinetic",
        caliber="medium",
        tags=["ap"],
    ),
    ProjectileType(
        id="shredder",
        mass=0.05,
        speed=2.8,
        radius=_r("medium", 1.1),
        base_damage=7.0,
        penetration=0.25,
        gravity_scale=1.0,
        splash_radius=_splash("medium", 7.0) * 1.2,
        splash_falloff=1.4,
        effect="shred",
        caliber="medium",
        tags=["anti_foliage"],
    ),
    ProjectileType(
        id="impact_charge",
        mass=0.12,
        speed=2.0,
        radius=_r("heavy", 1.0),
        base_damage=18.0,
        penetration=0.15,
        gravity_scale=1.15,
        splash_radius=_splash("heavy", 18.0) * 1.5,
        splash_falloff=1.1,
        effect="explosive",
        caliber="heavy",
        tags=["aoe"],
    ),
    # --- caliber class profiles (weapon system) — subunit sized ---
    ProjectileType(
        id="light_ball",
        mass=0.035,
        speed=4.2,
        radius=_r("light"),
        base_damage=6.5,
        penetration=0.35,
        gravity_scale=0.85,
        splash_radius=_splash("light", 6.5),
        effect="kinetic",
        caliber="light",
        tags=["caliber", "light", "ballistic"],
    ),
    ProjectileType(
        id="medium_ball",
        mass=0.06,
        speed=3.2,
        radius=_r("medium"),
        base_damage=10.0,
        penetration=0.40,
        gravity_scale=1.0,
        splash_radius=_splash("medium", 10.0),
        effect="kinetic",
        caliber="medium",
        tags=["caliber", "medium", "ballistic"],
    ),
    ProjectileType(
        id="heavy_ball",
        mass=0.14,
        speed=2.1,
        radius=_r("heavy"),
        base_damage=16.0,
        penetration=0.45,
        gravity_scale=1.25,
        splash_radius=_splash("heavy", 16.0),
        splash_falloff=1.2,
        effect="kinetic",
        caliber="heavy",
        tags=["caliber", "heavy", "ballistic"],
    ),
    ProjectileType(
        id="energy_beam",
        mass=0.001,
        speed=50.0,
        radius=_r("energy"),
        base_damage=12.0,
        penetration=0.7,
        gravity_scale=0.0,
        splash_radius=_splash("energy", 12.0) * 0.5,
        effect="energy",
        caliber="energy",
        tags=["caliber", "energy", "hitscan"],
    ),
    # Shotgun: light-caliber pellets (one subunit each), multi-spawn cone.
    ProjectileType(
        id="shotgun_light",
        mass=0.012,
        speed=3.6,
        radius=_r("light"),  # one subunit per pellet
        base_damage=3.2,
        penetration=0.22,
        gravity_scale=0.95,
        splash_radius=_splash("light", 3.2) * 0.7,
        splash_falloff=1.3,
        effect="kinetic",
        caliber="light",
        pellets=7,
        spread_deg=6.5,
        tags=["shotgun", "pellet", "light", "ballistic", "caliber"],
    ),
]


def projectiles_json() -> list[dict]:
    return [asdict(p) for p in PROJECTILES]


def get_projectile(pid: str) -> ProjectileType:
    for p in PROJECTILES:
        if p.id == pid:
            return p
    return PROJECTILES[0]
