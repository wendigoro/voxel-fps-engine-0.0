#pragma once
// In-Engine Planetary Destruction & Multi-Scale Simulation Engine
// Tier 0: Astronomical Multi-Planet System & Catastrophic Disruption Mechanics.
// Simulates multiple celestial bodies (planets, moons, rogue protoplanets), N-body
// orbital dynamics, Roche tidal limit disruption, primary collisions, and secondary
// debris impacts capable of destroying 1+ planets in cascading chain reactions.
//
// Governed by RULES.md: Authoritative on the simulation side, zero view-state feedback.

#include "planetary_types.hpp"
#include "planetary_sim.hpp"
#include "water_basins.hpp"

#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <memory>
#include <sstream>
#include <algorithm>
#include <iostream>

namespace planetary {

// Celestial Body Representation in Astronomical Tier
struct CelestialBody {
    uint32_t id = 0;
    std::string name;
    Vec3d positionM;
    Vec3d velocityMps;
    double radiusM = kStandardEarthRadiusM;
    double massKg = kStandardEarthMassKg;
    double meanDensityKgM3 = kStandardEarthDensity;
    bool isDisrupted = false;
    double accumulatedDamageEnergyJ = 0.0;

    PlanetaryDisruptionSim sim;
    OceanBasinManager oceans;

    CelestialBody() : sim(kStandardEarthRadiusM, kStandardEarthMassKg), oceans(kStandardEarthRadiusM) {}
    CelestialBody(uint32_t id_, const std::string& name_, const Vec3d& pos, const Vec3d& vel,
                  double radius, double mass, double density)
        : id(id_), name(name_), positionM(pos), velocityMps(vel),
          radiusM(radius), massKg(mass), meanDensityKgM3(density),
          sim(radius, mass), oceans(radius) {}
};

// Multi-Planet System Manager
class MultiPlanetSystem {
public:
    std::vector<CelestialBody> bodies;
    double simulationTimeS = 0.0;
    int totalDisruptedBodiesCount = 0;

    MultiPlanetSystem() = default;

    // Resets system to a canonical Earth-Moon + Rogue Protoplanet setup
    void initStandardEarthMoonImpactorSystem() {
        bodies.clear();
        simulationTimeS = 0.0;
        totalDisruptedBodiesCount = 0;

        // 1. Earth-Prime (Primary Terrestrial Planet)
        CelestialBody earth(
            1, "Earth-Prime",
            Vec3d(0, 0, 0),
            Vec3d(0, 0, 0),
            kStandardEarthRadiusM,
            kStandardEarthMassKg,
            kStandardEarthDensity
        );
        bodies.push_back(earth);

        // 2. Luna (Secondary Lunar Body in Proximity Orbit)
        // Scaled orbital radius for simulation testing (e.g. 45,000 km to allow rapid interaction)
        constexpr double kMoonRadiusM = 1737400.0;
        constexpr double kMoonMassKg  = 7.342e22;
        constexpr double kMoonDensity = 3344.0;
        constexpr double kOrbitDistM  = 4.5e7; // 45,000 km
        // Circular orbital velocity: v = sqrt(G * M / r)
        const double orbitVelMps = std::sqrt(kGravitationalConst * kStandardEarthMassKg / kOrbitDistM);

        CelestialBody moon(
            2, "Luna",
            Vec3d(kOrbitDistM, 0, 0),
            Vec3d(0, orbitVelMps, 0),
            kMoonRadiusM,
            kMoonMassKg,
            kMoonDensity
        );
        bodies.push_back(moon);
    }

    // Calculates Roche Tidal Disruption Radius (meters) between two bodies
    // d_Roche = 2.44 * R_primary * (rho_primary / rho_secondary)^(1/3)
    static double computeRocheLimitM(double primaryRadiusM, double primaryDensityKgM3, double secondaryDensityKgM3) {
        if (secondaryDensityKgM3 <= 0.0) return 0.0;
        return 2.44 * primaryRadiusM * std::cbrt(primaryDensityKgM3 / secondaryDensityKgM3);
    }

    // Advances N-body orbital dynamics and handles collisions / disruption
    void step(double dtSeconds) {
        simulationTimeS += dtSeconds;
        const size_t n = bodies.size();
        if (n == 0) return;

        // 1. Compute mutual N-body gravitational accelerations for intact bodies
        std::vector<Vec3d> accelerations(n, Vec3d(0, 0, 0));
        constexpr double kSofteningSq = 1.0e12; // 1000 km softening to prevent singular poles

        for (size_t i = 0; i < n; ++i) {
            if (bodies[i].isDisrupted) continue;
            for (size_t j = i + 1; j < n; ++j) {
                if (bodies[j].isDisrupted) continue;

                const Vec3d rVec = bodies[j].positionM - bodies[i].positionM;
                const double distSq = rVec.lengthSq() + kSofteningSq;
                const double invDist3 = 1.0 / (distSq * std::sqrt(distSq));

                const double fOverM_i = kGravitationalConst * bodies[j].massKg * invDist3;
                const double fOverM_j = kGravitationalConst * bodies[i].massKg * invDist3;

                accelerations[i] = accelerations[i] + (rVec * fOverM_i);
                accelerations[j] = accelerations[j] - (rVec * fOverM_j);

                // Check Roche tidal disruption:
                const double distM = std::sqrt(rVec.lengthSq());
                const double rocheLimit_i_on_j = computeRocheLimitM(bodies[i].radiusM, bodies[i].meanDensityKgM3, bodies[j].meanDensityKgM3);
                if (distM < rocheLimit_i_on_j && bodies[i].massKg > bodies[j].massKg * 5.0) {
                    // Body j gets tidally disrupted by Body i's gravitational gradient
                    disruptBody(bodies[j], bodies[i].positionM - bodies[j].positionM, 1.0e30);
                }
            }
        }

        // 2. Symplectic Verlet update for intact bodies
        for (size_t i = 0; i < n; ++i) {
            if (!bodies[i].isDisrupted) {
                bodies[i].velocityMps = bodies[i].velocityMps + (accelerations[i] * dtSeconds);
                bodies[i].positionM = bodies[i].positionM + (bodies[i].velocityMps * dtSeconds);
            } else {
                // If body has shattered, update its internal fragment cluster
                bodies[i].sim.stepOrbitalDynamics(dtSeconds);
            }
            bodies[i].oceans.stepWaves(dtSeconds);
        }

        // 3. Collision detection between intact bodies
        for (size_t i = 0; i < n; ++i) {
            if (bodies[i].isDisrupted) continue;
            for (size_t j = i + 1; j < n; ++j) {
                if (bodies[j].isDisrupted) continue;

                const Vec3d relPos = bodies[j].positionM - bodies[i].positionM;
                const double distM = std::sqrt(relPos.lengthSq());
                const double contactDistM = bodies[i].radiusM + bodies[j].radiusM;

                if (distM <= contactDistM) {
                    // Physical collision!
                    resolveBodyCollision(bodies[i], bodies[j]);
                }
            }
        }

        // 4. Secondary debris impact checks: shattered fragments impacting surviving intact planets
        resolveSecondaryFragmentCollisions(dtSeconds);

        // Update count of disrupted bodies
        int disruptedCount = 0;
        for (const auto& b : bodies) {
            if (b.isDisrupted) ++disruptedCount;
        }
        totalDisruptedBodiesCount = disruptedCount;
    }

    // Catastrophically disrupts a celestial body
    void disruptBody(CelestialBody& body, const Vec3d& impactDir, double impactorMass) {
        if (body.isDisrupted) return;
        body.isDisrupted = true;
        const Vec3d impactVel = impactDir.normalized() * 35000.0;
        body.sim.triggerCatastrophicDisruption(body.positionM, impactVel, impactorMass);
    }

    // Resolves direct collision between two celestial bodies
    void resolveBodyCollision(CelestialBody& a, CelestialBody& b) {
        const Vec3d relVel = b.velocityMps - a.velocityMps;
        const double relSpeed = std::sqrt(relVel.lengthSq());
        const double reducedMass = (a.massKg * b.massKg) / (a.massKg + b.massKg);
        const double collisionEnergyJ = 0.5 * reducedMass * relSpeed * relSpeed;

        a.accumulatedDamageEnergyJ += collisionEnergyJ;
        b.accumulatedDamageEnergyJ += collisionEnergyJ;

        const Vec3d contactNorm = (b.positionM - a.positionM).normalized();

        // Check if Body A reaches Benz & Asphaug disruption threshold
        const bool disruptA = willDisruptPlanet(collisionEnergyJ, a.massKg, a.radiusM);
        // Check if Body B reaches Benz & Asphaug disruption threshold
        const bool disruptB = willDisruptPlanet(collisionEnergyJ, b.massKg, b.radiusM);

        if (disruptA) {
            disruptBody(a, contactNorm, b.massKg);
        } else {
            a.sim.applyImpact(contactNorm, b.radiusM, relSpeed, b.meanDensityKgM3);
            a.oceans.triggerOceanImpact(contactNorm * a.radiusM, collisionEnergyJ);
        }

        if (disruptB) {
            disruptBody(b, contactNorm * -1.0, a.massKg);
        } else {
            b.sim.applyImpact(contactNorm * -1.0, a.radiusM, relSpeed, a.meanDensityKgM3);
            b.oceans.triggerOceanImpact(contactNorm * -b.radiusM, collisionEnergyJ);
        }
    }

    // Detects shattered high-energy fragments hitting surviving celestial bodies
    // (Cascading chain reaction that takes out secondary planets!)
    void resolveSecondaryFragmentCollisions(double dtSeconds) {
        for (auto& sourceBody : bodies) {
            if (!sourceBody.isDisrupted || sourceBody.sim.fragments.empty()) continue;

            for (auto& targetBody : bodies) {
                if (targetBody.id == sourceBody.id || targetBody.isDisrupted) continue;

                // Check each fragment from sourceBody against targetBody's gravitational capture cross-section
                double secondaryEnergyDepositJ = 0.0;
                auto it = sourceBody.sim.fragments.begin();
                while (it != sourceBody.sim.fragments.end()) {
                    // Fragment position in world/barycentric coordinates
                    const Vec3d worldFragPos = sourceBody.positionM + it->positionM;
                    const Vec3d toTarget = worldFragPos - targetBody.positionM;
                    const double distM = std::sqrt(toTarget.lengthSq());

                    // Target radius + gravitational capture radius
                    if (distM <= targetBody.radiusM * 1.05) {
                        // Impact! Calculate kinetic energy
                        const Vec3d worldFragVel = sourceBody.velocityMps + it->velocityMps;
                        const Vec3d relVel = worldFragVel - targetBody.velocityMps;
                        const double speedMps = std::sqrt(relVel.lengthSq());
                        const double fragEk = 0.5 * it->massKg * speedMps * speedMps;

                        secondaryEnergyDepositJ += fragEk;
                        // Fragment is consumed / accreted into target
                        it = sourceBody.sim.fragments.erase(it);
                    } else {
                        ++it;
                    }
                }

                if (secondaryEnergyDepositJ > 0.0) {
                    targetBody.accumulatedDamageEnergyJ += secondaryEnergyDepositJ;
                    // Test if secondary bombardment triggers catastrophic disruption of targetBody!
                    if (willDisruptPlanet(targetBody.accumulatedDamageEnergyJ, targetBody.massKg, targetBody.radiusM)) {
                        disruptBody(targetBody, (sourceBody.positionM - targetBody.positionM).normalized(), sourceBody.massKg * 0.1);
                    }
                }
            }
        }
    }

    // Launches a rogue protoplanetary impactor onto a specific celestial body
    void launchImpactor(uint32_t targetBodyId, double impactorRadiusM, double impactorSpeedMps,
                        const Vec3d& approachDirection = Vec3d(-1, 0, 0)) {
        for (auto& b : bodies) {
            if (b.id == targetBodyId && !b.isDisrupted) {
                const double impVolume = (4.0 / 3.0) * 3.14159265358979323846 * std::pow(impactorRadiusM, 3);
                const double impMass = impVolume * 3300.0; // Silicate/iron density
                const Vec3d normApp = approachDirection.normalized();
                const Vec3d spawnPos = b.positionM - normApp * ((b.radiusM + impactorRadiusM) * 1.05);
                const Vec3d spawnVel = normApp * impactorSpeedMps;

                CelestialBody impactor(
                    static_cast<uint32_t>(100 + bodies.size()), "Impactor-Protoplanet",
                    spawnPos, spawnVel, impactorRadiusM, impMass, 3300.0
                );
                bodies.push_back(impactor);
                break;
            }
        }
    }

    // JSON serialization for telemetry and UI synchronization
    std::string toJson() const {
        std::ostringstream ss;
        ss << "{\"timeS\":" << simulationTimeS << ",\"disruptedCount\":" << totalDisruptedBodiesCount << ",\"bodies\":[";
        for (size_t i = 0; i < bodies.size(); ++i) {
            const auto& b = bodies[i];
            ss << "{\"id\":" << b.id << ",\"name\":\"" << b.name << "\",";
            ss << "\"pos\":[" << b.positionM.x << "," << b.positionM.y << "," << b.positionM.z << "],";
            ss << "\"vel\":[" << b.velocityMps.x << "," << b.velocityMps.y << "," << b.velocityMps.z << "],";
            ss << "\"radius\":" << b.radiusM << ",\"mass\":" << b.massKg << ",";
            ss << "\"disrupted\":" << (b.isDisrupted ? "true" : "false") << ",";
            ss << "\"fragmentCount\":" << b.sim.fragments.size() << "}";
            if (i + 1 < bodies.size()) ss << ",";
        }
        ss << "]}";
        return ss.str();
    }
};

} // namespace planetary
