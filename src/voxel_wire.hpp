// The wire contract — the shared vocabulary between sim and view.
//
// This header is the ONLY thing a view client is allowed to include to
// understand world data. It deliberately contains no World type, no
// occupancy accessor, and no mesh code. A view compiled against this header
// alone cannot read the authoritative grid, because the grid is not here.
//
// Keeping the wire types independent of the sim's internal enum matters for a
// second reason: the sim is then free to reorder or extend sim::Block without
// silently changing what clients receive. Agreement is not assumed, it is
// asserted — sim_world.hpp static_asserts its enum against this one, so a
// mismatch is a compile error rather than a client seeing the wrong voxel.
//
// Transport rules (RULES.md):
//   * Fixed-width POD only. No pointers, no size_t, no std::string.
//   * Identical bytes to a shared-memory view and a UDP view; no
//     per-transport translation layer.
//   * A client is sent the cells it may see, and nothing else. There is no
//     field here that could carry occupancy a client was not shown.

#pragma once

#include <cstdint>

namespace wire {

// Grid dimensions, as the view must know them to lay out its own buffers.
// sim_world.hpp static_asserts its own dimensions against these.
static constexpr int kChunkSize = 32;                 // voxels per chunk axis
static constexpr int kChunksX = 6;                    // warehouse + river bank
static constexpr int kChunksY = 2;                    // height for walls/roof girders
static constexpr int kChunksZ = 5;                    // extended depth for river slice
static constexpr int kWorldW = kChunksX * kChunkSize; // 160
static constexpr int kWorldH = kChunksY * kChunkSize; // 64
static constexpr int kWorldD = kChunksZ * kChunkSize; // 128

// Block identity as it travels. Values are stable and are the wire
// representation shared with the painter's block table — never renumber.
// sim::Block must match these exactly; sim_world.hpp asserts it.
enum class BlockId : uint8_t {
    Air = 0,
    Dirt = 1,
    Concrete = 2,
    SheetMetal = 3,
    Girder = 4,
    Wood = 5,
    WoodDark = 6,
    Water = 7,        // still unit cubes; may occupy multi-cell clumps
    WaterCurrent = 8, // moving water source (same visual, current sampling)
    Moon = 9,         // cool emissive crescent grid
    LightBulb = 10,   // warm emissive indoor bulbs
    // Ground materials. Appended for generated terrain (src/terrain.hpp) and
    // painted ground; all solid, all unit cubes like every other block.
    Sand = 11,
    Grass = 12,
    Snow = 13,
    Asphalt = 14,
};

// 1 cell of padding on every side, so a view can decide whether a face on its
// chunk boundary is exposed without asking the sim anything. A view that
// lacked this would have to query the world to mesh, which is the coupling
// this whole arrangement exists to remove.
static constexpr int kSkirt = 1;
static constexpr int kSlice = kChunkSize + 2 * kSkirt;  // 34
static constexpr int kSkirtCells = kSlice * kSlice * kSlice; // 39304

// One block as it appears in a snapshot. Trivially copyable on purpose: this
// is memcpy'd into shared memory and, later, appended to a UDP packet.
// One cell on the wire: what occupies it, and how it is painted. `appear` is
// an index into the map palette (Palette below); 0 means "the material's own
// colour". Appearance is display data: the simulation never reads it back.
struct BlockCell {
    uint8_t id = 0;      // wire::BlockId
    uint8_t appear = 0;  // palette index, 0 = material default
};

static_assert(sizeof(BlockCell) == 2, "BlockCell is two bytes on the wire: block id + appearance");

// The map palette, sent once with the map. Entry 0 is never used (it means
// "material default"); entries 1..255 are authored colours.
struct PaletteColor {
    uint8_t r = 0, g = 0, b = 0;
};
static constexpr int kPaletteSize = 256;
struct Palette {
    PaletteColor colors[kPaletteSize];
    int used = 1; // entries in use, counting the reserved 0
};

// Local cell coordinates as a view addresses its own snapshot. Skirted, so
// valid range is -1 .. kChunkSize inclusive on each axis.
static constexpr int kSkirtIndex(int lx, int ly, int lz) {
    return ((ly + kSkirt) * kSlice + (lz + kSkirt)) * kSlice + (lx + kSkirt);
}

static constexpr bool kInSkirt(int lx, int ly, int lz) {
    return lx >= -kSkirt && ly >= -kSkirt && lz >= -kSkirt &&
           lx < kChunkSize + kSkirt && ly < kChunkSize + kSkirt && lz < kChunkSize + kSkirt;
}

static_assert(kSkirtIndex(kChunkSize, kChunkSize, kChunkSize) < kSkirtCells,
              "a chunk-edge cell must still land inside the snapshot");
static_assert(!kInSkirt(kChunkSize + 1, 0, 0), "one past the skirt must be rejected");
static_assert(!kInSkirt(-kSkirt - 1, 0, 0), "one before the skirt must be rejected");

} // namespace wire
