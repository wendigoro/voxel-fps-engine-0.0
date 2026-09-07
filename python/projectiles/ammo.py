"""Ammo subtypes (integration scaffold — caliber + effect_tags for future VFX)."""

from __future__ import annotations

from dataclasses import asdict, dataclass, field


@dataclass(frozen=True)
class AmmoType:
    id: str
    caliber: str  # light | medium | heavy | energy
    grain: float  # nominal mass proxy (grains-style scale, not SI)
    mass_scale: float  # multiplies projectile mass at fire time
    effect_tags: list[str] = field(default_factory=list)
    # shred | explosive | ap | incendiary placeholders — no deep VFX yet


# Scaffold only: engine-fire child may bind ammo_id → projectile caliber later.
AMMO: list[AmmoType] = [
    AmmoType(
        id="fmj_light",
        caliber="light",
        grain=55.0,
        mass_scale=1.0,
        effect_tags=["ap"],
    ),
    AmmoType(
        id="fmj_medium",
        caliber="medium",
        grain=120.0,
        mass_scale=1.0,
        effect_tags=["ap"],
    ),
    AmmoType(
        id="fmj_heavy",
        caliber="heavy",
        grain=220.0,
        mass_scale=1.05,
        effect_tags=["ap"],
    ),
    AmmoType(
        id="hollow_shred",
        caliber="medium",
        grain=100.0,
        mass_scale=0.95,
        effect_tags=["shred"],
    ),
    AmmoType(
        id="he_fragment",
        caliber="heavy",
        grain=180.0,
        mass_scale=1.1,
        effect_tags=["explosive"],
    ),
    AmmoType(
        id="incendiary_light",
        caliber="light",
        grain=50.0,
        mass_scale=0.9,
        effect_tags=["incendiary"],
    ),
    AmmoType(
        id="cell_energy",
        caliber="energy",
        grain=0.0,
        mass_scale=1.0,
        effect_tags=["energy"],
    ),
]


def ammo_json() -> list[dict]:
    return [asdict(a) for a in AMMO]


def get_ammo(ammo_id: str) -> AmmoType | None:
    for a in AMMO:
        if a.id == ammo_id:
            return a
    return None
