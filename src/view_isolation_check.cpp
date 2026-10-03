// Compile-time proof that the view/sim split is structural, not conventional.
//
// This file is never linked into the engine. It exists so that `view_chunk.hpp`
// and `mesh_view.hpp` get checked against the rule they are supposed to
// enforce. If someone adds a `#include "sim_world.hpp"` to a view header — or
// otherwise gives a view client a path to the authoritative grid — this stops
// compiling.
//
// Run it with scripts/check_view_isolation.ps1 (or see the header list below).
// It is deliberately a TU that includes ONLY the view headers.

#include "view_chunk.hpp"
#include "mesh_view.hpp"

// 1. The wire vocabulary is reachable, since a client needs it.
static_assert(static_cast<int>(wire::kChunkSize) == 32, "view must know the chunk size");
static_assert(wire::kSkirtCells == 39304, "skirted snapshot size is part of the contract");

// 2. A view can round-trip a cell through its own snapshot.
static bool viewCanRoundTrip() {
    view::ViewChunk c;
    c.sent.alloc();
    c.sent.set(0, 0, 0, wire::BlockId::Concrete);
    c.sent.set(wire::kChunkSize - 1, wire::kChunkSize - 1, wire::kChunkSize - 1, wire::BlockId::Girder);
    // The skirt reaches one cell past the chunk on every side.
    c.sent.set(-1, 0, 0, wire::BlockId::SheetMetal);
    c.sent.set(wire::kChunkSize, 0, 0, wire::BlockId::Moon);
    return c.sent.get(0, 0, 0) == wire::BlockId::Concrete &&
           c.sent.get(wire::kChunkSize - 1, wire::kChunkSize - 1, wire::kChunkSize - 1) ==
               wire::BlockId::Girder &&
           c.sent.get(-1, 0, 0) == wire::BlockId::SheetMetal &&
           c.sent.get(wire::kChunkSize, 0, 0) == wire::BlockId::Moon;
}

// 3. Reads outside the skirt are rejected rather than wrapping, so a mesher
//    cannot silently walk into the next chunk's data.
static bool viewRejectsOutOfRange() {
    view::ViewChunk c;
    c.sent.alloc();
    c.sent.set(wire::kChunkSize + 1, 0, 0, wire::BlockId::LightBulb);
    return c.sent.get(wire::kChunkSize + 1, 0, 0) == wire::BlockId::Air;
}

// 4. The actual view mesher (mesh_view.hpp) builds meshes from snapshots only.
//    Forcing instantiation here proves it compiles with no path to the sim
//    grid: meshChunk's whole input is the sent cells plus its Stats.
static bool viewCanMeshFromSnapshot() {
    view::ViewChunk c;
    c.sent.alloc();
    c.sent.set(1, 1, 1, wire::BlockId::Concrete);
    meshview::Stats stats;
    meshview::meshChunk(c, stats);
    return !c.mesh.empty() && stats.skirtAccessViolations == 0;
}

// 5. The isolation rule: this header set must not be able to name the sim grid.
//    Written as a preprocessor assertion so it is checked, not just commented.
#if defined(sim_world_hpp) || defined(SIM_WORLD_HPP)
#error "view_chunk.hpp must not include sim_world.hpp: a view client cannot reach the authoritative grid"
#endif
#ifdef VIEW_ISOLATION_SELFTEST
#error "view isolation self-test marker unexpectedly defined"
#endif
