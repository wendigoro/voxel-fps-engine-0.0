#pragma once
// MAP-mode voxfmt loader — the first engine consumer of the map format.
//
// Reads a data/voxfmt/*.vox.json document authored in "map" mode (schema.md,
// "Map mode") and turns its voxel grid into authoritative sim::World occupancy
// (RULES.md: the unit-cubic grid is the sole authority; a map can never
// introduce stretched or non-grid geometry).
//
// What the loader enforces, per the schema contract:
//   * format_version > 1 is refused (readers refuse what they don't understand).
//   * unit must be exactly 1 and voxel_size exactly kVoxelSize (0.001) — the
//     non-negotiable cubic-unit grid.
//   * mode must be "map".
//   * id_counters are restored to max(persisted, maxNumericSuffix(ids) + 1) so a
//     document written before counters existed, or one edited to add a
//     high-numbered entity, still cannot collide (schema.md "id_counters").
//   * a painter material with no sim::Block cup (bush_leaves, plexiglass,
//     carbon_fiber, treated_wood, custom) is dropped and counted, never
//     approximated into a wrong block.
//
// Pure text parsing only: no clocks, no randomness, no iteration-order
// dependence — safe for the fixed-timestep sim. Entity coordinates and voxel
// coordinates are integer cells on the same grid the world uses.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "jsonx.hpp"
#include "materials.hpp"
#include "sim_world.hpp"

namespace mapvox {

struct IdCounters {
    int evt = 0;   // next scripted_event id
    int npc = 0;   // next npc id
    int route = 0; // next patrol_route id
};

struct ScriptedEvent {
    int x = 0, y = 0, z = 0;
    std::string id, name, script, trigger;
    float radius = 0.0f;
    int cooldown = 0; // ticks (sim_hz is fixed at 120)
    std::string requiredSignal, emitSignal, condition;
    int repeat = 0;      // fire limit (meaningful when repeatSet)
    bool repeatSet = false; // absent (unlimited) vs authored
    bool enabled = false;
};

struct Npc {
    int x = 0, y = 0, z = 0;
    std::string id, name, npcType, aiProfile, patrolRoute;
    float health = 0.0f, maxHealth = 0.0f, speed = 0.0f, viewDist = 0.0f, viewAngle = 0.0f;
    std::string faction, dialogue, inventory;
    bool isStatic = false;
    int spawnTick = 0;
    std::string spawnCondition;
    bool enabled = false;
};

struct PatrolNode {
    int x = 0, y = 0, z = 0;
    float wait = 0.0f;
    std::string action;
};

struct PatrolRoute {
    std::string id, name;
    bool loop = false;
    std::vector<PatrolNode> nodes;
};

// Where the player stands on (re)spawn: the cell the feet occupy, and the
// facing. Authored on the map, so respawn never has to search the grid.
struct PlayerSpawn {
    bool present = false;
    int x = 0, y = 0, z = 0;
    float yaw = 0.0f, pitch = 0.0f;
};

// A world item resting on a cell. `item` is an id from data/items.
struct PickupPlacement {
    std::string item;
    int x = 0, y = 0, z = 0;
    int rot = 0; // quarter turns about +Y
};

// A light source (environment layer). It is not occupancy: it never blocks,
// collides or breaks. Position is in cell units, so (x + 0.5) is a cell centre.
struct LightPlacement {
    std::string kind;            // "bulb" today; the fixture model to draw
    float x = 0, y = 0, z = 0;   // cells
    float r = 1, g = 1, b = 1;   // linear colour
    float intensity = 1.0f;
    float radius = 0.06f;        // world units
};

// Map-wide environment (sky, moon).
struct Environment {
    bool present = false;
    float moonDir[3] = {0.32f, 0.82f, -0.48f}; // direction toward the moon
};

struct Voxel {
    int x = 0, y = 0, z = 0;
    sim::Block block = sim::Block::Air; // occupancy stamped into the world
    uint32_t rgb = 0;                   // painter color (metadata only; the sim grid has no per-cell color)
};

struct Doc {
    bool ok = false;                       // parse succeeded and passed every gate
    bool fileFound = false;                // file existed and was read (set by loadMapVox)
    std::string error;                     // first refusal reason
    int formatVersion = 0;
    bool unitOk = false;
    bool voxelSizeOk = false;
    bool modeMap = false;
    int sx = 0, sy = 0, sz = 0;            // dims (painter work area, cells)
    IdCounters persisted;                  // counters as authored in the file
    IdCounters restored;                   // max(persisted, maxSuffix+1) per family
    std::vector<ScriptedEvent> events;
    std::vector<Npc> npcs;
    std::vector<PatrolRoute> routes;
    std::vector<Voxel> voxels;
    int dropped = 0;                       // voxels skipped (no sim::Block cup)
    PlayerSpawn spawn;                     // "player_spawn" (optional)
    std::vector<LightPlacement> lights;    // "lights" (optional)
    Environment environment;               // "environment" (optional)
    std::vector<PickupPlacement> pickups;  // "pickups" (optional)
    // "cells_rle": run-length occupancy for whole maps. A palette of block
    // names, then runs as flat [y, z, x0, length, paletteIndex] quintuples
    // along +X. Cells not covered by a run are Air.
    std::vector<sim::Block> rlePalette;
    std::vector<int> rleRuns;
    int rleCells = 0;                      // cells the runs cover
    // "appearance": the map palette (0xRRGGBB, entry i is palette index i+1)
    // and run-length paint as [y, z, x0, length, paletteIndex] quintuples.
    std::vector<uint32_t> appearPalette;
    std::vector<int> appearRuns;
    int appearCells = 0;
};

// Every sim::Block by name, both directions. Painter materials map through
// blockFromMaterialName below; these extra names (water_current, light_bulb,
// moon) are engine blocks the painter cannot author, used by run-length maps.
inline const char* blockName(sim::Block b) {
    switch (b) {
    case sim::Block::Air: return "air";
    case sim::Block::Dirt: return "dirt";
    case sim::Block::Concrete: return "concrete";
    case sim::Block::SheetMetal: return "sheet_metal";
    case sim::Block::Girder: return "girder";
    case sim::Block::Wood: return "wood";
    case sim::Block::WoodDark: return "bush_branch";
    case sim::Block::Water: return "water";
    case sim::Block::WaterCurrent: return "water_current";
    case sim::Block::Moon: return "moon";
    case sim::Block::LightBulb: return "light_bulb";
    }
    return "air";
}

// Painter material name -> sim::Block. Only materials with a real occupancy cup
// map; anything else leaves `mapped` untouched and returns false (dropped).
inline bool blockFromMaterialName(const std::string& name, sim::Block& mapped) {
    if (name == "air") { mapped = sim::Block::Air; return true; }
    if (name == "wood") { mapped = sim::Block::Wood; return true; }
    if (name == "concrete") { mapped = sim::Block::Concrete; return true; }
    if (name == "dirt") { mapped = sim::Block::Dirt; return true; }
    if (name == "bush_branch") { mapped = sim::Block::WoodDark; return true; } // blockMaterial(WoodDark)=BushBranch round-trips
    if (name == "sheet_metal") { mapped = sim::Block::SheetMetal; return true; }
    if (name == "girder") { mapped = sim::Block::Girder; return true; }
    if (name == "water") { mapped = sim::Block::Water; return true; }
    if (name == "water_current") { mapped = sim::Block::WaterCurrent; return true; }
    if (name == "light_bulb") { mapped = sim::Block::LightBulb; return true; }
    if (name == "moon") { mapped = sim::Block::Moon; return true; }
    return false; // bush_leaves / plexiglass / carbon_fiber / treated_wood / custom
}

// Numeric suffix helper for id restoration: "evt_1" with prefix "evt_" -> 1.
// Returns the largest suffix seen for the family, or 0 when none matches.
inline int maxNumericSuffix(const std::vector<std::string>& ids, const char* prefix) {
    const size_t plen = std::char_traits<char>::length(prefix);
    int best = 0;
    for (const auto& id : ids) {
        if (id.size() <= plen || id.compare(0, plen, prefix) != 0) continue;
        const char* p = id.c_str() + plen;
        char* endp = nullptr;
        const long v = std::strtol(p, &endp, 10);
        if (endp && endp != p && *endp == '\0' && v > 0 && v > best) best = static_cast<int>(v);
    }
    return best;
}

// Restore id counters: max(authored counter, max suffix among existing ids + 1).
inline void restoreCounters(const Doc& doc, IdCounters& out) {
    std::vector<std::string> evtIds, npcIds, routeIds;
    for (const auto& e : doc.events) evtIds.push_back(e.id);
    for (const auto& n : doc.npcs) npcIds.push_back(n.id);
    for (const auto& r : doc.routes) routeIds.push_back(r.id);
    out.evt = doc.persisted.evt > maxNumericSuffix(evtIds, "evt_") + 1
                  ? doc.persisted.evt
                  : maxNumericSuffix(evtIds, "evt_") + 1;
    out.npc = doc.persisted.npc > maxNumericSuffix(npcIds, "npc_") + 1
                  ? doc.persisted.npc
                  : maxNumericSuffix(npcIds, "npc_") + 1;
    out.route = doc.persisted.route > maxNumericSuffix(routeIds, "route_") + 1
                    ? doc.persisted.route
                    : maxNumericSuffix(routeIds, "route_") + 1;
}

inline int jsonInt(const std::string& obj, const char* key) {
    return static_cast<int>(jsonExtractFloat(obj, key, 0.0f));
}

// Parse a MAP-mode document from text (pure; the smoke feeds crafted text to
// prove refusals without touching disk). Fills `doc` and returns doc.ok.
inline bool parseMapVox(const std::string& text, Doc& doc) {
    doc = Doc{};

    const float formatVersion = jsonExtractFloat(text, "format_version", 1.0f);
    if (formatVersion > 1.0001f) {
        doc.error = "unsupported format_version " + std::to_string(formatVersion);
        return false;
    }
    doc.formatVersion = static_cast<int>(formatVersion);

    doc.unitOk = std::fabs(jsonExtractFloat(text, "unit", 0.0f) - 1.0f) < 1e-6f;
    if (!doc.unitOk) { doc.error = "unit must be 1 (cubic unit grid)"; return false; }

    doc.voxelSizeOk = std::fabs(jsonExtractFloat(text, "voxel_size", 0.0f) - kVoxelSize) < 1e-9f;
    if (!doc.voxelSizeOk) { doc.error = "voxel_size must be " + std::to_string(kVoxelSize); return false; }

    doc.modeMap = jsonExtractString(text, "mode", "") == "map";
    if (!doc.modeMap) { doc.error = "mode must be \"map\""; return false; }

    const std::vector<int> dims = jsonExtractIntArray(text, "dims");
    if (dims.size() != 3 || dims[0] <= 0 || dims[1] <= 0 || dims[2] <= 0) {
        doc.error = "dims must be a positive [sx, sy, sz]";
        return false;
    }
    doc.sx = dims[0]; doc.sy = dims[1]; doc.sz = dims[2];

    // id_counters (optional section; absent = all zero)
    const std::string counters = jsonExtractObjectBody(text, "id_counters");
    if (!counters.empty()) {
        doc.persisted.evt = jsonInt(counters, "evt");
        doc.persisted.npc = jsonInt(counters, "npc");
        doc.persisted.route = jsonInt(counters, "route");
    }

    // scripted_events
    {
        const std::string arr = jsonExtractArrayBody(text, "scripted_events");
        size_t pos = 0, s = 0, e = 0;
        while (jsonxNextObject(arr, pos, s, e)) {
            const std::string o = arr.substr(s, e - s);
            ScriptedEvent ev;
            ev.x = jsonInt(o, "x"); ev.y = jsonInt(o, "y"); ev.z = jsonInt(o, "z");
            ev.id = jsonExtractString(o, "id", "");
            ev.name = jsonExtractString(o, "name", "");
            ev.script = jsonExtractString(o, "script", "");
            ev.trigger = jsonExtractString(o, "trigger", "");
            ev.radius = jsonExtractFloat(o, "radius", 0.0f);
            ev.cooldown = jsonInt(o, "cooldown");
            ev.requiredSignal = jsonExtractString(o, "required_signal", "");
            ev.emitSignal = jsonExtractString(o, "emit_signal", "");
            ev.condition = jsonExtractString(o, "condition", "");
            ev.repeatSet = o.find("\"repeat\"") != std::string::npos;
            ev.repeat = jsonInt(o, "repeat");
            ev.enabled = jsonExtractBool(o, "enabled", false);
            doc.events.push_back(ev);
            pos = e;
        }
    }

    // npcs
    {
        const std::string arr = jsonExtractArrayBody(text, "npcs");
        size_t pos = 0, s = 0, e = 0;
        while (jsonxNextObject(arr, pos, s, e)) {
            const std::string o = arr.substr(s, e - s);
            Npc n;
            n.x = jsonInt(o, "x"); n.y = jsonInt(o, "y"); n.z = jsonInt(o, "z");
            n.id = jsonExtractString(o, "id", "");
            n.name = jsonExtractString(o, "name", "");
            n.npcType = jsonExtractString(o, "type", "");
            n.aiProfile = jsonExtractString(o, "ai_profile", "");
            n.patrolRoute = jsonExtractString(o, "patrol_route", "");
            n.health = jsonExtractFloat(o, "health", 0.0f);
            n.maxHealth = jsonExtractFloat(o, "max_health", 0.0f);
            n.speed = jsonExtractFloat(o, "speed", 0.0f);
            n.viewDist = jsonExtractFloat(o, "view_dist", 0.0f);
            n.viewAngle = jsonExtractFloat(o, "view_angle", 0.0f);
            n.faction = jsonExtractString(o, "faction", "");
            n.dialogue = jsonExtractString(o, "dialogue", "");
            n.inventory = jsonExtractString(o, "inventory", "");
            n.isStatic = jsonExtractBool(o, "static", false);
            n.spawnTick = jsonInt(o, "spawn_tick");
            n.spawnCondition = jsonExtractString(o, "spawn_condition", "");
            n.enabled = jsonExtractBool(o, "enabled", false);
            doc.npcs.push_back(n);
            pos = e;
        }
    }

    // patrol_routes (nested "nodes" arrays)
    {
        const std::string arr = jsonExtractArrayBody(text, "patrol_routes");
        size_t pos = 0, s = 0, e = 0;
        while (jsonxNextObject(arr, pos, s, e)) {
            const std::string o = arr.substr(s, e - s);
            PatrolRoute r;
            r.id = jsonExtractString(o, "id", "");
            r.name = jsonExtractString(o, "name", "");
            r.loop = jsonExtractBool(o, "loop", false);
            const std::string nodes = jsonExtractArrayBody(o, "nodes");
            size_t np = 0, ns = 0, ne = 0;
            while (jsonxNextObject(nodes, np, ns, ne)) {
                const std::string no = nodes.substr(ns, ne - ns);
                PatrolNode nd;
                nd.x = jsonInt(no, "x"); nd.y = jsonInt(no, "y"); nd.z = jsonInt(no, "z");
                nd.wait = jsonExtractFloat(no, "wait", 0.0f);
                nd.action = jsonExtractString(no, "action", "");
                r.nodes.push_back(nd);
                np = ne;
            }
            doc.routes.push_back(r);
            pos = e;
        }
    }

    // voxels: mat -> block, dropping un-representable materials with a count
    {
        const std::string arr = jsonExtractArrayBody(text, "voxels");
        size_t pos = 0, s = 0, e = 0;
        while (jsonxNextObject(arr, pos, s, e)) {
            const std::string o = arr.substr(s, e - s);
            Voxel v;
            v.x = jsonInt(o, "x"); v.y = jsonInt(o, "y"); v.z = jsonInt(o, "z");
            const std::string mat = jsonExtractString(o, "mat", "");
            v.rgb = static_cast<uint32_t>(jsonInt(o, "rgb"));
            sim::Block mapped = sim::Block::Air;
            if (!blockFromMaterialName(mat, mapped)) {
                ++doc.dropped;
                pos = e;
                continue;
            }
            if (mapped == sim::Block::LightBulb || mapped == sim::Block::Moon) {
                doc.error = "\"" + mat + "\" is not occupancy: author it under lights / environment";
                return false;
            }
            v.block = mapped;
            doc.voxels.push_back(v);
            pos = e;
        }
    }

    // player_spawn
    {
        const std::string o = jsonExtractObjectBody(text, "player_spawn");
        if (!o.empty()) {
            doc.spawn.present = true;
            doc.spawn.x = jsonInt(o, "x"); doc.spawn.y = jsonInt(o, "y"); doc.spawn.z = jsonInt(o, "z");
            doc.spawn.yaw = jsonExtractFloat(o, "yaw", 0.0f);
            doc.spawn.pitch = jsonExtractFloat(o, "pitch", 0.0f);
        }
    }

    // lights
    {
        const std::string arr = jsonExtractArrayBody(text, "lights");
        size_t pos = 0, s = 0, e = 0;
        while (jsonxNextObject(arr, pos, s, e)) {
            const std::string o = arr.substr(s, e - s);
            LightPlacement l;
            l.kind = jsonExtractString(o, "kind", "bulb");
            l.x = jsonExtractFloat(o, "x", 0.0f);
            l.y = jsonExtractFloat(o, "y", 0.0f);
            l.z = jsonExtractFloat(o, "z", 0.0f);
            const std::vector<float> c = jsonExtractFloatArray(o, "color");
            if (c.size() == 3) { l.r = c[0]; l.g = c[1]; l.b = c[2]; }
            l.intensity = jsonExtractFloat(o, "intensity", 1.0f);
            l.radius = jsonExtractFloat(o, "radius", 0.06f);
            doc.lights.push_back(l);
            pos = e;
        }
    }

    // environment
    {
        const std::string o = jsonExtractObjectBody(text, "environment");
        if (!o.empty()) {
            doc.environment.present = true;
            const std::vector<float> d = jsonExtractFloatArray(o, "moon_dir");
            if (d.size() == 3)
                for (int i = 0; i < 3; ++i) doc.environment.moonDir[i] = d[i];
        }
    }

    // pickups
    {
        const std::string arr = jsonExtractArrayBody(text, "pickups");
        size_t pos = 0, s = 0, e = 0;
        while (jsonxNextObject(arr, pos, s, e)) {
            const std::string o = arr.substr(s, e - s);
            PickupPlacement pk;
            pk.item = jsonExtractString(o, "item", "");
            pk.x = jsonInt(o, "x"); pk.y = jsonInt(o, "y"); pk.z = jsonInt(o, "z");
            pk.rot = jsonInt(o, "rot") & 3;
            if (!pk.item.empty()) doc.pickups.push_back(pk);
            pos = e;
        }
    }

    // cells_rle: palette names first, so a run can be validated as it is read
    {
        const std::string rle = jsonExtractObjectBody(text, "cells_rle");
        if (!rle.empty()) {
            const std::string pal = jsonExtractArrayBody(rle, "palette");
            size_t i = 0;
            while ((i = pal.find('"', i)) != std::string::npos) {
                const size_t j = pal.find('"', i + 1);
                if (j == std::string::npos) break;
                sim::Block b = sim::Block::Air;
                if (!blockFromMaterialName(pal.substr(i + 1, j - i - 1), b)) {
                    doc.error = "cells_rle palette has an unknown block \"" + pal.substr(i + 1, j - i - 1) + "\"";
                    return false;
                }
                doc.rlePalette.push_back(b);
                i = j + 1;
            }
            doc.rleRuns = jsonExtractIntArray(rle, "runs");
            if (doc.rleRuns.size() % 5 != 0) {
                doc.error = "cells_rle runs must be [y, z, x0, length, palette] quintuples";
                return false;
            }
            for (size_t r = 0; r < doc.rleRuns.size(); r += 5) {
                const int len = doc.rleRuns[r + 3], pi = doc.rleRuns[r + 4];
                if (len <= 0 || pi < 0 || pi >= static_cast<int>(doc.rlePalette.size())) {
                    doc.error = "cells_rle run " + std::to_string(r / 5) + " is malformed";
                    return false;
                }
                const sim::Block rb = doc.rlePalette[pi];
                if (rb == sim::Block::LightBulb || rb == sim::Block::Moon) {
                    doc.error = std::string("cells_rle run ") + std::to_string(r / 5) + " is \"" +
                                blockName(rb) + "\", which is not occupancy: author it under lights / environment";
                    return false;
                }
                doc.rleCells += len;
            }
        }
    }

    // appearance (palette first, so each run's index can be checked)
    {
        const std::string ap = jsonExtractObjectBody(text, "appearance");
        if (!ap.empty()) {
            const std::vector<int> pal = jsonExtractIntArray(ap, "palette");
            if (pal.size() % 3 != 0 || pal.size() / 3 > 255) {
                doc.error = "appearance palette must be at most 255 [r, g, b] entries";
                return false;
            }
            for (size_t i = 0; i < pal.size(); i += 3)
                doc.appearPalette.push_back((uint32_t(pal[i] & 255) << 16) | (uint32_t(pal[i + 1] & 255) << 8) |
                                            uint32_t(pal[i + 2] & 255));
            doc.appearRuns = jsonExtractIntArray(ap, "runs");
            if (doc.appearRuns.size() % 5 != 0) {
                doc.error = "appearance runs must be [y, z, x0, length, palette] quintuples";
                return false;
            }
            for (size_t r = 0; r < doc.appearRuns.size(); r += 5) {
                const int len = doc.appearRuns[r + 3], pi = doc.appearRuns[r + 4];
                if (len <= 0 || pi < 1 || pi > static_cast<int>(doc.appearPalette.size())) {
                    doc.error = "appearance run " + std::to_string(r / 5) + " is malformed";
                    return false;
                }
                doc.appearCells += len;
            }
        }
    }

    restoreCounters(doc, doc.restored);
    doc.ok = true;
    return true;
}

// Load and parse a MAP document from disk. Sets doc.fileFound and doc.ok on the
// doc. Returns doc.ok (convenience for gating).
inline bool loadMapVox(const std::string& path, Doc& doc) {
    bool readable = false;
    const std::string text = jsonReadText(path, &readable);
    if (!readable) {
        doc = Doc{};
        doc.fileFound = false;
        doc.error = "unreadable: " + path;
        return false;
    }
    const bool ok = parseMapVox(text, doc);
    doc.fileFound = true;
    return ok;
}

struct StampResult {
    int written = 0;  // cells stamped into the world
    int skipped = 0;  // cells dropped: out of world bounds
    int painted = 0;  // cells given an appearance (palette colour)
};

// Stamp a parsed map's voxel grid into a sim::World at origins (ox, oy, oz).
// Every cell is a unit cube on the authoritative grid — no rescale, no offset
// reinterpretation. Cells outside the world are skipped and counted.
inline StampResult stampMapVox(const Doc& doc, sim::World& world, int ox, int oy, int oz) {
    StampResult r;
    for (const auto& v : doc.voxels) {
        const int wx = ox + v.x;
        const int wy = oy + v.y;
        const int wz = oz + v.z;
        if (!sim::World::inBounds(wx, wy, wz)) {
            ++r.skipped;
            continue;
        }
        world.set(wx, wy, wz, v.block);
        ++r.written;
        // Painter colour -> appearance layer. 0 means "no colour authored".
        if (v.rgb != 0 && v.block != sim::Block::Air) {
            world.setAppearance(wx, wy, wz, world.paletteIndexFor(v.rgb));
            ++r.painted;
        }
    }
    const auto& runs = doc.rleRuns;
    for (size_t i = 0; i + 4 < runs.size(); i += 5) {
        const sim::Block b = doc.rlePalette[runs[i + 4]];
        for (int k = 0; k < runs[i + 3]; ++k) {
            const int wx = ox + runs[i + 2] + k, wy = oy + runs[i], wz = oz + runs[i + 1];
            if (!sim::World::inBounds(wx, wy, wz)) { ++r.skipped; continue; }
            world.set(wx, wy, wz, b);
            ++r.written;
        }
    }
    // Appearance runs index the map's own palette, mapped into the world's.
    if (!doc.appearPalette.empty()) {
        std::vector<uint8_t> remap(doc.appearPalette.size() + 1, 0);
        for (size_t i = 0; i < doc.appearPalette.size(); ++i)
            remap[i + 1] = world.paletteIndexFor(doc.appearPalette[i]);
        const auto& ar = doc.appearRuns;
        for (size_t i = 0; i + 4 < ar.size(); i += 5) {
            for (int k = 0; k < ar[i + 3]; ++k) {
                const int wx = ox + ar[i + 2] + k, wy = oy + ar[i], wz = oz + ar[i + 1];
                if (!sim::World::inBounds(wx, wy, wz)) continue;
                world.setAppearance(wx, wy, wz, remap[ar[i + 4]]);
                ++r.painted;
            }
        }
    }
    return r;
}

// The world's appearance as an "appearance" section body, or "" when nothing
// is painted. Runs along +X like cells_rle; unpainted cells are not written.
inline std::string worldAppearanceRle(const sim::World& world) {
    std::string runs;
    bool first = true;
    for (int y = 0; y < sim::kWorldH; ++y)
        for (int z = 0; z < sim::kWorldD; ++z) {
            int x = 0;
            while (x < sim::kWorldW) {
                const uint8_t a = world.getAppearance(x, y, z);
                int len = 1;
                while (x + len < sim::kWorldW && world.getAppearance(x + len, y, z) == a) ++len;
                if (a != 0) {
                    runs += first ? "" : ",";
                    runs += std::to_string(y) + "," + std::to_string(z) + "," + std::to_string(x) + "," +
                            std::to_string(len) + "," + std::to_string(int(a));
                    first = false;
                }
                x += len;
            }
        }
    if (first) return "";
    std::string pal;
    for (size_t i = 1; i < world.palette.size(); ++i) {
        const uint32_t c = world.palette[i];
        pal += (i > 1 ? ", [" : "[") + std::to_string((c >> 16) & 255) + ", " +
               std::to_string((c >> 8) & 255) + ", " + std::to_string(c & 255) + "]";
    }
    return "{\"palette\": [" + pal + "], \"runs\": [" + runs + "]}";
}

// Write a whole world as a run-length map document (the inverse of
// cells_rle). Runs go along +X within each (y, z) row; Air is never written.
inline std::string worldToRle(const sim::World& world) {
    std::string runs;
    bool first = true;
    for (int y = 0; y < sim::kWorldH; ++y)
        for (int z = 0; z < sim::kWorldD; ++z) {
            int x = 0;
            while (x < sim::kWorldW) {
                const sim::Block b = world.get(x, y, z);
                int len = 1;
                while (x + len < sim::kWorldW && world.get(x + len, y, z) == b) ++len;
                if (b != sim::Block::Air) {
                    runs += first ? "" : ",";
                    runs += std::to_string(y) + "," + std::to_string(z) + "," + std::to_string(x) + "," +
                            std::to_string(len) + "," + std::to_string(static_cast<int>(b));
                    first = false;
                }
                x += len;
            }
        }
    // The palette is every block in enum order, so a run's index is its Block.
    std::string pal;
    for (int i = 0; i <= static_cast<int>(sim::Block::LightBulb); ++i) {
        pal += (i ? ", \"" : "\"");
        pal += blockName(static_cast<sim::Block>(i));
        pal += "\"";
    }
    return "{\"palette\": [" + pal + "], \"runs\": [" + runs + "]}";
}

} // namespace mapvox