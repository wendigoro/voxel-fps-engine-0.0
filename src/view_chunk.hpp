// View-side chunk state — what a client is allowed to know about the world.
//
// A ViewChunk holds NO authoritative grid. It holds exactly the cells the
// simulation chose to send (see sim::World::set and sim::visible), plus the
// mesh built from them. The view cannot derive occupancy it was not shown, so
// it cannot leak or invent it.
//
// The skirt (1 cell of padding on every side) is what lets the mesher decide
// whether a face is exposed WITHOUT reading the sim grid. Face culling asks
// "is my neighbour empty?" — that neighbour is exactly one cell outside the
// chunk, so the sim sends it too. Without the skirt the view would have to
// consult the world to answer, which is precisely the coupling being removed.

#pragma once

#include <cstdint>
#include <vector>

#include "sim_world.hpp"

namespace view {

// 1 cell of padding on every side, so a chunk can answer "is the neighbour to
// my edge solid?" from its own data.
static constexpr int kSkirt = 1;
static constexpr int kSlice = sim::kChunkSize + 2 * kSkirt;   // 34
static constexpr int kSkirtCells = kSlice * kSlice * kSlice;  // 39304

// A block as it travels over the wire. Fixed-width and POD: no pointers, no
// size_t, no std::string, so the same bytes go to a shared-memory view and a
// UDP view without a translation layer.
struct WireBlock {
    uint8_t id = 0;  // sim::Block
};

// The cells the sim sent for one chunk, with skirt. Index with skirtAt/skirtIndex.
struct SentCells {
    std::vector<WireBlock> cells;  // kSkirtCells, skirted

    void alloc() { cells.assign(kSkirtCells, WireBlock{}); }

    // Local coords are -1 .. kChunkSize (inclusive) after skirting.
    static inline int skirtIndex(int lx, int ly, int lz) {
        return ((ly + kSkirt) * kSlice + (lz + kSkirt)) * kSlice + (lx + kSkirt);
    }
    static inline bool inSkirt(int lx, int ly, int lz) {
        return lx >= -kSkirt && ly >= -kSkirt && lz >= -kSkirt &&
               lx < static_cast<int>(sim::kChunkSize) + kSkirt &&
               ly < static_cast<int>(sim::kChunkSize) + kSkirt &&
               lz < static_cast<int>(sim::kChunkSize) + kSkirt;
    }
};

// One chunk as the view sees it: the cells it was sent, the mesh built from
// those cells, and where that mesh lives in the shared vertex buffer.
struct ViewChunk {
    int cx = 0, cy = 0, cz = 0;

    // Occupancy exactly as sent. The only block data the view may consult.
    SentCells sent;

    // Mesh built purely from `sent`. No view code may look at sim::World here.
    struct Vertex {
        float x = 0, y = 0, z = 0;
        float nx = 0, ny = 0, nz = 0;
        float r = 0, g = 0, b = 0;
        float mat = 0;
    };
    std::vector<Vertex> mesh;

    // Sim version this mesh was built from. When sim bumps a chunk's version
    // (including for a neighbour-only change) the view sees the mismatch and
    // requests a fresh snapshot instead of guessing.
    uint32_t meshedVersion = 0;
    bool hasSnapshot = false;

    // View-buffer bookkeeping.
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    uint32_t slotCapacity = 0;
    bool wasVisible = true;

    bool snapshotStale(uint32_t simVersion) const {
        return !hasSnapshot || meshedVersion != simVersion;
    }
};

} // namespace view
