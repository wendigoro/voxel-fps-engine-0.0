#pragma once
// Projectile impact + material destruction (C++ side).
// Projectile type/effect tables are authored in Python and loaded from JSON.

#include "materials.hpp"

#include <algorithm>
#include <cmath>
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
    std::string caliber = "medium"; // light | medium | heavy | energy
    bool hitscan = false;
    float grain = 0.0f;         // ammo grain proxy (integration)
    std::string ammoId;         // optional ammo subtype id
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
        d.caliber = jsonExtractString(obj, "caliber", d.caliber);
        d.ammoId = jsonExtractString(obj, "ammo_id", "");
        d.grain = jsonExtractFloat(obj, "grain", d.grain);
        // hitscan: explicit flag or energy effect/caliber
        d.hitscan = (jsonExtractFloat(obj, "hitscan", 0.0f) > 0.5f) ||
                    d.effect == "energy" || d.caliber == "energy";
        if (d.hitscan) d.gravityScale = 0.0f;
        out.push_back(d);
    }
    return out;
}

inline ProjectileDef findProjectile(const std::vector<ProjectileDef>& defs, const std::string& id) {
    for (const auto& d : defs) if (d.id == id) return d;
    if (!defs.empty()) return defs.front();
    return ProjectileDef{};
}

// Returns true if voxel should be destroyed; updates remainingEnergy.
inline bool resolveVoxelHit(MaterialId mat, float& remainingEnergy, float penetration) {
    if (mat == MaterialId::Air) return false;
    // Optics plexiglass is indestructible (integration: absorbs, never breaks).
    if (mat == MaterialId::Plexiglass) {
        remainingEnergy *= 0.15f;
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
