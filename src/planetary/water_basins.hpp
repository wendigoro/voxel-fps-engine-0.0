#pragma once
// In-Engine Planetary Destruction & Multi-Scale Simulation Engine
// Tier 2: Bodies of Water & Ocean Basins Dynamics
// Bathymetry depth grid, thermal impact steam cavitation, spherical shallow water
// wave (mega-tsunami) propagation, antipodal focusing, and coastal structural coupling.
//
// Governed by RULES.md: Authoritative on the simulation side, zero view-state feedback.

#include "planetary_types.hpp"
#include "structural_matrix.hpp"

#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <iostream>

namespace planetary {

// Ocean Basin Physical Constants
inline constexpr double kStandardOceanDepthM    = 3682.0;    // Mean Earth ocean depth
inline constexpr double kMaxOceanDepthM         = 11000.0;   // Mariana Trench / Challenger Deep
inline constexpr double kWaterDensityKgM3       = 1000.0;    // Standard seawater density (approx kg/m^3)
inline constexpr double kWaterVapEnthalpyJPerKg = 2.26e6;    // Latent heat of vaporization for water (J/kg)
inline constexpr double kWaterSoundSpeedMps     = 1480.0;    // Acoustic speed in water

// Ocean Basin Cell Representation for Cubed-Sphere Quadtree
struct OceanCell {
    float waterDepthM = 0.0f;       // 0 for dry land / continental crust
    float waveElevationM = 0.0f;    // Surface wave displacement relative to sea level
    float velocityMps = 0.0f;       // Horizontal fluid flow speed
    bool isCavitatedSteam = false;  // True if water instantaneously vaporized by impact
};

// Represents a Mega-Tsunami Wave Front propagating across the planetary sphere
struct TsunamiWaveFront {
    Vec3d epicenter;
    double originTimeS = 0.0;
    double currentDistanceM = 0.0;
    double initialAmplitudeM = 500.0;  // Initial cavity collapse wave height
    double currentAmplitudeM = 500.0;
    double propagationSpeedMps = 190.0; // c = sqrt(g * d)
    bool hasReachedAntipode = false;
};

// Tier 2 Ocean Basins & Spherical Shallow Water Dynamics Manager
class OceanBasinManager {
public:
    double planetRadiusM = kStandardEarthRadiusM;
    double meanWaterDepthM = kStandardOceanDepthM;
    std::vector<TsunamiWaveFront> activeWaves;

    explicit OceanBasinManager(double radius = kStandardEarthRadiusM, double depth = kStandardOceanDepthM)
        : planetRadiusM(radius), meanWaterDepthM(depth) {}

    // Computes phase speed for shallow water gravity waves: c = sqrt(g * d)
    static double computeShallowWaterWaveSpeed(double waterDepthM, double gravityMps2 = 9.81) {
        if (waterDepthM <= 0.0) return 0.0;
        return std::sqrt(gravityMps2 * waterDepthM);
    }

    // Calculates instantaneous steam cavitation volume excavated by hypervelocity impact in ocean
    static double computeSteamCavitationVolumeM3(double impactEnergyJoules, double oceanDepthM = kStandardOceanDepthM) {
        // ~15% of total impact kinetic energy couples into thermal shock vaporization of water
        const double thermalEnergyWaterJ = impactEnergyJoules * 0.15;
        // Mass of water vaporized = Q_thermal / (delta_H_vap + c_p * delta_T)
        // Approx 2.6e6 J per kg of seawater heating to 100C + boiling
        const double vaporMassKg = thermalEnergyWaterJ / (2.6e6);
        const double vaporVolWaterEquivalentM3 = vaporMassKg / kWaterDensityKgM3;
        return vaporVolWaterEquivalentM3;
    }

    // Triggers ocean impact: cavitates water volume, opens transient crater in basalt floor,
    // and launches spherical mega-tsunami wave front
    void triggerOceanImpact(const Vec3d& impactPoint, double impactEnergyJoules) {
        const double waveSpeed = computeShallowWaterWaveSpeed(meanWaterDepthM);
        const double cavVolM3 = computeSteamCavitationVolumeM3(impactEnergyJoules, meanWaterDepthM);

        // Initial wave amplitude scales with cavitation cavity radius
        const double cavityRadiusM = std::cbrt((3.0 * cavVolM3) / (2.0 * 3.14159265358979323846));
        const double initialAmpM = std::clamp(cavityRadiusM * 0.25, 10.0, 3000.0);

        TsunamiWaveFront wave;
        wave.epicenter = impactPoint.normalized() * planetRadiusM;
        wave.originTimeS = 0.0;
        wave.currentDistanceM = cavityRadiusM;
        wave.initialAmplitudeM = initialAmpM;
        wave.currentAmplitudeM = initialAmpM;
        wave.propagationSpeedMps = waveSpeed;
        wave.hasReachedAntipode = false;

        activeWaves.push_back(wave);
    }

    // Advances spherical wave propagation by dtSeconds
    void stepWaves(double dtSeconds) {
        const double halfCircumferenceM = 3.14159265358979323846 * planetRadiusM;

        for (auto& wave : activeWaves) {
            wave.currentDistanceM += wave.propagationSpeedMps * dtSeconds;
            if (wave.currentDistanceM >= halfCircumferenceM) {
                wave.hasReachedAntipode = true;
                wave.currentDistanceM = halfCircumferenceM;
            }

            // Angular distance theta from epicenter in radians (0 to PI)
            const double theta = wave.currentDistanceM / planetRadiusM;
            // Spherical geometric dissipation: amplitude scales as 1 / sqrt(sin(theta))
            // Near equator (theta ~ PI/2), waves spread over max circumference.
            // As theta approaches PI (antipode), waves refocus and amplify!
            constexpr double kInitialTheta = 0.02; // Initial cavity radius angle
            const double sinTheta = std::max(0.015, std::sin(theta));
            const double sinInitial = std::sin(kInitialTheta);

            // Damping factor over time due to bottom friction
            const double frictionDamping = std::exp(-0.000005 * wave.currentDistanceM);

            wave.currentAmplitudeM = wave.initialAmplitudeM * std::sqrt(sinInitial / sinTheta) * frictionDamping;

            // Antipodal focusing boost if theta > 3.0 rad (~172 degrees)
            if (theta > 3.0) {
                const double focusFactor = 1.0 + (theta - 3.0) * 8.0;
                wave.currentAmplitudeM *= focusFactor;
            }
        }

        // Remove waves that have fully dissipated or settled after antipodal bounce
        activeWaves.erase(
            std::remove_if(activeWaves.begin(), activeWaves.end(),
                [](const TsunamiWaveFront& w) { return w.hasReachedAntipode && w.currentAmplitudeM < 1.0; }),
            activeWaves.end()
        );
    }

    // Evaluates tsunami wave height at an arbitrary point on the planetary surface
    double evaluateWaveHeightAt(const Vec3d& point) const {
        if (activeWaves.empty()) return 0.0;

        const Vec3d normP = point.normalized();
        double maxWaveHeight = 0.0;

        for (const auto& wave : activeWaves) {
            const Vec3d normE = wave.epicenter.normalized();
            const double dot = std::clamp(normP.dot(normE), -1.0, 1.0);
            const double angularDistRad = std::acos(dot);
            const double arcDistanceM = angularDistRad * planetRadiusM;

            // Wave front has a physical crest width (~50-100 km)
            constexpr double kCrestWidthM = 80000.0;
            const double distDiff = std::abs(arcDistanceM - wave.currentDistanceM);
            if (distDiff < kCrestWidthM) {
                const double phase = (kCrestWidthM - distDiff) / kCrestWidthM;
                const double h = wave.currentAmplitudeM * phase;
                if (h > maxWaveHeight) {
                    maxWaveHeight = h;
                }
            }
        }
        return maxWaveHeight;
    }

    // Calculates coastal run-up height when a tsunami wave strikes a continental shelf / shoreline
    // Synolakis (1987) law: R = 2.8 * H * (cot(beta))^0.2
    static double computeCoastalRunUpHeight(double deepWaterWaveHeightM, double shelfSlopeDeg = 2.5) {
        if (deepWaterWaveHeightM <= 0.0) return 0.0;
        const double slopeRad = shelfSlopeDeg * (3.14159265358979323846 / 180.0);
        const double cotBeta = 1.0 / std::tan(slopeRad);
        const double runUp = 2.8 * deepWaterWaveHeightM * std::pow(cotBeta, 0.2);
        return runUp;
    }

    // Hydrostatic + Hydrodynamic Surge Pressure (Pascals) exerted on coastal structures
    // P_surge = 0.5 * rho_w * g * H_runup^2 / H_base + rho_w * v^2
    static double computeSurgePressurePascals(double runUpHeightM, double surgeVelocityMps = 12.0) {
        if (runUpHeightM <= 0.0) return 0.0;
        // Hydrostatic head: rho * g * H
        const double pStatic = kWaterDensityKgM3 * 9.81 * runUpHeightM;
        // Hydrodynamic stagnation pressure: 0.5 * rho * v^2
        const double pDynamic = 0.5 * kWaterDensityKgM3 * surgeVelocityMps * surgeVelocityMps;
        return pStatic + pDynamic;
    }

    // =========================================================================
    // Tier 2 to Tier 3 Coupling: Mega-Tsunami Inundation -> Building Collapse
    // =========================================================================
    // When a tsunami wave front hits a coastal zone containing structural buildings,
    // hydrostatic surge pressure buckles exterior walls and destabilizes pillars.
    static void applyTsunamiSurgeToBuilding(structural::BuildingZone& zone, double runUpHeightM) {
        if (runUpHeightM < 2.0) return; // Below 2m: minor flooding, structural frame intact

        const double surgePressurePa = computeSurgePressurePascals(runUpHeightM);
        const double surgePressureKPa = surgePressurePa / 1000.0; // kPa

        // Concrete / masonry walls typically fail in shear/bending at 20-50 kPa
        if (surgePressureKPa >= 25.0) {
            const float wallDamageFrac = std::clamp(static_cast<float>((surgePressureKPa - 25.0) / 100.0), 0.0f, 0.95f);
            zone.metrics.intactWalls = static_cast<int>(zone.metrics.intactWalls * (1.0f - wallDamageFrac));

            // Heavy inundation (>10m runup / >150 kPa) washes out ground pillars and girders
            if (surgePressureKPa >= 150.0) {
                const float pillarDamageFrac = std::clamp(static_cast<float>((surgePressureKPa - 150.0) / 200.0), 0.0f, 0.70f);
                zone.metrics.intactPillars = static_cast<int>(zone.metrics.intactPillars * (1.0f - pillarDamageFrac));
                zone.metrics.intactGirders = static_cast<int>(zone.metrics.intactGirders * (1.0f - pillarDamageFrac * 0.5f));
            }

            zone.metrics.intactMembers = zone.metrics.intactWalls + zone.metrics.intactGirders + zone.metrics.intactPillars;
            const float total = static_cast<float>(zone.metrics.totalMembers);
            zone.metrics.overallIntegrity = (total > 0.0f) ? (static_cast<float>(zone.metrics.intactMembers) / total) : 1.0f;
            zone.metrics.stressFactor = 1.0f - zone.metrics.overallIntegrity;
        }
    }
};

} // namespace planetary
