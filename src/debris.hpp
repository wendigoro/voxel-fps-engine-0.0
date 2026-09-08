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
    // Scale bounce; kill some tangential energy via friction.
    const float vn = rx * nx + ry * ny + rz * nz;
    float tx = rx - vn * nx, ty = ry - vn * ny, tz = rz - vn * nz;
    tx *= (1.0f - friction);
    ty *= (1.0f - friction);
    tz *= (1.0f - friction);
    // Ensure outgoing normal component is away from surface.
    float outN = std::fabs(vn) * restitution;
    // If reflected into surface, flip.
    if (vn < 0.0f) outN = std::fabs(vn) * restitution;
    else outN = vn * restitution;
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
        nx = (vx > 0.0f) ? -1.0f : 1.0f; // outward from face we hit
    } else if (ay >= ax && ay >= az) {
        ny = (vy > 0.0f) ? -1.0f : 1.0f;
    } else {
        nz = (vz > 0.0f) ? -1.0f : 1.0f;
    }
}

// ---- Sub-voxel debris (8^3 cells per unit voxel, visual only) ----
inline constexpr int kDebrisSubDiv = 8;
inline constexpr float kDebrisSubSize = 0.001f / static_cast<float>(kDebrisSubDiv); // VOXEL_SIZE/8
inline constexpr int kMaxDebris = 2048;
inline constexpr int kMaxSpawnPerVoxel = 36; // not full 512 — keep shotgun volleys smooth
inline constexpr int kHardMaxSpawnBurst = 96; // total chips from one multi-pellet frame

struct DebrisParticle {
    float px = 0, py = 0, pz = 0;
    float vx = 0, vy = 0, vz = 0;
    float life = 0;      // seconds remaining
    float maxLife = 1;   // for fade
    float cr = 0.5f, cg = 0.5f, cb = 0.5f;
    float half = kDebrisSubSize * 0.45f;
    MaterialId mat = MaterialId::Dirt;
    bool alive = false;
    int bounces = 0;
};

struct DebrisSystem {
    std::vector<DebrisParticle> particles;
    int spawnedTotal = 0;
    int activePeak = 0;
    int ricochets = 0;
    int spawnBurstFrame = 0; // chips spawned this frame (cap)
    int aliveCached = 0;
    uint32_t rng = 0xA341316Cu;

    void clear() {
        particles.clear();
        spawnedTotal = 0;
        activePeak = 0;
        // keep ricochets cumulative for smoke
    }

    float frand() {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>((rng >> 8) & 0xFFFF) / 65535.0f;
    }

    float frandSigned() { return frand() * 2.0f - 1.0f; }

    void materialColor(MaterialId id, float& r, float& g, float& b) const {
        switch (id) {
        case MaterialId::Wood:        r = 0.45f; g = 0.28f; b = 0.12f; break;
        case MaterialId::Concrete:    r = 0.55f; g = 0.55f; b = 0.52f; break;
        case MaterialId::Dirt:        r = 0.35f; g = 0.25f; b = 0.12f; break;
        case MaterialId::BushLeaves:  r = 0.15f; g = 0.45f; b = 0.12f; break;
        case MaterialId::BushBranch:  r = 0.30f; g = 0.20f; b = 0.08f; break;
        case MaterialId::SheetMetal:  r = 0.65f; g = 0.68f; b = 0.72f; break;
        case MaterialId::Girder:      r = 0.40f; g = 0.42f; b = 0.45f; break;
        case MaterialId::CarbonFiber: r = 0.12f; g = 0.12f; b = 0.14f; break;
        case MaterialId::TreatedWood: r = 0.40f; g = 0.26f; b = 0.14f; break;
        case MaterialId::Plexiglass:  r = 0.70f; g = 0.85f; b = 0.95f; break;
        default:                      r = 0.5f;  g = 0.5f;  b = 0.5f;  break;
        }
    }

    void beginFrame() { spawnBurstFrame = 0; }

    // Spawn visual 8x8x8-cell chips from a destroyed unit voxel.
    // aoeScale: caliber/damage area factor (1 = nominal). Density reduces chip count & speed.
    void spawnFromVoxel(int ix, int iy, int iz, MaterialId mat,
                        float impactDx, float impactDy, float impactDz,
                        float energy, float voxelSize, float aoeScale = 1.0f) {
        if (mat == MaterialId::Air || mat == MaterialId::Water) return;
        if (spawnBurstFrame >= kHardMaxSpawnBurst) return;
        const auto& props = materialProps(mat);
        // Density resists fragmentation; fragility + energy + AOE scale promote it.
        const float dens = std::max(0.15f, props.density);
        const float densFactor = 1.0f / std::sqrt(dens); // denser → fewer chips
        float aoe = std::clamp(aoeScale, 0.25f, 4.0f);
        int budget = static_cast<int>(
            (4.0f + props.fragility * 28.0f + std::min(energy, 28.0f) * 0.55f) * aoe * densFactor);
        budget = std::clamp(budget, 3, kMaxSpawnPerVoxel);
        budget = std::min(budget, kHardMaxSpawnBurst - spawnBurstFrame);
        if (budget <= 0) return;

        float ilen = std::sqrt(impactDx * impactDx + impactDy * impactDy + impactDz * impactDz);
        if (ilen < 1e-6f) { impactDx = 0; impactDy = 1; impactDz = 0; ilen = 1; }
        impactDx /= ilen; impactDy /= ilen; impactDz /= ilen;

        float cr, cg, cb;
        materialColor(mat, cr, cg, cb);
        // Dense materials eject slower, lighter chips fly farther.
        const float massScale = std::max(0.25f, props.density * props.weight * 0.18f);
        const float baseSpeed = (0.015f + energy * 0.0035f * (0.45f + props.fragility)) * densFactor * std::sqrt(aoe);

        // Sample a sparse set of the 8^3 lattice (visual proxy for full sub-grid).
        for (int n = 0; n < budget; ++n) {
            if (static_cast<int>(particles.size()) >= kMaxDebris) {
                // Reuse dead slots
                bool reused = false;
                for (auto& p : particles) {
                    if (!p.alive) {
                        fillParticle(p, ix, iy, iz, mat, cr, cg, cb,
                                     impactDx, impactDy, impactDz,
                                     baseSpeed, massScale, props.fragility, voxelSize);
                        reused = true;
                        break;
                    }
                }
                if (!reused) break;
            } else {
                DebrisParticle p;
                fillParticle(p, ix, iy, iz, mat, cr, cg, cb,
                             impactDx, impactDy, impactDz,
                             baseSpeed, massScale, props.fragility, voxelSize);
                particles.push_back(p);
            }
            ++spawnedTotal;
            ++spawnBurstFrame;
        }
    }

    void fillParticle(DebrisParticle& p, int ix, int iy, int iz, MaterialId mat,
                      float cr, float cg, float cb,
                      float idx, float idy, float idz,
                      float baseSpeed, float massScale, float fragility,
                      float voxelSize) {
        // Sub-cell index within the unit voxel's 8x8x8 lattice
        const int sx = static_cast<int>(frand() * kDebrisSubDiv) % kDebrisSubDiv;
        const int sy = static_cast<int>(frand() * kDebrisSubDiv) % kDebrisSubDiv;
        const int sz = static_cast<int>(frand() * kDebrisSubDiv) % kDebrisSubDiv;
        const float sub = voxelSize / static_cast<float>(kDebrisSubDiv);
        p.px = (static_cast<float>(ix) + (sx + 0.5f) / kDebrisSubDiv) * voxelSize;
        p.py = (static_cast<float>(iy) + (sy + 0.5f) / kDebrisSubDiv) * voxelSize;
        p.pz = (static_cast<float>(iz) + (sz + 0.5f) / kDebrisSubDiv) * voxelSize;

        // Explode opposite impact + random scatter (matrix-scaled).
        Mat3 scatter = Mat3::scale(baseSpeed / std::max(0.25f, massScale));
        float jx = -idx + frandSigned() * (0.55f + fragility);
        float jy = -idy + frandSigned() * (0.55f + fragility) + 0.35f; // loft
        float jz = -idz + frandSigned() * (0.55f + fragility);
        float jlen = std::sqrt(jx * jx + jy * jy + jz * jz);
        if (jlen > 1e-6f) { jx /= jlen; jy /= jlen; jz /= jlen; }
        scatter.mulVec(jx, jy, jz, p.vx, p.vy, p.vz);
        // Bias from sub-cell offset outward from voxel center
        p.vx += (sx - 3.5f) * sub * 8.0f;
        p.vy += (sy - 3.5f) * sub * 8.0f + baseSpeed * 0.4f;
        p.vz += (sz - 3.5f) * sub * 8.0f;

        p.life = 0.45f + frand() * 1.1f + (1.0f - fragility) * 0.4f;
        p.maxLife = p.life;
        p.cr = cr * (0.85f + frand() * 0.3f);
        p.cg = cg * (0.85f + frand() * 0.3f);
        p.cb = cb * (0.85f + frand() * 0.3f);
        p.half = sub * (0.35f + frand() * 0.25f);
        p.mat = mat;
        p.alive = true;
        p.bounces = 0;
    }

    int activeCount() const { return aliveCached; }

    // Integrate debris under gravity; simple ground/ricochet on Y=0 plane and voxel faces optional.
    void update(float dt, float gravity) {
        int active = 0;
        for (auto& p : particles) {
            if (!p.alive) continue;
            p.vy -= gravity * dt;
            p.px += p.vx * dt;
            p.py += p.vy * dt;
            p.pz += p.vz * dt;
            // Ground plane bounce (world y = 0)
            if (p.py < p.half) {
                p.py = p.half;
                ricochetVelocity(p.vx, p.vy, p.vz, 0, 1, 0, 0.25f, 0.4f);
                p.vx *= 0.85f; p.vz *= 0.85f;
                ++p.bounces;
                if (p.bounces > 4 || (p.vx * p.vx + p.vy * p.vy + p.vz * p.vz) < 1e-6f) {
                    p.vx = p.vy = p.vz = 0;
                }
            }
            p.life -= dt;
            if (p.life <= 0.0f) p.alive = false;
            else ++active;
        }
        aliveCached = active;
        if (active > activePeak) activePeak = active;
        // Compact occasionally
        if (particles.size() > 512) {
            particles.erase(std::remove_if(particles.begin(), particles.end(),
                [](const DebrisParticle& p) { return !p.alive; }), particles.end());
        }
    }
};

// Emit axis-aligned cube faces for a debris particle into a vertex-like sink.
// Caller supplies a push(px,py,pz, nx,ny,nz, cr,cg,cb, mat) callback pattern via template.
template <typename PushVert>
inline void emitDebrisCube(const DebrisParticle& p, float matId, PushVert&& push) {
    if (!p.alive) return;
    const float h = p.half;
    const float x0 = p.px - h, x1 = p.px + h;
    const float y0 = p.py - h, y1 = p.py + h;
    const float z0 = p.pz - h, z1 = p.pz + h;
    const float fade = std::clamp(p.life / std::max(0.05f, p.maxLife), 0.15f, 1.0f);
    const float cr = p.cr * fade, cg = p.cg * fade, cb = p.cb * fade;
    // +X -X +Y -Y +Z -Z (two tris each)
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
