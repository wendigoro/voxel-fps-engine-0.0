#pragma once
// Visual degradation: 8x8x8 sub-voxel debris + matrix trajectory/ricochet.
// Display/effects only — occupancy remains the cubic unit grid (VOXEL_SIZE).

#include "materials.hpp"

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
inline constexpr float kDebrisMatId = 5.0f;
inline constexpr float kMuzzleMatId = 6.0f;

struct DebrisParticle {
    float px = 0, py = 0, pz = 0;
    float vx = 0, vy = 0, vz = 0;
    float life = 0;
    float maxLife = 1;
    float cr = 0.5f, cg = 0.5f, cb = 0.5f;
    float half = kDebrisSubSize * 0.5f * kDebrisVisualScale;
    MaterialId mat = MaterialId::Dirt;
    bool alive = false;
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
    bool meshDirty = true;
    uint32_t rng = 0xA341316Cu;

    void clear() {
        particles.clear();
        freeList.clear();
        spawnedTotal = 0;
        activePeak = 0;
        aliveCached = 0;
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

    // Spawn visual 8x8x8-cell chips from a destroyed (or chipped) unit voxel.
    void spawnFromVoxel(int ix, int iy, int iz, MaterialId mat,
                        float impactDx, float impactDy, float impactDz,
                        float energy, float voxelSize, float aoeScale = 1.0f) {
        if (mat == MaterialId::Air || mat == MaterialId::Water) return;
        if (spawnBurstFrame >= kHardMaxSpawnBurst) return;
        const auto& props = materialProps(mat);
        const float dens = std::max(0.15f, props.density);
        const float densFactor = 1.0f / std::sqrt(dens);
        float aoe = std::clamp(aoeScale, 0.25f, 4.0f);
        int budget = static_cast<int>(
            (6.0f + props.fragility * 22.0f + std::min(energy, 32.0f) * 0.65f) * aoe * densFactor);
        budget = std::clamp(budget, 4, kMaxSpawnPerVoxel);
        budget = std::min(budget, kHardMaxSpawnBurst - spawnBurstFrame);
        if (budget <= 0) return;

        float ilen = std::sqrt(impactDx * impactDx + impactDy * impactDy + impactDz * impactDz);
        if (ilen < 1e-6f) { impactDx = 0; impactDy = 1; impactDz = 0; ilen = 1; }
        impactDx /= ilen; impactDy /= ilen; impactDz /= ilen;

        float cr, cg, cb;
        materialColor(mat, cr, cg, cb);
        const float massScale = std::max(0.25f, props.density * props.weight * 0.18f);
        // World-scale speeds: chips must travel multiple unit voxels to read on camera.
        const float baseSpeed =
            (0.055f + energy * 0.0085f * (0.45f + props.fragility)) * densFactor * std::sqrt(aoe);

        for (int n = 0; n < budget; ++n) {
            DebrisParticle* slot = allocParticle();
            if (!slot) break;
            fillParticle(*slot, ix, iy, iz, mat, cr, cg, cb,
                         impactDx, impactDy, impactDz,
                         baseSpeed, massScale, props.fragility, voxelSize);
            ++spawnedTotal;
            ++spawnBurstFrame;
        }
        meshDirty = true;
    }

    void fillParticle(DebrisParticle& p, int ix, int iy, int iz, MaterialId mat,
                      float cr, float cg, float cb,
                      float idx, float idy, float idz,
                      float baseSpeed, float massScale, float fragility,
                      float voxelSize) {
        // Snap chip center to the 8x8x8 sub-lattice inside the unit voxel.
        const int sx = static_cast<int>(frand() * kDebrisSubDiv) % kDebrisSubDiv;
        const int sy = static_cast<int>(frand() * kDebrisSubDiv) % kDebrisSubDiv;
        const int sz = static_cast<int>(frand() * kDebrisSubDiv) % kDebrisSubDiv;
        const float sub = voxelSize / static_cast<float>(kDebrisSubDiv);
        p.sx = static_cast<uint8_t>(sx);
        p.sy = static_cast<uint8_t>(sy);
        p.sz = static_cast<uint8_t>(sz);
        p.px = (static_cast<float>(ix) + (sx + 0.5f) / kDebrisSubDiv) * voxelSize;
        p.py = (static_cast<float>(iy) + (sy + 0.5f) / kDebrisSubDiv) * voxelSize;
        p.pz = (static_cast<float>(iz) + (sz + 0.5f) / kDebrisSubDiv) * voxelSize;

        // Explode opposite impact + lattice scatter (matrix-scaled).
        Mat3 scatter = Mat3::scale(baseSpeed / std::max(0.25f, massScale));
        float jx = -idx + frandSigned() * (0.55f + fragility);
        float jy = -idy + frandSigned() * (0.55f + fragility) + 0.55f; // loft
        float jz = -idz + frandSigned() * (0.55f + fragility);
        float jlen = std::sqrt(jx * jx + jy * jy + jz * jz);
        if (jlen > 1e-6f) { jx /= jlen; jy /= jlen; jz /= jlen; }
        scatter.mulVec(jx, jy, jz, p.vx, p.vy, p.vz);
        // Outward bias from sub-cell relative to unit center.
        p.vx += (sx - 3.5f) * sub * 18.0f;
        p.vy += (sy - 3.5f) * sub * 18.0f + baseSpeed * 0.55f;
        p.vz += (sz - 3.5f) * sub * 18.0f;

        p.life = 0.70f + frand() * 1.35f + (1.0f - fragility) * 0.35f;
        p.maxLife = p.life;
        // Slightly brighter than block albedo so chips pop against night lighting.
        p.cr = std::min(1.0f, cr * (1.05f + frand() * 0.35f));
        p.cg = std::min(1.0f, cg * (1.05f + frand() * 0.35f));
        p.cb = std::min(1.0f, cb * (1.05f + frand() * 0.35f));
        // Cubic chip: half-extent of a multi-subcell cube (still lattice-centered).
        p.half = sub * 0.5f * kDebrisVisualScale * (0.85f + frand() * 0.35f);
        p.mat = mat;
        p.alive = true;
        p.bounces = 0;
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
    const float boost = 0.55f + 0.45f * fade;
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
