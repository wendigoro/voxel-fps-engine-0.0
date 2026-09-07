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

struct ProjectileDef {
    std::string id = "slug";
    float mass = 0.05f;
    float speed = 25.0f;
    float radius = 0.02f;       // world units
    float baseDamage = 8.0f;    // energy scale
    float penetration = 0.35f;  // 0..1 leftover energy fraction on break
    float gravityScale = 1.0f;
    float splashRadius = 0.0f;  // world units
    float splashFalloff = 1.0f;
    std::string effect = "kinetic"; // kinetic | explosive | shred | energy
    std::string caliber;            // light | medium | heavy | energy (optional)
    bool hitscan = false;           // energy_beam / instant ray
    std::string ammoId;             // optional ammo subtype hook
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
    if (k + 4 <= obj.size() && obj.compare(k, 4, "true") == 0) return true;
    if (k + 5 <= obj.size() && obj.compare(k, 5, "false") == 0) return false;
    return fallback;
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
        d.ammoId = jsonExtractString(obj, "ammo_id", "");
        if (d.effect == "energy" || d.caliber == "energy" || d.id == "energy_beam") {
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
    std::string id = "fmj_medium";
    std::string caliber = "medium"; // light | medium | heavy | energy | energy_beam
    std::vector<std::string> effectTags; // future VFX hooks only
};

struct WeaponDef {
    std::string id = "starter_rifle";
    std::string caliber = "medium";
    bool hitscan = false;
    std::string fireMode = "semi";
    float damage = 10.0f;
    float impact = 8.0f;
    float recoil = 6.0f;
    float handling = 10.0f;
    float weight = 4.0f;
    float optic = 1.0f;
    std::string ammoId = "fmj_medium";
    AmmoDef ammo{};
};

inline bool caliberIsHitscan(const std::string& caliber) {
    return caliber == "energy_beam" || caliber == "energy" || caliber == "hitscan";
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
    // Prefer mats projectile ids: light_ball / medium_ball / heavy_ball / energy_beam.
    const char* preferred = nullptr;
    if (caliber == "light") preferred = "light_ball";
    else if (caliber == "heavy") preferred = "heavy_ball";
    else if (caliberIsHitscan(caliber)) preferred = "energy_beam";
    else preferred = "medium_ball";

    for (const auto& d : defs) {
        if (d.id == preferred) return d;
    }
    for (const auto& d : defs) {
        if (d.caliber == caliber || (caliberIsHitscan(caliber) && d.hitscan)) return d;
    }
    // Exact caliber-as-id (legacy)
    for (const auto& d : defs) {
        if (d.id == caliber) return d;
    }
    return caliberFallbackDef(caliber);
}

inline ProjectileDef scaleProjectileForWeapon(ProjectileDef def, const WeaponDef& w) {
    // Painted stats scale the caliber base (damage←barrel, impact←action).
    const float dmgScale = std::max(0.25f, w.damage / 10.0f);
    const float impactScale = std::max(0.25f, w.impact / 8.0f);
    def.baseDamage *= dmgScale;
    def.mass *= (0.85f + 0.15f * impactScale);
    def.penetration = std::min(0.95f, def.penetration * (0.9f + 0.1f * impactScale));
    if (w.hitscan || caliberIsHitscan(w.caliber)) {
        def.gravityScale = 0.0f;
        def.id = def.id.empty() ? "energy_beam" : def.id;
    }
    return def;
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
    w.ammoId = "fmj_medium";
    w.ammo.id = "fmj_medium";
    w.ammo.caliber = "medium";
    w.ammo.effectTags = {"ap"};
    return w;
}

inline AmmoDef parseAmmoObject(const std::string& obj) {
    AmmoDef a;
    a.id = jsonExtractString(obj, "id", a.id);
    a.caliber = jsonExtractString(obj, "caliber", a.caliber);
    // effect_tags: naive scan for quoted strings inside the array if present
    const std::string pat = "\"effect_tags\"";
    size_t k = obj.find(pat);
    if (k != std::string::npos) {
        size_t lb = obj.find('[', k);
        size_t rb = (lb == std::string::npos) ? std::string::npos : obj.find(']', lb);
        if (lb != std::string::npos && rb != std::string::npos) {
            std::string arr = obj.substr(lb + 1, rb - lb - 1);
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
    }
    return a;
}

inline WeaponDef parseWeaponObject(const std::string& text) {
    WeaponDef w = defaultWeaponDef();
    // Prefer top-level object body
    std::string obj = text;
    size_t start = text.find('{');
    size_t end = text.rfind('}');
    if (start != std::string::npos && end != std::string::npos && end > start)
        obj = text.substr(start, end - start + 1);

    w.id = jsonExtractString(obj, "id", w.id);
    w.caliber = jsonExtractString(obj, "caliber", w.caliber);
    w.fireMode = jsonExtractString(obj, "fire_mode", w.fireMode);
    w.ammoId = jsonExtractString(obj, "ammo_id", w.ammoId);
    w.damage = jsonExtractFloat(obj, "damage", w.damage);
    w.impact = jsonExtractFloat(obj, "impact", w.impact);
    w.recoil = jsonExtractFloat(obj, "recoil", w.recoil);
    w.handling = jsonExtractFloat(obj, "handling", w.handling);
    w.weight = jsonExtractFloat(obj, "weight", w.weight);
    w.optic = jsonExtractFloat(obj, "optic", w.optic);

    // hitscan bool: true/false token after key
    {
        const std::string pat = "\"hitscan\"";
        size_t k = obj.find(pat);
        if (k != std::string::npos) {
            k = obj.find(':', k);
            if (k != std::string::npos) {
                std::string tail = obj.substr(k + 1, 16);
                if (tail.find("true") != std::string::npos) w.hitscan = true;
                else if (tail.find("false") != std::string::npos) w.hitscan = false;
            }
        }
    }
    if (caliberIsHitscan(w.caliber)) w.hitscan = true;

    // nested ammo object (first nested { after "ammo")
    {
        const std::string pat = "\"ammo\"";
        size_t k = obj.find(pat);
        if (k != std::string::npos) {
            size_t b = obj.find('{', k);
            if (b != std::string::npos) {
                size_t e = obj.find('}', b);
                if (e != std::string::npos) {
                    w.ammo = parseAmmoObject(obj.substr(b, e - b + 1));
                    if (!w.ammo.id.empty()) w.ammoId = w.ammo.id;
                    if (!w.ammo.caliber.empty() && w.caliber.empty())
                        w.caliber = w.ammo.caliber;
                }
            }
        }
    }
    if (w.ammo.id.empty()) w.ammo.id = w.ammoId;
    if (w.ammo.caliber.empty()) w.ammo.caliber = w.caliber;
    return w;
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
