#pragma once
// Structural Integrity Matrix System
//
// Calculates load capacity, stress factors, and structural stability for assigned
// building zones (e.g. Warehouse). Tracks primary vertical supports (Pillars),
// horizontal load transfer / truss members (Girders), and shear walls (Walls).
//
// Governed by RULES.md: Authoritative on the simulation side, serialized to JSON
// for UI / WebView2 / Telemetry without violating the sim/view boundary.

#include "sim_world.hpp"
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstdint>

namespace structural {

enum class MemberType : uint8_t {
    None = 0,
    Girder,     // Horizontal / diagonal steel beams and roof trusses
    Pillar,     // Vertical load-bearing columns transferring weight to ground
    Wall,       // Shear walls and perimeter partition surfaces
    Slab        // Ground / foundation slab
};

enum class ZoneStatus : uint8_t {
    Nominal = 0,    // >= 85% integrity: Solid, full load capacity
    Stressed,       // 65% - 84%: Minor fractures, creaking under stress
    Compromised,    // 40% - 64%: Serious structural damage, load capacity halved
    Critical,       // 20% - 39%: Impending failure, imminent partial collapse
    Collapsed       // < 20%: Major structural failure
};

inline const char* statusToString(ZoneStatus s) {
    switch (s) {
        case ZoneStatus::Nominal:     return "Nominal";
        case ZoneStatus::Stressed:    return "Stressed";
        case ZoneStatus::Compromised: return "Compromised";
        case ZoneStatus::Critical:    return "Critical";
        case ZoneStatus::Collapsed:   return "Collapsed";
        default:                      return "Unknown";
    }
}

struct ZoneBounds {
    int minX = 0, minY = 1, minZ = 0;
    int maxX = 191, maxY = 63, maxZ = 159;

    bool contains(int x, int y, int z) const {
        return x >= minX && x <= maxX &&
               y >= minY && y <= maxY &&
               z >= minZ && z <= maxZ;
    }
};

struct ZoneMetrics {
    int totalGirders = 0;
    int intactGirders = 0;
    int totalPillars = 0;
    int intactPillars = 0;
    int totalWalls = 0;
    int intactWalls = 0;
    int totalMembers = 0;
    int intactMembers = 0;

    float girderRatio = 1.0f;
    float pillarRatio = 1.0f;
    float wallRatio = 1.0f;
    float overallIntegrity = 1.0f;     // 0.0 to 1.0
    float loadCapacityRatio = 1.0f;
    float stressFactor = 0.0f;         // 1.0 - overallIntegrity
    ZoneStatus status = ZoneStatus::Nominal;
};

class BuildingZone {
public:
    std::string id = "warehouse";
    std::string name = "Warehouse Bay";
    ZoneBounds bounds{};
    ZoneMetrics metrics{};

    // Initial baseline counts captured upon zone registration
    int initGirders = 0;
    int initPillars = 0;
    int initWalls = 0;

    BuildingZone() = default;
    BuildingZone(std::string zoneId, std::string zoneName, ZoneBounds b)
        : id(std::move(zoneId)), name(std::move(zoneName)), bounds(b) {}

    // Categorize a world cell at (x, y, z) into a structural member type
    MemberType classifyCell(const sim::World& world, int x, int y, int z) const {
        if (!bounds.contains(x, y, z)) return MemberType::None;
        const sim::Block b = world.get(x, y, z);
        if (b == sim::Block::Air || b == sim::Block::Water || b == sim::Block::WaterCurrent) {
            return MemberType::None;
        }

        // Foundation slab: y <= 1
        if (y <= 1) {
            return MemberType::Slab;
        }

        // Girders: Steel structural framing, roof trusses, crane rails
        if (b == sim::Block::Girder) {
            return MemberType::Girder;
        }

        // Concrete: Pillars & primary vertical load-bearing columns
        if (b == sim::Block::Concrete) {
            return MemberType::Pillar;
        }

        // Sheet metal, Wood, WoodDark: Shear walls, exterior panels, partition walls
        if (b == sim::Block::SheetMetal || b == sim::Block::Wood || b == sim::Block::WoodDark) {
            return MemberType::Wall;
        }

        return MemberType::None;
    }

    void scanBaseline(const sim::World& world) {
        initGirders = 0;
        initPillars = 0;
        initWalls = 0;

        for (int y = bounds.minY; y <= bounds.maxY; ++y) {
            for (int z = bounds.minZ; z <= bounds.maxZ; ++z) {
                for (int x = bounds.minX; x <= bounds.maxX; ++x) {
                    MemberType t = classifyCell(world, x, y, z);
                    if (t == MemberType::Girder) ++initGirders;
                    else if (t == MemberType::Pillar) ++initPillars;
                    else if (t == MemberType::Wall) ++initWalls;
                }
            }
        }
        evaluate(world);
    }

    void evaluate(const sim::World& world) {
        int curGirders = 0;
        int curPillars = 0;
        int curWalls = 0;

        for (int y = bounds.minY; y <= bounds.maxY; ++y) {
            for (int z = bounds.minZ; z <= bounds.maxZ; ++z) {
                for (int x = bounds.minX; x <= bounds.maxX; ++x) {
                    MemberType t = classifyCell(world, x, y, z);
                    if (t == MemberType::Girder) ++curGirders;
                    else if (t == MemberType::Pillar) ++curPillars;
                    else if (t == MemberType::Wall) ++curWalls;
                }
            }
        }

        metrics.totalGirders = initGirders;
        metrics.intactGirders = curGirders;
        metrics.totalPillars = initPillars;
        metrics.intactPillars = curPillars;
        metrics.totalWalls = initWalls;
        metrics.intactWalls = curWalls;

        metrics.totalMembers = initGirders + initPillars + initWalls;
        metrics.intactMembers = curGirders + curPillars + curWalls;

        metrics.girderRatio = initGirders > 0 ? std::clamp(static_cast<float>(curGirders) / static_cast<float>(initGirders), 0.0f, 1.0f) : 1.0f;
        metrics.pillarRatio = initPillars > 0 ? std::clamp(static_cast<float>(curPillars) / static_cast<float>(initPillars), 0.0f, 1.0f) : 1.0f;
        metrics.wallRatio = initWalls > 0 ? std::clamp(static_cast<float>(curWalls) / static_cast<float>(initWalls), 0.0f, 1.0f) : 1.0f;

        // Weighted structural calculation:
        // Pillars carry 45% (primary vertical load bearing)
        // Girders carry 35% (roof trusses & cross spans)
        // Walls carry 20% (shear & lateral stability)
        float weightPillar = 0.45f;
        float weightGirder = 0.35f;
        float weightWall = 0.20f;

        float totalWeight = 0.0f;
        float sumIntegrity = 0.0f;
        if (initPillars > 0) { totalWeight += weightPillar; sumIntegrity += weightPillar * metrics.pillarRatio; }
        if (initGirders > 0) { totalWeight += weightGirder; sumIntegrity += weightGirder * metrics.girderRatio; }
        if (initWalls > 0)   { totalWeight += weightWall;   sumIntegrity += weightWall * metrics.wallRatio; }

        if (totalWeight > 0.0f) {
            metrics.overallIntegrity = std::clamp(sumIntegrity / totalWeight, 0.0f, 1.0f);
        } else {
            metrics.overallIntegrity = 1.0f;
        }

        metrics.loadCapacityRatio = metrics.overallIntegrity;
        metrics.stressFactor = 1.0f - metrics.overallIntegrity;

        if (metrics.overallIntegrity >= 0.85f) {
            metrics.status = ZoneStatus::Nominal;
        } else if (metrics.overallIntegrity >= 0.65f) {
            metrics.status = ZoneStatus::Stressed;
        } else if (metrics.overallIntegrity >= 0.40f) {
            metrics.status = ZoneStatus::Compromised;
        } else if (metrics.overallIntegrity >= 0.20f) {
            metrics.status = ZoneStatus::Critical;
        } else {
            metrics.status = ZoneStatus::Collapsed;
        }
    }

    std::string toJson() const {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(4);
        ss << "{\"id\":\"" << id << "\","
           << "\"name\":\"" << name << "\","
           << "\"integrity\":" << metrics.overallIntegrity << ","
           << "\"status\":\"" << statusToString(metrics.status) << "\","
           << "\"loadCapacity\":" << metrics.loadCapacityRatio << ","
           << "\"stressFactor\":" << metrics.stressFactor << ","
           << "\"girders\":{\"intact\":" << metrics.intactGirders << ",\"total\":" << metrics.totalGirders << ",\"ratio\":" << metrics.girderRatio << "},"
           << "\"pillars\":{\"intact\":" << metrics.intactPillars << ",\"total\":" << metrics.totalPillars << ",\"ratio\":" << metrics.pillarRatio << "},"
           << "\"walls\":{\"intact\":" << metrics.intactWalls << ",\"total\":" << metrics.totalWalls << ",\"ratio\":" << metrics.wallRatio << "},"
           << "\"bounds\":{\"min\":[" << bounds.minX << "," << bounds.minY << "," << bounds.minZ << "],"
           << "\"max\":[" << bounds.maxX << "," << bounds.maxY << "," << bounds.maxZ << "]}}";
        return ss.str();
    }
};

class MatrixSystem {
public:
    std::vector<BuildingZone> zones;

    void clear() {
        zones.clear();
    }

    void registerZone(const BuildingZone& zone) {
        zones.push_back(zone);
    }

    void initDefaultZones(const sim::World& world) {
        zones.clear();
        // Zone 1: The Warehouse (Bay + Trusses + Perimeter)
        ZoneBounds wb;
        wb.minX = 0;
        wb.minY = 2; // above slab (y=0,1 is floor)
        wb.minZ = 0;
        wb.maxX = sim::kWorldW - 1;
        wb.maxY = sim::kWorldH - 1;
        wb.maxZ = sim::kWorldD - 1;

        BuildingZone warehouseZone("warehouse", "Warehouse Main Bay", wb);
        warehouseZone.scanBaseline(world);
        zones.push_back(warehouseZone);
    }

    void evaluateAll(const sim::World& world) {
        for (auto& z : zones) {
            z.evaluate(world);
        }
    }

    const BuildingZone* findZone(const std::string& zoneId) const {
        for (const auto& z : zones) {
            if (z.id == zoneId) return &z;
        }
        return nullptr;
    }

    BuildingZone* findZoneMut(const std::string& zoneId) {
        for (auto& z : zones) {
            if (z.id == zoneId) return &z;
        }
        return nullptr;
    }

    std::string toJson() const {
        std::ostringstream ss;
        ss << "[";
        for (size_t i = 0; i < zones.size(); ++i) {
            if (i > 0) ss << ",";
            ss << zones[i].toJson();
        }
        ss << "]";
        return ss.str();
    }

    // Deterministic self-test verification
    bool selfTest(sim::World& world) {
        initDefaultZones(world);
        const BuildingZone* wz = findZone("warehouse");
        if (!wz) return false;

        // Must find structural members in warehouse
        if (wz->initGirders == 0 && wz->initPillars == 0 && wz->initWalls == 0) {
            return false;
        }

        // Initial integrity must be 1.0 (Nominal)
        if (std::fabs(wz->metrics.overallIntegrity - 1.0f) > 1e-4f) {
            return false;
        }
        if (wz->metrics.status != ZoneStatus::Nominal) {
            return false;
        }

        // Test demolition sensitivity:
        // Find an intact girder or wall cell to test
        int testX = -1, testY = -1, testZ = -1;
        sim::Block originalBlock = sim::Block::Air;

        for (int y = wz->bounds.minY; y <= wz->bounds.maxY && testX < 0; ++y) {
            for (int z = wz->bounds.minZ; z <= wz->bounds.maxZ && testX < 0; ++z) {
                for (int x = wz->bounds.minX; x <= wz->bounds.maxX && testX < 0; ++x) {
                    sim::Block b = world.get(x, y, z);
                    if (b == sim::Block::Girder || b == sim::Block::Concrete) {
                        testX = x; testY = y; testZ = z;
                        originalBlock = b;
                        break;
                    }
                }
            }
        }

        if (testX >= 0) {
            // Punch out member
            world.set(testX, testY, testZ, sim::Block::Air);
            evaluateAll(world);

            const BuildingZone* afterDemo = findZone("warehouse");
            if (!afterDemo || afterDemo->metrics.overallIntegrity >= 1.0f) {
                // Restore before failing
                world.set(testX, testY, testZ, originalBlock);
                return false;
            }

            // Restore member
            world.set(testX, testY, testZ, originalBlock);
            evaluateAll(world);

            const BuildingZone* afterRestore = findZone("warehouse");
            if (!afterRestore || std::fabs(afterRestore->metrics.overallIntegrity - 1.0f) > 1e-4f) {
                return false;
            }
        }

        return true;
    }
};

} // namespace structural
