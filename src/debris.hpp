#pragma once
// Visual degradation: 8x8x8 sub-voxel debris + matrix trajectory/ricochet.
// Display/effects only — occupancy remains the cubic unit grid (VOXEL_SIZE).

#include "materials.hpp"
#include "render_class.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// ---- 3x3 matrix calculator (trajectory / ricochet) ----
struct Mat3 {
    float m[9]{}; // row-major

    static Mat3 identity() {
        Mat3 r{};
        r.m[0] = r.m[4] = r.m[8] = 1.0f;
        return r;
    }

    static Mat3 scale(float s) {
        Mat3 r{};
        r.m[0] = r.m[4] = r.m[8] = s;
        return r;
    }

    // Householder reflection matrix I - 2 n n^T (n unit).
    static Mat3 householderReflect(float nx, float ny, float nz) {
        Mat3 r = identity();
        const float x2 = 2.0f * nx, y2 = 2.0f * ny, z2 = 2.0f * nz;
        r.m[0] -= x2 * nx; r.m[1] -= x2 * ny; r.m[2] -= x2 * nz;
        r.m[3] -= y2 * nx; r.m[4] -= y2 * ny; r.m[5] -= y2 * nz;
        r.m[6] -= z2 * nx; r.m[7] -= z2 * ny; r.m[8] -= z2 * nz;
        return r;
    }

    void mulVec(float x, float y, float z, float& ox, float& oy, float& oz) const {
        ox = m[0] * x + m[1] * y + m[2] * z;
        oy = m[3] * x + m[4] * y + m[5] * z;
        oz = m[6] * x + m[7] * y + m[8] * z;
    }
};

inline void mat3MulVec(const Mat3& a, float x, float y, float z, float& ox, float& oy, float& oz) {
    a.mulVec(x, y, z, ox, oy, oz);
}

// Reflect velocity about a unit surface normal using Householder matrix.
inline void ricochetVelocity(float& vx, float& vy, float& vz,
                             float nx, float ny, float nz,
                             float restitution, float friction) {
    float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len < 1e-8f) return;
    nx /= len; ny /= len; nz /= len;
    Mat3 R = Mat3::householderReflect(nx, ny, nz);
    float rx, ry, rz;
    R.mulVec(vx, vy, vz, rx, ry, rz);
    const float vn = rx * nx + ry * ny + rz * nz;
    float tx = rx - vn * nx, ty = ry - vn * ny, tz = rz - vn * nz;
    tx *= (1.0f - friction);
    ty *= (1.0f - friction);
    tz *= (1.0f - friction);
    float outN = (vn < 0.0f) ? (std::fabs(vn) * restitution) : (vn * restitution);
    vx = tx + outN * nx;
    vy = ty + outN * ny;
    vz = tz + outN * nz;
}

// Dominant face normal from inbound velocity (axis-aligned voxel faces).
inline void faceNormalFromVelocity(float vx, float vy, float vz,
                                   float& nx, float& ny, float& nz) {
    const float ax = std::fabs(vx), ay = std::fabs(vy), az = std::fabs(vz);
    nx = ny = nz = 0.0f;
    if (ax >= ay && ax >= az) {
        nx = (vx > 0.0f) ? -1.0f : 1.0f;
    } else if (ay >= ax && ay >= az) {
        ny = (vy > 0.0f) ? -1.0f : 1.0f;
    } else {
        nz = (vz > 0.0f) ? -1.0f : 1.0f;
    }
}

// ---- Sub-voxel debris (8^3 cells per unit voxel, visual only) ----
inline constexpr int kDebrisSubDiv = 8;
inline constexpr float kDebrisSubSize = 0.001f / static_cast<float>(kDebrisSubDiv); // VOXEL_SIZE/8
// Readable chip scale: still snaps to the 8^3 lattice origin, but drawn large enough
// to cover multiple sub-cells so impacts read as painted cubic debris (not sub-pixel).
inline constexpr float kDebrisVisualScale = 3.25f; // chip edge ~= 3.25 sub-cells
inline constexpr int kMaxDebris = 1536;
inline constexpr int kMaxSpawnPerVoxel = 28;
inline constexpr int kHardMaxSpawnBurst = 120;
// Shader mat id for debris cubes (see voxel.frag).
inline constexpr float kDebrisMatId = rc::attr(rc::RenderClass::Debris);
inline constexpr float kMuzzleMatId = rc::attr(rc::RenderClass::Muzzle);

struct SublatticeUnit {
    uint8_t sx = 0;
    uint8_t sy = 0;
    uint8_t sz = 0;
};

struct SublatticeImpact {
    int count = 1;                      // 1 (direct <= 45 deg) or 2 (oblique > 45 deg)
    SublatticeUnit units[2]{};         // impacted sublattice cells (0..7)
    float cosTheta = 1.0f;             // cos(angle between ray and normal)
    float hitX = 0, hitY = 0, hitZ = 0; // continuous world hit point
    float nx = 0, ny = 0, nz = 0;      // struck face normal
};

// Returns the 1 or 2 impacted sublattice units (0..7 inside the unit voxel)
// based on angle of impact:
// - Direct impact (<= 45 deg incidence, cosTheta >= 0.7071): 1 sublattice unit at entry point.
// - Oblique impact (> 45 deg incidence, cosTheta < 0.7071): 2 sublattice units along tangential graze.
inline SublatticeImpact computeSublatticeImpact(int ix, int iy, int iz,
                                               float hitX, float hitY, float hitZ,
                                               float dx, float dy, float dz,
                                               float nx, float ny, float nz,
                                               float voxelSize) {
    float dlen = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dlen > 1e-6f) { dx /= dlen; dy /= dlen; dz /= dlen; }
    else { dx = 0.0f; dy = 0.0f; dz = -1.0f; }

    float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (nlen > 1e-6f) { nx /= nlen; ny /= nlen; nz /= nlen; }
    else { nx = 0.0f; ny = 1.0f; nz = 0.0f; }

    float rx = (hitX - static_cast<float>(ix) * voxelSize) / voxelSize;
    float ry = (hitY - static_cast<float>(iy) * voxelSize) / voxelSize;
    float rz = (hitZ - static_cast<float>(iz) * voxelSize) / voxelSize;

    rx = std::clamp(rx, 0.0f, 0.9999f);
    ry = std::clamp(ry, 0.0f, 0.9999f);
    rz = std::clamp(rz, 0.0f, 0.9999f);

    int sx0 = std::clamp(static_cast<int>(std::floor(rx * static_cast<float>(kDebrisSubDiv))), 0, kDebrisSubDiv - 1);
    int sy0 = std::clamp(static_cast<int>(std::floor(ry * static_cast<float>(kDebrisSubDiv))), 0, kDebrisSubDiv - 1);
    int sz0 = std::clamp(static_cast<int>(std::floor(rz * static_cast<float>(kDebrisSubDiv))), 0, kDebrisSubDiv - 1);

    // Snap the entry coordinate along normal axis to the boundary face
    if (nx > 0.5f) sx0 = kDebrisSubDiv - 1;
    else if (nx < -0.5f) sx0 = 0;
    if (ny > 0.5f) sy0 = kDebrisSubDiv - 1;
    else if (ny < -0.5f) sy0 = 0;
    if (nz > 0.5f) sz0 = kDebrisSubDiv - 1;
    else if (nz < -0.5f) sz0 = 0;

    // Angle of incidence with surface normal.
    // Inbound direction points into the surface (dx*nx + dy*ny + dz*nz <= 0).
    float dot = dx * nx + dy * ny + dz * nz;
    float cosTheta = std::clamp(-dot, 0.0f, 1.0f);

    SublatticeImpact imp{};
    imp.cosTheta = cosTheta;
    imp.hitX = hitX; imp.hitY = hitY; imp.hitZ = hitZ;
    imp.nx = nx; imp.ny = ny; imp.nz = nz;
    imp.units[0] = {static_cast<uint8_t>(sx0), static_cast<uint8_t>(sy0), static_cast<uint8_t>(sz0)};

    // Direct / steep impact: angle <= 45 deg (cosTheta >= 1 / sqrt(2) ~= 0.7071f) -> 1 unit
    if (cosTheta >= 0.7071f) {
        imp.count = 1;
        return imp;
    }

    // Oblique / glancing impact: angle > 45 deg -> 2 units
    imp.count = 2;

    // Tangential direction on surface: T = D - (D . N) * N = D + cosTheta * N
    float tx = dx + cosTheta * nx;
    float ty = dy + cosTheta * ny;
    float tz = dz + cosTheta * nz;
    float tlen = std::sqrt(tx * tx + ty * ty + tz * tz);
    if (tlen > 1e-6f) { tx /= tlen; ty /= tlen; tz /= tlen; }
    else {
        if (std::fabs(nx) < 0.8f) { tx = 1.0f; ty = 0.0f; tz = 0.0f; }
        else { tx = 0.0f; ty = 1.0f; tz = 0.0f; }
    }

    int stepSx = 0, stepSy = 0, stepSz = 0;
    const float atx = std::fabs(tx), aty = std::fabs(ty), atz = std::fabs(tz);
    if (atx >= aty && atx >= atz) {
        stepSx = (tx > 0.0f) ? 1 : -1;
    } else if (aty >= atx && aty >= atz) {
        stepSy = (ty > 0.0f) ? 1 : -1;
    } else {
        stepSz = (tz > 0.0f) ? 1 : -1;
    }

    int sx1 = sx0 + stepSx;
    int sy1 = sy0 + stepSy;
    int sz1 = sz0 + stepSz;

    // If stepping outside 0..7 on face, step inward along -N (sub-cell penetration)
    if (sx1 < 0 || sx1 >= kDebrisSubDiv ||
        sy1 < 0 || sy1 >= kDebrisSubDiv ||
        sz1 < 0 || sz1 >= kDebrisSubDiv) {
        sx1 = sx0 - static_cast<int>(std::round(nx));
        sy1 = sy0 - static_cast<int>(std::round(ny));
        sz1 = sz0 - static_cast<int>(std::round(nz));
    }

    sx1 = std::clamp(sx1, 0, kDebrisSubDiv - 1);
    sy1 = std::clamp(sy1, 0, kDebrisSubDiv - 1);
    sz1 = std::clamp(sz1, 0, kDebrisSubDiv - 1);

    if (sx1 == sx0 && sy1 == sy0 && sz1 == sz0) {
        if (sx0 > 0) sx1 = sx0 - 1;
        else if (sx0 < kDebrisSubDiv - 1) sx1 = sx0 + 1;
        else if (sy0 > 0) sy1 = sy0 - 1;
        else if (sy0 < kDebrisSubDiv - 1) sy1 = sy0 + 1;
        else if (sz0 > 0) sz1 = sz0 - 1;
        else sz1 = sz0 + 1;
    }

    imp.units[1] = {static_cast<uint8_t>(sx1), static_cast<uint8_t>(sy1), static_cast<uint8_t>(sz1)};
    return imp;
}

struct DebrisParticle {
    float px = 0, py = 0, pz = 0;
    float vx = 0, vy = 0, vz = 0;
    float life = 0;
    float maxLife = 1;
    float cr = 0.5f, cg = 0.5f, cb = 0.5f;
    float half = kDebrisSubSize * 0.5f * kDebrisVisualScale;
    MaterialId mat = MaterialId::Dirt;
    bool alive = false;
    bool stationary = false;
    bool emissive = false;
    int bounces = 0;
    uint8_t sx = 0, sy = 0, sz = 0; // sub-lattice cell (0..7)
};

struct DebrisSystem {
    std::vector<DebrisParticle> particles;
    std::vector<int> freeList; // dead indices for O(1) reuse
    int spawnedTotal = 0;
    int activePeak = 0;
    int ricochets = 0;
    int spawnBurstFrame = 0;
    int aliveCached = 0;
    int sublatticeImpactsDirect = 0;
    int sublatticeImpactsOblique = 0;
    int sublatticeUnitsImpacted = 0;
    bool meshDirty = true;
    uint32_t rng = 0xA341316Cu;

    void clear() {
        particles.clear();
        freeList.clear();
        spawnedTotal = 0;
        activePeak = 0;
        aliveCached = 0;
        sublatticeImpactsDirect = 0;
        sublatticeImpactsOblique = 0;
        sublatticeUnitsImpacted = 0;
        meshDirty = true;
    }

    float frand() {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>((rng >> 8) & 0xFFFF) / 65535.0f;
    }

    float frandSigned() { return frand() * 2.0f - 1.0f; }

    void materialColor(MaterialId id, float& r, float& g, float& b) const {
        switch (id) {
        case MaterialId::Wood:        r = 0.55f; g = 0.34f; b = 0.16f; break;
        case MaterialId::Concrete:    r = 0.72f; g = 0.70f; b = 0.66f; break;
        case MaterialId::Dirt:        r = 0.48f; g = 0.34f; b = 0.16f; break;
        case MaterialId::BushLeaves:  r = 0.22f; g = 0.58f; b = 0.18f; break;
        case MaterialId::BushBranch:  r = 0.38f; g = 0.26f; b = 0.12f; break;
        case MaterialId::SheetMetal:  r = 0.78f; g = 0.82f; b = 0.88f; break;
        case MaterialId::Girder:      r = 0.52f; g = 0.28f; b = 0.16f; break;
        case MaterialId::CarbonFiber: r = 0.18f; g = 0.18f; b = 0.22f; break;
        case MaterialId::TreatedWood: r = 0.50f; g = 0.32f; b = 0.18f; break;
        case MaterialId::Plexiglass:  r = 0.78f; g = 0.92f; b = 1.00f; break;
        case MaterialId::Sand:        r = 0.82f; g = 0.74f; b = 0.54f; break;
        case MaterialId::Grass:       r = 0.30f; g = 0.52f; b = 0.20f; break;
        case MaterialId::Snow:        r = 0.92f; g = 0.94f; b = 0.97f; break;
        case MaterialId::Asphalt:     r = 0.26f; g = 0.26f; b = 0.28f; break;
        default:                      r = 0.62f; g = 0.62f; b = 0.62f; break;
        }
    }

    void beginFrame() { spawnBurstFrame = 0; }

    DebrisParticle* allocParticle() {
        if (!freeList.empty()) {
            int idx = freeList.back();
            freeList.pop_back();
            return &particles[static_cast<size_t>(idx)];
        }
        if (static_cast<int>(particles.size()) >= kMaxDebris) return nullptr;
        particles.emplace_back();
        return &particles.back();
    }

    // Spawn sublattice impact effects: 1 or 2 impacted units depending on angle of impact.
    // Creates spark flash cubes, stationary crater marks, and localized chip emission.
    void spawnSublatticeImpact(int ix, int iy, int iz, MaterialId mat,
                               const SublatticeImpact& sub,
                               float energy, float voxelSize, float aoeScale = 1.0f) {
        if (mat == MaterialId::Air || mat == MaterialId::Water) return;
        if (spawnBurstFrame >= kHardMaxSpawnBurst) return;

        SublatticeImpact effSub = sub;
        if (effSub.count <= 0) {
            float hitX = (static_cast<float>(ix) + 0.5f) * voxelSize;
            float hitY = (static_cast<float>(iy) + 0.5f) * voxelSize;
            float hitZ = (static_cast<float>(iz) + 0.5f) * voxelSize;
            effSub = computeSublatticeImpact(ix, iy, iz, hitX, hitY, hitZ, 0, 0, -1, 0, 0, 1, voxelSize);
        }

        if (effSub.count == 1) ++sublatticeImpactsDirect;
        else ++sublatticeImpactsOblique;
        sublatticeUnitsImpacted += effSub.count;

        const auto& props = materialProps(mat);
        const float dens = std::max(0.15f, props.density);
        const float densFactor = 1.0f / std::sqrt(dens);
        float aoe = std::clamp(aoeScale, 0.25f, 4.0f);
        int budget = static_cast<int>(
            (6.0f + props.fragility * 22.0f + std::min(energy, 32.0f) * 0.65f) * aoe * densFactor);
        budget = std::clamp(budget, 4, kMaxSpawnPerVoxel);
        budget = std::min(budget, kHardMaxSpawnBurst - spawnBurstFrame);
        if (budget <= 0) return;

        float cr, cg, cb;
        materialColor(mat, cr, cg, cb);
        const float massScale = std::max(0.25f, props.density * props.weight * 0.18f);
        const float baseSpeed =
            (0.055f + energy * 0.0085f * (0.45f + props.fragility)) * densFactor * std::sqrt(aoe);

        const int unitCount = std::clamp(effSub.count, 1, 2);
        const float subSize = voxelSize / static_cast<float>(kDebrisSubDiv);

        for (int u = 0; u < unitCount; ++u) {
            const auto& su = effSub.units[u];

            // 1. Impact Spark Flash (emissive, brief, bounces away from face)
            if (spawnBurstFrame < kHardMaxSpawnBurst) {
                DebrisParticle* spark = allocParticle();
                if (spark) {
                    spark->sx = su.sx; spark->sy = su.sy; spark->sz = su.sz;
                    spark->px = (static_cast<float>(ix) + (su.sx + 0.5f) / static_cast<float>(kDebrisSubDiv)) * voxelSize + effSub.nx * (subSize * 0.35f);
                    spark->py = (static_cast<float>(iy) + (su.sy + 0.5f) / static_cast<float>(kDebrisSubDiv)) * voxelSize + effSub.ny * (subSize * 0.35f);
                    spark->pz = (static_cast<float>(iz) + (su.sz + 0.5f) / static_cast<float>(kDebrisSubDiv)) * voxelSize + effSub.nz * (subSize * 0.35f);
                    spark->vx = effSub.nx * 0.08f + frandSigned() * 0.03f;
                    spark->vy = effSub.ny * 0.08f + frandSigned() * 0.03f + 0.02f;
                    spark->vz = effSub.nz * 0.08f + frandSigned() * 0.03f;
                    spark->life = 0.09f + frand() * 0.04f;
                    spark->maxLife = spark->life;
                    spark->cr = 1.0f; spark->cg = 0.92f; spark->cb = 0.65f;
                    spark->half = subSize * 0.65f;
                    spark->mat = mat;
                    spark->alive = true;
                    spark->stationary = false;
                    spark->emissive = true;
                    spark->bounces = 0;
                    ++spawnedTotal;
                    ++spawnBurstFrame;
                }
            }

            // 2. Surface Crater Dent Mark (stationary chipped dent cube at the sublattice unit)
            if (spawnBurstFrame < kHardMaxSpawnBurst) {
                DebrisParticle* dent = allocParticle();
                if (dent) {
                    dent->sx = su.sx; dent->sy = su.sy; dent->sz = su.sz;
                    dent->px = (static_cast<float>(ix) + (su.sx + 0.5f) / static_cast<float>(kDebrisSubDiv)) * voxelSize;
                    dent->py = (static_cast<float>(iy) + (su.sy + 0.5f) / static_cast<float>(kDebrisSubDiv)) * voxelSize;
                    dent->pz = (static_cast<float>(iz) + (su.sz + 0.5f) / static_cast<float>(kDebrisSubDiv)) * voxelSize;
                    dent->vx = 0.0f; dent->vy = 0.0f; dent->vz = 0.0f;
                    dent->life = 1.8f;
                    dent->maxLife = 1.8f;
                    dent->cr = cr * 0.45f; dent->cg = cg * 0.45f; dent->cb = cb * 0.45f;
                    dent->half = subSize * 0.48f;
                    dent->mat = mat;
                    dent->alive = true;
                    dent->stationary = true;
                    dent->emissive = false;
                    dent->bounces = 99;
                    ++spawnedTotal;
                    ++spawnBurstFrame;
                }
            }

            // 3. Debris Chips originating from this specific impacted sublattice unit
            int chipsForThisUnit = budget / unitCount;
            if (u == 0) chipsForThisUnit += (budget % unitCount);
            for (int c = 0; c < chipsForThisUnit; ++c) {
                if (spawnBurstFrame >= kHardMaxSpawnBurst) break;
                DebrisParticle* slot = allocParticle();
                if (!slot) break;
                fillSublatticeParticle(*slot, ix, iy, iz, su.sx, su.sy, su.sz, mat, cr, cg, cb,
                                       effSub, baseSpeed, massScale, props.fragility, voxelSize);
                ++spawnedTotal;
                ++spawnBurstFrame;
            }
        }
        meshDirty = true;
    }

    void fillSublatticeParticle(DebrisParticle& p, int ix, int iy, int iz,
                                uint8_t sx, uint8_t sy, uint8_t sz, MaterialId mat,
                                float cr, float cg, float cb,
                                const SublatticeImpact& sub,
                                float baseSpeed, float massScale, float fragility,
                                float voxelSize) {
        const float subSize = voxelSize / static_cast<float>(kDebrisSubDiv);
        p.sx = sx; p.sy = sy; p.sz = sz;
        p.px = (static_cast<float>(ix) + (sx + 0.5f) / static_cast<float>(kDebrisSubDiv)) * voxelSize;
        p.py = (static_cast<float>(iy) + (sy + 0.5f) / static_cast<float>(kDebrisSubDiv)) * voxelSize;
        p.pz = (static_cast<float>(iz) + (sz + 0.5f) / static_cast<float>(kDebrisSubDiv)) * voxelSize;

        float dirX, dirY, dirZ;
        if (sub.count == 1) {
            // Direct impact: rebound cone off surface normal + loft
            dirX = sub.nx * 0.75f + frandSigned() * (0.45f + fragility);
            dirY = sub.ny * 0.75f + frandSigned() * (0.45f + fragility) + 0.40f;
            dirZ = sub.nz * 0.75f + frandSigned() * (0.45f + fragility);
        } else {
            // Oblique impact: graze spray along tangential direction + scatter
            float gx = -sub.nx * 0.2f + (1.0f - std::fabs(sub.nx)) * frandSigned() * 0.8f;
            float gy = -sub.ny * 0.2f + 0.35f;
            float gz = -sub.nz * 0.2f + (1.0f - std::fabs(sub.nz)) * frandSigned() * 0.8f;
            dirX = sub.nx * 0.35f + gx;
            dirY = sub.ny * 0.35f + gy;
            dirZ = sub.nz * 0.35f + gz;
        }
        float dlen = std::sqrt(dirX * dirX + dirY * dirY + dirZ * dirZ);
        if (dlen > 1e-6f) { dirX /= dlen; dirY /= dlen; dirZ /= dlen; }
        else { dirX = 0; dirY = 1.0f; dirZ = 0; }

        Mat3 scatter = Mat3::scale(baseSpeed / std::max(0.25f, massScale));
        scatter.mulVec(dirX, dirY, dirZ, p.vx, p.vy, p.vz);

        p.life = 0.70f + frand() * 1.35f + (1.0f - fragility) * 0.35f;
        p.maxLife = p.life;
        p.cr = std::min(1.0f, cr * (1.05f + frand() * 0.35f));
        p.cg = std::min(1.0f, cg * (1.05f + frand() * 0.35f));
        p.cb = std::min(1.0f, cb * (1.05f + frand() * 0.35f));
        p.half = subSize * 0.5f * kDebrisVisualScale * (0.85f + frand() * 0.35f);
        p.mat = mat;
        p.alive = true;
        p.stationary = false;
        p.emissive = false;
        p.bounces = 0;
    }

    // Spawn visual chips from a unit voxel; wraps computeSublatticeImpact + spawnSublatticeImpact.
    void spawnFromVoxel(int ix, int iy, int iz, MaterialId mat,
                        float impactDx, float impactDy, float impactDz,
                        float energy, float voxelSize, float aoeScale = 1.0f) {
        float nx, ny, nz;
        faceNormalFromVelocity(impactDx, impactDy, impactDz, nx, ny, nz);
        float hitX = (static_cast<float>(ix) + 0.5f + nx * 0.5f) * voxelSize;
        float hitY = (static_cast<float>(iy) + 0.5f + ny * 0.5f) * voxelSize;
        float hitZ = (static_cast<float>(iz) + 0.5f + nz * 0.5f) * voxelSize;
        SublatticeImpact sub = computeSublatticeImpact(ix, iy, iz, hitX, hitY, hitZ,
                                                      impactDx, impactDy, impactDz,
                                                      nx, ny, nz, voxelSize);
        spawnSublatticeImpact(ix, iy, iz, mat, sub, energy, voxelSize, aoeScale);
    }

    int activeCount() const { return aliveCached; }

    void update(float dt, float gravity) {
        int active = 0;
        bool any = false;
        const float damp = std::max(0.0f, 1.0f - 0.35f * dt);
        for (int i = 0; i < static_cast<int>(particles.size()); ++i) {
            DebrisParticle& p = particles[static_cast<size_t>(i)];
            if (!p.alive) continue;
            any = true;
            if (!p.stationary) {
                const bool wasAirborne = p.py > p.half + 1e-5f;
                // Integrate; only apply gravity while airborne so resting chips don't "re-hit".
                if (wasAirborne) p.vy -= gravity * dt;
                p.px += p.vx * dt;
                p.py += p.vy * dt;
                p.pz += p.vz * dt;
                // Ground plane bounce (world y = 0)
                if (p.py < p.half) {
                    p.py = p.half;
                    if (wasAirborne && p.vy < -0.01f) {
                        ricochetVelocity(p.vx, p.vy, p.vz, 0, 1, 0, 0.28f, 0.42f);
                        p.vx *= 0.82f; p.vz *= 0.82f;
                        ++p.bounces;
                        ++ricochets;
                    } else {
                        p.vy = 0.0f;
                        p.vx *= 0.88f; p.vz *= 0.88f;
                    }
                    if (p.bounces > 5 || (p.vx * p.vx + p.vy * p.vy + p.vz * p.vz) < 4e-6f) {
                        p.vx = p.vy = p.vz = 0;
                    }
                } else {
                    // Air drag keeps chips from flying forever at high energy.
                    p.vx *= damp; p.vy *= damp; p.vz *= damp;
                }
            }
            p.life -= dt;
            if (p.life <= 0.0f) {
                p.alive = false;
                freeList.push_back(i);
            } else {
                ++active;
            }
        }
        aliveCached = active;
        if (active > activePeak) activePeak = active;
        if (any || meshDirty) meshDirty = true;

        // Compact only when free-list grows large (avoids per-frame erase churn).
        if (freeList.size() > 256 && particles.size() > 400) {
            std::vector<DebrisParticle> compact;
            compact.reserve(static_cast<size_t>(active + 8));
            for (const auto& p : particles) if (p.alive) compact.push_back(p);
            particles.swap(compact);
            freeList.clear();
            aliveCached = static_cast<int>(particles.size());
            meshDirty = true;
        }
    }
};

// Emit axis-aligned cube faces for a debris particle (sharp unit-style cubes).
// push(px,py,pz, nx,ny,nz, cr,cg,cb, matId)
template <typename PushVert>
inline void emitDebrisCube(const DebrisParticle& p, float matId, PushVert&& push) {
    if (!p.alive) return;
    const float h = p.half;
    const float x0 = p.px - h, x1 = p.px + h;
    const float y0 = p.py - h, y1 = p.py + h;
    const float z0 = p.pz - h, z1 = p.pz + h;
    const float fade = std::clamp(p.life / std::max(0.05f, p.maxLife), 0.22f, 1.0f);
    // Keep albedo bright while alive; fade only near end.
    const float boost = p.emissive ? 1.0f : (0.55f + 0.45f * fade);
    const float cr = p.cr * boost, cg = p.cg * boost, cb = p.cb * boost;
    auto quad = [&](float ax, float ay, float az, float bx, float by, float bz,
                    float cx, float cy, float cz, float dx, float dy, float dz,
                    float nx, float ny, float nz) {
        push(ax, ay, az, nx, ny, nz, cr, cg, cb, matId);
        push(bx, by, bz, nx, ny, nz, cr, cg, cb, matId);
        push(cx, cy, cz, nx, ny, nz, cr, cg, cb, matId);
        push(ax, ay, az, nx, ny, nz, cr, cg, cb, matId);
        push(cx, cy, cz, nx, ny, nz, cr, cg, cb, matId);
        push(dx, dy, dz, nx, ny, nz, cr, cg, cb, matId);
    };
    quad(x1,y0,z0, x1,y1,z0, x1,y1,z1, x1,y0,z1,  1,0,0);
    quad(x0,y0,z1, x0,y1,z1, x0,y1,z0, x0,y0,z0, -1,0,0);
    quad(x0,y1,z0, x0,y1,z1, x1,y1,z1, x1,y1,z0,  0,1,0);
    quad(x0,y0,z1, x0,y0,z0, x1,y0,z0, x1,y0,z1,  0,-1,0);
    quad(x0,y0,z1, x1,y0,z1, x1,y1,z1, x0,y1,z1,  0,0,1);
    quad(x1,y0,z0, x0,y0,z0, x0,y1,z0, x1,y1,z0,  0,0,-1);
}

// Axis-aligned emissive cube (muzzle flash / spark), not lattice-bound.
template <typename PushVert>
inline void emitFlashCube(float px, float py, float pz, float half,
                          float cr, float cg, float cb, float matId, PushVert&& push) {
    const float x0 = px - half, x1 = px + half;
    const float y0 = py - half, y1 = py + half;
    const float z0 = pz - half, z1 = pz + half;
    auto quad = [&](float ax, float ay, float az, float bx, float by, float bz,
                    float cx, float cy, float cz, float dx, float dy, float dz,
                    float nx, float ny, float nz) {
        push(ax, ay, az, nx, ny, nz, cr, cg, cb, matId);
        push(bx, by, bz, nx, ny, nz, cr, cg, cb, matId);
        push(cx, cy, cz, nx, ny, nz, cr, cg, cb, matId);
        push(ax, ay, az, nx, ny, nz, cr, cg, cb, matId);
        push(cx, cy, cz, nx, ny, nz, cr, cg, cb, matId);
        push(dx, dy, dz, nx, ny, nz, cr, cg, cb, matId);
    };
    quad(x1,y0,z0, x1,y1,z0, x1,y1,z1, x1,y0,z1,  1,0,0);
    quad(x0,y0,z1, x0,y1,z1, x0,y1,z0, x0,y0,z0, -1,0,0);
    quad(x0,y1,z0, x0,y1,z1, x1,y1,z1, x1,y1,z0,  0,1,0);
    quad(x0,y0,z1, x0,y0,z0, x1,y0,z0, x1,y0,z1,  0,-1,0);
    quad(x0,y0,z1, x1,y0,z1, x1,y1,z1, x0,y1,z1,  0,0,1);
    quad(x1,y0,z0, x0,y0,z0, x0,y1,z0, x1,y1,z0,  0,0,-1);
}

// Headless contract checks for sublattice impacts (1 or 2 units based on impact angle).
inline bool sublatticeImpactSelfTest() {
    const float vs = 0.001f;
    // 1. Direct hit (normal impact, angle 0 deg, cosTheta = 1.0 >= 0.7071) -> 1 unit
    {
        SublatticeImpact sub = computeSublatticeImpact(10, 5, 20,
            (10.0f + 0.5f) * vs, (5.0f + 0.5f) * vs, 20.0f * vs,
            0.0f, 0.0f, 1.0f,
            0.0f, 0.0f, -1.0f, vs);
        if (sub.count != 1) return false;
        if (sub.cosTheta < 0.999f) return false;
        if (sub.units[0].sx >= kDebrisSubDiv || sub.units[0].sy >= kDebrisSubDiv || sub.units[0].sz >= kDebrisSubDiv)
            return false;
        if (sub.units[0].sz != 0) return false; // struck -Z face
    }
    // 2. Oblique hit (angle 60 deg, cosTheta = 0.5 < 0.7071) -> 2 units along tangent
    {
        SublatticeImpact sub = computeSublatticeImpact(10, 5, 20,
            (10.0f + 0.35f) * vs, (5.0f + 0.5f) * vs, 20.0f * vs,
            0.866025f, 0.0f, 0.5f,
            0.0f, 0.0f, -1.0f, vs);
        if (sub.count != 2) return false;
        if (std::fabs(sub.cosTheta - 0.5f) > 0.01f) return false;
        if (sub.units[0].sx >= kDebrisSubDiv || sub.units[0].sy >= kDebrisSubDiv || sub.units[0].sz >= kDebrisSubDiv)
            return false;
        if (sub.units[1].sx >= kDebrisSubDiv || sub.units[1].sy >= kDebrisSubDiv || sub.units[1].sz >= kDebrisSubDiv)
            return false;
        if (sub.units[0].sx == sub.units[1].sx &&
            sub.units[0].sy == sub.units[1].sy &&
            sub.units[0].sz == sub.units[1].sz)
            return false; // units must be distinct
        if (sub.units[1].sx != sub.units[0].sx + 1) return false; // stepped along +X tangent
    }
    // 3. Edge clamp test (oblique hit starting at boundary sx=7)
    {
        SublatticeImpact sub = computeSublatticeImpact(10, 5, 20,
            (10.0f + 0.95f) * vs, (5.0f + 0.5f) * vs, 20.0f * vs,
            0.866025f, 0.0f, 0.5f,
            0.0f, 0.0f, -1.0f, vs);
        if (sub.count != 2) return false;
        if (sub.units[0].sx != 7) return false;
        if (sub.units[1].sx >= kDebrisSubDiv || sub.units[1].sy >= kDebrisSubDiv || sub.units[1].sz >= kDebrisSubDiv)
            return false;
        if (sub.units[0].sx == sub.units[1].sx &&
            sub.units[0].sy == sub.units[1].sy &&
            sub.units[0].sz == sub.units[1].sz)
            return false;
    }
    // 4. DebrisSystem spawnSublatticeImpact allocation & particle setup
    {
        DebrisSystem ds;
        SublatticeImpact sub = computeSublatticeImpact(10, 5, 20,
            (10.0f + 0.35f) * vs, (5.0f + 0.5f) * vs, 20.0f * vs,
            0.866025f, 0.0f, 0.5f,
            0.0f, 0.0f, -1.0f, vs);
        ds.spawnSublatticeImpact(10, 5, 20, MaterialId::Concrete, sub, 15.0f, vs, 1.0f);
        if (ds.particles.empty() || ds.spawnedTotal <= 0) return false;
        if (ds.sublatticeImpactsOblique != 1) return false;
        if (ds.sublatticeUnitsImpacted != 2) return false;
        bool hasSpark = false;
        bool hasDent = false;
        for (const auto& p : ds.particles) {
            if (p.alive && p.emissive) hasSpark = true;
            if (p.alive && p.stationary) hasDent = true;
        }
        if (!hasSpark || !hasDent) return false;
    }
    return true;
}
