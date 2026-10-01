#pragma once
// Load-time terrain: chunk classes and a road network, generated from a class
// and a seed carried by the map's "terrain" section (schema.md, "Terrain").
//
// What this is
//   A height field over the world, cut into 2x2-chunk regions (64x64 cells).
//   Each region is assigned a chunk class from the map's terrain family by a
//   seeded pick, and its columns are filled with that class's materials. A road
//   network is planned over the same region grid, and each road node names the
//   prefab to stamp and the quarter turn to stamp it at.
//
// What this is not
//   It is not a second source of truth and it is not allowed to become one:
//   everything it produces is ordinary unit-cubic occupancy written into
//   sim::World, the same authority the map loader writes to (RULES.md,
//   "Authority and the view/sim split"). Nothing here is ever read back as a
//   display transform, and nothing here is ever fed a clock.
//
// Determinism (RULES.md, "Determinism (mandatory)")
//   Every number below is an integer produced by an explicit hash of the
//   coordinates and the seed. There is no rand(), no time, no float
//   interpolation, and no iteration-order dependence, so the same map and seed
//   generate the same world on every machine — which is what makes the gate's
//   golden hash meaningful.
//
// Non-destructive
//   The generator FILLS. It never clears a cell the map already filled, so a
//   terrain section can be added to a map that has hand-authored structure
//   without that structure being demolished. A road that meets high ground is
//   raised onto an embankment rather than cut through it.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "sim_world.hpp"

namespace terrain {

// One terrain module: 2x2 chunks, the base unit for both chunk classes and road
// segments. 64 cells across, in the same integer grid as everything else.
static constexpr int kRegionCells = 2 * sim::kChunkSize;

// How many cells tall a road tile is: a base course and the drivable surface.
// The tile is stamped at `level - 1`, so its top layer lands exactly on the
// network's level, and gradeRoads fills to `level - 2` and stops, leaving
// exactly these two cells for the tile. The number lives here because it is a
// contract between three places -- this file, scripts/build_terrain_prefabs.py,
// and the engine's altitude gate -- and a road whose surface sits one cell off
// its own level still passes a check that only looks at which SIDES it reaches.
static constexpr int kRoadLayers = 2;

// Road geometry the generator must guarantee room for. The stamped prefab owns
// the road's own width; this is the band the generator grades flat underneath
// it, deliberately wider, so a kerb can never end up hanging over a hillside.
// The gate checks the real road surface, not this constant.
static constexpr int kRoadCutHalfWidth = 8;

// Cells of Dirt under a class's surface material.
static constexpr int kSubDepth = 2;

// ---------------------------------------------------------------------------
// Seeded integer noise
// ---------------------------------------------------------------------------

// A 32-bit finalizer (the "lowbias32" constants). No state, no clock: the same
// input always gives the same output, on any machine.
inline uint32_t mix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

inline uint32_t hashCell(int32_t x, int32_t y, int32_t z, uint32_t salt) {
    uint32_t h = salt;
    h = mix32(h ^ (static_cast<uint32_t>(x) * 0x9e3779b1u));
    h = mix32(h ^ (static_cast<uint32_t>(y) * 0x85ebca6bu));
    h = mix32(h ^ (static_cast<uint32_t>(z) * 0xc2b2ae35u));
    return h;
}

// Floor division that also works for negatives, so an offset coordinate cannot
// fold onto a different lattice cell than its unshifted neighbour.
inline int32_t floorDiv(int32_t a, int32_t b) {
    const int32_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// Smoothstep on a 0..65535 fraction, in 16.16 fixed point. Integer in, integer
// out: no float rounding to disagree about across compilers.
inline uint32_t smooth16(uint32_t f) {
    return static_cast<uint32_t>((static_cast<uint64_t>(f) * f * (3u * 65536u - 2u * f)) >> 32);
}

inline uint32_t lerp16(uint32_t a, uint32_t b, uint32_t t) {
    return a + static_cast<uint32_t>((static_cast<int64_t>(b) - static_cast<int64_t>(a)) *
                                     static_cast<int64_t>(t) >> 16);
}

// Value noise on an integer lattice of period `period` cells, as 0..65535.
// Trilinear over eight hashed corners, smoothed per axis.
inline uint32_t valueNoise16(int32_t x, int32_t y, int32_t z, uint32_t salt, int32_t period) {
    if (period < 1) period = 1;
    const int32_t ax = floorDiv(x, period), ay = floorDiv(y, period), az = floorDiv(z, period);
    const auto frac = [period](int32_t cell, int32_t lattice) {
        const int32_t rem = cell - lattice * period;
        return static_cast<uint32_t>(std::min<int32_t>(65535, (rem * 65536) / period));
    };
    const uint32_t fx = smooth16(frac(x, ax)), fy = smooth16(frac(y, ay)), fz = smooth16(frac(z, az));
    uint32_t c[2][2][2];
    for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx)
                c[dx][dy][dz] = hashCell(ax + dx, ay + dy, az + dz, salt) & 0xFFFFu;
    const uint32_t x00 = lerp16(c[0][0][0], c[1][0][0], fx), x10 = lerp16(c[0][1][0], c[1][1][0], fx);
    const uint32_t x01 = lerp16(c[0][0][1], c[1][0][1], fx), x11 = lerp16(c[0][1][1], c[1][1][1], fx);
    return lerp16(lerp16(x00, x10, fy), lerp16(x01, x11, fy), fz);
}

// ---------------------------------------------------------------------------
// Chunk classes and terrain families
// ---------------------------------------------------------------------------

// A chunk class: what its ground is made of, how high it may get above the
// family base, how steep a step its border may make, and the decoration prefab
// scattered across it. `slope` is the whole reason terrain stays walkable: a
// talus pass clamps every step to it, so no region ever presents a cliff.
struct ClassProfile {
    const char* name;
    sim::Block surface;
    sim::Block sub;
    int relief;   // cells above the family base
    int slope;    // max step to a neighbouring column
    const char* prop; // decoration prefab id ("" = none)
};

static const ClassProfile kClasses[] = {
    {"grass", sim::Block::Grass, sim::Block::Dirt, 6, 2, "prop_shrub"},
    {"sand",  sim::Block::Sand,  sim::Block::Dirt, 4, 2, "prop_rock"},
    {"snow",  sim::Block::Snow,  sim::Block::Dirt, 8, 3, "prop_pine"},
};
static constexpr int kClassCount = static_cast<int>(sizeof(kClasses) / sizeof(kClasses[0]));

// A terrain family: a base height, how much relief it allows, and the weighted
// mix of chunk classes a map of this family is built from. Weights are integers
// out of 16 so the pick stays integer-only.
struct FamilyProfile {
    const char* name;
    int base;
    int relief;
    int classWeights[kClassCount];
};

static const FamilyProfile kFamilies[] = {
    //              base relief grass sand snow
    {"valley",       18,   10, { 10,  4,  2}},
    {"dunes",        16,    8, {  2, 12,  2}},
    {"tundra",       24,   12, {  3,  2, 11}},
    {"parkland",     14,    6, { 13,  2,  1}},
};
static constexpr int kFamilyCount = static_cast<int>(sizeof(kFamilies) / sizeof(kFamilies[0]));

// What a map's "terrain" section asks for. Parsed by map_vox.hpp; everything
// with a default is optional.
struct Spec {
    bool present = false;
    std::string family = "valley";
    uint32_t seed = 0;
    int base = -1;    // family base height override
    bool props = true;
    bool roads = true;
    int roadX = -1;   // region column the cross road runs down; -1 = seeded
};

inline const FamilyProfile* familyByName(const std::string& name) {
    for (int i = 0; i < kFamilyCount; ++i)
        if (name == kFamilies[i].name) return &kFamilies[i];
    return nullptr;
}

inline int regionCountX() { return (sim::kWorldW + kRegionCells - 1) / kRegionCells; }
inline int regionCountZ() { return (sim::kWorldD + kRegionCells - 1) / kRegionCells; }

inline int regionOfX(int x) { return x / kRegionCells; }
inline int regionOfZ(int z) { return z / kRegionCells; }

// The chunk class for one region: a weighted pick over the family's mix, from
// the region's own hash. Same seed, same class, every run.
inline int classForRegion(const Spec& spec, const FamilyProfile& fam, int rx, int rz) {
    const uint32_t roll = hashCell(rx, 0x5eed, rz, spec.seed ^ 0x7777u) % 16u;
    uint32_t acc = 0;
    for (int i = 0; i < kClassCount; ++i) {
        acc += static_cast<uint32_t>(fam.classWeights[i]);
        if (roll < acc) return i;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The height field
// ---------------------------------------------------------------------------

// Column heights for the whole world plane: height[x + z * kWorldW] is the y of
// the topmost generated cell in that column (0 = nothing generated there).
//
// Built in three passes: a per-region target from the family, two integer
// octaves of detail on top, then a talus pass that clamps every step to the
// class's slope. Order matters — the talus pass runs last, so the field it
// hands on is the one with the walkability guarantee.
inline std::vector<int> heightField(const Spec& spec, const FamilyProfile& fam, int base) {
    const int rxN = regionCountX(), rzN = regionCountZ();
    std::vector<int> regionH(static_cast<size_t>(rxN) * rzN, base);
    std::vector<int> regionClass(static_cast<size_t>(rxN) * rzN, 0);
    for (int rz = 0; rz < rzN; ++rz)
        for (int rx = 0; rx < rxN; ++rx) {
            const size_t ri = static_cast<size_t>(rz) * rxN + rx;
            const int cls = classForRegion(spec, fam, rx, rz);
            regionClass[ri] = cls;
            const int span = kClasses[cls].relief + fam.relief;
            const uint32_t n = valueNoise16(rx, 17, rz, spec.seed ^ 0x51edu, 1);
            regionH[ri] = base + static_cast<int>(static_cast<uint64_t>(n) * span / 65536u);
        }

    std::vector<int> h(static_cast<size_t>(sim::kWorldW) * sim::kWorldD, 0);
    for (int z = 0; z < sim::kWorldD; ++z)
        for (int x = 0; x < sim::kWorldW; ++x) {
            const size_t ri = static_cast<size_t>(regionOfZ(z)) * rxN + regionOfX(x);
            const uint32_t coarse = valueNoise16(x, 3, z, spec.seed ^ 0x1111u, 8);
            const uint32_t fine = valueNoise16(x, 9, z, spec.seed ^ 0x2222u, 3);
            const int detail = static_cast<int>(static_cast<int32_t>(coarse) - 32768) / 16384 +
                               static_cast<int>(static_cast<int32_t>(fine) - 32768) / 32768;
            h[static_cast<size_t>(z) * sim::kWorldW + x] = regionH[ri] + detail;
        }

    // Talus: repeat until stable. Each column may not stand more than SLOPE
    // above any neighbour, where SLOPE is the smaller of the two classes'
    // allowances — the steeper of the pair does not get to present a wall to
    // the gentler one, and a border between two gentle classes is gentle in
    // both directions. The pass only ever lowers a column, so it cannot cycle
    // and the result does not depend on scan order.
    for (int pass = 0; pass < 8; ++pass) {
        bool changed = false;
        for (int z = 0; z < sim::kWorldD; ++z)
            for (int x = 0; x < sim::kWorldW; ++x) {
                const size_t i = static_cast<size_t>(z) * sim::kWorldW + x;
                const int here = kClasses[classForRegion(spec, fam, regionOfX(x), regionOfZ(z))].slope;
                int limit = h[i] - here;
                const auto step = [&](int nx, int nz) {
                    if (nx < 0 || nz < 0 || nx >= sim::kWorldW || nz >= sim::kWorldD) return;
                    const int there = kClasses[classForRegion(spec, fam, regionOfX(nx),
                                                              regionOfZ(nz))].slope;
                    limit = std::max(limit, h[static_cast<size_t>(nz) * sim::kWorldW + nx] -
                                             std::min(here, there));
                };
                step(x - 1, z);
                step(x + 1, z);
                step(x, z - 1);
                step(x, z + 1);
                if (h[i] > limit) { h[i] = limit; changed = true; }
            }
        if (!changed) break;
    }

    for (int i = 0; i < static_cast<int>(h.size()); ++i)
        h[i] = std::clamp(h[i], 1, sim::kWorldH - 8);
    return h;
}

// ---------------------------------------------------------------------------
// The road network
// ---------------------------------------------------------------------------

// Side bits of a road node's connectivity. A node's mask is which of its four
// region edges a road leaves by; the kind of prefab and the quarter turn it is
// stamped at both fall out of that mask.
enum RoadSide : int { kSideW = 1, kSideE = 2, kSideS = 4, kSideN = 8 };

struct RoadNode {
    int rx = 0, rz = 0;
    int mask = 0;
    int rot = 0;          // quarter turns about +Y for stampMapVox
    const char* prefab = "";
    int x0 = 0, z0 = 0;   // region origin in cells
    int level = 0;        // y of the road's top surface
};

struct PropPlacement {
    std::string prefab;
    int x = 0, y = 0, z = 0;
    int rot = 0;
};

// What shape of road a node needs. The kind, not the sides: two nodes can need
// the same asset turned differently, so the asset is authored once per kind in
// a canonical orientation and the quarter turn is derived.
enum RoadKind { kRoadNone, kRoadEnd, kRoadStraight, kRoadCorner, kRoadTee, kRoadCross };

inline int popSides(int mask) {
    int n = 0;
    for (int s = 1; s <= 8; s <<= 1) n += (mask & s) ? 1 : 0;
    return n;
}

inline RoadKind kindForMask(int mask) {
    const int n = popSides(mask);
    if (n == 0) return kRoadNone;
    if (n == 1) return kRoadEnd;
    if (n == 2) return (mask == (kSideW | kSideE) || mask == (kSideS | kSideN)) ? kRoadStraight
                                                                                  : kRoadCorner;
    if (n == 3) return kRoadTee;
    if (n == 4) return kRoadCross;
    return kRoadNone;
}

// The prefab for a kind, and the sides it presents before any rotation. Every
// shape reachable by quarter turns is generated from these, so an asset only
// has to be authored once and no orientation can be missed.
inline const char* roadPrefabForKind(RoadKind kind) {
    switch (kind) {
    case kRoadEnd:      return "road_end";
    case kRoadStraight: return "road_straight";
    case kRoadCorner:   return "road_corner";
    case kRoadTee:      return "road_tee";
    case kRoadCross:    return "road_cross";
    default:            return "";
    }
}

inline int canonicalMask(RoadKind kind) {
    switch (kind) {
    case kRoadEnd:      return kSideW;
    case kRoadStraight: return kSideW | kSideE;
    case kRoadCorner:   return kSideW | kSideN;
    case kRoadTee:      return kSideW | kSideE | kSideN;
    case kRoadCross:    return kSideW | kSideE | kSideS | kSideN;
    default:            return 0;
    }
}

inline const char* roadPrefabForMask(int mask) { return roadPrefabForKind(kindForMask(mask)); }

// The mask a canonical asset presents after `rot` quarter turns.
//
// This must be the same transform mapvox::stampMapVox applies to local (x, z),
// or a rotated road points the wrong way. Rather than restate that permutation
// as a hand-written side cycle (which is how it was wrong once already), it is
// derived from the same two operations the stampper uses: `stampLocal`, below,
// is the single definition, and the stampper is checked against it by the gate.
struct Side { int dx, dz, bit; };
static constexpr Side kSideDirs[4] = {
    {-1, 0, kSideW},
    {1, 0, kSideE},
    {0, -1, kSideS},
    {0, 1, kSideN},
};

// Where a local cell lands under `rot` quarter turns, in local coordinates of
// the turned footprint. Copy of mapvox::stampMapVox's `place`.
//
// The footprint must be odd and the cell centred on it: this is called with a
// 3x3 centre so the arithmetic is about *directions*, which is all a road mask
// needs. The stampper's mapping of a direction does not depend on the footprint
// size (each case only differences the coordinates), so a 3x3 centre gives the
// same side mapping a 64x64 road tile would.
inline void stampLocal(int sx, int sz, int lx, int lz, int rot, int& rx, int& rz) {
    switch (rot & 3) {
    case 1: rx = sz - 1 - lz; rz = lx; break;
    case 2: rx = sx - 1 - lx; rz = sz - 1 - lz; break;
    case 3: rx = lz; rz = sx - 1 - lx; break;
    default: rx = lx; rz = lz; break;
    }
}

inline int rotateMask(int mask, int rot) {
    static constexpr int kHalf = 1;  // centre of the 3x3 footprint above
    int out = 0;
    for (const auto& s : kSideDirs) {
        if (!(mask & s.bit)) continue;
        int rx = 0, rz = 0;
        stampLocal(2 * kHalf + 1, 2 * kHalf + 1, kHalf + s.dx, kHalf + s.dz, rot, rx, rz);
        const int ndx = rx - kHalf, ndz = rz - kHalf;
        for (const auto& t : kSideDirs)
            if (t.dx == ndx && t.dz == ndz) { out |= t.bit; break; }
    }
    return out;
}

// The quarter turn that turns a canonical asset into `mask`, or -1 if the
// asset's orbit does not include that shape. Rotations are tried in order, so
// the choice is deterministic and the same mask always turns the same way.
inline int rotationForMask(int mask) {
    const RoadKind kind = kindForMask(mask);
    if (kind == kRoadNone) return -1;
    const int canonical = canonicalMask(kind);
    for (int rot = 0; rot < 4; ++rot)
        if (rotateMask(canonical, rot) == mask) return rot;
    return -1;
}

// Is this cell inside the graded band of a road node? The band is the region's
// centre rows/columns, wider than the road itself (see kRoadCutHalfWidth).
inline bool inRoadCorridor(const RoadNode& node, int x, int z) {
    const int rxN = regionCountX(), rzN = regionCountZ();
    const int cx = node.x0 + kRegionCells / 2;
    const int cz = node.z0 + kRegionCells / 2;
    if (rxN == 1 && rzN == 1) return true;
    const bool alongX = (node.mask & kSideW) || (node.mask & kSideE);
    const bool alongZ = (node.mask & kSideS) || (node.mask & kSideN);
    if (alongX && alongZ) return std::abs(x - cx) <= kRoadCutHalfWidth ||
                                std::abs(z - cz) <= kRoadCutHalfWidth;
    if (alongX) return std::abs(z - cz) <= kRoadCutHalfWidth;
    if (alongZ) return std::abs(x - cx) <= kRoadCutHalfWidth;
    return false;
}

// The network: one road along +X through the middle row of regions, one down
// +Z through a seeded column, meeting at a crossroads. The layout is a function
// of the seed alone, and every node is a region the map's own size can hold.
inline std::vector<RoadNode> planRoads(const Spec& spec) {
    std::vector<RoadNode> nodes;
    if (!spec.roads) return nodes;
    const int rxN = regionCountX(), rzN = regionCountZ();
    if (rxN < 1 || rzN < 1) return nodes;
    const int rowZ = std::min(rzN - 1, std::max(0, rzN / 2));
    const int colX = spec.roadX >= 0
                         ? std::clamp(spec.roadX, 0, rxN - 1)
                         : static_cast<int>(hashCell(3, 11, 5, spec.seed ^ 0x0b0bu) %
                                            static_cast<uint32_t>(rxN));

    for (int rz = 0; rz < rzN; ++rz)
        for (int rx = 0; rx < rxN; ++rx) {
            const bool onRow = (rz == rowZ);
            const bool onCol = (rx == colX);
            if (!onRow && !onCol) continue;
            int mask = 0;
            // A road leaves a region by an edge only when the neighbour across
            // that edge is itself part of the network, so the network has no
            // stub pointing at open terrain.
            if (onRow) {
                if (rx > 0) mask |= kSideW;
                if (rx + 1 < rxN) mask |= kSideE;
            }
            if (onCol) {
                if (rz > 0) mask |= kSideS;
                if (rz + 1 < rzN) mask |= kSideN;
            }
            RoadNode n;
            n.rx = rx; n.rz = rz; n.mask = mask;
            n.x0 = rx * kRegionCells; n.z0 = rz * kRegionCells;
            n.prefab = roadPrefabForMask(mask);
            n.rot = rotationForMask(mask);
            if (n.prefab[0] == '\0' || n.rot < 0) continue; // no asset for this shape
            nodes.push_back(n);
        }
    return nodes;
}

// One level for the whole network, so a road never steps between regions: the
// highest ground any corridor crosses, plus the two cells the road surface and
// its base course need above it.
inline int roadLevel(const Spec& spec, const std::vector<RoadNode>& nodes,
                     const std::vector<int>& heights) {
    int top = 0;
    for (const auto& n : nodes)
        for (int z = n.z0; z < n.z0 + kRegionCells && z < sim::kWorldD; ++z)
            for (int x = n.x0; x < n.x0 + kRegionCells && x < sim::kWorldW; ++x)
                if (inRoadCorridor(n, x, z))
                    top = std::max(top, heights[static_cast<size_t>(z) * sim::kWorldW + x]);
    (void)spec;
    return std::min(sim::kWorldH - 4, top + 2);
}

// ---------------------------------------------------------------------------
// Writing the world
// ---------------------------------------------------------------------------

struct Report {
    int columns = 0;         // columns that received ground
    int cells = 0;           // unit cubes written
    int roadCells = 0;       // embankment + kerb footing cells written under a road
    int regions = 0;         // regions classified
    int roadNodes = 0;       // road nodes planned
    int props = 0;           // decoration prefabs placed
    int propsMissing = 0;    // decoration prefabs that would not load
    int roadLevelY = -1;     // the network's surface height
    int heightMin = 0;
    int heightMax = 0;
    int classCounts[kClassCount] = {};
    // Cells the map had already filled, which the generator left alone. This is
    // the fill-only contract working, not damage: `nonDestructive` in the gate
    // proves the authored cell survived, and this only says how often the
    // generator had to step around one.
    int authoredSkipped = 0;
    // The same thing, counted from the prefab passes (roads, decorations).
    // These are stamped in src/main.cpp's generateMapTerrain, so they live here
    // to keep the one number meaning "authored cells the generator stepped
    // around" rather than "authored cells the height field stepped around".
    int prefabDeclined = 0;
};

// Fill the world from a height field: the class's surface material on top, its
// sub material under it, and its family's own material below that. Only Air is
// written, so authored map structure survives underneath and above.
inline void fillTerrain(sim::World& world, const Spec& spec, const FamilyProfile& fam,
                        const std::vector<int>& heights, Report& rep) {
    const int rxN = regionCountX(), rzN = regionCountZ();
    for (int rz = 0; rz < rzN; ++rz)
        for (int rx = 0; rx < rxN; ++rx) ++rep.regions;

    for (int z = 0; z < sim::kWorldD; ++z)
        for (int x = 0; x < sim::kWorldW; ++x) {
            const size_t ri = static_cast<size_t>(regionOfZ(z)) * rxN + regionOfX(x);
            const int cls = classForRegion(spec, fam, regionOfX(x), regionOfZ(z));
            const int h = heights[static_cast<size_t>(z) * sim::kWorldW + x];
            if (h < 1) continue;
            rep.heightMin = rep.columns ? std::min(rep.heightMin, h) : h;
            rep.heightMax = std::max(rep.heightMax, h);
            rep.classCounts[cls] += 1;
            bool any = false;
            for (int y = 0; y <= h; ++y) {
                if (world.get(x, y, z) != sim::Block::Air) { ++rep.authoredSkipped; continue; }
                const sim::Block b = (y == h) ? kClasses[cls].surface
                                              : (y >= h - kSubDepth ? kClasses[cls].sub
                                                                    : sim::Block::Dirt);
                world.fillColumn(x, z, y, y, b);
                ++rep.cells;
                any = true;
            }
            if (any) ++rep.columns;
        }
}

// Raise the graded band under each road node to one level, so the stamped road
// surface is continuous across the whole network. Raising, never cutting.
inline void gradeRoads(sim::World& world, const std::vector<RoadNode>& nodes, int level,
                       Report& rep) {
    rep.roadLevelY = level;
    for (const auto& n : nodes) {
        for (int z = n.z0; z < n.z0 + kRegionCells && z < sim::kWorldD; ++z)
            for (int x = n.x0; x < n.x0 + kRegionCells && x < sim::kWorldW; ++x) {
                if (!inRoadCorridor(n, x, z)) continue;
                // Fill up to the level's sub-base, leaving exactly the kRoadLayers
                // cells the road prefab itself occupies. Deriving the bound from
                // the constant keeps the two in step: grading to level-2 while the
                // tile is two cells tall completes the column, and grading to
                // level-2 against a three-cell tile would not.
                for (int y = 0; y <= level - kRoadLayers; ++y) {
                    if (world.get(x, y, z) != sim::Block::Air) { ++rep.authoredSkipped; continue; }
                    world.fillColumn(x, z, y, y, sim::Block::Dirt);
                    ++rep.cells;
                    ++rep.roadCells;
                }
            }
    }
}

// Decoration prefab placements: a few per region, on that region's own surface,
// never on a road and never floating. The positions come from the seed, so they
// are part of the golden hash.
inline std::vector<PropPlacement> planProps(const Spec& spec, const FamilyProfile& fam,
                                            const std::vector<int>& heights,
                                            const std::vector<RoadNode>& nodes) {
    std::vector<PropPlacement> out;
    if (!spec.props) return out;
    const int rxN = regionCountX(), rzN = regionCountZ();
    for (int rz = 0; rz < rzN; ++rz)
        for (int rx = 0; rx < rxN; ++rx) {
            const int cls = classForRegion(spec, fam, rx, rz);
            const char* prop = kClasses[cls].prop;
            if (prop[0] == '\0') continue;
            const uint32_t count = hashCell(rx, 23, rz, spec.seed ^ 0x9999u) % 3u;
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t h = hashCell(rx * 7 + static_cast<int>(i), 29, rz, spec.seed ^ 0x9999u);
                // An 8-cell grid inside the region, kept off the region edge so
                // a prop never straddles a border.
                const int x = rx * kRegionCells + 8 + static_cast<int>(h % 48u);
                const int z = rz * kRegionCells + 8 + static_cast<int>((h >> 8) % 48u);
                if (x >= sim::kWorldW || z >= sim::kWorldD) continue;
                const int y = heights[static_cast<size_t>(z) * sim::kWorldW + x] + 1;
                if (y >= sim::kWorldH - 3) continue;
                bool onRoad = false;
                for (const auto& n : nodes)
                    if (inRoadCorridor(n, x, z)) { onRoad = true; break; }
                if (onRoad) continue;
                PropPlacement p;
                p.prefab = prop;
                p.x = x; p.y = y; p.z = z;
                p.rot = static_cast<int>((h >> 16) & 3u);
                out.push_back(p);
            }
        }
    return out;
}

// ---------------------------------------------------------------------------
// Gate support
// ---------------------------------------------------------------------------

// FNV-1a 64 over the world's occupancy, the same function the sim fingerprint
// uses. Hashing the generated grid is what makes "same seed, same world" a
// checkable claim instead of a claim.
inline uint64_t hashOccupancy(const sim::World& world) {
    uint64_t h = 1469598103934665603ull;
    const auto add = [&h](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= static_cast<uint8_t>(v >> (i * 8));
            h *= 1099511628211ull;
        }
    };
    for (int y = 0; y < sim::kWorldH; ++y)
        for (int z = 0; z < sim::kWorldD; ++z)
            for (int x = 0; x < sim::kWorldW; ++x) add(static_cast<uint8_t>(world.get(x, y, z)));
    return h;
}

} // namespace terrain
