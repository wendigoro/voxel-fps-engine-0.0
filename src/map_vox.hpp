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
};

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
            v.block = mapped;
            doc.voxels.push_back(v);
            pos = e;
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
    }
    return r;
}

} // namespace mapvox