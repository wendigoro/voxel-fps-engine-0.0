#pragma once
// Material qualities used by C++ destruction resolution.
// IDs must stay in sync with python/projectiles/materials.py

#include <cmath>
#include <cstdint>
#include <string>

enum class MaterialId : uint8_t {
    Air = 0,
    Wood,
    Concrete,
    Dirt,
    BushLeaves,
    BushBranch,
    SheetMetal,
    Girder,
    Water,
    Plexiglass,
    CarbonFiber,
    TreatedWood,
    Count
};

struct MaterialProps {
    const char* name;
    float density;    // relative kg/m^3 scale (normalized, not SI exact)
    float weight;     // per-voxel mass scale multiplier
    float fragility;  // 0..1 higher = shatters easier
    float toughness;  // energy threshold scale
    float damping;    // residual energy absorbed on hit (0..1)
};

// Must match engine VOXEL_SIZE (1000x smaller than original 1.0 unit blocks).
inline constexpr float kVoxelSize = 0.001f;
inline constexpr float kVoxelVolume = kVoxelSize * kVoxelSize * kVoxelSize;

inline const MaterialProps& materialProps(MaterialId id) {
    static const MaterialProps kTable[] = {
        // name          density  weight  fragility toughness damping
        {"air",            0.001f, 0.001f, 1.00f,     0.01f,    0.00f},
        {"wood",           0.70f,  1.00f,  0.45f,     1.20f,    0.35f},
        {"concrete",       2.40f,  1.35f,  0.15f,     3.50f,    0.55f},
        {"dirt",           1.20f,  1.10f,  0.65f,     0.70f,    0.40f},
        {"bush_leaves",    0.15f,  0.35f,  0.95f,     0.15f,    0.10f},
        {"bush_branch",    0.55f,  0.80f,  0.55f,     0.85f,    0.30f},
        {"sheet_metal",    7.80f,  0.55f,  0.35f,     2.10f,    0.28f},
        {"girder",         7.85f,  1.40f,  0.12f,     4.20f,    0.50f},
        {"water",          1.00f,  1.00f,  1.00f,     0.05f,    0.05f},
        {"plexiglass",     1.18f,  1.00f,  0.05f,    99.00f,    0.20f},
        {"carbon_fiber",   1.75f,  0.70f,  0.22f,     3.80f,    0.32f},
        {"treated_wood",   0.78f,  1.05f,  0.32f,     1.70f,    0.38f},
    };
    const auto idx = static_cast<uint8_t>(id);
    if (idx >= static_cast<uint8_t>(MaterialId::Count)) return kTable[0];
    return kTable[idx];
}

inline float voxelMass(MaterialId id) {
    const auto& m = materialProps(id);
    // Mass from unit-cube volume at engine voxel scale.
    return m.density * m.weight * kVoxelVolume * 1.0e6f;
}

inline float breakEnergyThreshold(MaterialId id) {
    const auto& m = materialProps(id);
    const float frag = std::max(0.05f, m.fragility);
    return m.toughness * voxelMass(id) / frag;
}

inline MaterialId materialFromName(const std::string& name) {
    if (name == "wood") return MaterialId::Wood;
    if (name == "concrete") return MaterialId::Concrete;
    if (name == "dirt") return MaterialId::Dirt;
    if (name == "bush_leaves") return MaterialId::BushLeaves;
    if (name == "bush_branch") return MaterialId::BushBranch;
    if (name == "sheet_metal") return MaterialId::SheetMetal;
    if (name == "girder") return MaterialId::Girder;
    if (name == "water") return MaterialId::Water;
    if (name == "plexiglass") return MaterialId::Plexiglass;
    if (name == "carbon_fiber") return MaterialId::CarbonFiber;
    if (name == "treated_wood") return MaterialId::TreatedWood;
    return MaterialId::Air;
}
