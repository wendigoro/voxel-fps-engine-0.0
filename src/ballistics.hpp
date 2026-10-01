#pragma once
// Ballistics: how rounds move through the authoritative world and what they
// break. The first simulation module split out of main.cpp (Phase 2d).
//
// Scope: hitscan rays (unit-grid DDA), ballistic projectile integration, and
// splash. Weapon/ammo selection, aim and pellet spread, recoil and the muzzle
// flash stay with the caller; they decide WHAT is fired and where, this
// decides what it DOES.
//
// The module reads and writes only what it is handed: the sim::World, its own
// State, and a Hooks object. It has no globals and no view access. Everything
// it does to the rest of the simulation (a voxel broke, a round ricocheted, a
// body was in the way) is reported through Hooks, so the caller owns debris,
// damage intake, remesh flags and telemetry, and a headless test can supply
// its own.
//
// Behaviour is a faithful extraction of the old main.cpp code, down to the
// order of floating-point operations: sim_fingerprint in the smoke/stress
// report reproduces exactly across the move. That includes one inherited
// quirk, kept on purpose so the move could be proved: State::last carries the
// most recent impact's direction, energy and AOE scale ACROSS calls, the way
// the old g_lastImpact* / g_lastAoeScale globals did. A splash leaves its last
// per-cell AOE scale there and the next direct hit, even another projectile's,
// throws its debris with it. It only shapes debris, never occupancy or damage.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "debris.hpp"       // faceNormalFromVelocity, ricochetVelocity
#include "destruction.hpp"  // ProjectileDef/Runtime, energy + material rules
#include "health.hpp"       // ArmorZone, kSelfFireDamage
#include "sim_world.hpp"

namespace ballistics {

// How the most recent impact looked. Debris uses it to throw chips.
struct Impact {
    float dx = 0.0f, dy = 0.0f, dz = -1.0f; // travel direction at the hit
    float energy = 10.0f;                   // energy at the struck cell
    float aoeScale = 1.0f;                  // chip spread scale
};

struct State {
    std::vector<ProjectileRuntime> projectiles;
    Impact last; // persists across calls; see the header comment
};

// Everything ballistics does to the rest of the simulation.
struct Hooks {
    virtual ~Hooks() = default;
    // A voxel broke. The cell is already Air; `was` is what it held.
    virtual void voxelDestroyed(int x, int y, int z, sim::Block was, MaterialId mat,
                                const Impact& imp) = 0;
    // A round bounced off (x,y,z) without breaking it.
    virtual void ricochet(int x, int y, int z, MaterialId mat, const Impact& imp) = 0;
    // Segment from a to b (world units) against the player's body. Returns the
    // zone struck, or ArmorZone::Count on a miss or when there is no live body.
    virtual ArmorZone bodySweep(float ax, float ay, float az, float bx, float by, float bz,
                                float radiusCells) = 0;
    // Distance (world units) from a point to the player's body, and the zone
    // nearest to it.
    virtual float bodyDistance(float x, float y, float z, ArmorZone& nearest) = 0;
    // Apply `energy` of `effect` to the body at `zone`.
    virtual void damageBody(float energy, const std::string& effect, ArmorZone zone) = 0;
};

// Block -> material, as far as impacts are concerned.
inline MaterialId blockMaterial(sim::Block b) {
    using sim::Block;
    switch (b) {
    case Block::Dirt: return MaterialId::Dirt;
    case Block::Concrete: return MaterialId::Concrete;
    case Block::SheetMetal: return MaterialId::SheetMetal;
    case Block::Girder: return MaterialId::Girder;
    case Block::Wood: return MaterialId::Wood;
    case Block::WoodDark: return MaterialId::BushBranch;
    case Block::Water:
    case Block::WaterCurrent: return MaterialId::Water;
    case Block::Moon:
    case Block::LightBulb: return MaterialId::Air; // emissive, no impact mass
    default: return MaterialId::Air;
    }
}

inline void destroyVoxel(State& st, sim::World& world, Hooks& hooks, int x, int y, int z) {
    if (!sim::World::inBounds(x, y, z)) return;
    const sim::Block b = world.get(x, y, z);
    if (b == sim::Block::Air) return;
    const MaterialId mat = blockMaterial(b);
    world.set(x, y, z, sim::Block::Air);
    hooks.voxelDestroyed(x, y, z, b, mat, st.last);
}

inline void applySplash(State& st, sim::World& world, Hooks& hooks, int cx, int cy, int cz,
                        float radius, float energy, const ProjectileDef& def) {
    if (radius <= 0.0f) return;
    const float vs = sim::kVoxelSize;
    // Expand splash by caliber/damage AOE, then density-scale per cell.
    const float aoe = impactAoeScale(def);
    float effectiveR = radius * std::max(0.5f, aoe);
    int r = std::max(1, static_cast<int>(effectiveR / vs) + 1);
    // Cap neighborhood for shotgun volleys (performance).
    if (def.pellets > 1) r = std::min(r, 3);
    else r = std::min(r, 6);
    for (int dz = -r; dz <= r; ++dz)
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                if (dx == 0 && dy == 0 && dz == 0) continue;
                const int x = cx + dx, y = cy + dy, z = cz + dz;
                if (!sim::World::inBounds(x, y, z)) continue;
                const float dist = std::sqrt(float(dx * dx + dy * dy + dz * dz)) * vs;
                const MaterialId mat = blockMaterial(world.get(x, y, z));
                if (mat == MaterialId::Air || mat == MaterialId::Plexiglass) continue;
                const float cellR = densityScaledSplash(effectiveR, mat);
                if (dist > cellR) continue;
                const float fall = std::pow(std::max(0.0f, 1.0f - dist / std::max(cellR, 1e-6f)),
                                            def.splashFalloff);
                // Dense materials soak energy harder beyond threshold already.
                const float densMul = 1.0f / std::sqrt(std::max(0.2f, materialProps(mat).density));
                const float e = energy * fall * 0.65f * effectMultiplier(def.effect, mat) * densMul;
                const float thr = breakEnergyThreshold(mat);
                st.last.aoeScale = aoe * densMul;
                st.last.energy = e;
                if (e >= thr * 0.8f) destroyVoxel(st, world, hooks, x, y, z);
            }

    // Splash reaches bodies too. Self damage is ON, so the player's own grenade
    // hurts them; the shooter is only excluded from their own bullet's
    // *direct* hit (see ProjectileRuntime::ownerIsPlayer).
    if (health::kSelfFireDamage) {
        const float impactX = (static_cast<float>(cx) + 0.5f) * vs;
        const float impactY = (static_cast<float>(cy) + 0.5f) * vs;
        const float impactZ = (static_cast<float>(cz) + 0.5f) * vs;
        ArmorZone zone = ArmorZone::Count;
        const float bodyDist = hooks.bodyDistance(impactX, impactY, impactZ, zone);
        if (bodyDist <= effectiveR) {
            const float fall = std::pow(std::max(0.0f, 1.0f - bodyDist / std::max(effectiveR, 1e-6f)),
                                        def.splashFalloff);
            hooks.damageBody(energy * fall * 0.65f, def.effect, zone);
        }
    }
}

// Unit-grid DDA ray (Amanatides & Woo) from `eye` along `aimDir`. Hitscan and
// energy rounds only (the gravity_scale = 0 path). Returns voxels broken.
inline int fireHitscan(State& st, sim::World& world, Hooks& hooks, const ProjectileDef& def,
                       float energyScale, float eyeX, float eyeY, float eyeZ,
                       float aimX, float aimY, float aimZ) {
    const float vs = sim::kVoxelSize;
    // Normalise the aim exactly as the old Vec3::normalized() did.
    float fx = aimX, fy = aimY, fz = aimZ;
    {
        const float l = std::sqrt(fx * fx + fy * fy + fz * fz);
        if (l > 1e-8f) { const float k = 1.0f / l; fx *= k; fy *= k; fz *= k; }
        else { fx = 0.0f; fy = 1.0f; fz = 0.0f; }
    }
    // Start slightly forward of the eye in world space.
    float ox = (eyeX + fx * 0.02f) / vs;
    float oy = (eyeY + fy * 0.02f) / vs;
    float oz = (eyeZ + fz * 0.02f) / vs;
    float dx = fx, dy = fy, dz = fz;
    // Avoid zero-direction components for DDA.
    const float eps = 1e-8f;
    if (std::fabs(dx) < eps) dx = (dx < 0.0f ? -eps : eps);
    if (std::fabs(dy) < eps) dy = (dy < 0.0f ? -eps : eps);
    if (std::fabs(dz) < eps) dz = (dz < 0.0f ? -eps : eps);

    int ix = static_cast<int>(std::floor(ox));
    int iy = static_cast<int>(std::floor(oy));
    int iz = static_cast<int>(std::floor(oz));

    const int stepX = dx > 0.0f ? 1 : -1;
    const int stepY = dy > 0.0f ? 1 : -1;
    const int stepZ = dz > 0.0f ? 1 : -1;

    // World-space distance to cross one unit voxel on each axis.
    const float tDeltaX = vs / std::fabs(dx);
    const float tDeltaY = vs / std::fabs(dy);
    const float tDeltaZ = vs / std::fabs(dz);

    // tMax: world distance along ray to next voxel boundary on each axis.
    float tMaxX = (stepX > 0)
        ? ((static_cast<float>(ix) + 1.0f - ox) / dx) * vs
        : ((ox - static_cast<float>(ix)) / -dx) * vs;
    float tMaxY = (stepY > 0)
        ? ((static_cast<float>(iy) + 1.0f - oy) / dy) * vs
        : ((oy - static_cast<float>(iy)) / -dy) * vs;
    float tMaxZ = (stepZ > 0)
        ? ((static_cast<float>(iz) + 1.0f - oz) / dz) * vs
        : ((oz - static_cast<float>(iz)) / -dz) * vs;

    float energy = kineticEnergy(def.mass, def.speed) * (def.baseDamage / 10.0f) * energyScale;
    // Energy beams still use kineticEnergy scale; mass is tiny but baseDamage carries power.
    if (def.gravityScale <= 0.0f)
        energy = std::max(energy, def.baseDamage * 1.5f * energyScale);

    const float maxDist = 1.75f; // world units (~1750 unit voxels)
    float traveled = 0.0f;
    int breaks = 0;
    const int maxSteps = static_cast<int>(maxDist / vs) + 2;
    // One bullet, one body: the sweep is sampled per cell, and the 5-cell-wide
    // player would otherwise be counted once per cell it spans. The shooter's
    // own shot is excluded (the ray origin is inside their head); enemy fire
    // will use the same path once it exists.
    bool bodyHit = false;
    const float radiusCells = std::max(0.5f, std::min(2.0f, def.radius / vs));
    float prevWx = eyeX + fx * 0.02f;
    float prevWy = eyeY + fy * 0.02f;
    float prevWz = eyeZ + fz * 0.02f;

    for (int step = 0; step < maxSteps; ++step) {
        // Body sweep for this cell, tested in cell space against the same box
        // the armor zones tile.
        {
            const float curWx = (static_cast<float>(ix) + 0.5f) * vs;
            const float curWy = (static_cast<float>(iy) + 0.5f) * vs;
            const float curWz = (static_cast<float>(iz) + 0.5f) * vs;
            if (!bodyHit) {
                const ArmorZone zone =
                    hooks.bodySweep(prevWx, prevWy, prevWz, curWx, curWy, curWz, radiusCells);
                if (zone != ArmorZone::Count) {
                    bodyHit = true;
                    hooks.damageBody(energy, def.effect, zone);
                    // Soft target: a penetrating round keeps going, weaker.
                    energy *= (1.0f - std::min(0.95f, def.penetration));
                    if (energy < 0.05f) break;
                }
            }
            prevWx = curWx;
            prevWy = curWy;
            prevWz = curWz;
        }
        if (sim::World::inBounds(ix, iy, iz)) {
            const MaterialId mat = blockMaterial(world.get(ix, iy, iz));
            if (mat != MaterialId::Air) {
                float e = energy * effectMultiplier(def.effect, mat);
                st.last.dx = dx; st.last.dy = dy; st.last.dz = dz;
                st.last.energy = e;
                if (resolveVoxelHit(mat, e, def.penetration)) {
                    destroyVoxel(st, world, hooks, ix, iy, iz);
                    applySplash(st, world, hooks, ix, iy, iz, def.splashRadius, energy, def);
                    energy = e;
                    ++breaks;
                    if (energy < 0.05f) break;
                } else {
                    // Tough surface: spark chips without destroying occupancy.
                    st.last.aoeScale = impactAoeScale(def) * 0.5f;
                    hooks.ricochet(ix, iy, iz, mat, Impact{dx, dy, dz, e * 0.35f, st.last.aoeScale});
                    energy = e;
                    break;
                }
            }
        } else if (traveled > 0.05f) {
            // Left the map after traveling — end ray.
            break;
        }

        // Step to next voxel face.
        if (tMaxX < tMaxY) {
            if (tMaxX < tMaxZ) {
                traveled = tMaxX;
                tMaxX += tDeltaX;
                ix += stepX;
            } else {
                traveled = tMaxZ;
                tMaxZ += tDeltaZ;
                iz += stepZ;
            }
        } else {
            if (tMaxY < tMaxZ) {
                traveled = tMaxY;
                tMaxY += tDeltaY;
                iy += stepY;
            } else {
                traveled = tMaxZ;
                tMaxZ += tDeltaZ;
                iz += stepZ;
            }
        }
        if (traveled > maxDist) break;
    }
    return breaks;
}

// Launch a ballistic round from just ahead of `eye` along `aimDir`.
inline void spawnProjectile(State& st, const ProjectileDef& def, float eyeX, float eyeY, float eyeZ,
                            float aimX, float aimY, float aimZ) {
    float fx = aimX, fy = aimY, fz = aimZ;
    {
        const float l = std::sqrt(fx * fx + fy * fy + fz * fz);
        if (l > 1e-8f) { const float k = 1.0f / l; fx *= k; fy *= k; fz *= k; }
        else { fx = 0.0f; fy = 1.0f; fz = 0.0f; }
    }
    ProjectileRuntime p;
    p.def = def;
    // Spawn just ahead of the eye; subunit-sized projectiles use def.radius.
    const float muzzle = std::max(0.02f, def.radius * 40.0f);
    p.px = eyeX + fx * muzzle;
    p.py = eyeY + fy * muzzle;
    p.pz = eyeZ + fz * muzzle;
    p.vx = fx * def.speed;
    p.vy = fy * def.speed;
    p.vz = fz * def.speed;
    p.energy = kineticEnergy(def.mass, def.speed) * (def.baseDamage / 10.0f);
    p.alive = true;
    p.ownerIsPlayer = true; // never self-inflicted by the shooter's own bullet
    st.projectiles.push_back(p);
}

// Advance every live projectile by dt: gravity, 4 sub-steps, body sweep, then
// the voxel it lands in (break + splash, or ricochet). Dead rounds are removed.
inline void stepProjectiles(State& st, sim::World& world, Hooks& hooks, float dt) {
    const float vs = sim::kVoxelSize;
    for (auto& p : st.projectiles) {
        if (!p.alive) continue;
        // Gravity (matches Python WORLD_GRAVITY * gravity_scale)
        p.vy -= kWorldGravity * p.def.gravityScale * dt;

        const int steps = 4;
        const float sdt = dt / static_cast<float>(steps);
        const float radiusCells = std::max(0.5f, std::min(2.0f, p.def.radius / vs));
        for (int s = 0; s < steps && p.alive; ++s) {
            const float prevWx = p.px, prevWy = p.py, prevWz = p.pz;
            p.px += p.vx * sdt;
            p.py += p.vy * sdt;
            p.pz += p.vz * sdt;

            // Body sweep first, so a body is hit before the voxel it stands in.
            // One bullet, one body (ownerIsPlayer excludes the shooter).
            if (!p.ownerIsPlayer && health::kSelfFireDamage) {
                const ArmorZone zone =
                    hooks.bodySweep(prevWx, prevWy, prevWz, p.px, p.py, p.pz, radiusCells);
                if (zone != ArmorZone::Count) {
                    hooks.damageBody(p.energy, p.def.effect, zone);
                    p.energy *= (1.0f - std::min(0.95f, p.def.penetration));
                    if (p.energy < 0.05f) p.alive = false;
                }
            }

            const int ix = static_cast<int>(std::floor(p.px / vs));
            const int iy = static_cast<int>(std::floor(p.py / vs));
            const int iz = static_cast<int>(std::floor(p.pz / vs));
            if (!sim::World::inBounds(ix, iy, iz)) {
                // allow mild overshoot above world; kill if far
                if (p.py < -0.5f || p.py > 2.0f ||
                    p.px < -0.5f || p.px > sim::kWorldW * vs + 0.5f ||
                    p.pz < -0.5f || p.pz > sim::kWorldD * vs + 0.5f) {
                    p.alive = false;
                }
                continue;
            }

            const MaterialId mat = blockMaterial(world.get(ix, iy, iz));
            if (mat == MaterialId::Air) continue;

            float e = p.energy * effectMultiplier(p.def.effect, mat);
            st.last.dx = p.vx; st.last.dy = p.vy; st.last.dz = p.vz;
            st.last.energy = e;
            if (resolveVoxelHit(mat, e, p.def.penetration)) {
                destroyVoxel(st, world, hooks, ix, iy, iz);
                applySplash(st, world, hooks, ix, iy, iz, p.def.splashRadius, p.energy, p.def);
                p.energy = e;
                if (p.def.effect == "explosive" || p.energy < 0.05f) p.alive = false;
            } else {
                // Matrix ricochet — bounce off without destroying occupancy.
                float nx, ny, nz;
                faceNormalFromVelocity(p.vx, p.vy, p.vz, nx, ny, nz);
                const auto& mp = materialProps(mat);
                ricochetVelocity(p.vx, p.vy, p.vz, nx, ny, nz,
                                 0.20f + (1.0f - mp.fragility) * 0.25f,
                                 0.30f + mp.damping * 0.3f);
                p.energy = e * (1.0f - mp.damping * 0.5f);
                st.last.aoeScale = impactAoeScale(p.def) * 0.55f;
                // Chips fly along the incoming direction, not the bounce.
                hooks.ricochet(ix, iy, iz, mat,
                               Impact{st.last.dx, st.last.dy, st.last.dz, e * 0.4f, st.last.aoeScale});
                // Nudge out of cell to avoid re-hit same voxel
                p.px += nx * vs * 0.6f;
                p.py += ny * vs * 0.6f;
                p.pz += nz * vs * 0.6f;
                if (p.energy < 0.08f || (p.vx * p.vx + p.vy * p.vy + p.vz * p.vz) < 1e-5f)
                    p.alive = false;
            }
        }
    }
    st.projectiles.erase(
        std::remove_if(st.projectiles.begin(), st.projectiles.end(),
                       [](const ProjectileRuntime& p) { return !p.alive; }),
        st.projectiles.end());
}

// ---- headless contract checks (engine smoke; exit code 6 on failure) ----
//
// Each case builds a blank world, places a few cells by hand, and drives the
// module through recording hooks, so it checks the module's own contract and
// does not depend on the warehouse map or on main.cpp's hooks.

struct SelfTestReport {
    bool hitscanBreaksSoft = false;   // a dirt cell on the ray becomes Air, hook sees it once
    bool ricochetKeepsCell = false;   // too little energy: cell stays, ricochet hook fires, no break
    bool oneBulletOneBody = false;    // sweep reports a hit on every cell; damage lands once
    bool projectileBreaks = false;    // a ballistic round flies, breaks into a slab, dies
    bool shooterNotSwept = false;     // the player's own round is never body-tested
    bool ok() const {
        return hitscanBreaksSoft && ricochetKeepsCell && oneBulletOneBody && projectileBreaks &&
               shooterNotSwept;
    }
};

namespace detail {
struct RecordingHooks final : Hooks {
    int destroyed = 0, ricochets = 0, sweeps = 0, damage = 0;
    int lastX = -1, lastY = -1, lastZ = -1;
    ArmorZone sweepAnswer = ArmorZone::Count;
    void voxelDestroyed(int x, int y, int z, sim::Block, MaterialId, const Impact&) override {
        ++destroyed; lastX = x; lastY = y; lastZ = z;
    }
    void ricochet(int, int, int, MaterialId, const Impact&) override { ++ricochets; }
    ArmorZone bodySweep(float, float, float, float, float, float, float) override {
        ++sweeps;
        return sweepAnswer;
    }
    float bodyDistance(float, float, float, ArmorZone& nearest) override {
        nearest = ArmorZone::Count;
        return 1e9f; // no body anywhere near: splash never reaches it
    }
    void damageBody(float, const std::string&, ArmorZone) override { ++damage; }
};

inline void blankWorld(sim::World& w) {
    w.alloc();
    for (int cy = 0; cy < sim::kChunksY; ++cy)
        for (int cz = 0; cz < sim::kChunksZ; ++cz)
            for (int cx = 0; cx < sim::kChunksX; ++cx) {
                sim::Chunk& c = w.chunks[sim::World::chunkIndex(cx, cy, cz)];
                c.cx = cx; c.cy = cy; c.cz = cz;
                c.voxels.assign(sim::kVoxelsPerChunk, sim::Block::Air);
            }
}

// World-space centre of a cell.
inline float cellCentre(int i) { return (static_cast<float>(i) + 0.5f) * sim::kVoxelSize; }
} // namespace detail

inline SelfTestReport selfTest() {
    SelfTestReport rep;
    const int x = 40, y = 20, z = 30;

    // 1. Hitscan breaks a soft cell on the ray, and reports it exactly once.
    {
        sim::World w; detail::blankWorld(w);
        w.set(x, y, z + 40, sim::Block::Dirt);
        State st; detail::RecordingHooks h;
        ProjectileDef def;
        const int breaks = fireHitscan(st, w, h, def, 1.0f, detail::cellCentre(x), detail::cellCentre(y),
                                       detail::cellCentre(z), 0.0f, 0.0f, 1.0f);
        rep.hitscanBreaksSoft = breaks == 1 && h.destroyed == 1 && h.lastZ == z + 40 &&
                                w.get(x, y, z + 40) == sim::Block::Air;
    }
    // 2. A round without the energy to break a girder chips it and stops.
    {
        sim::World w; detail::blankWorld(w);
        w.set(x, y, z + 40, sim::Block::Girder);
        State st; detail::RecordingHooks h;
        ProjectileDef def;
        def.mass = 1e-6f; def.speed = 0.01f; def.baseDamage = 0.01f;
        const int breaks = fireHitscan(st, w, h, def, 1.0f, detail::cellCentre(x), detail::cellCentre(y),
                                       detail::cellCentre(z), 0.0f, 0.0f, 1.0f);
        rep.ricochetKeepsCell = breaks == 0 && h.destroyed == 0 && h.ricochets == 1 &&
                                w.get(x, y, z + 40) == sim::Block::Girder;
    }
    // 3. One bullet, one body: even when every cell of the ray reports the
    //    body, damage is applied once.
    {
        sim::World w; detail::blankWorld(w);
        State st; detail::RecordingHooks h;
        h.sweepAnswer = ArmorZone::Chest;
        ProjectileDef def;
        def.penetration = 0.9f; // keeps going after the body, so later cells are swept too
        fireHitscan(st, w, h, def, 1.0f, detail::cellCentre(x), detail::cellCentre(y),
                    detail::cellCentre(z), 0.0f, 0.0f, 1.0f);
        rep.oneBulletOneBody = h.damage == 1 && h.sweeps == 1;
    }
    // 4 + 5. A ballistic round breaks into a slab thick enough that sub-steps
    //        cannot tunnel it, then dies; being the player's own round, it is
    //        never body-tested on the way.
    {
        sim::World w; detail::blankWorld(w);
        for (int dz = 60; dz < 90; ++dz)
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) w.set(x + dx, y + dy, z + dz, sim::Block::Dirt);
        State st; detail::RecordingHooks h;
        ProjectileDef def;
        def.speed = 2.0f; def.gravityScale = 0.0f; def.effect = "explosive";
        spawnProjectile(st, def, detail::cellCentre(x), detail::cellCentre(y), detail::cellCentre(z),
                        0.0f, 0.0f, 1.0f);
        for (int t = 0; t < 240 && !st.projectiles.empty(); ++t) stepProjectiles(st, w, h, 1.0f / 120.0f);
        rep.projectileBreaks = h.destroyed >= 1 && h.lastZ >= z + 60 && st.projectiles.empty();
        rep.shooterNotSwept = h.sweeps == 0;
    }
    return rep;
}

} // namespace ballistics
