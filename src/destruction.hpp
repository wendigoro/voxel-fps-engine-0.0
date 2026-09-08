#pragma once
// Projectile impact + material destruction (C++ side).
// Projectile type/effect tables are authored in Python and loaded from JSON.

#include "materials.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// One 8x8x8 subunit of a unit voxel (matches debris.hpp / Python defs).
inline constexpr float kSubEdge = 0.001f / 8.0f;
inline constexpr float kSubRadius = kSubEdge * 0.5f; // fits one subunit cube

struct ProjectileDef {
    std::string id = "slug";
    float mass = 0.05f;
    float speed = 25.0f;
    float radius = kSubRadius;  // world units — default one subunit
    float baseDamage = 8.0f;    // energy scale
    float penetration = 0.35f;  // 0..1 leftover energy fraction on break
    float gravityScale = 1.0f;
    float splashRadius = 0.0f;  // world units
    float splashFalloff = 1.0f;
    std::string effect = "kinetic"; // kinetic | explosive | shred | energy
    std::string caliber;            // light | medium | heavy | energy (optional)
    bool hitscan = false;           // energy_beam / instant ray
    std::string ammoId;             // optional ammo subtype hook
    int pellets = 1;                // >1 shotgun multi-spawn
    float spreadDeg = 0.0f;         // cone half-angle degrees
};

struct ImpactEvent {
    int x = 0, y = 0, z = 0;
    float energy = 0.0f;
    std::string effect;
    float splashRadius = 0.0f;
};

struct ProjectileRuntime {
    ProjectileDef def;
    float px = 0, py = 0, pz = 0;
    float vx = 0, vy = 0, vz = 0;
    float energy = 0;
    bool alive = false;
};

// Shared gravity with Python effects loop (m/s^2 style, world-unit scaled).
inline constexpr float kWorldGravity = 9.81f * 0.35f; // tuned for micro-world

inline float kineticEnergy(float mass, float speed) {
    return 0.5f * mass * speed * speed * 100.0f; // scale for micro voxels
}

// Very small JSON helpers (schema is controlled by our Python exporter).
inline std::string jsonExtractString(const std::string& obj, const char* key, const std::string& fallback = {}) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t k = obj.find(pat);
    if (k == std::string::npos) return fallback;
    k = obj.find(':', k);
    if (k == std::string::npos) return fallback;
    k = obj.find('"', k);
    if (k == std::string::npos) return fallback;
    size_t e = obj.find('"', k + 1);
    if (e == std::string::npos) return fallback;
    return obj.substr(k + 1, e - k - 1);
}

inline float jsonExtractFloat(const std::string& obj, const char* key, float fallback) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t k = obj.find(pat);
    if (k == std::string::npos) return fallback;
    k = obj.find(':', k);
    if (k == std::string::npos) return fallback;
    k++;
    while (k < obj.size() && (obj[k] == ' ' || obj[k] == '\t')) k++;
    try {
        return std::stof(obj.substr(k));
    } catch (...) {
        return fallback;
    }
}

inline bool jsonExtractBool(const std::string& obj, const char* key, bool fallback) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t k = obj.find(pat);
    if (k == std::string::npos) return fallback;
    k = obj.find(':', k);
    if (k == std::string::npos) return fallback;
    k++;
    while (k < obj.size() && (obj[k] == ' ' || obj[k] == '\t' || obj[k] == '\n' || obj[k] == '\r')) k++;
    if (k >= obj.size()) return fallback;
    // Accept true/false and numeric 0/1 (weapon export uses 0|1).
    if (obj[k] == '1') return true;
    if (obj[k] == '0') return false;
    if (k + 4 <= obj.size() && obj.compare(k, 4, "true") == 0) return true;
    if (k + 5 <= obj.size() && obj.compare(k, 5, "false") == 0) return false;
    return fallback;
}

// Extract balanced {...} body immediately after "key": (empty if missing).
inline std::string jsonExtractObjectBody(const std::string& text, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t k = text.find(pat);
    if (k == std::string::npos) return {};
    k = text.find('{', k);
    if (k == std::string::npos) return {};
    int depth = 0;
    for (size_t i = k; i < text.size(); ++i) {
        char c = text[i];
        if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) return text.substr(k, i - k + 1);
        }
    }
    return {};
}

// Extract first JSON array body after "key": [ ... ]
inline std::string jsonExtractArrayBody(const std::string& text, const char* key) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t k = text.find(pat);
    if (k == std::string::npos) return {};
    k = text.find('[', k);
    if (k == std::string::npos) return {};
    int depth = 0;
    for (size_t i = k; i < text.size(); ++i) {
        char c = text[i];
        if (c == '[') depth++;
        else if (c == ']') {
            depth--;
            if (depth == 0) return text.substr(k, i - k + 1);
        }
    }
    return {};
}

inline std::vector<ProjectileDef> loadProjectileDefs(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    std::vector<ProjectileDef> out;
    size_t pos = 0;
    while (true) {
        size_t start = text.find('{', pos);
        if (start == std::string::npos) break;
        size_t end = text.find('}', start);
        if (end == std::string::npos) break;
        std::string obj = text.substr(start, end - start + 1);
        pos = end + 1;

        // Projectile objects must include id + speed (materials only have id).
        if (obj.find("\"id\"") == std::string::npos) continue;
        if (obj.find("\"speed\"") == std::string::npos) continue;
        if (obj.find("\"base_damage\"") == std::string::npos &&
            obj.find("\"mass\"") == std::string::npos) continue;

        ProjectileDef d;
        d.id = jsonExtractString(obj, "id", "slug");
        d.mass = jsonExtractFloat(obj, "mass", d.mass);
        d.speed = jsonExtractFloat(obj, "speed", d.speed);
        d.radius = jsonExtractFloat(obj, "radius", d.radius);
        d.baseDamage = jsonExtractFloat(obj, "base_damage", d.baseDamage);
        d.penetration = jsonExtractFloat(obj, "penetration", d.penetration);
        d.gravityScale = jsonExtractFloat(obj, "gravity_scale", d.gravityScale);
        d.splashRadius = jsonExtractFloat(obj, "splash_radius", d.splashRadius);
        d.splashFalloff = jsonExtractFloat(obj, "splash_falloff", d.splashFalloff);
        d.effect = jsonExtractString(obj, "effect", "kinetic");
        d.caliber = jsonExtractString(obj, "caliber", "");
        d.hitscan = jsonExtractBool(obj, "hitscan", false);
        // Numeric hitscan: 0/1 (jsonExtractBool handles); also accept bare 1 after key via float.
        if (!d.hitscan) {
            float hn = jsonExtractFloat(obj, "hitscan", 0.0f);
            if (hn >= 0.5f) d.hitscan = true;
        }
        d.ammoId = jsonExtractString(obj, "ammo_id", "");
        d.pellets = static_cast<int>(jsonExtractFloat(obj, "pellets", 1.0f));
        if (d.pellets < 1) d.pellets = 1;
        if (d.pellets > 12) d.pellets = 12; // hard cap for stability
        d.spreadDeg = jsonExtractFloat(obj, "spread_deg", 0.0f);
        // Clamp radius to subunit scale (legacy JSON may still have larger radii).
        if (d.radius > kSubRadius * 8.0f) d.radius = kSubRadius * 3.0f;
        if (d.radius < kSubRadius * 0.25f) d.radius = kSubRadius;
        if (d.effect == "energy" || d.caliber == "energy" || d.caliber == "energy_beam" ||
            d.id == "energy_beam") {
            d.hitscan = true;
            d.gravityScale = 0.0f;
        }
        out.push_back(d);
    }
    return out;
}

inline ProjectileDef findProjectile(const std::vector<ProjectileDef>& defs, const std::string& id) {
    for (const auto& d : defs) if (d.id == id) return d;
    if (!defs.empty()) return defs.front();
    return ProjectileDef{};
}

// ---- Weapon assembly / ammo scaffold (engine integration) ----
// Part ids (painter/export): none=0 barrel=1 action=2 bolt_chamber=3 trigger=4 grip_stock=5 sight=6
// Part→stat: barrel=damage action=impact bolt_chamber=recoil trigger=handling grip_stock=weight sight=optic

enum class WeaponPartId : uint8_t {
    None = 0,
    Barrel = 1,
    Action = 2,
    BoltChamber = 3,
    Trigger = 4,
    GripStock = 5,
    Sight = 6
};

struct AmmoDef {
    std::string id = "medium_fmj";
    std::string caliber = "medium"; // light | medium | heavy | energy | energy_beam
    float grain = 150.0f;
    float massScale = 1.0f;
    float damageScale = 1.0f;
    float penetrationScale = 1.0f;
    float gravityScale = 1.0f;
    bool hitscan = false;
    std::string effect = "kinetic";
    std::vector<std::string> effectTags;
    std::string notes;
};

struct WeaponDef {
    std::string id = "starter_rifle";
    std::string caliber = "medium";
    bool hitscan = false;
    std::string fireMode = "semi"; // semi | auto | bolt
    float damage = 10.0f;
    float impact = 8.0f;
    float recoil = 6.0f;
    float handling = 10.0f;
    float weight = 4.0f;
    float optic = 1.0f;
    std::string ammoId = "medium_fmj";
    AmmoDef ammo{};
};

inline bool caliberIsHitscan(const std::string& caliber) {
    return caliber == "energy_beam" || caliber == "energy" || caliber == "hitscan";
}

// Canonicalize caliber labels used across hotkeys / Python / painter.
inline std::string normalizeCaliber(const std::string& caliber) {
    if (caliber == "energy_beam" || caliber == "hitscan") return "energy";
    if (caliber.empty()) return "medium";
    return caliber;
}

// Map legacy engine ids → Python ammo.py ids.
inline std::string normalizeAmmoId(const std::string& id) {
    if (id == "fmj_light") return "light_fmj";
    if (id == "fmj_medium") return "medium_fmj";
    if (id == "fmj_heavy") return "heavy_fmj";
    if (id == "cell_energy") return "energy_bolt";
    return id;
}

inline std::string defaultAmmoIdForCaliber(const std::string& caliber) {
    const std::string c = normalizeCaliber(caliber);
    if (c == "light") return "light_fmj";
    if (c == "heavy") return "heavy_fmj";
    if (c == "energy") return "energy_bolt";
    return "medium_fmj";
}

inline bool ammoHasTag(const AmmoDef& a, const char* tag) {
    for (const auto& t : a.effectTags)
        if (t == tag) return true;
    return false;
}

// Built-in caliber ballistic/energy profiles (unit-grid VOXEL_SIZE=0.001).
// Prefer matching ids from projectiles.json when present.
inline ProjectileDef caliberFallbackDef(const std::string& caliber) {
    ProjectileDef d;
    if (caliber == "light") {
        d.id = "light_ball";
        d.mass = 0.03f;
        d.speed = 4.0f;
        d.radius = 0.0015f;
        d.baseDamage = 6.0f;
        d.penetration = 0.30f;
        d.gravityScale = 1.0f;
        d.effect = "kinetic";
        d.caliber = "light";
    } else if (caliber == "heavy") {
        d.id = "heavy_ball";
        d.mass = 0.11f;
        d.speed = 2.6f;
        d.radius = 0.0032f;
        d.baseDamage = 16.0f;
        d.penetration = 0.55f;
        d.gravityScale = 1.1f;
        d.effect = "kinetic";
        d.caliber = "heavy";
    } else if (caliberIsHitscan(caliber)) {
        d.id = "energy_beam";
        d.mass = 0.01f;
        d.speed = 80.0f;
        d.radius = 0.0012f;
        d.baseDamage = 14.0f;
        d.penetration = 0.70f;
        d.gravityScale = 0.0f;
        d.effect = "energy";
        d.caliber = "energy";
        d.hitscan = true;
    } else {
        // medium (default / near medium_ball)
        d.id = "medium_ball";
        d.mass = 0.06f;
        d.speed = 3.4f;
        d.radius = 0.0022f;
        d.baseDamage = 11.0f;
        d.penetration = 0.42f;
        d.gravityScale = 1.0f;
        d.effect = "kinetic";
        d.caliber = "medium";
    }
    return d;
}

inline ProjectileDef projectileForCaliber(const std::vector<ProjectileDef>& defs,
                                          const std::string& caliber) {
    // Prefer caliber ball ids: light_ball / medium_ball / heavy_ball / energy_beam.
    const std::string c = normalizeCaliber(caliber);
    const char* preferred = nullptr;
    if (c == "light") preferred = "light_ball";
    else if (c == "heavy") preferred = "heavy_ball";
    else if (c == "energy") preferred = "energy_beam";
    else preferred = "medium_ball";

    for (const auto& d : defs) {
        if (d.id == preferred) return d;
    }
    for (const auto& d : defs) {
        if (normalizeCaliber(d.caliber) == c || (c == "energy" && d.hitscan)) return d;
    }
    for (const auto& d : defs) {
        if (d.id == caliber || d.id == c) return d;
    }
    return caliberFallbackDef(c == "energy" ? "energy_beam" : c);
}

inline AmmoDef parseAmmoObject(const std::string& obj) {
    AmmoDef a;
    a.id = normalizeAmmoId(jsonExtractString(obj, "id", a.id));
    a.caliber = normalizeCaliber(jsonExtractString(obj, "caliber", a.caliber));
    a.grain = jsonExtractFloat(obj, "grain", a.grain);
    a.massScale = jsonExtractFloat(obj, "mass_scale", a.massScale);
    a.damageScale = jsonExtractFloat(obj, "damage_scale", a.damageScale);
    a.penetrationScale = jsonExtractFloat(obj, "penetration_scale", a.penetrationScale);
    a.gravityScale = jsonExtractFloat(obj, "gravity_scale", a.gravityScale);
    a.hitscan = jsonExtractBool(obj, "hitscan", a.hitscan);
    if (!a.hitscan && jsonExtractFloat(obj, "hitscan", 0.0f) >= 0.5f) a.hitscan = true;
    a.effect = jsonExtractString(obj, "effect", a.effect);
    a.notes = jsonExtractString(obj, "notes", a.notes);
    std::string arr = jsonExtractArrayBody(obj, "effect_tags");
    if (!arr.empty()) {
        size_t p = 0;
        while (true) {
            size_t q0 = arr.find('"', p);
            if (q0 == std::string::npos) break;
            size_t q1 = arr.find('"', q0 + 1);
            if (q1 == std::string::npos) break;
            a.effectTags.push_back(arr.substr(q0 + 1, q1 - q0 - 1));
            p = q1 + 1;
        }
    }
    if (a.caliber == "energy" || a.effect == "energy") a.hitscan = true;
    return a;
}

inline std::vector<AmmoDef> loadAmmoDefs(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    // Prefer the top-level "ammo" array so we don't confuse projectile objects.
    std::string arr = jsonExtractArrayBody(text, "ammo");
    const std::string& src = arr.empty() ? text : arr;

    std::vector<AmmoDef> out;
    size_t pos = 0;
    while (true) {
        size_t start = src.find('{', pos);
        if (start == std::string::npos) break;
        size_t end = src.find('}', start);
        if (end == std::string::npos) break;
        std::string obj = src.substr(start, end - start + 1);
        pos = end + 1;
        if (obj.find("\"id\"") == std::string::npos) continue;
        // Ammo rows carry grain and/or mass_scale; projectiles carry speed.
        if (obj.find("\"mass_scale\"") == std::string::npos &&
            obj.find("\"grain\"") == std::string::npos)
            continue;
        if (obj.find("\"speed\"") != std::string::npos) continue;
        AmmoDef a = parseAmmoObject(obj);
        if (!a.id.empty()) out.push_back(a);
    }
    return out;
}

inline AmmoDef findAmmo(const std::vector<AmmoDef>& defs, const std::string& id) {
    const std::string want = normalizeAmmoId(id);
    for (const auto& a : defs)
        if (a.id == want) return a;
    return {};
}

inline AmmoDef findAmmoForCaliber(const std::vector<AmmoDef>& defs, const std::string& caliber,
                                  const std::string& preferredId = {}) {
    if (!preferredId.empty()) {
        AmmoDef hit = findAmmo(defs, preferredId);
        if (!hit.id.empty() && (hit.caliber.empty() ||
                                normalizeCaliber(hit.caliber) == normalizeCaliber(caliber) ||
                                (caliberIsHitscan(caliber) && hit.hitscan)))
            return hit;
    }
    const std::string c = normalizeCaliber(caliber);
    const std::string defId = defaultAmmoIdForCaliber(c);
    AmmoDef defHit = findAmmo(defs, defId);
    if (!defHit.id.empty()) return defHit;
    for (const auto& a : defs)
        if (normalizeCaliber(a.caliber) == c) return a;
    AmmoDef fallback;
    fallback.id = defId;
    fallback.caliber = c;
    fallback.hitscan = (c == "energy");
    if (fallback.hitscan) {
        fallback.effect = "energy";
        fallback.gravityScale = 0.0f;
        fallback.massScale = 0.01f;
    }
    return fallback;
}

inline std::vector<AmmoDef> ammosForCaliber(const std::vector<AmmoDef>& defs,
                                           const std::string& caliber) {
    const std::string c = normalizeCaliber(caliber);
    std::vector<AmmoDef> out;
    for (const auto& a : defs)
        if (normalizeCaliber(a.caliber) == c) out.push_back(a);
    return out;
}

// AOE scale from projectile caliber + damage (density applied at impact site).
inline float impactAoeScale(const ProjectileDef& def) {
    float cal = 1.0f;
    const std::string c = normalizeCaliber(def.caliber.empty() ? "medium" : def.caliber);
    if (c == "light") cal = 0.85f;
    else if (c == "heavy") cal = 1.45f;
    else if (c == "energy") cal = 1.1f;
    else cal = 1.0f;
    const float dmg = std::max(0.5f, def.baseDamage / 10.0f);
    // radius relative to one subunit contributes area
    const float rScale = std::clamp(def.radius / kSubRadius, 0.5f, 4.0f);
    return cal * dmg * std::sqrt(rScale);
}

// Effective splash radius after material density (denser absorbs AOE).
inline float densityScaledSplash(float baseSplash, MaterialId mat) {
    if (baseSplash <= 0.0f) return 0.0f;
    const float dens = std::max(0.2f, materialProps(mat).density);
    return baseSplash / std::sqrt(dens);
}

inline ProjectileDef scaleProjectileForWeapon(ProjectileDef def, const WeaponDef& w) {
    // Painted stats scale the caliber base (damage←barrel, impact←action).
    const float dmgScale = std::max(0.25f, w.damage / 10.0f);
    const float impactScale = std::max(0.25f, w.impact / 8.0f);
    def.baseDamage *= dmgScale;
    def.mass *= (0.85f + 0.15f * impactScale);
    def.penetration = std::min(0.95f, def.penetration * (0.9f + 0.1f * impactScale));
    // Weapon damage expands subunit AOE footprint.
    def.splashRadius = std::max(def.splashRadius, def.radius * 2.0f) * (0.75f + dmgScale * 0.5f);
    def.radius = std::max(kSubRadius * 0.5f, def.radius);

    // Ammo subtype scales (Python ammo.py / projectiles.json ammo[]).
    const AmmoDef& a = w.ammo;
    if (!a.id.empty() || a.massScale != 1.0f || a.damageScale != 1.0f) {
        def.mass *= std::max(0.01f, a.massScale);
        def.baseDamage *= std::max(0.1f, a.damageScale);
        def.penetration = std::min(0.99f, def.penetration * std::max(0.1f, a.penetrationScale));
        def.gravityScale *= a.gravityScale;
        if (!a.effect.empty()) def.effect = a.effect;
        def.ammoId = a.id.empty() ? w.ammoId : a.id;
        if (a.effect == "explosive" || ammoHasTag(a, "he") || ammoHasTag(a, "explosive")) {
            def.splashRadius = std::max(def.splashRadius, 0.010f * std::max(1.0f, a.damageScale));
            def.splashFalloff = std::max(def.splashFalloff, 1.1f);
            def.effect = "explosive";
        }
        if (a.effect == "shred" || ammoHasTag(a, "shred")) {
            def.effect = "shred";
            def.splashRadius = std::max(def.splashRadius, 0.005f);
        }
        if (ammoHasTag(a, "pierce") || a.id.find("pierce") != std::string::npos) {
            def.penetration = std::min(0.99f, def.penetration * 1.35f);
        }
        if (a.hitscan || a.effect == "energy") {
            def.hitscan = true;
            def.gravityScale = 0.0f;
        }
    }

    if (w.hitscan || caliberIsHitscan(w.caliber) || a.hitscan) {
        def.gravityScale = 0.0f;
        def.hitscan = true;
        if (def.id.empty() || def.id == "medium_ball") def.id = "energy_beam";
    }
    return def;
}

inline float fireCooldownForWeapon(const WeaponDef& w) {
    // Higher handling → faster follow-up. Fire mode adjusts cadence.
    float cd = std::max(0.05f, 0.40f - w.handling * 0.018f);
    const std::string& m = w.fireMode;
    if (m == "auto") {
        cd = std::max(0.04f, cd * 0.55f);
    } else if (m == "bolt") {
        cd = std::max(cd, 0.55f + std::max(0.0f, w.recoil) * 0.025f);
    }
    // Weight slightly slows cyclic rate.
    cd *= (1.0f + std::max(0.0f, w.weight) * 0.008f);
    return cd;
}

inline WeaponDef defaultWeaponDef() {
    WeaponDef w;
    w.id = "starter_rifle";
    w.caliber = "medium";
    w.hitscan = false;
    w.fireMode = "semi";
    w.damage = 12.0f;
    w.impact = 9.0f;
    w.recoil = 6.5f;
    w.handling = 11.0f;
    w.weight = 3.8f;
    w.optic = 2.0f;
    w.ammoId = "medium_fmj";
    w.ammo.id = "medium_fmj";
    w.ammo.caliber = "medium";
    w.ammo.effect = "kinetic";
    w.ammo.effectTags = {"ballistic", "fmj"};
    return w;
}

inline WeaponDef parseWeaponObject(const std::string& text) {
    WeaponDef w = defaultWeaponDef();
    std::string obj = text;
    size_t start = text.find('{');
    size_t end = text.rfind('}');
    if (start != std::string::npos && end != std::string::npos && end > start)
        obj = text.substr(start, end - start + 1);

    w.id = jsonExtractString(obj, "id", w.id);
    w.caliber = normalizeCaliber(jsonExtractString(obj, "caliber", w.caliber));
    w.fireMode = jsonExtractString(obj, "fire_mode", w.fireMode);
    if (w.fireMode != "semi" && w.fireMode != "auto" && w.fireMode != "bolt")
        w.fireMode = "semi";
    w.ammoId = normalizeAmmoId(jsonExtractString(obj, "ammo_id", w.ammoId));
    w.hitscan = jsonExtractBool(obj, "hitscan", w.hitscan);
    if (!w.hitscan && jsonExtractFloat(obj, "hitscan", 0.0f) >= 0.5f) w.hitscan = true;

    // Nested stats object is the source of truth for composed painter export.
    std::string stats = jsonExtractObjectBody(obj, "stats");
    const std::string& statSrc = stats.empty() ? obj : stats;
    w.damage = jsonExtractFloat(statSrc, "damage", w.damage);
    w.impact = jsonExtractFloat(statSrc, "impact", w.impact);
    w.recoil = jsonExtractFloat(statSrc, "recoil", w.recoil);
    w.handling = jsonExtractFloat(statSrc, "handling", w.handling);
    w.weight = jsonExtractFloat(statSrc, "weight", w.weight);
    w.optic = jsonExtractFloat(statSrc, "optic", w.optic);

    if (caliberIsHitscan(w.caliber)) w.hitscan = true;

    std::string ammoBody = jsonExtractObjectBody(obj, "ammo");
    if (!ammoBody.empty()) {
        w.ammo = parseAmmoObject(ammoBody);
        if (!w.ammo.id.empty()) w.ammoId = normalizeAmmoId(w.ammo.id);
        if (!w.ammo.caliber.empty())
            w.caliber = normalizeCaliber(w.ammo.caliber);
    }
    if (w.ammoId.empty()) w.ammoId = defaultAmmoIdForCaliber(w.caliber);
    if (w.ammo.id.empty()) w.ammo.id = w.ammoId;
    if (w.ammo.caliber.empty()) w.ammo.caliber = w.caliber;
    return w;
}

// Bind ammo table row onto a loaded weapon (fills scales/effect).
inline void bindWeaponAmmo(WeaponDef& w, const std::vector<AmmoDef>& ammoTable) {
    w.ammoId = normalizeAmmoId(w.ammoId.empty() ? defaultAmmoIdForCaliber(w.caliber) : w.ammoId);
    AmmoDef found = findAmmoForCaliber(ammoTable, w.caliber, w.ammoId);
    if (!found.id.empty()) {
        w.ammo = found;
        w.ammoId = found.id;
        if (found.hitscan) w.hitscan = true;
    } else {
        w.ammo.id = w.ammoId;
        w.ammo.caliber = normalizeCaliber(w.caliber);
    }
}

inline WeaponDef loadWeaponDef(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        WeaponDef miss;
        miss.id.clear();
        return miss;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    WeaponDef w = parseWeaponObject(ss.str());
    if (w.id.empty()) w = defaultWeaponDef();
    return w;
}

// Returns true if voxel should be destroyed; updates remainingEnergy.
inline bool resolveVoxelHit(MaterialId mat, float& remainingEnergy, float penetration) {
    if (mat == MaterialId::Air) return false;
    // Plexiglass is indestructible (weapon glass / barriers).
    if (mat == MaterialId::Plexiglass) {
        const auto& m = materialProps(mat);
        remainingEnergy *= (1.0f - m.damping);
        return false;
    }
    const auto& m = materialProps(mat);
    const float thr = breakEnergyThreshold(mat);
    if (remainingEnergy < thr) {
        remainingEnergy *= (1.0f - m.damping);
        return false;
    }
    remainingEnergy = remainingEnergy * penetration * (1.0f - m.damping * 0.5f);
    return true;
}

inline float effectMultiplier(const std::string& effect, MaterialId mat) {
    if (effect == "shred") {
        if (mat == MaterialId::BushLeaves || mat == MaterialId::BushBranch) return 2.4f;
        if (mat == MaterialId::Wood) return 1.3f;
        return 0.85f;
    }
    if (effect == "explosive") {
        if (mat == MaterialId::Dirt || mat == MaterialId::BushLeaves) return 1.6f;
        if (mat == MaterialId::Concrete) return 0.75f;
        return 1.2f;
    }
    // kinetic
    if (mat == MaterialId::Concrete) return 0.9f;
    if (mat == MaterialId::BushLeaves) return 1.5f;
    return 1.0f;
}
