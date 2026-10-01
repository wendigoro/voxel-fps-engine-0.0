#pragma once
// Render classes: which layer a vertex belongs to, and so how the shaders treat
// it. Every vertex carries one (Vertex::mat, a float attribute holding the
// class number). This header is the single list; shaders/render_class.glsl
// mirrors it and scripts/check_constants.py fails the build if they differ.
//
// Layers (RULES.md, "Render layers") and the classes that draw them:
//
//   Surface      World, Water           chunk meshes derived from sent cells
//   Environment  Bulb, Moon, Sky        light fixtures and sky (Bulb/Moon are
//                                       still grid blocks until lights move to
//                                       the environment layer)
//   Effects      Debris, Muzzle         debris sub-lattice chips, muzzle flash
//   Entities     WorldPickup            world items, lit like World
//   Overlay      InventoryLattice       separate pass, cleared depth, no fisheye
//
// The numbers are part of the vertex format and of saved captures, so a class
// keeps its number forever; add new classes at the end.
//
// View-only: the simulation never reads a render class (RULES.md forbids
// deciding a simulation outcome from "shader-class ids").

#include <cstdint>

namespace rc {

enum class RenderClass : uint8_t {
    World = 0,            // lit opaque chunk surface
    Water = 1,            // tide-animated surface, still unit cells underneath
    Bulb = 2,             // warm emissive light fixture
    Moon = 3,             // moon sprite; drawn at the far plane
    Sky = 4,              // pixel sky dome; drawn at the far plane
    Debris = 5,           // cubic debris chips (effects)
    Muzzle = 6,           // muzzle flash cubes; small depth bias
    InventoryLattice = 7, // inventory overlay pass only; skips the fisheye
    WorldPickup = 8,      // world items; shaded exactly like World
    Count
};

// The value written into the vertex attribute.
constexpr float attr(RenderClass c) { return static_cast<float>(static_cast<uint8_t>(c)); }

} // namespace rc
