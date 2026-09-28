#pragma once
// Health, damage intake, armor mitigation, death/respawn (RULES.md rule 15).
//
// HP is deliberately NOT the voxel-destruction `energy` unit. Energy is tuned
// against material break thresholds, so treating it as HP would silently
// retune player survivability every time a material stat moved. The only bridge
// between the two is `biologicalDamage()` below, and it is a single named
// constant so a rebalance never has to touch the voxel code.
//
// Everything here is header-only and side-effect free except the ActorHealth
// mutators, so the smoke test can exercise the full damage pipeline without a
// GPU, a world, or a running frame loop. That matters because `--smoke` skips
// player physics entirely (see updateCamera in main.cpp).
//
// Kept per-actor from day one (Phase 3 of the health plan) so the first enemy
// is a new type rather than a refactor of the player's own state.

#include "inventory.hpp" // ArmorZone, ZoneBox, armorZoneAt, hitCellInBounds

#include <algorithm>
#include <cmath>
#include <string>

namespace health {

// ---- confirmed design decisions -------------------------------------------
inline constexpr float kMaxHealth = 125.0f;            // 125 HP, not 100
inline constexpr bool kSelfFireDamage = true;         // self damage on
// Armor absorbs 0.5% of incoming damage per point of armor_points. Absorption
// (not a flat multiplier) means a shot can still be lethal through a cheap
// piece, which is the whole point of armor being an economy.
inline constexpr float kArmorAbsorbPerPoint = 0.005f;
// Hard cap so armor can never make an actor invulnerable. 100 points = 50%.
inline constexpr float kMaxArmorAbsorb = 0.5f;

// ---- damage intake tuning -------------------------------------------------
// 1 point of a medium-caliber rifle hit (energy ~63 after weapon scaling) is
// worth ~17.7 HP, so a full 125 HP player survives about seven rifle hits.
inline constexpr float kBioScale = 0.28f;
// Beams carry enormous energy for their penetration, not their stopping power.
inline constexpr float kEnergyEffectScale = 0.35f;
// One damage application may never exceed this fraction of max HP, so a stray
// splash tick (or an uninitialized energy) cannot one-shot a full actor.
inline constexpr float kMaxSingleHitFraction = 0.6f;

// ---- fall damage (calibrated to the micro-world) ---------------------------
// kWorldGravity is 3.4335 world-units/s^2 and terminal velocity is clamped to
// 0.25, so the whole reachable fall range is ~0.05..0.25. 0.09 is roughly a
// one-voxel step (v = sqrt(2*g*0.001) = 0.083) and must stay free.
inline constexpr float kFallSafeSpeed = 0.09f;
inline constexpr float kFallSpeedPerHp = 200.0f;
inline constexpr float kMaxFallDamage = 40.0f;

// ---- drowning -------------------------------------------------------------
inline constexpr float kBreathSeconds = 20.0f;
inline constexpr float kDrownDamagePerSecond = 9.0f;

// ---- death / respawn ------------------------------------------------------
inline constexpr float kRespawnSeconds = 3.0f;
// How long a screen-wide damage flash decays, in seconds.
inline constexpr float kDamageFlashSeconds = 0.45f;

// Body geometry comes straight from inventory.hpp so it can never drift from
// collision or armor tiling: x/z span cells -2..+2 and y spans 0..18 from the
// feet, which is exactly the box armorZonesTileHitbox() tiles.
inline constexpr int kBodyHalfCells = kHitHalfXZ;
inline constexpr int kBodyTopCell = kHitHeight - 1;

inline float clampAbsorb(float v) {
    return std::max(0.0f, std::min(kMaxArmorAbsorb, v));
}

// Fraction of incoming damage a given armor piece soaks away.
inline float armorAbsorption(float armorPoints) {
    return clampAbsorb(armorPoints * kArmorAbsorbPerPoint);
}

// Voxel-destruction energy -> biological HP. `effect` is the projectile effect
// string ("kinetic", "energy", "explosive", "shred", "").
inline float biologicalDamage(float energy, const std::string& effect) {
    if (energy <= 0.0f) return 0.0f;
    float e = energy;
    if (effect == "energy") e *= kEnergyEffectScale;
    else if (effect == "explosive") e *= 0.8f;
    else if (effect == "shred") e *= 0.7f;
    return e * kBioScale;
}

// ---- body-space geometry --------------------------------------------------
// All of these work in *cell space*: a point is world/kVoxelSize minus the
// body's origin cell. Staying in integer-cell space is what keeps the hit
// resolution on the same unit grid as collision and armor tiling (RULES.md
// rules 1 and 12); no world-space AABB is ever built.

struct BodyCellOrigin {
    int x = 0, y = 0, z = 0; // cell containing the body centre column / feet
};

inline BodyCellOrigin bodyCellOrigin(float px, float py, float pz) {
    return BodyCellOrigin{static_cast<int>(std::floor(px / kVoxelSize)),
                          static_cast<int>(std::floor(py / kVoxelSize)),
                          static_cast<int>(std::floor(pz / kVoxelSize))};
}

// Segment vs axis-aligned box, in cell space. Returns the entry parameter
// t in [0,1], or -1 when the segment misses. `pad` inflates the box by a
// projectile radius so near misses still register as grazes.
inline float segmentCellBoxEntry(const float p0[3], const float p1[3],
                                 const float bmin[3], const float bmax[3]) {
    float tEnter = 0.0f;
    float tExit = 1.0f;
    for (int a = 0; a < 3; ++a) {
        const float d = p1[a] - p0[a];
        if (std::fabs(d) < 1e-9f) {
            if (p0[a] < bmin[a] || p0[a] > bmax[a]) return -1.0f;
            continue;
        }
        float t0 = (bmin[a] - p0[a]) / d;
        float t1 = (bmax[a] - p0[a]) / d;
        if (t0 > t1) std::swap(t0, t1);
        tEnter = std::max(tEnter, t0);
        tExit = std::min(tExit, t1);
        if (tEnter > tExit) return -1.0f;
    }
    return tEnter;
}

// Resolve a world-space segment against the body, returning the armor zone it
// enters at and the world-space entry point. `padCells` is the projectile radius
// in cells. Returns ArmorZone::Count on a miss.
struct BodyHit {
    ArmorZone zone = ArmorZone::Count;
    float wx = 0.0f, wy = 0.0f, wz = 0.0f; // world-space entry point
};

inline BodyHit segmentHitBody(const BodyCellOrigin& org, float x0, float y0, float z0,
                              float x1, float y1, float z1, float padCells = 0.0f) {
    BodyHit hit;
    const float inv = 1.0f / kVoxelSize;
    const float p0[3] = {x0 * inv - static_cast<float>(org.x),
                         y0 * inv - static_cast<float>(org.y),
                         z0 * inv - static_cast<float>(org.z)};
    const float p1[3] = {x1 * inv - static_cast<float>(org.x),
                         y1 * inv - static_cast<float>(org.y),
                         z1 * inv - static_cast<float>(org.z)};
    const float h = static_cast<float>(kBodyHalfCells);
    const float top = static_cast<float>(kBodyTopCell + 1);
    const float bmin[3] = {-h - padCells, -padCells, -h - padCells};
    const float bmax[3] = {h + 1.0f + padCells, top + padCells, h + 1.0f + padCells};
    const float t = segmentCellBoxEntry(p0, p1, bmin, bmax);
    if (t < 0.0f) return hit;
    hit.wx = x0 + (x1 - x0) * t;
    hit.wy = y0 + (y1 - y0) * t;
    hit.wz = z0 + (z1 - z0) * t;
    // Entering cell of the body, clamped into the tiled hitbox.
    const int cx = std::max(-kBodyHalfCells,
                            std::min(kBodyHalfCells, static_cast<int>(std::floor(p0[0] + (p1[0] - p0[0]) * t))));
    const int cy = std::max(0, std::min(kBodyTopCell, static_cast<int>(std::floor(p0[1] + (p1[1] - p0[1]) * t))));
    const int cz = std::max(-kBodyHalfCells,
                            std::min(kBodyHalfCells, static_cast<int>(std::floor(p0[2] + (p1[2] - p0[2]) * t))));
    hit.zone = armorZoneAt(cx, cy, cz);
    return hit;
}

// Distance from a world-space point to the body's occupied cell box, in world
// units; 0 when the point is inside. Splash uses this instead of a second
// geometry path so the blast always prices against the same cell box that
// segmentHitBody() and armorZoneAt() agree on.
inline float distanceToBody(const BodyCellOrigin& org, float wx, float wy, float wz) {
    const float inv = 1.0f / kVoxelSize;
    const float px = wx * inv - static_cast<float>(org.x);
    const float py = wy * inv - static_cast<float>(org.y);
    const float pz = wz * inv - static_cast<float>(org.z);
    const float h = static_cast<float>(kBodyHalfCells);
    const float top = static_cast<float>(kBodyTopCell + 1);
    const float dx = std::max(-h - px, px - (h + 1.0f));
    const float dy = std::max(-py, py - top);
    const float dz = std::max(-h - pz, pz - (h + 1.0f));
    // Any axis inside the slab means the point is already touching the body.
    if (dx <= 0.0f || dy <= 0.0f || dz <= 0.0f) return 0.0f;
    return std::sqrt(dx * dx + dy * dy + dz * dz) * kVoxelSize;
}

// Zone nearest a world-space point, for radial damage with no travel direction
// (splash). Clamps the point into the hitbox first, so a blast at the player's
// feet resolves to legs rather than falling off the bottom of the box.
inline ArmorZone zoneNearestPoint(const BodyCellOrigin& org, float wx, float wy, float wz) {
    const float inv = 1.0f / kVoxelSize;
    const int cx = static_cast<int>(std::floor(wx * inv)) - org.x;
    const int cy = static_cast<int>(std::floor(wy * inv)) - org.y;
    const int cz = static_cast<int>(std::floor(wz * inv)) - org.z;
    const int clampX = std::max(-kBodyHalfCells, std::min(kBodyHalfCells, cx));
    const int clampY = std::max(0, std::min(kBodyTopCell, cy));
    const int clampZ = std::max(-kBodyHalfCells, std::min(kBodyHalfCells, cz));
    return armorZoneAt(clampX, clampY, clampZ);
}

// ---- actor state ----------------------------------------------------------

struct ActorHealth {
    float health = kMaxHealth;
    float maxHealth = kMaxHealth;
    // Per-zone damage that armor absorbed, for the HUD/stat readout.
    float absorbed[static_cast<int>(ArmorZone::Count)] = {0, 0, 0, 0};
    bool dead = false;
    // Seconds since the last damage application; drives the screen flash.
    float sinceLastHit = 999.0f;
    // Seconds of breath left; counts up while submerged, refills in air.
    float breath = kBreathSeconds;
    // Countdown to respawn while dead.
    float respawnTimer = 0.0f;
    // Running totals, useful for smoke assertions.
    float damageTaken = 0.0f;
    float damageAbsorbed = 0.0f;
    int hitCount = 0;

    float healthFraction() const {
        return maxHealth > 0.0f ? std::max(0.0f, std::min(1.0f, health / maxHealth)) : 0.0f;
    }
};

// Absorption applied for one hit: the armor covering `zone` soaks
// `armorAbsorbPerPoint * points` of the damage. Overkill is not a thing here —
// the armor only decides what fraction of the hit arrives, so a stronger hit
// still lands for its full remaining weight.
struct DamageResult {
    float raw = 0.0f;      // HP before armor
    float absorbed = 0.0f; // HP soaked by armor
    float applied = 0.0f;  // HP actually lost
    bool killed = false;
};

// Apply damage to one zone. `armorPoints` is the equipped piece's armor_points.
inline DamageResult applyDamage(ActorHealth& h, float rawDamage, ArmorZone zone, float armorPoints) {
    DamageResult r;
    if (rawDamage <= 0.0f || h.dead) return r;
    r.raw = rawDamage;
    // Cap a single application so no one tick can take the whole pool.
    r.raw = std::min(r.raw, h.maxHealth * kMaxSingleHitFraction);
    const float frac = (zone < ArmorZone::Count) ? armorAbsorption(armorPoints) : 0.0f;
    r.absorbed = r.raw * frac;
    r.applied = r.raw - r.absorbed;
    h.health = std::max(0.0f, h.health - r.applied);
    h.damageTaken += r.applied;
    h.damageAbsorbed += r.absorbed;
    if (zone < ArmorZone::Count) h.absorbed[static_cast<int>(zone)] += r.absorbed;
    h.sinceLastHit = 0.0f;
    ++h.hitCount;
    if (h.health <= 0.0f) {
        h.health = 0.0f;
        h.dead = true;
        h.respawnTimer = kRespawnSeconds;
        r.killed = true;
    }
    return r;
}

// Fall damage from an impact speed. Free below kFallSafeSpeed (~one voxel).
inline float fallDamageForImpactSpeed(float impactSpeed) {
    if (impactSpeed <= kFallSafeSpeed) return 0.0f;
    const float over = impactSpeed - kFallSafeSpeed;
    return std::min(kMaxFallDamage, over * kFallSpeedPerHp);
}

// Advance breath/drowning. `submerged` is the existing character probe result.
// Returns HP lost to drowning this tick.
inline float updateBreath(ActorHealth& h, bool submerged, float dt) {
    if (h.dead) return 0.0f;
    if (submerged) {
        h.breath = std::max(0.0f, h.breath - dt);
        if (h.breath <= 0.0f) {
            // Drowning bypasses armor: it is not an impact.
            const float dmg = kDrownDamagePerSecond * dt;
            h.health = std::max(0.0f, h.health - dmg);
            h.damageTaken += dmg;
            h.sinceLastHit = 0.0f;
            if (h.health <= 0.0f) {
                h.health = 0.0f;
                h.dead = true;
                h.respawnTimer = kRespawnSeconds;
            }
            return dmg;
        }
    } else {
        // Refill twice as fast as it drains, so surfacing briefly is a real
        // recovery and a full breath takes half the time to earn back.
        h.breath = std::min(kBreathSeconds, h.breath + dt * 2.0f);
    }
    return 0.0f;
}

// Per-frame housekeeping: flash decay and the respawn countdown. Returns true
// on the frame the actor should respawn.
inline bool updateActorHealth(ActorHealth& h, float dt) {
    h.sinceLastHit += dt;
    if (!h.dead) return false;
    h.respawnTimer -= dt;
    if (h.respawnTimer > 0.0f) return false;
    h.health = h.maxHealth;
    h.dead = false;
    h.breath = kBreathSeconds;
    h.respawnTimer = 0.0f;
    h.sinceLastHit = 999.0f;
    return true;
}

// Screen-flash strength, 0..1.
inline float damageFlash(const ActorHealth& h) {
    if (h.sinceLastHit >= kDamageFlashSeconds) return 0.0f;
    return 1.0f - (h.sinceLastHit / kDamageFlashSeconds);
}

inline void respawnActor(ActorHealth& h) {
    h.health = h.maxHealth;
    h.dead = false;
    h.breath = kBreathSeconds;
    h.respawnTimer = 0.0f;
    h.sinceLastHit = 999.0f;
}

} // namespace health
