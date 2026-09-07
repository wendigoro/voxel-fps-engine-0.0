"""Starter material table mirrored by src/materials.hpp."""

from __future__ import annotations

from dataclasses import asdict, dataclass


@dataclass(frozen=True)
class Material:
    id: str
    density: float
    weight: float
    fragility: float
    toughness: float
    damping: float


MATERIALS: dict[str, Material] = {
    "wood": Material("wood", density=0.70, weight=1.00, fragility=0.45, toughness=1.20, damping=0.35),
    "concrete": Material("concrete", density=2.40, weight=1.35, fragility=0.15, toughness=3.50, damping=0.55),
    "dirt": Material("dirt", density=1.20, weight=1.10, fragility=0.65, toughness=0.70, damping=0.40),
    "bush_leaves": Material(
        "bush_leaves", density=0.15, weight=0.35, fragility=0.95, toughness=0.15, damping=0.10
    ),
    "bush_branch": Material(
        "bush_branch", density=0.55, weight=0.80, fragility=0.55, toughness=0.85, damping=0.30
    ),
    "sheet_metal": Material(
        "sheet_metal", density=7.80, weight=0.55, fragility=0.35, toughness=2.10, damping=0.28
    ),
    "girder": Material("girder", density=7.85, weight=1.40, fragility=0.12, toughness=4.20, damping=0.50),
    "water": Material("water", density=1.00, weight=1.00, fragility=1.00, toughness=0.05, damping=0.05),
}

# Engine unit voxel edge length (1000x smaller than original 1.0 blocks).
VOXEL_SIZE = 0.001


def materials_json() -> list[dict]:
    return [asdict(m) for m in MATERIALS.values()]
