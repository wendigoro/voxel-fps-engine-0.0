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

#include <cstdint>
#include <vector>

#include "materials.hpp"

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
    Water,
    WaterCurrent,
    Moon,
    LightBulb,
};

// Unit cube. Every solid is 1x1x1 voxels on the impact grid; no stretched
// planes. Scale comes from materials.hpp (kVoxelSize) — the single source of
// truth, deliberately not redefined here.
static constexpr float kVoxelSize = ::kVoxelSize;

static constexpr int kChunkSize = 32;                 // voxels per chunk axis
static constexpr int kChunksX = 6;                    // warehouse + river bank
static constexpr int kChunksY = 2;                    // height for walls/roof girders
static constexpr int kChunksZ = 5;                    // extended depth for river slice
static constexpr int kWorldW = kChunksX * kChunkSize; // 160
static constexpr int kWorldH = kChunksY * kChunkSize; // 64
static constexpr int kWorldD = kChunksZ * kChunkSize; // 128
static constexpr int kVoxelsPerChunk = kChunkSize * kChunkSize * kChunkSize;

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

} // namespace sim
