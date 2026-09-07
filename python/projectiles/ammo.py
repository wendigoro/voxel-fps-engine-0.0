"""Ammo subtypes — simplified grain/caliber integration for effects hooks.

Effects are tag-only for now; full VFX/physics responses land later.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass, field


@dataclass
class AmmoType:
    id: str
    caliber: str  # light | medium | heavy | energy
    grain: float  # ballistic grain proxy (energy uses "charge" as grain)
    mass_scale: float = 1.0
    damage_scale: float = 1.0
    penetration_scale: float = 1.0
    gravity_scale: float = 1.0
    hitscan: bool = False
    effect: str = "kinetic"  # base effect family
    effect_tags: list[str] = field(default_factory=list)  # future: shred, ap, explosive, incendiary, ...
    notes: str = ""


# Robust-but-simple table: caliber classes + a few effect subtypes.
AMMO: list[AmmoType] = [
    # --- light ---
    AmmoType("light_fmj", "light", grain=55, mass_scale=0.85, damage_scale=0.9, penetration_scale=0.9,
             gravity_scale=0.9, effect="kinetic", effect_tags=["ballistic", "fmj"], notes="light FMJ"),
    AmmoType("light_hp", "light", grain=50, mass_scale=0.8, damage_scale=1.05, penetration_scale=0.55,
             gravity_scale=0.9, effect="kinetic", effect_tags=["ballistic", "hollow_point"], notes="light HP"),
    AmmoType("light_ap", "light", grain=62, mass_scale=0.9, damage_scale=0.95, penetration_scale=1.35,
             gravity_scale=0.88, effect="kinetic", effect_tags=["ballistic", "ap"], notes="light AP"),
    # --- medium ---
    AmmoType("medium_fmj", "medium", grain=150, mass_scale=1.0, damage_scale=1.0, penetration_scale=1.0,
             gravity_scale=1.0, effect="kinetic", effect_tags=["ballistic", "fmj"], notes="medium default"),
    AmmoType("medium_tracer", "medium", grain=145, mass_scale=1.0, damage_scale=1.0, penetration_scale=0.95,
             gravity_scale=1.0, effect="kinetic", effect_tags=["ballistic", "tracer"], notes="tracer hook"),
    AmmoType("medium_shred", "medium", grain=140, mass_scale=0.95, damage_scale=0.9, penetration_scale=0.7,
             gravity_scale=1.0, effect="shred", effect_tags=["ballistic", "shred"], notes="shred subtype"),
    # --- heavy ---
    AmmoType("heavy_fmj", "heavy", grain=220, mass_scale=1.35, damage_scale=1.35, penetration_scale=1.1,
             gravity_scale=1.2, effect="kinetic", effect_tags=["ballistic", "fmj"], notes="heavy slug"),
    AmmoType("heavy_he", "heavy", grain=210, mass_scale=1.4, damage_scale=1.5, penetration_scale=0.6,
             gravity_scale=1.25, effect="explosive", effect_tags=["ballistic", "explosive", "he"],
             notes="HE shell — splash later"),
    # --- energy (hitscan) ---
    AmmoType("energy_bolt", "energy", grain=100, mass_scale=0.01, damage_scale=1.1, penetration_scale=1.2,
             gravity_scale=0.0, hitscan=True, effect="energy", effect_tags=["hitscan", "energy"],
             notes="default beam charge"),
    AmmoType("energy_pierce", "energy", grain=120, mass_scale=0.01, damage_scale=0.95, penetration_scale=1.8,
             gravity_scale=0.0, hitscan=True, effect="energy", effect_tags=["hitscan", "energy", "pierce"],
             notes="pierce charge — multi-hit later"),
]


def ammo_json() -> list[dict]:
    return [asdict(a) for a in AMMO]


def get_ammo(aid: str) -> AmmoType:
    for a in AMMO:
        if a.id == aid:
            return a
    return AMMO[0]


def ammo_for_caliber(caliber: str) -> list[AmmoType]:
    return [a for a in AMMO if a.caliber == caliber]
