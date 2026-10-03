// View-side chunk state — what a client is allowed to know about the world.
//
// A ViewChunk holds NO authoritative grid. It holds exactly the cells the
// simulation chose to send, plus the mesh built from them. The view cannot
// derive occupancy it was not shown, so it cannot leak or invent it.
//
// This header includes voxel_wire.hpp and nothing else. It does NOT include
// sim_world.hpp, on purpose: a view client compiled against this header has no
// path to sim::World even accidentally. That is the structural half of the
// split — the send path in main.cpp is the other half.
//
// The skirt (1 cell of padding on every side) is what lets the mesher decide
// whether a face is exposed WITHOUT reading the sim grid. Face culling asks
// "is my neighbour empty?" — that neighbour is exactly one cell outside the
// chunk, so the sim sends it too. Without the skirt the view would have to
// consult the world to answer, which is precisely the coupling being removed.

#pragma once

// A view client may include this header and voxel_wire.hpp. It must never
// include sim_world.hpp — see src/view_isolation_check.cpp, which fails to
// compile if it does.
#define VIEW_CHUNK_HPP

#include <cstdint>
#include <vector>

#include "voxel_wire.hpp"

namespace view {

// Re-exported so view code writes view::kSkirt while the values stay owned by
// the single wire definition.
static constexpr int kSkirt = wire::kSkirt;
static constexpr int kSlice = wire::kSlice;
static constexpr int kSkirtCells = wire::kSkirtCells;

// The cells the sim sent for one chunk, with skirt. Read via skirtIndex/inSkirt
// so a caller cannot walk off the edge without noticing.
struct SentCells {
    std::vector<wire::BlockCell> cells;  // kSkirtCells, skirted

    void alloc() { cells.assign(kSkirtCells, wire::BlockCell{}); }

    static inline int skirtIndex(int lx, int ly, int lz) { return wire::kSkirtIndex(lx, ly, lz); }
    static inline bool inSkirt(int lx, int ly, int lz) { return wire::kInSkirt(lx, ly, lz); }

    wire::BlockId get(int lx, int ly, int lz) const {
        if (!inSkirt(lx, ly, lz)) return wire::BlockId::Air;  // world edge
        return static_cast<wire::BlockId>(cells[skirtIndex(lx, ly, lz)].id);
    }
    void set(int lx, int ly, int lz, wire::BlockId b) {
        if (!inSkirt(lx, ly, lz)) return;
        cells[skirtIndex(lx, ly, lz)].id = static_cast<uint8_t>(b);
    }
    // Palette index the sim sent for a cell; 0 = the material's own colour.
    uint8_t appearance(int lx, int ly, int lz) const {
        if (!inSkirt(lx, ly, lz)) return 0;
        return cells[skirtIndex(lx, ly, lz)].appear;
    }
    void setAppearance(int lx, int ly, int lz, uint8_t a) {
        if (!inSkirt(lx, ly, lz)) return;
        cells[skirtIndex(lx, ly, lz)].appear = a;
    }
};

// One chunk as the view sees it: the cells it was sent, the mesh built from
// those cells, and where that mesh lives in the shared vertex buffer.
struct ViewChunk {
    int cx = 0, cy = 0, cz = 0;

    // Occupancy exactly as sent. The only block data the view may consult.
    SentCells sent;

    // The map palette the cells' appearance indices refer to (owned by the
    // view, sent once with the map). Null means every cell uses its material
    // colour.
    const wire::Palette* palette = nullptr;

    // Block id -> texture layer + 1 (0 = untextured), owned by the view and
    // resolved against the loaded texture set (src/textures.hpp). Null means
    // no textures.
    const uint8_t* texLayerPlus1 = nullptr;

    // Mesh built purely from `sent`. No view code may look at sim::World here.
    struct Vertex {
        float x = 0, y = 0, z = 0;
        float nx = 0, ny = 0, nz = 0;
        float r = 0, g = 0, b = 0;
        float mat = 0;
        float texLayer = 0;  // texture layer + 1; 0 = untextured
        float painted = 0;   // 1 when the cell has an appearance (palette) colour
        float shade = 1;     // face shade x corner AO already folded into r,g,b
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
