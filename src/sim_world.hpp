// Authoritative voxel occupancy — the simulation side of the world grid.
//
// This header is the ONLY place a client is allowed to learn occupancy from.
// The rules it exists to enforce (RULES.md, "Authority and the view/sim split"):
//
//   * The unit-cubic grid is the sole authority for occupancy, collision,
//     impact, destruction, damage, and visibility.
//   * A view never holds this grid. It holds only the cells sim::visible chose
//     to send, in a ViewChunk.
//   * No derived surface is ever fed back. sim::World can be read by anything
//     that IS the simulation; view code must go through the send path.
//
// Nothing here knows about meshes, vertex buffers, or the camera.

#pragma once

// A named guard in addition to #pragma once, so the view-isolation self-test
// (src/view_isolation_check.cpp) can detect a forbidden include. #pragma once
// is invisible to the preprocessor; this is not.
#define SIM_WORLD_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "materials.hpp"
#include "voxel_wire.hpp"

namespace sim {

// Voxel identity. Values are stable: they are the wire representation and are
// shared with the painter's block table, so never renumber them.
enum class Block : uint8_t {
    Air = 0,
    Dirt,
    Concrete,
    SheetMetal,
    Girder,
    Wood,
    WoodDark,
    Water,        // still unit cubes; may occupy multi-cell clumps
    WaterCurrent, // moving water source (same visual, current sampling)
    Moon,         // cool emissive crescent grid
    LightBulb     // warm emissive indoor bulbs
};

// Agreement with the wire is asserted, never assumed. A view reads
// wire::BlockId; if the two enums ever drift, a client silently renders the
// wrong voxel. Failing at compile time is the only cheap moment to catch that.
static_assert(static_cast<uint8_t>(Block::Air) == static_cast<uint8_t>(wire::BlockId::Air));
static_assert(static_cast<uint8_t>(Block::Dirt) == static_cast<uint8_t>(wire::BlockId::Dirt));
static_assert(static_cast<uint8_t>(Block::Concrete) == static_cast<uint8_t>(wire::BlockId::Concrete));
static_assert(static_cast<uint8_t>(Block::SheetMetal) == static_cast<uint8_t>(wire::BlockId::SheetMetal));
static_assert(static_cast<uint8_t>(Block::Girder) == static_cast<uint8_t>(wire::BlockId::Girder));
static_assert(static_cast<uint8_t>(Block::Wood) == static_cast<uint8_t>(wire::BlockId::Wood));
static_assert(static_cast<uint8_t>(Block::WoodDark) == static_cast<uint8_t>(wire::BlockId::WoodDark));
static_assert(static_cast<uint8_t>(Block::Water) == static_cast<uint8_t>(wire::BlockId::Water));
static_assert(static_cast<uint8_t>(Block::WaterCurrent) == static_cast<uint8_t>(wire::BlockId::WaterCurrent));
static_assert(static_cast<uint8_t>(Block::Moon) == static_cast<uint8_t>(wire::BlockId::Moon));
static_assert(static_cast<uint8_t>(Block::LightBulb) == static_cast<uint8_t>(wire::BlockId::LightBulb));

// Unit cube. Every solid is 1x1x1 voxels on the impact grid; no stretched
// planes. Scale comes from materials.hpp (kVoxelSize) — the single source of
// truth, deliberately not redefined here.
static constexpr float kVoxelSize = ::kVoxelSize;

static constexpr int kChunkSize = wire::kChunkSize;
static constexpr int kChunksX = wire::kChunksX;
static constexpr int kChunksY = wire::kChunksY;
static constexpr int kChunksZ = wire::kChunksZ;
static constexpr int kWorldW = wire::kWorldW;
static constexpr int kWorldH = wire::kWorldH;
static constexpr int kWorldD = wire::kWorldD;
static constexpr int kVoxelsPerChunk = kChunkSize * kChunkSize * kChunkSize;

// A skirted snapshot is the chunk plus a 1-cell shell, so a view can answer
// edge-face questions on its own. Stripping the shell must recover the chunk.
static_assert(kVoxelsPerChunk ==
                  static_cast<int>((wire::kSlice - 2 * wire::kSkirt) *
                                   (wire::kSlice - 2 * wire::kSkirt) *
                                   (wire::kSlice - 2 * wire::kSkirt)),
              "stripping the skirt must recover exactly the chunk volume");
static_assert(wire::kSkirt >= 1, "a skirt of 0 would force the view to ask the sim about edges");

// The 6 unit-cube face directions, shared by the occupancy writer and the
// mesher so they can never disagree about which neighbours matter.
static constexpr int kFaceOX[6] = {1, -1, 0, 0, 0, 0};
static constexpr int kFaceOY[6] = {0, 0, 1, -1, 0, 0};
static constexpr int kFaceOZ[6] = {0, 0, 0, 0, 1, -1};

// One chunk of authoritative occupancy. No mesh state, no camera, no view data.
struct Chunk {
    int cx = 0, cy = 0, cz = 0;
    std::vector<Block> voxels;  // kChunkSize^3

    // Bumped whenever occupancy changes, including changes that only affect a
    // NEIGHBOUR's exposed faces. The view compares this against the version it
    // last meshed to decide whether its snapshot is stale — so the sim never
    // has to reach into view state to tell it to re-mesh.
    uint32_t version = 0;
};

struct World {
    std::vector<Chunk> chunks;

    void alloc() { chunks.resize(static_cast<size_t>(kChunksX) * kChunksY * kChunksZ); }
    static constexpr int chunkCount() { return kChunksX * kChunksY * kChunksZ; }

    static inline int chunkIndex(int cx, int cy, int cz) {
        return (cy * kChunksZ + cz) * kChunksX + cx;
    }
    static inline int localIndex(int lx, int ly, int lz) {
        return (ly * kChunkSize + lz) * kChunkSize + lx;
    }

    static inline bool inBounds(int x, int y, int z) {
        return x >= 0 && y >= 0 && z >= 0 && x < kWorldW && y < kWorldH && z < kWorldD;
    }

    // Out-of-world reads are Air. This is a deliberate rule: the mesher relies on
    // it to close the world shell without a special case at the border.
    Block get(int x, int y, int z) const {
        if (!inBounds(x, y, z)) return Block::Air;
        return chunks[chunkIndex(x / kChunkSize, y / kChunkSize, z / kChunkSize)]
            .voxels[localIndex(x % kChunkSize, y % kChunkSize, z % kChunkSize)];
    }

    // Write occupancy and dirty every chunk whose exposed faces this changes.
    //
    // The neighbour marking is not an optimisation detail — it is correctness.
    // A mesher culls each face against its 6 neighbours, so clearing a cell on
    // a chunk seam changes which faces the NEIGHBOURING chunk shows. Marking
    // only the owning chunk leaves a stale interior face floating inside a new
    // hole, which is a rendering corruption a client could otherwise be blamed
    // for. Bumping the neighbour versions (not just the owner) is what makes
    // "send me the cells that changed" correct at the seam.
    void set(int x, int y, int z, Block b) {
        if (!inBounds(x, y, z)) return;
        Chunk& owner = chunks[chunkIndex(x / kChunkSize, y / kChunkSize, z / kChunkSize)];
        owner.voxels[localIndex(x % kChunkSize, y % kChunkSize, z % kChunkSize)] = b;
        ++owner.version;
        for (int f = 0; f < 6; ++f) {
            const int nx = x + kFaceOX[f], ny = y + kFaceOY[f], nz = z + kFaceOZ[f];
            if (!inBounds(nx, ny, nz)) continue;
            Chunk& nb = chunks[chunkIndex(nx / kChunkSize, ny / kChunkSize, nz / kChunkSize)];
            ++nb.version;
        }
    }

    Block& ref(int x, int y, int z) {
        return chunks[chunkIndex(x / kChunkSize, y / kChunkSize, z / kChunkSize)]
            .voxels[localIndex(x % kChunkSize, y % kChunkSize, z % kChunkSize)];
    }
};

namespace visible {

// Authoritative per-client visible set (RULES.md, "Visibility filtering (anti-cheat)").
// The simulation computes this set; a client view is sent ONLY chunks in this set.
struct VisibleSet {
    uint64_t chunkMask = 0;

    bool isChunkVisible(int cx, int cy, int cz) const {
        if (cx < 0 || cx >= kChunksX || cy < 0 || cy >= kChunksY || cz < 0 || cz >= kChunksZ)
            return false;
        int idx = World::chunkIndex(cx, cy, cz);
        return (chunkMask & (1ULL << idx)) != 0;
    }

    void markVisible(int cx, int cy, int cz) {
        if (cx < 0 || cx >= kChunksX || cy < 0 || cy >= kChunksY || cz < 0 || cz >= kChunksZ)
            return;
        int idx = World::chunkIndex(cx, cy, cz);
        chunkMask |= (1ULL << idx);
    }

    int visibleCount() const {
        int count = 0;
        for (int i = 0; i < World::chunkCount(); ++i) {
            if (chunkMask & (1ULL << i)) ++count;
        }
        return count;
    }
};

// Compute authoritative client visible set.
// Uses player eye position, camera forward vector, and an air ray budget.
// Chunks not reached by air rays / outside the view cone remain occluded (anti-ESP).
inline VisibleSet computeVisibleSet(const World& world,
                                    float eyeX, float eyeY, float eyeZ,
                                    float fwdX, float fwdY, float fwdZ) {
    VisibleSet vis;
    const int ecx = std::clamp(static_cast<int>(eyeX / (kChunkSize * kVoxelSize)), 0, kChunksX - 1);
    const int ecy = std::clamp(static_cast<int>(eyeY / (kChunkSize * kVoxelSize)), 0, kChunksY - 1);
    const int ecz = std::clamp(static_cast<int>(eyeZ / (kChunkSize * kVoxelSize)), 0, kChunksZ - 1);

    // 1. The chunk holding the eye is always visible.
    vis.markVisible(ecx, ecy, ecz);

    // 2. Immediate 1-chunk neighborhood around the player (immediate collision/feel zone).
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                vis.markVisible(ecx + dx, ecy + dy, ecz + dz);
            }
        }
    }

    // 3. Air ray budgeting: cast rays covering the fisheye view cone.
    const float fwdLen = std::sqrt(fwdX * fwdX + fwdY * fwdY + fwdZ * fwdZ);
    float fx = (fwdLen > 1e-5f) ? (fwdX / fwdLen) : 0.0f;
    float fy = (fwdLen > 1e-5f) ? (fwdY / fwdLen) : 0.0f;
    float fz = (fwdLen > 1e-5f) ? (fwdZ / fwdLen) : 1.0f;

    // Camera ray basis
    float rx = -fz, ry = 0.0f, rz = fx;
    float rlen = std::sqrt(rx * rx + rz * rz);
    if (rlen < 1e-5f) { rx = 1.0f; rz = 0.0f; }
    else { rx /= rlen; rz /= rlen; }
    float ux = ry * fz - rz * fy;
    float uy = rz * fx - rx * fz;
    float uz = rx * fy - ry * fx;

    const int kRaysYaw = 24;
    const int kRaysPitch = 12;
    const float kMaxDistMeters = 0.30f;
    const float kStepMeters = kVoxelSize * 1.5f;
    const int kMaxSteps = static_cast<int>(kMaxDistMeters / kStepMeters);

    for (int py = 0; py < kRaysPitch; ++py) {
        float vAngle = -0.85f + 1.70f * (static_cast<float>(py) / (kRaysPitch - 1));
        for (int px = 0; px < kRaysYaw; ++px) {
            float hAngle = -1.60f + 3.20f * (static_cast<float>(px) / (kRaysYaw - 1));

            float dx = fx + rx * hAngle + ux * vAngle;
            float dy = fy + ry * hAngle + uy * vAngle;
            float dz = fz + rz * hAngle + uz * vAngle;
            float dlen = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dlen < 1e-5f) continue;
            dx /= dlen; dy /= dlen; dz /= dlen;

            float curX = eyeX;
            float curY = eyeY;
            float curZ = eyeZ;

            for (int s = 0; s < kMaxSteps; ++s) {
                curX += dx * kStepMeters;
                curY += dy * kStepMeters;
                curZ += dz * kStepMeters;

                int gx = static_cast<int>(curX / kVoxelSize);
                int gy = static_cast<int>(curY / kVoxelSize);
                int gz = static_cast<int>(curZ / kVoxelSize);

                if (!world.inBounds(gx, gy, gz)) {
                    break;
                }

                int cx = gx / kChunkSize;
                int cy = gy / kChunkSize;
                int cz = gz / kChunkSize;
                vis.markVisible(cx, cy, cz);

                Block b = world.get(gx, gy, gz);
                if (b != Block::Air && b != Block::Water && b != Block::WaterCurrent) {
                    // Hit opaque solid block: this chunk surface was seen, but light/vision cannot pass through.
                    break;
                }
            }
        }
    }

    return vis;
}

} // namespace visible

} // namespace sim
