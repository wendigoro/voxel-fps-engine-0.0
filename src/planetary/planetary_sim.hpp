#pragma once
// In-Engine Planetary Destruction & Multi-Scale Simulation Engine
// High-Energy Shockwave, Hugoniot EOS, Crater Excavation, Antipodal Focusing,
// Catastrophic Disruption, and Orbital SPH Fragment Simulation.
//
// Governed by RULES.md: Authoritative on the simulation side, zero view-state feedback.

#include "planetary_types.hpp"
#include "plate_zones.hpp"

#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <iostream>

namespace planetary {

// Hugoniot Equation of State Material Parameters
struct HugoniotParams {
    float c0Mps;    // Bulk sound speed (m/s)
    float sParam;   // Hugoniot slope parameter (dimensionless)
    float densityKgM3;
    float meltPressureGPa;  // Pressure threshold for complete melting
    float vaporPressureGPa; // Pressure threshold for vaporization
};

inline HugoniotParams getHugoniotParams(CrustMaterial mat) {
    switch (mat) {
        case CrustMaterial::GraniteContinental:
            return { 3680.0f, 1.24f, 2650.0f, 45.0f, 100.0f };
        case CrustMaterial::BasaltOceanic:
            return { 2600.0f, 1.48f, 2860.0f, 50.0f, 110.0f };
        case CrustMaterial::SilicateMantle:
            return { 4500.0f, 1.30f, 3300.0f, 65.0f, 140.0f };
        case CrustMaterial::MetallicCore:
            return { 3800.0f, 1.58f, 7870.0f, 150.0f, 300.0f };
        case CrustMaterial::WaterLiquid:
            return { 1480.0f, 1.75f, 1000.0f, 5.0f, 15.0f };
        default:
            return { 3000.0f, 1.35f, 2700.0f, 40.0f, 90.0f };
    }
}

// Calculates peak shock velocity and shock pressure from projectile particle velocity Up
inline void computeHugoniotShock(CrustMaterial mat, float particleVelocityMps,
                                 float& outShockSpeedMps, float& outPressureGPa) {
    const HugoniotParams hp = getHugoniotParams(mat);
    outShockSpeedMps = hp.c0Mps + hp.sParam * particleVelocityMps;
    // P = rho * Us * Up (Pascals -> GPa: divide by 1e9)
    outPressureGPa = (hp.densityKgM3 * outShockSpeedMps * particleVelocityMps) / 1.0e9f;
}

// Crater Scaling & Excavation Results
struct CraterDimensions {
    double transientDiameterM = 0.0;
    double transientDepthM = 0.0;
    double finalDiameterM = 0.0;
    double meltVolumeKm3 = 0.0;
    double vaporVolumeKm3 = 0.0;
    double ejectaMassKg = 0.0;
};

// Computes crater dimensions using Schmidt & Holsapple pi-group scaling
inline CraterDimensions calculateCraterScaling(double impactorRadiusM, double impactorVelocityMps,
                                               double impactorDensityKgM3 = 3000.0,
                                               double targetDensityKgM3 = 2700.0,
                                               double gravityMps2 = 9.81) {
    CraterDimensions dims;
    const double impactorMass = (4.0 / 3.0) * 3.14159265358979323846 * std::pow(impactorRadiusM, 3) * impactorDensityKgM3;
    const double kineticEnergy = 0.5 * impactorMass * impactorVelocityMps * impactorVelocityMps;

    // Transient diameter D_tc: Schmidt-Holsapple gravity regime
    const double dImp = impactorRadiusM * 2.0;
    dims.transientDiameterM = 1.161 * std::pow(impactorDensityKgM3 / targetDensityKgM3, 0.333)
                              * std::pow(dImp, 0.78)
                              * std::pow(impactorVelocityMps, 0.44)
                              * std::pow(gravityMps2, -0.22);

    dims.transientDepthM = dims.transientDiameterM / 3.0;
    // Final collapsed / complex crater diameter
    dims.finalDiameterM = 1.17 * std::pow(dims.transientDiameterM, 1.13) / std::pow(3200.0, 0.13);

    // Melt & vapor volumes (scaled to kinetic energy in Joules)
    dims.meltVolumeKm3 = (kineticEnergy / 1.0e21) * 250.0;   // ~250 km^3 per 10^21 J
    dims.vaporVolumeKm3 = (kineticEnergy / 1.0e21) * 45.0;   // ~45 km^3 per 10^21 J
    dims.ejectaMassKg = impactorMass * 250.0;
    return dims;
}

// Antipodal Focusing Mechanics
// Calculates ground acceleration and seismic amplification on the opposite hemisphere
struct AntipodalFocusResult {
    Vec3d antipodalPoint;
    double groundAccelerationG = 0.0; // In standard Earth g's
    float overpressureMPa = 0.0f;
    bool extensionalCrustBlowout = false;
};

inline AntipodalFocusResult computeAntipodalFocus(const Vec3d& impactEpicenter, double impactEnergyJoules,
                                                  double planetRadiusM = kStandardEarthRadiusM) {
    AntipodalFocusResult res;
    res.antipodalPoint = impactEpicenter.normalized() * -planetRadiusM;

    // Geometric focusing amplification factor: rays converge from all 360 azimuths
    constexpr double kFocusFactor = 18.5;
    // Base antipodal seismic acceleration without focusing
    const double baseG = (impactEnergyJoules / 1.0e24) * 0.12; // E.g. Chicxulub ~10^23 J gives ~0.01g
    res.groundAccelerationG = baseG * kFocusFactor;
    res.overpressureMPa = static_cast<float>(res.groundAccelerationG * 9.81 * 2700.0 * 1000.0 / 1e6); // ~MPa

    // If antipodal acceleration exceeds ~2.5g and overpressure exceeds crust tensile strength (~60 MPa)
    if (res.groundAccelerationG >= 2.5 && res.overpressureMPa >= 60.0f) {
        res.extensionalCrustBlowout = true;
    }
    return res;
}

// Planetary Fragment for Catastrophic Disruption (SPH / N-Body Orbital Physics)
struct PlanetaryFragment {
    uint32_t id = 0;
    Vec3d positionM;
    Vec3d velocityMps;
    double massKg = 0.0;
    double radiusM = 0.0;
    float temperatureK = 1800.0f; // Molten core/mantle default
    bool isMolten = true;
    CrustMaterial composition = CrustMaterial::SilicateMantle;
};

// Symplectic N-Body Orbital & Planetary Disruption Simulator
class PlanetaryDisruptionSim {
public:
    double planetRadius = kStandardEarthRadiusM;
    double planetMass = kStandardEarthMassKg;
    bool planetDisrupted = false;
    double totalKineticEnergyInputJ = 0.0;

    TectonicPlateGraph plates;
    std::vector<PlanetaryFragment> fragments;

    PlanetaryDisruptionSim() : plates(kStandardEarthRadiusM) {}
    explicit PlanetaryDisruptionSim(double radius, double mass)
        : planetRadius(radius), planetMass(mass), plates(radius) {}

    // Executes a hypervelocity impact on the planet
    void applyImpact(const Vec3d& impactDir, double impactorRadiusM, double impactorVelocityMps,
                     double impactorDensityKgM3 = 3000.0) {
        const double impVolume = (4.0 / 3.0) * 3.14159265358979323846 * std::pow(impactorRadiusM, 3);
        const double impMass = impVolume * impactorDensityKgM3;
        const double energyJ = 0.5 * impMass * impactorVelocityMps * impactorVelocityMps;
        totalKineticEnergyInputJ += energyJ;

        const Vec3d epicenter = impactDir.normalized() * planetRadius;

        // 1. Shock transmission through lithosphere
        plates.applySeismicImpactShock(epicenter, energyJ);

        // 2. Check Catastrophic Disruption Threshold
        if (willDisruptPlanet(totalKineticEnergyInputJ, planetMass, planetRadius)) {
            triggerCatastrophicDisruption(epicenter, impactDir.normalized() * impactorVelocityMps, impMass);
        }
    }

    // Catastrophically shatters the planetary core and mantle into orbital fragments
    void triggerCatastrophicDisruption(const Vec3d& impactPoint, const Vec3d& impactVel, double impactorMass) {
        planetDisrupted = true;
        fragments.clear();

        constexpr int kFragmentCount = 32;
        fragments.reserve(kFragmentCount);

        const double totalMass = planetMass + impactorMass;
        // Largest remnant retains ~30-40% of mass
        const double largestMass = totalMass * 0.35;
        const double remMassPerFrag = (totalMass - largestMass) / (kFragmentCount - 1);

        // 1. Primary core remnant
        PlanetaryFragment coreRemnant;
        coreRemnant.id = 0;
        coreRemnant.positionM = Vec3d(0, 0, 0);
        coreRemnant.velocityMps = impactVel * (impactorMass / totalMass) * 0.2;
        coreRemnant.massKg = largestMass;
        coreRemnant.radiusM = planetRadius * std::cbrt(largestMass / totalMass);
        coreRemnant.temperatureK = 4500.0f; // Core temperature
        coreRemnant.isMolten = true;
        coreRemnant.composition = CrustMaterial::MetallicCore;
        fragments.push_back(coreRemnant);

        // 2. Ejected mantle and crust shards
        for (int i = 1; i < kFragmentCount; ++i) {
            const double phi = 3.14159265358979323846 * (3.0 - std::sqrt(5.0)) * i; // Golden spiral
            const double y = 1.0 - (static_cast<double>(i) / (kFragmentCount - 1)) * 2.0;
            const double radiusAtY = std::sqrt(1.0 - y * y);
            const double x = std::cos(phi) * radiusAtY;
            const double z = std::sin(phi) * radiusAtY;

            const Vec3d shardDir = Vec3d(x, y, z).normalized();
            PlanetaryFragment shard;
            shard.id = static_cast<uint32_t>(i);
            // Spawned radially outward
            shard.positionM = shardDir * (planetRadius * (1.1 + 0.1 * (i % 5)));
            // Ejection velocity combines radial explosion + forward impact momentum
            const double ejectaSpeedMps = 6000.0 + (i % 7) * 2000.0;
            shard.velocityMps = (shardDir * ejectaSpeedMps) + (impactVel * 0.15);
            shard.massKg = remMassPerFrag;
            shard.radiusM = planetRadius * std::cbrt(remMassPerFrag / totalMass);
            shard.temperatureK = 2200.0f;
            shard.isMolten = true;
            shard.composition = (i % 3 == 0) ? CrustMaterial::GraniteContinental : CrustMaterial::SilicateMantle;

            fragments.push_back(shard);
        }
    }

    // Steps orbital physics for shattered fragments via Verlet N-Body integration
    void stepOrbitalDynamics(double dtSeconds) {
        if (!planetDisrupted || fragments.empty()) return;

        const size_t n = fragments.size();
        std::vector<Vec3d> accelerations(n, Vec3d(0, 0, 0));
        constexpr double kSofteningSq = 1.0e10; // 100 km softening to prevent singular poles

        // Compute mutual gravitational forces
        for (size_t i = 0; i < n; ++i) {
            for (size_t j = i + 1; j < n; ++j) {
                const Vec3d rVec = fragments[j].positionM - fragments[i].positionM;
                const double distSq = rVec.lengthSq() + kSofteningSq;
                const double invDist3 = 1.0 / (distSq * std::sqrt(distSq));

                const double fOverM_i = kGravitationalConst * fragments[j].massKg * invDist3;
                const double fOverM_j = kGravitationalConst * fragments[i].massKg * invDist3;

                accelerations[i] = accelerations[i] + (rVec * fOverM_i);
                accelerations[j] = accelerations[j] - (rVec * fOverM_j);
            }
        }

        // Symplectic Verlet position & velocity update
        for (size_t i = 0; i < n; ++i) {
            fragments[i].velocityMps = fragments[i].velocityMps + (accelerations[i] * dtSeconds);
            fragments[i].positionM = fragments[i].positionM + (fragments[i].velocityMps * dtSeconds);
        }
    }
};

} // namespace planetary
