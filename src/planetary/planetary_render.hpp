#pragma once
// In-Engine Planetary Destruction & Multi-Scale Simulation Engine
// View-Side Planetary Rendering Snapshot, Clipmap Mesh Nodes, and Seismic Coupling.
//
// Governed by RULES.md: View only reads what simulation sends; zero feedback to sim.

#include "planetary_types.hpp"
#include "planetary_sim.hpp"
#include "structural_matrix.hpp"

#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>

namespace planetary {

// View-Side Instanced Planetary Shard Mesh Proxy
struct ShardRenderProxy {
    uint32_t id = 0;
    Vec3d positionM;
    float radiusM = 0.0f;
    float temperatureK = 1800.0f;
    bool isMolten = true;
    uint8_t materialClass = 7; // SilicateMantle
};

// View-Side Shockwave Visual Ring
struct ShockwaveVisualRing {
    Vec3d epicenter;
    float currentRadiusKm = 0.0f;
    float maxRadiusKm = 0.0f;
    float intensity = 1.0f; // 0..1 fading over time
    bool isAntipodalConverging = false;
};

// Complete View-Side Snapshot for Planetary Visualization
// Sent from simulation to view without violating the authority boundary.
struct PlanetaryViewSnapshot {
    double planetRadiusM = kStandardEarthRadiusM;
    bool isDisrupted = false;
    float globalMeltFraction = 0.0f;

    // Active visual shockwaves
    std::vector<ShockwaveVisualRing> activeShockwaves;

    // Disrupted orbital shard instances
    std::vector<ShardRenderProxy> orbitalShards;

    // Atmospheric scattering parameters (Rayleigh / Mie)
    float atmosphereHeightKm = 100.0f;
    float atmosphereDensitySeaLevel = 1.225f;
    float rayleighScatteringR = 5.8e-6f;
    float rayleighScatteringG = 13.5e-6f;
    float rayleighScatteringB = 33.1e-6f;

    // Mantle thermal glow
    float mantleEmissiveIntensity = 2.5f;

    void clear() {
        activeShockwaves.clear();
        orbitalShards.clear();
        isDisrupted = false;
        globalMeltFraction = 0.0f;
    }
};

// Creates a view-side snapshot from simulation state
inline PlanetaryViewSnapshot extractPlanetarySnapshot(const PlanetaryDisruptionSim& sim) {
    PlanetaryViewSnapshot snap;
    snap.planetRadiusM = sim.planetRadius;
    snap.isDisrupted = sim.planetDisrupted;

    if (sim.planetDisrupted) {
        snap.orbitalShards.reserve(sim.fragments.size());
        for (const auto& f : sim.fragments) {
            ShardRenderProxy p;
            p.id = f.id;
            p.positionM = f.positionM;
            p.radiusM = static_cast<float>(f.radiusM);
            p.temperatureK = f.temperatureK;
            p.isMolten = f.isMolten;
            p.materialClass = static_cast<uint8_t>(f.composition);
            snap.orbitalShards.push_back(p);
        }
    }
    return snap;
}

// =========================================================================
// Tier 1 to Tier 3 Seismic Coupling: Ground Motion -> Structural Integrity
// =========================================================================
// Bridges planetary lithosphere shock (Tier 1) to the Structural Integrity Matrix
// (Tier 3, structural_matrix.hpp) for buildings in local zones.
inline void applySeismicGroundMotionToBuilding(structural::BuildingZone& zone, float pgaG, float durationS = 5.0f) {
    // Peak Ground Acceleration (PGA in g):
    // 0.1g - 0.2g: Moderate shaking, superficial cracks
    // 0.2g - 0.5g: Strong shaking, walls cracked, shear stress increases
    // 0.5g - 1.0g: Severe shaking, weak columns buckle
    // > 1.0g: Violent ground rupture, major structural collapse
    if (pgaG < 0.05f) return;

    // Calculate seismic damage factor based on PGA and shaking duration
    const float seismicImpulse = pgaG * (durationS / 5.0f);

    // If PGA exceeds 0.35g, structural members accumulate seismic stress:
    if (seismicImpulse > 0.35f) {
        const float damageFraction = std::clamp((seismicImpulse - 0.35f) * 0.4f, 0.0f, 0.85f);
        zone.metrics.intactWalls = static_cast<int>(zone.metrics.intactWalls * (1.0f - damageFraction));
        if (seismicImpulse > 0.8f) {
            zone.metrics.intactGirders = static_cast<int>(zone.metrics.intactGirders * (1.0f - damageFraction * 0.5f));
        }
        zone.metrics.intactMembers = zone.metrics.intactWalls + zone.metrics.intactGirders + zone.metrics.intactPillars;
        const float total = static_cast<float>(zone.metrics.totalMembers);
        zone.metrics.overallIntegrity = (total > 0.0f) ? (static_cast<float>(zone.metrics.intactMembers) / total) : 1.0f;
        zone.metrics.stressFactor = 1.0f - zone.metrics.overallIntegrity;
    }
}

} // namespace planetary
