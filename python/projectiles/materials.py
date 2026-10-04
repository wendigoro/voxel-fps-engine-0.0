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
    "plexiglass": Material(
        "plexiglass", density=1.20, weight=0.40, fragility=0.00, toughness=9999.0, damping=0.95
    ),
    "carbon_fiber": Material(
        "carbon_fiber", density=1.75, weight=0.45, fragility=0.25, toughness=2.80, damping=0.22
    ),
    "treated_wood": Material(
        "treated_wood", density=0.75, weight=0.95, fragility=0.35, toughness=1.50, damping=0.42
    ),
    # Ground materials: generated terrain (src/terrain.hpp) and painted ground.
    "sand": Material("sand", density=1.60, weight=1.60, fragility=0.80, toughness=0.40, damping=0.45),
    "grass": Material("grass", density=0.50, weight=0.70, fragility=0.95, toughness=0.10, damping=0.20),
    "snow": Material("snow", density=0.30, weight=0.40, fragility=0.99, toughness=0.06, damping=0.30),
    "asphalt": Material("asphalt", density=2.30, weight=1.30, fragility=0.12, toughness=2.20, damping=0.50),
}

# Engine unit voxel edge length (1000x smaller than original 1.0 blocks).
# This is the single Python declaration. C++ reads it from src/materials.hpp
# (kVoxelSize); the Java painter has its own copy in VoxelGrid.java.
# scripts/check_constants.py asserts all of them still agree.
VOXEL_SIZE = 0.001

# Scale applied to density*weight*volume to reach engine mass units.
# MUST match the 1.0e6f literal in voxelMass() in src/materials.hpp.
# Getting this wrong scales every destruction threshold in the engine.
MASS_SCALE = 1.0e6


def voxel_mass(m: Material) -> float:
    """Canonical voxel mass. Mirrors voxelMass() in src/materials.hpp."""
    return m.density * m.weight * (VOXEL_SIZE**3) * MASS_SCALE


def break_threshold(m: Material) -> float:
    """Canonical break threshold. Mirrors breakEnergyThreshold() in src/materials.hpp."""
    return m.toughness * voxel_mass(m) / max(0.05, m.fragility)


def materials_json() -> list[dict]:
    return [asdict(m) for m in MATERIALS.values()]
