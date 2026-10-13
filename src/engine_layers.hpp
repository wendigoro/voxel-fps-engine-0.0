#pragma once
// In-Engine Layering Coordinator: First-Person Gameplay alongside Planetary Destruction
// Bridges Astronomical Tier 0, Plate Zones Tier 1, Ocean Basins Tier 2, Structural Tier 3,
// and Micro-Grid Tier 4 (1mm voxels, player kinematics, ballistics, containers, and loot).
//
// Governed by RULES.md: Simulation is sole physical authority; view renders derived geometry without feedback.

#include "planetary/planetary_types.hpp"
#include "planetary/multi_planet.hpp"
#include "planetary/water_basins.hpp"
#include "planetary/clipmap_mesh.hpp"
#include "planetary/planetary_render.hpp"
#include "structural_matrix.hpp"
#include "loot_system.hpp"
#include "sim_world.hpp"

#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <memory>
#include <sstream>
#include <algorithm>
#include <iostream>

namespace layers {

// View modes allowing seamless coexistence of first-person gameplay and orbital views
enum class EngineViewMode : uint8_t {
    FirstPerson        = 0, // Canonical FPS ground-level view (player capsule, weapon viewmodel, local world)
    ThirdPerson        = 1, // Over-the-shoulder external character camera
    PlanetaryOrbital   = 2, // Space / orbital camera viewing cubed-sphere planets and collisions (10^7 - 10^8 m)
    LayeredComposite   = 3  // Ground-level FPS view with real-time celestial sky projections and orbital PiP monitor
};

inline const char* viewModeToString(EngineViewMode mode) {
    switch (mode) {
        case EngineViewMode::FirstPerson:      return "FirstPerson";
        case EngineViewMode::ThirdPerson:      return "ThirdPerson";
        case EngineViewMode::PlanetaryOrbital: return "PlanetaryOrbital";
        case EngineViewMode::LayeredComposite: return "LayeredComposite";
        default:                               return "Unknown";
    }
}

// World Container Entity placed in the local micro-grid / building
struct WorldContainer {
    uint32_t id = 0;
    loot::ContainerType type = loot::ContainerType::WeaponLocker;
    std::string name;
    int cx = 0, cy = 0, cz = 0; // Unit-cell anchor
    int rot = 0;                // Quarter-turns about +Y
    bool isOpened = false;
    uint64_t seed = 1337;
    std::vector<loot::LootItemEntry> rolledItems;
};

// Represents a celestial body or incandescent shard projected into the first-person sky dome
struct CelestialSkyProjection {
    uint32_t id = 0;
    std::string name;
    planetary::Vec3d bearingDir; // Unit direction vector from player eye towards celestial object
    float angularSizeRad = 0.035f;
    float temperatureK = 300.0f;
    bool isMolten = false;
    float r = 1.0f, g = 1.0f, b = 1.0f;
};

// Master Engine Layer Coordinator
class EngineLayerCoordinator {
public:
    EngineViewMode currentViewMode = EngineViewMode::FirstPerson;
    bool planetarySimActive = true;
    double planetaryTimeScale = 1.0; // Scaled time for astronomical orbits

    // Local seismic screen shake magnitude (applied to first-person camera during planetary impacts)
    float seismicShakeMagnitude = 0.0f;

    // Placed world containers in the active map
    std::vector<WorldContainer> containers;
    int hoveredContainerIndex = -1;

    // Projected celestial objects for the sky pass
    std::vector<CelestialSkyProjection> skyProjections;

    EngineLayerCoordinator() = default;

    // Initializes default warehouse world containers
    void initDefaultWarehouseContainers() {
        containers.clear();

        // 1. Weapon Locker inside armory bay
        WorldContainer locker;
        locker.id = 1;
        locker.type = loot::ContainerType::WeaponLocker;
        locker.name = "Armory Weapon Locker";
        locker.cx = 92; locker.cy = 3; locker.cz = 120; locker.rot = 0;
        locker.seed = 202601;
        containers.push_back(locker);

        // 2. Wall Safe inside supervisor office
        WorldContainer safe;
        safe.id = 2;
        safe.type = loot::ContainerType::WallSafe;
        safe.name = "Secure Wall Safe";
        safe.cx = 97; safe.cy = 3; safe.cz = 118; safe.rot = 1;
        safe.seed = 202602;
        containers.push_back(safe);

        // 3. Military Footlocker on loading bay slab
        WorldContainer footlocker;
        footlocker.id = 3;
        footlocker.type = loot::ContainerType::MilitaryFootlocker;
        footlocker.name = "Military Supply Footlocker";
        footlocker.cx = 104; footlocker.cy = 3; footlocker.cz = 122; footlocker.rot = 0;
        footlocker.seed = 202603;
        containers.push_back(footlocker);

        // 4. Medical Cabinet in triage area
        WorldContainer medCabinet;
        medCabinet.id = 4;
        medCabinet.type = loot::ContainerType::MedicalCabinet;
        medCabinet.name = "Field Medical Cabinet";
        medCabinet.cx = 88; medCabinet.cy = 3; medCabinet.cz = 125; medCabinet.rot = 2;
        medCabinet.seed = 202604;
        containers.push_back(medCabinet);
    }

    // Toggles between first-person and planetary orbital view
    void cycleViewMode() {
        switch (currentViewMode) {
            case EngineViewMode::FirstPerson:
                currentViewMode = EngineViewMode::PlanetaryOrbital;
                break;
            case EngineViewMode::PlanetaryOrbital:
                currentViewMode = EngineViewMode::LayeredComposite;
                break;
            case EngineViewMode::LayeredComposite:
            default:
                currentViewMode = EngineViewMode::FirstPerson;
                break;
        }
    }

    // Opens a hovered container, generating cascading loot drops
    bool openContainer(size_t index, std::vector<loot::LootItemEntry>& outDroppedItems) {
        if (index >= containers.size()) return false;
        WorldContainer& cont = containers[index];
        if (cont.isOpened) return false;

        cont.isOpened = true;
        cont.rolledItems = loot::openContainer(cont.type, cont.seed);
        outDroppedItems = cont.rolledItems;
        return true;
    }

    // Finds container under crosshair within reach (meters)
    int findContainerUnderCrosshair(const planetary::Vec3d& rayOrigin, const planetary::Vec3d& rayDir, double maxReachM = 2.5) {
        int bestIdx = -1;
        double bestDist = maxReachM;

        for (size_t i = 0; i < containers.size(); ++i) {
            const auto& c = containers[i];
            // Container center in world meters
            const planetary::Vec3d contCenter(c.cx * 0.001 + 0.0005, c.cy * 0.001 + 0.001, c.cz * 0.001 + 0.0005);
            const planetary::Vec3d toCont = contCenter - rayOrigin;
            const double projDist = toCont.dot(rayDir);
            if (projDist > 0.0 && projDist < bestDist) {
                const planetary::Vec3d perp = toCont - (rayDir * projDist);
                if (perp.length() < 0.0015) { // Within 1.5m radius
                    bestDist = projDist;
                    bestIdx = static_cast<int>(i);
                }
            }
        }
        hoveredContainerIndex = bestIdx;
        return bestIdx;
    }

    // Advances the multi-layered simulation step
    void step(double dtSeconds, planetary::MultiPlanetSystem& planetSys,
              structural::MatrixSystem& structuralMatrix, sim::World* world) {
        // 1. Advance Planetary Simulation if active
        if (planetarySimActive) {
            planetSys.step(dtSeconds * planetaryTimeScale);
        }

        // 2. Cross-Layer Seismic Coupling: Planetary Shock -> Local Building Zone
        // If an impact occurred on Earth-Prime (Body 1), calculate ground motion at warehouse
        if (!planetSys.bodies.empty() && planetSys.bodies[0].accumulatedDamageEnergyJ > 0.0) {
            const double energyJ = planetSys.bodies[0].accumulatedDamageEnergyJ;
            // Epicentral distance ~ 2,500 km (regional impact shockwave)
            constexpr double kEpicenterDistKm = 2500.0;
            // Attenuation: PGA (g) ~ (E / 1e24)^0.5 * (100 / distKm)^1.2
            const double pgaG = std::pow(energyJ / 1.0e24, 0.45) * std::pow(100.0 / kEpicenterDistKm, 1.2) * 0.15;
            const float clampedPga = static_cast<float>(std::clamp(pgaG, 0.0, 4.0));

            if (clampedPga > 0.05f) {
                seismicShakeMagnitude = std::min(1.0f, clampedPga * 0.25f);
                for (auto& zone : structuralMatrix.zones) {
                    planetary::applySeismicGroundMotionToBuilding(zone, clampedPga, static_cast<float>(dtSeconds));
                }

                // If ground motion is extreme (>0.8g), demolish structural members in local world
                if (clampedPga > 0.8f && world) {
                    for (int y = 2; y <= 40; y += 4) {
                        for (int z = 20; z <= 140; z += 12) {
                            for (int x = 20; x <= 170; x += 12) {
                                if (world->get(x, y, z) == sim::Block::Girder) {
                                    world->set(x, y, z, sim::Block::Air);
                                }
                            }
                        }
                    }
                    structuralMatrix.evaluateAll(*world);
                }
            }
        }

        // Dampen seismic camera shake over time
        seismicShakeMagnitude = std::max(0.0f, seismicShakeMagnitude - static_cast<float>(dtSeconds * 0.5));

        // 3. Update Planetary Celestial Sky Projections
        updateSkyProjections(planetSys);
    }

    // Projects orbital celestial bodies and molten fragments into player sky dome
    void updateSkyProjections(const planetary::MultiPlanetSystem& planetSys) {
        skyProjections.clear();
        if (planetSys.bodies.size() < 2) return;

        const planetary::Vec3d earthPos = planetSys.bodies[0].positionM;

        // Project intact celestial bodies (Luna, Impactor Protoplanet)
        for (size_t i = 1; i < planetSys.bodies.size(); ++i) {
            const auto& b = planetSys.bodies[i];
            const planetary::Vec3d toBody = b.positionM - earthPos;
            const double distM = toBody.length();
            if (distM < 1.0) continue;

            const planetary::Vec3d normBearing = toBody * (1.0 / distM);
            // In upper hemisphere / above local horizon
            CelestialSkyProjection proj;
            proj.id = b.id;
            proj.name = b.name;
            proj.bearingDir = normBearing;
            proj.angularSizeRad = static_cast<float>(std::clamp((b.radiusM * 2.0) / distM, 0.005, 0.25));
            proj.temperatureK = b.isDisrupted ? 2400.0f : 300.0f;
            proj.isMolten = b.isDisrupted;

            if (b.isDisrupted) {
                // Molten orange-red incandescent glow
                proj.r = 1.0f; proj.g = 0.45f; proj.b = 0.15f;
            } else {
                // Luna silver-white
                proj.r = 0.95f; proj.g = 0.95f; proj.b = 1.0f;
            }
            skyProjections.push_back(proj);

            // Project individual molten orbital fragments if body is disrupted
            if (b.isDisrupted && !b.sim.fragments.empty()) {
                for (size_t fi = 0; fi < std::min<size_t>(b.sim.fragments.size(), 16); ++fi) {
                    const auto& frag = b.sim.fragments[fi];
                    const planetary::Vec3d fragWorldPos = b.positionM + frag.positionM;
                    const planetary::Vec3d toFrag = fragWorldPos - earthPos;
                    const double fragDistM = toFrag.length();
                    if (fragDistM < 1.0) continue;

                    CelestialSkyProjection fragProj;
                    fragProj.id = static_cast<uint32_t>(1000 + fi);
                    fragProj.name = "Molten Fragment " + std::to_string(fi);
                    fragProj.bearingDir = toFrag * (1.0 / fragDistM);
                    fragProj.angularSizeRad = 0.008f;
                    fragProj.temperatureK = frag.temperatureK;
                    fragProj.isMolten = true;
                    fragProj.r = 1.0f; fragProj.g = 0.35f; fragProj.b = 0.1f;
                    skyProjections.push_back(fragProj);
                }
            }
        }
    }

    // JSON serialization for UI telemetry and Command Center synchronization
    std::string toJson() const {
        std::ostringstream ss;
        ss << "{\"viewMode\":\"" << viewModeToString(currentViewMode) << "\",";
        ss << "\"simActive\":" << (planetarySimActive ? "true" : "false") << ",";
        ss << "\"timeScale\":" << planetaryTimeScale << ",";
        ss << "\"seismicShake\":" << seismicShakeMagnitude << ",";
        ss << "\"hoveredContainer\":" << hoveredContainerIndex << ",";
        ss << "\"containers\":[";
        for (size_t i = 0; i < containers.size(); ++i) {
            const auto& c = containers[i];
            ss << "{\"id\":" << c.id << ",\"name\":\"" << c.name << "\",\"type\":\"" << loot::containerTypeToString(c.type) << "\",";
            ss << "\"pos\":[" << c.cx << "," << c.cy << "," << c.cz << "],\"opened\":" << (c.isOpened ? "true" : "false") << "}";
            if (i + 1 < containers.size()) ss << ",";
        }
        ss << "],\"skyProjections\":[";
        for (size_t i = 0; i < skyProjections.size(); ++i) {
            const auto& p = skyProjections[i];
            ss << "{\"id\":" << p.id << ",\"name\":\"" << p.name << "\",\"molten\":" << (p.isMolten ? "true" : "false") << ",";
            ss << "\"bearing\":[" << p.bearingDir.x << "," << p.bearingDir.y << "," << p.bearingDir.z << "],";
            ss << "\"size\":" << p.angularSizeRad << ",\"temp\":" << p.temperatureK << "}";
            if (i + 1 < skyProjections.size()) ss << ",";
        }
        ss << "]}";
        return ss.str();
    }
};

// =========================================================================
// Autonomous Self-Test Suite for Engine Layering (Dedicated Exit Code 17)
// =========================================================================
inline bool selfTest() {
    std::cout << "== layers::selfTest() ==" << std::endl;

    // Test 1: View mode cycles correctly
    {
        EngineLayerCoordinator coord;
        if (coord.currentViewMode != EngineViewMode::FirstPerson) {
            std::cerr << "FAIL: Default view mode is not FirstPerson" << std::endl;
            return false;
        }
        coord.cycleViewMode();
        if (coord.currentViewMode != EngineViewMode::PlanetaryOrbital) {
            std::cerr << "FAIL: Cycle view mode failed to reach PlanetaryOrbital" << std::endl;
            return false;
        }
        coord.cycleViewMode();
        if (coord.currentViewMode != EngineViewMode::LayeredComposite) {
            std::cerr << "FAIL: Cycle view mode failed to reach LayeredComposite" << std::endl;
            return false;
        }
        coord.cycleViewMode();
        if (coord.currentViewMode != EngineViewMode::FirstPerson) {
            std::cerr << "FAIL: Cycle view mode failed to return to FirstPerson" << std::endl;
            return false;
        }
    }

    // Test 2: World container placement and crosshair raycasting
    {
        EngineLayerCoordinator coord;
        coord.initDefaultWarehouseContainers();
        if (coord.containers.size() != 4) {
            std::cerr << "FAIL: Default container count unexpected: " << coord.containers.size() << std::endl;
            return false;
        }

        // Raycast from player aiming at Weapon Locker (cx=92, cy=3, cz=120)
        const planetary::Vec3d eye(0.092, 0.003, 0.122);
        const planetary::Vec3d dir(0.0, 0.0, -1.0);
        const int hit = coord.findContainerUnderCrosshair(eye, dir, 3.0);
        if (hit != 0) {
            std::cerr << "FAIL: Failed to detect weapon locker under crosshair, hit=" << hit << std::endl;
            return false;
        }
    }

    // Test 3: Opening container triggers cascading loot drop
    {
        EngineLayerCoordinator coord;
        coord.initDefaultWarehouseContainers();
        std::vector<loot::LootItemEntry> drops;
        const bool opened = coord.openContainer(0, drops);
        if (!opened || drops.empty()) {
            std::cerr << "FAIL: Opening weapon locker failed to produce drops" << std::endl;
            return false;
        }
        if (!coord.containers[0].isOpened) {
            std::cerr << "FAIL: Container isOpened flag not updated" << std::endl;
            return false;
        }
        // Attempting to reopen already-opened container should return false
        if (coord.openContainer(0, drops)) {
            std::cerr << "FAIL: Reopening already-opened container should be rejected" << std::endl;
            return false;
        }
    }

    // Test 4: Cross-layer seismic coupling from planetary impact to building matrix
    {
        EngineLayerCoordinator coord;
        planetary::MultiPlanetSystem planetSys;
        planetSys.initStandardEarthMoonImpactorSystem();
        structural::MatrixSystem structuralMatrix;

        // Create mock building zone
        structural::BuildingZone zone;
        zone.metrics.totalWalls = 100;
        zone.metrics.intactWalls = 100;
        zone.metrics.totalGirders = 50;
        zone.metrics.intactGirders = 50;
        zone.metrics.totalPillars = 20;
        zone.metrics.intactPillars = 20;
        zone.metrics.totalMembers = 170;
        zone.metrics.intactMembers = 170;
        zone.metrics.overallIntegrity = 1.0f;
        structuralMatrix.zones.push_back(zone);

        // Deposit high-energy planetary impact on Earth (Body 0)
        planetSys.bodies[0].accumulatedDamageEnergyJ = 5.0e30; // Planet-disrupting seismic impact

        coord.step(1.0, planetSys, structuralMatrix, nullptr);

        // Seismic shake should be positive
        if (coord.seismicShakeMagnitude <= 0.0f) {
            std::cerr << "FAIL: Planetary impact produced non-positive camera tremor" << std::endl;
            return false;
        }
        // Building walls in structural zone should have suffered seismic degradation
        if (structuralMatrix.zones[0].metrics.intactWalls >= 100) {
            std::cerr << "FAIL: Planetary seismic shock failed to degrade building walls" << std::endl;
            return false;
        }
    }

    // Test 5: Celestial sky projection of orbital bodies and molten shards
    {
        EngineLayerCoordinator coord;
        planetary::MultiPlanetSystem planetSys;
        planetSys.initStandardEarthMoonImpactorSystem();

        coord.updateSkyProjections(planetSys);
        // Expect at least Luna projected into sky
        if (coord.skyProjections.empty()) {
            std::cerr << "FAIL: No celestial bodies projected into sky" << std::endl;
            return false;
        }

        // Catastrophically disrupt Luna into fragments
        planetSys.disruptBody(planetSys.bodies[1], planetary::Vec3d(0, 1, 0), 1.0e22);
        coord.updateSkyProjections(planetSys);

        // Shards should be added to sky projections
        bool foundMolten = false;
        for (const auto& p : coord.skyProjections) {
            if (p.isMolten) { foundMolten = true; break; }
        }
        if (!foundMolten) {
            std::cerr << "FAIL: Disrupted celestial fragments not projected as molten sky objects" << std::endl;
            return false;
        }
    }

    std::cout << "layers::selfTest(): PASS (All 5 test vectors OK)" << std::endl;
    return true;
}

} // namespace layers
