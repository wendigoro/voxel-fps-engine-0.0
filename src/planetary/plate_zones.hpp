#pragma once
// In-Engine Planetary Destruction & Multi-Scale Simulation Engine
// Plate Zones: Tectonic Plates, Crust Thickness, Fault Dynamics, and MVT Data Stripping.
//
// Governed by RULES.md: Authoritative on the simulation side, zero view-state feedback.

#include "planetary_types.hpp"

#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <iostream>

namespace planetary {

// Plate Boundary Types
enum class BoundaryType : uint8_t {
    Convergent = 0, // Subduction zones, collision mountain orogeny
    Divergent  = 1, // Mid-ocean spreading ridges, continental rifts
    Transform  = 2  // Strike-slip lateral faults (e.g. San Andreas style)
};

inline const char* boundaryTypeToString(BoundaryType t) {
    switch (t) {
        case BoundaryType::Convergent: return "Convergent";
        case BoundaryType::Divergent:  return "Divergent";
        case BoundaryType::Transform:  return "Transform";
        default:                       return "Unknown";
    }
}

// Individual Tectonic Plate
struct TectonicPlate {
    uint8_t id = 0;
    std::string name;
    Vec3d centroid;                 // Normalized centroid direction on unit sphere
    Vec3d driftVelocityMps;         // Continental drift vector (m/s)
    float crustThicknessKm = 35.0f; // 5-10 km oceanic, 30-70 km continental
    float densityKgM3 = 2750.0f;    // 2700 granitic, 3000 basaltic
    CrustMaterial primaryMaterial = CrustMaterial::GraniteContinental;
    float accumulatedStressMPa = 0.0f; // Shear stress (MPa)
    float yieldStrengthMPa = 100.0f;   // Rupture threshold (~100 MPa)
    bool isRuptured = false;           // True if hypervelocity shock severed plate
    float moltenFraction = 0.0f;       // Fraction melted by impact / mantle plume
};

// Plate Boundary / Fault Line Segment
struct PlateBoundary {
    uint32_t id = 0;
    uint8_t plateA = 0;
    uint8_t plateB = 0;
    BoundaryType type = BoundaryType::Convergent;
    Vec3d midPoint;                // Normalized location on sphere
    float faultFriction = 0.6f;    // Byerlee's law friction coefficient (~0.6)
    float shearStressMPa = 0.0f;
    float slipDeficitM = 0.0f;
    bool isRuptured = false;
};

// MVT Data Stripper: Enforces the stripping contract from Scout/Dell PMTiles
struct MvtDataStripper {
    int roadsStripped = 0;
    int buildingsStripped = 0;
    int foliageStripped = 0;
    int biomesPreserved = 0;
    int platesPreserved = 0;
    int waterPreserved = 0;

    // Evaluates a feature layer name; returns true if feature should be kept, false if stripped
    bool filterLayer(const std::string& layerName, uint8_t zoom) {
        if (zoom <= 18) {
            // Macro zoom: Strict stripping
            if (layerName == "roads" || layerName == "transportation" || layerName == "highways") {
                roadsStripped++;
                return false;
            }
            if (layerName == "buildings" || layerName == "structures" || layerName == "addresses") {
                buildingsStripped++;
                return false;
            }
            if (layerName == "foliage" || layerName == "trees" || layerName == "pois" || layerName == "places") {
                foliageStripped++;
                return false;
            }
            if (layerName == "landuse" || layerName == "biomes" || layerName == "earth") {
                biomesPreserved++;
                return true;
            }
            if (layerName == "plates" || layerName == "faults" || layerName == "tectonics") {
                platesPreserved++;
                return true;
            }
            if (layerName == "water" || layerName == "oceans" || layerName == "seas") {
                waterPreserved++;
                return true;
            }
            return false;
        }
        // Micro zoom: Keep all
        return true;
    }
};

// Tectonic Plate Graph & Lithospheric Sim
class TectonicPlateGraph {
public:
    double planetRadius = kStandardEarthRadiusM;
    std::vector<TectonicPlate> plates;
    std::vector<PlateBoundary> boundaries;

    TectonicPlateGraph() {
        initStandardPlates();
    }

    explicit TectonicPlateGraph(double radius) : planetRadius(radius) {
        initStandardPlates();
    }

    void initStandardPlates() {
        plates.clear();
        boundaries.clear();

        // 8 Major Tectonic Plates (Centroids on normalized unit sphere)
        // 0: Pacific (Oceanic, Basalt)
        plates.push_back({ 0, "Pacific", Vec3d(0.0, -0.3, 0.95).normalized(), Vec3d(-1e-9, 0, 1e-9), 7.0f, 3000.0f, CrustMaterial::BasaltOceanic, 15.0f, 90.0f, false, 0.0f });
        // 1: North American (Continental, Granite)
        plates.push_back({ 1, "NorthAmerican", Vec3d(-0.6, 0.7, 0.38).normalized(), Vec3d(-5e-10, 0, -2e-10), 38.0f, 2700.0f, CrustMaterial::GraniteContinental, 22.0f, 110.0f, false, 0.0f });
        // 2: Eurasian (Continental, Granite)
        plates.push_back({ 2, "Eurasian", Vec3d(0.5, 0.75, 0.42).normalized(), Vec3d(3e-10, 0, -1e-10), 42.0f, 2720.0f, CrustMaterial::GraniteContinental, 30.0f, 120.0f, false, 0.0f });
        // 3: African (Continental, Granite)
        plates.push_back({ 3, "African", Vec3d(0.3, 0.1, -0.95).normalized(), Vec3d(2e-10, 0, 4e-10), 35.0f, 2750.0f, CrustMaterial::GraniteContinental, 18.0f, 100.0f, false, 0.0f });
        // 4: Antarctic (Continental / Ice, Granite)
        plates.push_back({ 4, "Antarctic", Vec3d(0.0, -0.98, 0.1).normalized(), Vec3d(0, 0, 1e-10), 33.0f, 2680.0f, CrustMaterial::IceShield, 8.0f, 85.0f, false, 0.0f });
        // 5: Indo-Australian (Continental / Oceanic)
        plates.push_back({ 5, "IndoAustralian", Vec3d(0.8, -0.35, 0.48).normalized(), Vec3d(1e-9, 0, -5e-10), 28.0f, 2820.0f, CrustMaterial::GraniteContinental, 25.0f, 105.0f, false, 0.0f });
        // 6: South American (Continental)
        plates.push_back({ 6, "SouthAmerican", Vec3d(-0.7, -0.25, -0.66).normalized(), Vec3d(-4e-10, 0, 0), 36.0f, 2710.0f, CrustMaterial::GraniteContinental, 20.0f, 100.0f, false, 0.0f });
        // 7: Nazca (Oceanic, Basalt)
        plates.push_back({ 7, "Nazca", Vec3d(-0.6, -0.2, 0.77).normalized(), Vec3d(1.2e-9, 0, 0), 6.5f, 3050.0f, CrustMaterial::BasaltOceanic, 35.0f, 80.0f, false, 0.0f });

        // Major Inter-Plate Boundaries
        boundaries.push_back({ 0, 0, 1, BoundaryType::Convergent, Vec3d(-0.3, 0.5, 0.8).normalized(), 0.6f, 45.0f, 0.5f, false });
        boundaries.push_back({ 1, 1, 2, BoundaryType::Divergent,  Vec3d(0.0, 0.85, 0.5).normalized(), 0.5f, 20.0f, 0.2f, false });
        boundaries.push_back({ 2, 6, 7, BoundaryType::Convergent, Vec3d(-0.7, -0.2, -0.1).normalized(),0.7f, 65.0f, 0.8f, false });
        boundaries.push_back({ 3, 0, 5, BoundaryType::Transform,  Vec3d(0.6, -0.4, 0.7).normalized(), 0.65f, 38.0f, 0.4f, false });
        boundaries.push_back({ 4, 3, 2, BoundaryType::Convergent, Vec3d(0.4, 0.45, -0.7).normalized(),0.6f, 32.0f, 0.3f, false });
    }

    // Resolves closest tectonic plate for a given directional unit vector on the sphere
    uint8_t getPlateAt(const Vec3d& unitDir) const {
        double maxDot = -2.0;
        uint8_t bestId = 0;
        for (const auto& p : plates) {
            const double d = unitDir.dot(p.centroid);
            if (d > maxDot) {
                maxDot = d;
                bestId = p.id;
            }
        }
        return bestId;
    }

    // Retrieves crust thickness at a 3D position
    float getCrustThicknessKm(const Vec3d& pos) const {
        const Vec3d dir = pos.normalized();
        const uint8_t pId = getPlateAt(dir);
        if (pId < plates.size()) {
            return plates[pId].crustThicknessKm;
        }
        return 30.0f;
    }

    // Simulates hypervelocity impact shockwave transmission through the lithosphere:
    // Calculates peak overpressure, increases shear stress along fault boundaries,
    // and flags ruptured plates and severed fault lines.
    void applySeismicImpactShock(const Vec3d& epicenterM, double impactEnergyJoules) {
        const Vec3d epicDir = epicenterM.normalized();
        // Seismic body wave velocity ~ 8 km/s
        // Peak shock pressure at distance r: P(r) = P_0 * (r / r_0)^-1.8
        for (auto& p : plates) {
            const double angDistRad = std::acos(std::clamp(epicDir.dot(p.centroid), -1.0, 1.0));
            const double distM = angDistRad * planetRadius;

            // Pressure attenuation in Megapascals
            // E.g. 10^26 J (Cretaceous-scale) -> hundreds of GPa at epicenter, tens of MPa globally
            const double peakPressureMPa = (impactEnergyJoules / 1e20) / std::max(1.0, std::pow(distM / 1000.0, 1.2));
            p.accumulatedStressMPa += static_cast<float>(peakPressureMPa);

            if (p.accumulatedStressMPa >= p.yieldStrengthMPa) {
                p.isRuptured = true;
            }
            if (peakPressureMPa > 50000.0) { // > 50 GPa -> thermal melting
                p.moltenFraction = std::clamp(p.moltenFraction + 0.5f, 0.0f, 1.0f);
                p.primaryMaterial = CrustMaterial::MoltenMagma;
            }
        }

        for (auto& b : boundaries) {
            const double angDistRad = std::acos(std::clamp(epicDir.dot(b.midPoint), -1.0, 1.0));
            const double distM = angDistRad * planetRadius;
            const double faultStressIncrease = (impactEnergyJoules / 1e20) / std::max(1.0, std::pow(distM / 1000.0, 1.1));
            b.shearStressMPa += static_cast<float>(faultStressIncrease);

            if (b.shearStressMPa > 80.0f) {
                b.isRuptured = true;
            }
        }
    }
};

} // namespace planetary
