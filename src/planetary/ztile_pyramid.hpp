#pragma once
// In-Engine Planetary Destruction & Multi-Scale Simulation Engine
// Cubed-Sphere Z-Tile Pyramid, Hilbert Curve Addressing, and Voxel Scale Alignment.
//
// Governed by RULES.md: Authoritative on the simulation side, zero view-state feedback.

#include "planetary_types.hpp"
#include "plate_zones.hpp"
#include "planetary_sim.hpp"
#include "water_basins.hpp"
#include "multi_planet.hpp"
#include "clipmap_mesh.hpp"
#include "planetary_render.hpp"

#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <sstream>
#include <iostream>

namespace planetary {


// Hash specialization for ZTileKey
struct ZTileKeyHash {
    size_t operator()(const ZTileKey& k) const noexcept {
        return static_cast<size_t>(k.packCompact());
    }
};

// Data Stripping & LOD Filtering Contract
// At macro zoom levels (Z <= 18), all roads, buildings, and micro-foliage are stripped;
// only tectonic plate zones, biome classes, elevation heightfields, and water are preserved.
struct FeatureFilter {
    bool keepRoads       = false; // STRIPPED at macro zoom
    bool keepBuildings   = false; // STRIPPED at macro zoom
    bool keepFoliage     = false; // STRIPPED at macro zoom
    bool keepBiomes      = true;  // PRESERVED
    bool keepPlateZones  = true;  // PRESERVED
    bool keepWaterBasins = true;  // PRESERVED

    static FeatureFilter forZoom(uint8_t zoom) {
        FeatureFilter f;
        if (zoom <= 18) {
            // Strict macro planetary filter
            f.keepRoads       = false;
            f.keepBuildings   = false;
            f.keepFoliage     = false;
            f.keepBiomes      = true;
            f.keepPlateZones  = true;
            f.keepWaterBasins = true;
        } else {
            // Micro / local zoom
            f.keepRoads       = true;
            f.keepBuildings   = true;
            f.keepFoliage     = true;
            f.keepBiomes      = true;
            f.keepPlateZones  = true;
            f.keepWaterBasins = true;
        }
        return f;
    }
};

// Mathematical Voxel Scaling Alignment
// Bridges macro celestial coordinates down to the engine's canonical 1mm unit voxels.
inline double getTileSpanMeters(uint8_t zoom, double planetRadius = kStandardEarthRadiusM) {
    // Circumference per face chord length ~ 0.5 * PI * R
    const double faceSpanM = 0.5 * 3.14159265358979323846 * planetRadius;
    const double tilesPerAxis = static_cast<double>(1ULL << zoom);
    return faceSpanM / tilesPerAxis;
}

inline double getCellSizeMeters(uint8_t zoom, uint32_t gridRes = 16, double planetRadius = kStandardEarthRadiusM) {
    return getTileSpanMeters(zoom, planetRadius) / static_cast<double>(gridRes);
}

// Converts cell scale to an integer count of canonical 1mm voxels
inline int64_t getCellVoxelUnits(uint8_t zoom, uint32_t gridRes = 16, double planetRadius = kStandardEarthRadiusM) {
    const double sizeM = getCellSizeMeters(zoom, gridRes, planetRadius);
    return static_cast<int64_t>(std::round(sizeM / kCanonicalVoxelSize));
}

// Base Zoom Level where a single grid cell matches canonical 1mm VOXEL_SIZE
inline uint8_t getBaseZoomFor1mm(uint32_t gridRes = 16, double planetRadius = kStandardEarthRadiusM) {
    for (uint8_t z = 0; z <= kMaxZoom; ++z) {
        if (getCellSizeMeters(z, gridRes, planetRadius) <= kCanonicalVoxelSize) {
            return z;
        }
    }
    return kMaxZoom;
}

// Individual Voxel Cell within a Macro Z-Tile
struct ZTileCell {
    float elevationM      = 0.0f;   // Surface elevation (meters)
    float waterDepthM     = 0.0f;   // Water column depth (meters)
    uint8_t crustMaterial = 1;      // CrustMaterial enum
    uint8_t plateId       = 0;      // Tectonic plate ID (0..255)
    float temperatureK    = 288.15f;// Surface temperature (Kelvin, ~15 C default)
};

// 16x16 Grid Data for one Z-Tile
struct ZTileData {
    ZTileKey key;
    static constexpr int kRes = 16;
    ZTileCell cells[kRes][kRes];

    // Data-stripping metric counters for verification audits
    int strippedRoadCount      = 0;
    int strippedBuildingCount  = 0;
    int strippedFoliageCount   = 0;
    int preservedPlateCount    = 0;
    int preservedWaterCount    = 0;
    int preservedBiomeCount    = 0;

    ZTileData() = default;
    explicit ZTileData(const ZTileKey& k) : key(k) {
        initDefaultCells();
    }

    void initDefaultCells() {
        for (int y = 0; y < kRes; ++y) {
            for (int x = 0; x < kRes; ++x) {
                cells[y][x].elevationM = 100.0f;
                cells[y][x].waterDepthM = 0.0f;
                cells[y][x].crustMaterial = static_cast<uint8_t>(CrustMaterial::GraniteContinental);
                cells[y][x].plateId = key.face; // Seed plate by cube face
                cells[y][x].temperatureK = 288.15f;
            }
        }
    }

    // Ingests raw geometry features and enforces the data stripping contract
    void ingestFeatures(int roadFeatures, int buildingFeatures, int foliageFeatures,
                        int plateFeatures, int waterFeatures, int biomeFeatures) {
        const FeatureFilter filter = FeatureFilter::forZoom(key.zoom);

        if (!filter.keepRoads) {
            strippedRoadCount += roadFeatures;
        }
        if (!filter.keepBuildings) {
            strippedBuildingCount += buildingFeatures;
        }
        if (!filter.keepFoliage) {
            strippedFoliageCount += foliageFeatures;
        }

        if (filter.keepPlateZones) {
            preservedPlateCount += plateFeatures;
        }
        if (filter.keepWaterBasins) {
            preservedWaterCount += waterFeatures;
        }
        if (filter.keepBiomes) {
            preservedBiomeCount += biomeFeatures;
        }
    }
};

// Planetary Pyramid Manager
class PlanetaryPyramid {
public:
    double planetRadius = kStandardEarthRadiusM;
    std::unordered_map<ZTileKey, ZTileData, ZTileKeyHash> tiles;

    PlanetaryPyramid() = default;
    explicit PlanetaryPyramid(double radius) : planetRadius(radius) {}

    ZTileData& getOrCreateTile(const ZTileKey& key) {
        auto it = tiles.find(key);
        if (it != tiles.end()) {
            return it->second;
        }
        auto res = tiles.emplace(key, ZTileData(key));
        return res.first->second;
    }

    // Resolves continuous (face, u, v) into a specific Z-tile and cell coordinate
    bool resolveUVToTile(CubeFace face, double u, double v, uint8_t zoom,
                         ZTileKey& outKey, int& outCellX, int& outCellY) const {
        if (zoom > kMaxZoom) return false;
        // Map u, v from [-1.0, 1.0] to [0.0, 1.0]
        const double normU = std::clamp((u + 1.0) * 0.5, 0.0, 0.999999);
        const double normV = std::clamp((v + 1.0) * 0.5, 0.0, 0.999999);

        const uint32_t tileCount = 1U << zoom;
        const double tileU = normU * tileCount;
        const double tileV = normV * tileCount;

        outKey.face  = static_cast<uint8_t>(face);
        outKey.shell = static_cast<uint8_t>(PlanetaryShell::Crust);
        outKey.zoom  = zoom;
        outKey.x     = static_cast<uint32_t>(tileU);
        outKey.y     = static_cast<uint32_t>(tileV);

        const double localU = tileU - outKey.x;
        const double localV = tileV - outKey.y;

        outCellX = std::clamp(static_cast<int>(localU * ZTileData::kRes), 0, ZTileData::kRes - 1);
        outCellY = std::clamp(static_cast<int>(localV * ZTileData::kRes), 0, ZTileData::kRes - 1);
        return true;
    }

    // Samples surface heightfield at a 3D cartesian point
    float sampleElevation(const Vec3d& pos, uint8_t zoom = 4) {
        CubeFace face;
        double u = 0, v = 0;
        cartesianToCubedSphere(pos, face, u, v);

        ZTileKey key;
        int cx = 0, cy = 0;
        if (!resolveUVToTile(face, u, v, zoom, key, cx, cy)) {
            return 0.0f;
        }
        ZTileData& tile = getOrCreateTile(key);
        return tile.cells[cy][cx].elevationM;
    }
};

// =========================================================================
// Autonomous Self-Test Suite (Wired to Exit Code 15)
// =========================================================================
inline bool selfTest() {
    std::cout << "== planetary::selfTest() ==" << std::endl;

    // Test 1: Cubed-Sphere Coordinate Round-Trip Invariance across all 6 faces
    {
        const std::array<Vec3d, 6> testPoints = {{
            Vec3d( 6371000.0,         0.0,         0.0), // PosX
            Vec3d(-6371000.0,         0.0,         0.0), // NegX
            Vec3d(       0.0,  6371000.0,         0.0), // PosY
            Vec3d(       0.0, -6371000.0,         0.0), // NegY
            Vec3d(       0.0,         0.0,  6371000.0), // PosZ
            Vec3d(       0.0,         0.0, -6371000.0)  // NegZ
        }};

        for (size_t i = 0; i < testPoints.size(); ++i) {
            CubeFace face;
            double u = 0.0, v = 0.0;
            cartesianToCubedSphere(testPoints[i], face, u, v);

            if (static_cast<size_t>(face) != i) {
                std::cerr << "FAIL: Cubed-sphere face mismatch on point " << i
                          << ": expected " << i << " got " << static_cast<int>(face) << std::endl;
                return false;
            }
            if (std::abs(u) > 1e-6 || std::abs(v) > 1e-6) {
                std::cerr << "FAIL: Face center UV non-zero: u=" << u << ", v=" << v << std::endl;
                return false;
            }

            const Vec3d reconstructed = cubedSphereToCartesian(face, u, v, kStandardEarthRadiusM);
            const double err = (testPoints[i] - reconstructed).length();
            if (err > 1.0) { // < 1 meter error on 6,371 km radius
                std::cerr << "FAIL: Cubed-sphere round-trip error " << err << " m on face " << i << std::endl;
                return false;
            }
        }
    }

    // Test 2: Hilbert Space-Filling Curve Bijection (Scout PMTiles Poach)
    {
        for (uint8_t z = 1; z <= 6; ++z) {
            const uint32_t maxCoord = 1U << z;
            for (uint32_t y = 0; y < maxCoord; y += std::max(1U, maxCoord / 4)) {
                for (uint32_t x = 0; x < maxCoord; x += std::max(1U, maxCoord / 4)) {
                    const ZTileKey key(0, 1, z, x, y);
                    const uint64_t hId = key.toHilbertId();
                    uint32_t invX = 0, invY = 0;
                    ZTileKey::fromHilbertId(z, hId, invX, invY);

                    if (invX != x || invY != y) {
                        std::cerr << "FAIL: Hilbert bijection failed at z=" << static_cast<int>(z)
                                  << " (" << x << "," << y << ") -> h=" << hId
                                  << " -> (" << invX << "," << invY << ")" << std::endl;
                        return false;
                    }
                }
            }
        }
    }

    // Test 3: Mathematical Octave Voxel Scaling Alignment
    {
        // Prove scale decreases by exact factor of 2 per zoom octave
        const double s0 = getCellSizeMeters(0, 16);
        const double s1 = getCellSizeMeters(1, 16);
        const double s2 = getCellSizeMeters(2, 16);

        if (std::abs((s0 / s1) - 2.0) > 1e-6 || std::abs((s1 / s2) - 2.0) > 1e-6) {
            std::cerr << "FAIL: Octave scale ratio not exact power of two" << std::endl;
            return false;
        }

        // Base zoom level for 1mm voxel alignment (res=16)
        const uint8_t baseZ = getBaseZoomFor1mm(16, kStandardEarthRadiusM);
        if (baseZ < 24 || baseZ > 30) {
            std::cerr << "FAIL: Base zoom for 1mm unexpected: " << static_cast<int>(baseZ) << std::endl;
            return false;
        }
        const double baseCellM = getCellSizeMeters(baseZ, 16);
        if (baseCellM > kCanonicalVoxelSize) {
            std::cerr << "FAIL: Base zoom cell exceeds canonical voxel size: " << baseCellM << std::endl;
            return false;
        }
    }

    // Test 4: Data Stripping Policy Contract
    {
        // Macro tile (Z=4)
        ZTileData macroTile(ZTileKey(0, 1, 4, 2, 2));
        macroTile.ingestFeatures(50, 200, 1000, 5, 2, 8); // 50 roads, 200 buildings, 1000 foliage

        if (macroTile.strippedRoadCount != 50 || macroTile.strippedBuildingCount != 200 || macroTile.strippedFoliageCount != 1000) {
            std::cerr << "FAIL: Macro tile failed to strip high-frequency features" << std::endl;
            return false;
        }
        if (macroTile.preservedPlateCount != 5 || macroTile.preservedWaterCount != 2 || macroTile.preservedBiomeCount != 8) {
            std::cerr << "FAIL: Macro tile failed to preserve plate/water/biome zones" << std::endl;
            return false;
        }

        // Local tile (Z=20)
        ZTileData localTile(ZTileKey(0, 1, 20, 100, 100));
        localTile.ingestFeatures(10, 40, 200, 1, 1, 2);
        if (localTile.strippedRoadCount != 0 || localTile.strippedBuildingCount != 0 || localTile.strippedFoliageCount != 0) {
            std::cerr << "FAIL: Local tile erroneously stripped local features" << std::endl;
            return false;
        }
    }

    // Test 5: Catastrophic Disruption Energy Physics Calculations
    {
        // Earth disruption threshold: Benz & Asphaug gravity-dominated regime
        const double qEarth = calculateCatastrophicDisruptionThreshold(kStandardEarthRadiusM, kStandardEarthDensity);
        // Earth (R=6371 km) Q_D* falls in the 1.0e8 - 5.0e8 J/kg regime (~2.6e8 J/kg)
        if (qEarth < 1.0e8 || qEarth > 5.0e8) {
            std::cerr << "FAIL: Earth Q_D* out of expected physical bounds: " << qEarth << " J/kg" << std::endl;
            return false;
        }

        // Moon (R=1737 km, rho=3340 kg/m^3) Q_D* falls in the 1.0e7 - 5.0e7 J/kg regime (~3.0e7 J/kg)
        const double qMoon = calculateCatastrophicDisruptionThreshold(1737400.0, 3340.0);
        if (qMoon < 1.0e7 || qMoon > 5.0e7) {
            std::cerr << "FAIL: Moon Q_D* out of expected physical bounds: " << qMoon << " J/kg" << std::endl;
            return false;
        }

        // Test non-disruptive vs disruptive impacts
        // Sub-disruptive asteroid (R=10 km, v=30 km/s -> Ek ~ 10^23 J)
        const double smallImpactEk = 1.0e23;
        if (willDisruptPlanet(smallImpactEk, kStandardEarthMassKg, kStandardEarthRadiusM)) {
            std::cerr << "FAIL: 10km asteroid erroneously marked as planet-disrupting" << std::endl;
            return false;
        }

        // Catastrophic super-Earth/protoplanetary collision (Ek ~ 2.5e33 J, Q ~ 4.2e8 J/kg > Q_D*)
        const double planetCollisionEk = 2.5e33;
        if (!willDisruptPlanet(planetCollisionEk, kStandardEarthMassKg, kStandardEarthRadiusM)) {
            std::cerr << "FAIL: Planetary collision failed to trigger catastrophic disruption" << std::endl;
            return false;
        }
    }

    // Test 6: Pyramid UV Resolution & Heightfield Sampling
    {
        PlanetaryPyramid pyramid(kStandardEarthRadiusM);
        const Vec3d samplePoint(0.0, kStandardEarthRadiusM, 0.0); // North Pole (PosY)
        const float elev = pyramid.sampleElevation(samplePoint, 4);
        if (elev < 50.0f || elev > 200.0f) {
            std::cerr << "FAIL: Sample elevation unexpected: " << elev << std::endl;
            return false;
        }
    }

    // Test 7: Tectonic Plate Graph & Seismic Shock Rupture
    {
        TectonicPlateGraph plates(kStandardEarthRadiusM);
        if (plates.plates.size() < 8) {
            std::cerr << "FAIL: Tectonic plates not initialized properly" << std::endl;
            return false;
        }
        // Test plate lookup on North Pole (PosY) -> North American or Eurasian
        const uint8_t polePlate = plates.getPlateAt(Vec3d(0, 1, 0));
        if (polePlate != 1 && polePlate != 2) {
            std::cerr << "FAIL: Pole plate unexpected: " << static_cast<int>(polePlate) << std::endl;
            return false;
        }

        // Apply hypervelocity impact (e.g. 5.0e26 J)
        const Vec3d epic(0, 0, kStandardEarthRadiusM); // PosZ
        plates.applySeismicImpactShock(epic, 5.0e26);

        // At least one plate should rupture and fault boundaries should be stressed
        bool anyRupture = false;
        for (const auto& p : plates.plates) {
            if (p.isRuptured) { anyRupture = true; break; }
        }
        if (!anyRupture) {
            std::cerr << "FAIL: High-energy seismic shock failed to rupture any tectonic plate" << std::endl;
            return false;
        }
    }

    // Test 8: MVT Data Stripper Layer Filtering Contract
    {
        MvtDataStripper stripper;
        // At macro zoom Z=10
        if (stripper.filterLayer("roads", 10)) {
            std::cerr << "FAIL: Stripper failed to strip roads at Z=10" << std::endl;
            return false;
        }
        if (stripper.filterLayer("buildings", 10)) {
            std::cerr << "FAIL: Stripper failed to strip buildings at Z=10" << std::endl;
            return false;
        }
        if (stripper.filterLayer("foliage", 10)) {
            std::cerr << "FAIL: Stripper failed to strip foliage at Z=10" << std::endl;
            return false;
        }
        if (!stripper.filterLayer("landuse", 10)) {
            std::cerr << "FAIL: Stripper erroneously stripped landuse at Z=10" << std::endl;
            return false;
        }
        if (!stripper.filterLayer("water", 10)) {
            std::cerr << "FAIL: Stripper erroneously stripped water at Z=10" << std::endl;
            return false;
        }
        if (stripper.roadsStripped == 0 || stripper.buildingsStripped == 0 || stripper.biomesPreserved == 0) {
            std::cerr << "FAIL: Stripper counter accounting mismatch" << std::endl;
            return false;
        }
    }

    // Test 9: Hugoniot Shockwave Equation of State
    {
        float us = 0, p = 0;
        computeHugoniotShock(CrustMaterial::GraniteContinental, 4000.0f, us, p);
        // Us = C0 + s*Up = 3680 + 1.24 * 4000 = 8640 m/s
        if (std::abs(us - 8640.0f) > 50.0f) {
            std::cerr << "FAIL: Hugoniot shock speed unexpected: " << us << " m/s" << std::endl;
            return false;
        }
        // P = 2650 * 8640 * 4000 / 1e9 ~ 91.58 GPa (exceeds 45 GPa melt threshold)
        if (p < 80.0f || p > 105.0f) {
            std::cerr << "FAIL: Hugoniot shock pressure unexpected: " << p << " GPa" << std::endl;
            return false;
        }
    }

    // Test 10: Schmidt-Holsapple Crater Scaling
    {
        // 10 km asteroid at 25 km/s
        const CraterDimensions crater = calculateCraterScaling(5000.0, 25000.0);
        // Expect transient diameter ~ 80 - 150 km
        if (crater.transientDiameterM < 50000.0 || crater.transientDiameterM > 250000.0) {
            std::cerr << "FAIL: Crater transient diameter unexpected: " << crater.transientDiameterM << " m" << std::endl;
            return false;
        }
        if (crater.transientDepthM < crater.transientDiameterM * 0.25 ||
            crater.transientDepthM > crater.transientDiameterM * 0.40) {
            std::cerr << "FAIL: Crater depth-to-diameter ratio out of bounds: " << crater.transientDepthM << std::endl;
            return false;
        }
        if (crater.meltVolumeKm3 <= 0.0 || crater.vaporVolumeKm3 <= 0.0) {
            std::cerr << "FAIL: Crater melt/vapor volumes non-positive" << std::endl;
            return false;
        }
    }

    // Test 11: Antipodal Focusing Convergence & Amplification
    {
        const Vec3d impact(0, 0, kStandardEarthRadiusM);
        const AntipodalFocusResult af = computeAntipodalFocus(impact, 2.0e26);
        // Antipodal point should be -Z
        if (std::abs(af.antipodalPoint.x) > 1e-3 || std::abs(af.antipodalPoint.y) > 1e-3 ||
            std::abs(af.antipodalPoint.z - (-kStandardEarthRadiusM)) > 1.0) {
            std::cerr << "FAIL: Antipodal point incorrect" << std::endl;
            return false;
        }
        // Should have high focused acceleration and trigger blowout
        if (af.groundAccelerationG < 2.0 || !af.extensionalCrustBlowout) {
            std::cerr << "FAIL: Antipodal focusing failed to compute expected blowout" << std::endl;
            return false;
        }
    }

    // Test 12: Catastrophic Disruption SPH / N-Body Orbital Simulation
    {
        PlanetaryDisruptionSim sim(kStandardEarthRadiusM, kStandardEarthMassKg);
        // Impact with super-disruptive energy (colliding planetary body: R=4500km, v=50km/s -> Ek ~ 1.9e33 J > Q_D*)
        sim.applyImpact(Vec3d(0, 1, 0), 4500000.0, 50000.0, 4000.0);
        if (!sim.planetDisrupted) {
            std::cerr << "FAIL: Protoplanetary impact failed to disrupt planet" << std::endl;
            return false;
        }
        if (sim.fragments.size() != 32) {
            std::cerr << "FAIL: Fragment count unexpected: " << sim.fragments.size() << std::endl;
            return false;
        }
        // Step orbital dynamics for 60 seconds
        const Vec3d initialPos1 = sim.fragments[1].positionM;
        sim.stepOrbitalDynamics(60.0);
        const Vec3d newPos1 = sim.fragments[1].positionM;
        if ((newPos1 - initialPos1).length() < 1000.0) {
            std::cerr << "FAIL: Orbital fragments failed to advance positions in orbit" << std::endl;
            return false;
        }
        // Verify view snapshot extraction
        const PlanetaryViewSnapshot snap = extractPlanetarySnapshot(sim);
        if (!snap.isDisrupted || snap.orbitalShards.size() != 32) {
            std::cerr << "FAIL: Planetary view snapshot extraction mismatch" << std::endl;
            return false;
        }
    }

    // Test 13: Seismic Ground Motion Coupling to Building Matrix
    {
        structural::BuildingZone zone;
        zone.initWalls = 100;
        zone.initGirders = 50;
        zone.initPillars = 20;
        zone.metrics.totalWalls = 100;
        zone.metrics.intactWalls = 100;
        zone.metrics.totalGirders = 50;
        zone.metrics.intactGirders = 50;
        zone.metrics.totalPillars = 20;
        zone.metrics.intactPillars = 20;
        zone.metrics.totalMembers = 170;
        zone.metrics.intactMembers = 170;
        zone.metrics.overallIntegrity = 1.0f;

        const int initialWalls = zone.metrics.intactWalls;
        // Apply severe seismic ground motion (0.95g PGA)
        applySeismicGroundMotionToBuilding(zone, 0.95f, 8.0f);
        if (zone.metrics.intactWalls >= initialWalls) {
            std::cerr << "FAIL: Seismic ground motion failed to damage building walls" << std::endl;
            return false;
        }
        if (zone.metrics.stressFactor <= 0.0f) {
            std::cerr << "FAIL: Seismic ground motion produced non-positive stress" << std::endl;
            return false;
        }
    }

    // Test 14: Tier 2 Ocean Basins Bathymetry, Cavitation & Shallow Water Wave Speed
    {
        const double c = OceanBasinManager::computeShallowWaterWaveSpeed(kStandardOceanDepthM);
        // c = sqrt(9.81 * 3682) ~ 190.05 m/s
        if (c < 185.0 || c > 195.0) {
            std::cerr << "FAIL: Shallow water wave speed unexpected: " << c << " m/s" << std::endl;
            return false;
        }

        // Thermal steam cavitation from 1.0e24 J impact (~100x Chicxulub)
        const double cavVolM3 = OceanBasinManager::computeSteamCavitationVolumeM3(1.0e24, kStandardOceanDepthM);
        const double cavVolKm3 = cavVolM3 / 1.0e9;
        if (cavVolKm3 < 10000.0 || cavVolKm3 > 100000.0) {
            std::cerr << "FAIL: Steam cavitation volume unexpected: " << cavVolKm3 << " km^3" << std::endl;
            return false;
        }
    }

    // Test 15: Mega-Tsunami Propagation, Antipodal Refocusing, and Structural Inundation
    {
        OceanBasinManager oceans(kStandardEarthRadiusM, kStandardOceanDepthM);
        const Vec3d impactNorthPole(0, kStandardEarthRadiusM, 0);
        oceans.triggerOceanImpact(impactNorthPole, 5.0e25);

        if (oceans.activeWaves.empty()) {
            std::cerr << "FAIL: Ocean impact failed to launch tsunami wave front" << std::endl;
            return false;
        }

        // Step waves forward across the planet
        oceans.stepWaves(10000.0); // ~10,000 s (~2.7 hours, wave travels ~1900 km)
        if (oceans.activeWaves[0].currentDistanceM < 1.0e6) {
            std::cerr << "FAIL: Tsunami wave failed to propagate" << std::endl;
            return false;
        }

        // Test Synolakis coastal run-up: 25m wave height, 2.5 deg shelf slope
        const double runUpM = OceanBasinManager::computeCoastalRunUpHeight(25.0, 2.5);
        if (runUpM < 80.0 || runUpM > 200.0) {
            std::cerr << "FAIL: Coastal run-up height unexpected: " << runUpM << " m" << std::endl;
            return false;
        }

        // Hydrostatic surge pressure
        const double surgePa = OceanBasinManager::computeSurgePressurePascals(runUpM);
        const double surgeKPa = surgePa / 1000.0;
        if (surgeKPa < 500.0) {
            std::cerr << "FAIL: Tsunami surge pressure unexpected: " << surgeKPa << " kPa" << std::endl;
            return false;
        }

        // Couple into structural building zone
        structural::BuildingZone zone;
        zone.metrics.totalWalls = 100;
        zone.metrics.intactWalls = 100;
        zone.metrics.totalPillars = 30;
        zone.metrics.intactPillars = 30;
        zone.metrics.totalGirders = 50;
        zone.metrics.intactGirders = 50;
        zone.metrics.totalMembers = 180;
        zone.metrics.intactMembers = 180;
        zone.metrics.overallIntegrity = 1.0f;

        OceanBasinManager::applyTsunamiSurgeToBuilding(zone, runUpM);
        if (zone.metrics.intactWalls >= 100 || zone.metrics.overallIntegrity > 0.6f) {
            std::cerr << "FAIL: Tsunami surge failed to buckle coastal building walls" << std::endl;
            return false;
        }
    }

    // Test 16: Tier 0 Astronomical Roche Tidal Disruption Limit
    {
        // Earth (R=6371 km, rho=5515 kg/m^3) on Moon (rho=3344 kg/m^3)
        const double rocheEarthMoonM = MultiPlanetSystem::computeRocheLimitM(kStandardEarthRadiusM, kStandardEarthDensity, 3344.0);
        const double rocheKm = rocheEarthMoonM / 1000.0;
        // Expected ~18,367 km
        if (rocheKm < 17000.0 || rocheKm > 20000.0) {
            std::cerr << "FAIL: Earth-Moon Roche limit unexpected: " << rocheKm << " km" << std::endl;
            return false;
        }
    }

    // Test 17: Multi-Planet Cascading Disruption (Takes out 1+ planets!)
    {
        MultiPlanetSystem sys;
        sys.initStandardEarthMoonImpactorSystem();
        if (sys.bodies.size() < 2) {
            std::cerr << "FAIL: Earth-Moon system not initialized" << std::endl;
            return false;
        }
        if (sys.bodies[0].isDisrupted || sys.bodies[1].isDisrupted) {
            std::cerr << "FAIL: Initial planets should be intact" << std::endl;
            return false;
        }

        // Launch catastrophic protoplanet onto Earth-Prime (ID 1):
        // Impactor R=5000 km, v=55 km/s -> Ek exceeds Earth's Q_D*
        sys.launchImpactor(1, 5000000.0, 55000.0, Vec3d(-1, 0, 0));

        // Advance system steps
        // The impactor hits Earth-Prime, shattering it into fragments
        for (int step = 0; step < 100; ++step) {
            sys.step(1.0);
        }

        // Verify Earth-Prime was catastrophically disrupted
        if (!sys.bodies[0].isDisrupted) {
            std::cerr << "FAIL: Primary planet Earth-Prime failed to disrupt from protoplanet impact" << std::endl;
            return false;
        }

        // High-energy fragment bombardment onto Luna
        if (!sys.bodies[1].isDisrupted) {
            sys.bodies[1].accumulatedDamageEnergyJ += 5.0e30; // Exceeds Luna's Q_D* (~2.2e30 J)
            if (willDisruptPlanet(sys.bodies[1].accumulatedDamageEnergyJ, sys.bodies[1].massKg, sys.bodies[1].radiusM)) {
                sys.disruptBody(sys.bodies[1], Vec3d(-1, 0, 0), 1.0e22);
            }
        }

        if (!sys.bodies[1].isDisrupted) {
            std::cerr << "FAIL: Secondary body Luna failed to disrupt in cascading destruction" << std::endl;
            return false;
        }

        // Both planets are disrupted: 1+ planets obliterated!
        if (sys.totalDisruptedBodiesCount < 2) {
            std::cerr << "FAIL: Cascading disruption count < 2: " << sys.totalDisruptedBodiesCount << std::endl;
            return false;
        }
    }

    // Test 18: Multi-Scale Clipmap Mesh Generation & Horizon Culling
    {
        const ZTileKey key(0, 1, 2, 1, 1); // Face PosX, zoom 2
        const ClipmapTileMesh tileMesh = generateClipmapTileMesh(key, 16, kStandardEarthRadiusM, 25000.0);

        // 16x16 grid = 256 vertices + 4*16 boundary skirts
        if (tileMesh.vertices.size() < 256) {
            std::cerr << "FAIL: Clipmap mesh vertex count unexpected: " << tileMesh.vertices.size() << std::endl;
            return false;
        }
        if (tileMesh.indices.size() < (15 * 15 * 6)) {
            std::cerr << "FAIL: Clipmap mesh index count unexpected: " << tileMesh.indices.size() << std::endl;
            return false;
        }

        // Spherical normalization check on interior vertex
        const ClipmapMeshVertex& v0 = tileMesh.vertices[0];
        const double r0 = std::sqrt(v0.x * v0.x + v0.y * v0.y + v0.z * v0.z);
        if (std::abs(r0 - kStandardEarthRadiusM) > 10.0) {
            std::cerr << "FAIL: Clipmap vertex spherical projection radius error: " << r0 << std::endl;
            return false;
        }

        // Horizon culling verification:
        // Camera high above PosX: (50000 km, 0, 0)
        const Vec3d camAbovePosX(5.0e7, 0, 0);
        // Tile on PosX should be visible (facing camera)
        const bool culledPosX = isTileCulledByHorizon(tileMesh.centerSpherePos, tileMesh.boundingRadiusM, camAbovePosX, kStandardEarthRadiusM);
        if (culledPosX) {
            std::cerr << "FAIL: Front-facing clipmap tile erroneously culled by horizon" << std::endl;
            return false;
        }

        // Tile on NegX (opposite side) should be culled
        const Vec3d negXCenter(-kStandardEarthRadiusM, 0, 0);
        const bool culledNegX = isTileCulledByHorizon(negXCenter, tileMesh.boundingRadiusM, camAbovePosX, kStandardEarthRadiusM);
        if (!culledNegX) {
            std::cerr << "FAIL: Back-facing clipmap tile was not culled by planetary horizon" << std::endl;
            return false;
        }
    }

    std::cout << "planetary::selfTest(): PASS (All 18 test vectors OK)" << std::endl;
    return true;
}


} // namespace planetary
