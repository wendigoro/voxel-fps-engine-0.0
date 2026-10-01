#pragma once
// The view-side mesher — builds chunk meshes from the SNAPSHOTS the simulation
// sent, and from nothing else (RULES.md, "Authority and the view/sim split").
//
// This header is deliberately sim-free: it includes only view_chunk.hpp /
// voxel_wire.hpp, never sim_world.hpp. That absence is the point. meshChunk has
// no sim::World parameter and no path to one — every face-exposure question
// ("is my neighbour empty?") is answered from the 1-cell skirt the simulation
// included in the snapshot. A client compiled against this header can produce
// the identical mesh and cannot consult, leak, or invent occupancy it was never
// shown.
//
// The wire contract (voxel_wire.hpp) asserts that sim::Block and wire::BlockId
// are the same values, so rejecting the sim type loses nothing: the mesher
// classifies blocks by their wire id and its own color table. The 6 face
// directions mirror the simulation's writer axes; the seam stays correct
// because sendChunkSnapshot (in main.cpp, the sim side) ships a symmetric
// 1-cell skirt and the sim bumps the version of every chunk whose exposed faces
// a write changes.
//
// Skirt-access telemetry is carried in an explicit Stats object so the code
// never reaches for a global: callers accumulate it and assert it stayed zero
// (the sim-view smoke does exactly that).

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "render_class.hpp"
#include "view_chunk.hpp"

namespace meshview {

// Fifteen-digit exact: RULES.md fixes the unit cube at 0.001. main.cpp also
// static_asserts this equals kVoxelSize, so the view's copy cannot drift from
// the sim's copy without a compile error.
static constexpr float kVoxelSize = 0.001f;

// The 6 unit-cube face directions. Must agree with the simulation's occupancy
// writer axes (sim_world.hpp kFaceO*); agreement is enforced by the symmetric
// skirt the send path ships plus the fixed axis directions below.
static constexpr int kFaceOX[6] = {1, -1, 0, 0, 0, 0};
static constexpr int kFaceOY[6] = {0, 0, 1, -1, 0, 0};
static constexpr int kFaceOZ[6] = {0, 0, 0, 0, 1, -1};

// Mesh-time telemetry. Only skirtAccessViolations is meaningful today; keep the
// struct so adding counters does not churn every call site.
struct Stats {
    uint64_t skirtAccessViolations = 0; // attempts to read outside the sent skirt
};

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float X, float Y, float Z) : x(X), y(Y), z(Z) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    float length() const { return std::sqrt(dot(*this)); }
    Vec3 normalized() const {
        float l = length();
        return l > 1e-8f ? (*this) * (1.0f / l) : Vec3(0, 1, 0);
    }
};

inline bool isWater(wire::BlockId b) {
    return b == wire::BlockId::Water || b == wire::BlockId::WaterCurrent;
}

// Solid cells are occupancy that shades an AO corner; water is passable.
inline bool isVoxelSolidForAo(wire::BlockId b) {
    return b != wire::BlockId::Air && !isWater(b);
}

// View-side colour table. The derived surface may paint any way it likes; this
// is the fixed lookup for the block palette as the wire carries it.
inline Vec3 blockColor(wire::BlockId b) {
    switch (b) {
    case wire::BlockId::Dirt:         return {0.28f, 0.20f, 0.12f};
    case wire::BlockId::Concrete:     return {0.40f, 0.40f, 0.42f};
    case wire::BlockId::SheetMetal:   return {0.48f, 0.50f, 0.52f};
    case wire::BlockId::Girder:       return {0.28f, 0.10f, 0.08f};
    case wire::BlockId::Wood:         return {0.34f, 0.22f, 0.12f};
    case wire::BlockId::WoodDark:     return {0.32f, 0.18f, 0.08f};
    case wire::BlockId::Water:        return {0.12f, 0.28f, 0.42f};
    case wire::BlockId::WaterCurrent: return {0.10f, 0.35f, 0.48f};
    case wire::BlockId::Moon:         return {0.75f, 0.80f, 0.90f};
    case wire::BlockId::LightBulb:    return {1.00f, 0.75f, 0.45f};
    default:                          return {1, 0, 1};
    }
}

// A block as read out of a sent snapshot. This is the ONLY way view-side code
// learns occupancy — it has no other source. Reads outside the skirt are
// counted, not silently served.
inline wire::BlockId sentBlockAt(const view::ViewChunk& vc, Stats& stats, int lx, int ly, int lz) {
    if (!view::SentCells::inSkirt(lx, ly, lz)) {
        ++stats.skirtAccessViolations;
        return wire::BlockId::Air;
    }
    return vc.sent.get(lx, ly, lz);
}

inline bool isVoxelSolidForAo(const view::ViewChunk& vc, Stats& stats, int lx, int ly, int lz) {
    return isVoxelSolidForAo(sentBlockAt(vc, stats, lx, ly, lz));
}

// Sharp vertex face emit: 6 unique verts/face (2 tris), hard face normals, no sharing.
inline void emitSharpFace(std::vector<view::ViewChunk::Vertex>& out, int ix, int iy, int iz,
                          int face, const Vec3& color, float mat = 0.0f) {
    // unit cube corners in voxel space, scaled to world by kVoxelSize
    static const float F[6][4][3] = {
        {{1,0,0},{1,1,0},{1,1,1},{1,0,1}}, // +X
        {{0,0,1},{0,1,1},{0,1,0},{0,0,0}}, // -X
        {{0,1,0},{0,1,1},{1,1,1},{1,1,0}}, // +Y
        {{0,0,1},{0,0,0},{1,0,0},{1,0,1}}, // -Y
        {{1,0,1},{1,1,1},{0,1,1},{0,0,1}}, // +Z
        {{0,0,0},{0,1,0},{1,1,0},{1,0,0}}, // -Z
    };
    static const float N[6][3] = {
        {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
    };
    // CCW when viewed from outside, matching Vulkan front-face CCW + Y-flip proj
    static const int IDX[6] = {0, 1, 2, 0, 2, 3};
    static const float faceShade[6] = {0.82f, 0.68f, 1.0f, 0.52f, 0.90f, 0.74f};

    const float ox = ix * kVoxelSize;
    const float oy = iy * kVoxelSize;
    const float oz = iz * kVoxelSize;
    Vec3 c = color * faceShade[face];

    for (int i = 0; i < 6; ++i) {
        const float* p = F[face][IDX[i]];
        out.push_back(view::ViewChunk::Vertex{
            ox + p[0] * kVoxelSize,
            oy + p[1] * kVoxelSize,
            oz + p[2] * kVoxelSize,
            N[face][0], N[face][1], N[face][2],
            c.x, c.y, c.z,
            mat
        });
    }
}

// Surface smoothing and Corner Ambient Occlusion (Milestone 4).
// Computes Minecraft-style 3-neighbor corner AO and smooth vertex normals
// from adjacent blocks in the 1-cell skirt, while occupancy remains strictly 1x1x1 cubes.
inline void emitSmoothedFace(view::ViewChunk& vc, Stats& stats,
                             int lx, int ly, int lz,
                             int gx, int gy, int gz, int face,
                             const Vec3& color, float mat = 0.0f) {
    static const float F[6][4][3] = {
        {{1,0,0},{1,1,0},{1,1,1},{1,0,1}}, // +X
        {{0,0,1},{0,1,1},{0,1,0},{0,0,0}}, // -X
        {{0,1,0},{0,1,1},{1,1,1},{1,1,0}}, // +Y
        {{0,0,1},{0,0,0},{1,0,0},{1,0,1}}, // -Y
        {{1,0,1},{1,1,1},{0,1,1},{0,0,1}}, // +Z
        {{0,0,0},{0,1,0},{1,1,0},{1,0,0}}, // -Z
    };
    static const float N[6][3] = {
        {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
    };
    static const float faceShade[6] = {0.82f, 0.68f, 1.0f, 0.52f, 0.90f, 0.74f};
    static const float kAoCurve[4] = {0.58f, 0.72f, 0.86f, 1.0f};

    const float ox = gx * kVoxelSize;
    const float oy = gy * kVoxelSize;
    const float oz = gz * kVoxelSize;

    const int nx = static_cast<int>(N[face][0]);
    const int ny = static_cast<int>(N[face][1]);
    const int nz = static_cast<int>(N[face][2]);
    const int adjX = lx + nx;
    const int adjY = ly + ny;
    const int adjZ = lz + nz;

    int aoVal[4] = {3, 3, 3, 3};
    Vec3 cornerNorm[4];
    Vec3 cornerCol[4];

    for (int k = 0; k < 4; ++k) {
        const float* p = F[face][k];
        const int px = static_cast<int>(p[0]);
        const int py = static_cast<int>(p[1]);
        const int pz = static_cast<int>(p[2]);

        const int dx = 2 * px - 1;
        const int dy = 2 * py - 1;
        const int dz = 2 * pz - 1;

        int ux = 0, uy = 0, uz = 0;
        int vx = 0, vy = 0, vz = 0;
        if (nx != 0) {
            uy = dy;
            vz = dz;
        } else if (ny != 0) {
            ux = dx;
            vz = dz;
        } else {
            ux = dx;
            vy = dy;
        }

        // Corner Ambient Occlusion (Minecraft-style 3-neighbor test)
        if (mat == 0.0f) {
            bool s1 = isVoxelSolidForAo(vc, stats, adjX + ux, adjY + uy, adjZ + uz);
            bool s2 = isVoxelSolidForAo(vc, stats, adjX + vx, adjY + vy, adjZ + vz);
            bool sc = isVoxelSolidForAo(vc, stats, adjX + ux + vx, adjY + uy + vy, adjZ + uz + vz);
            aoVal[k] = (s1 && s2) ? 0 : 3 - (static_cast<int>(s1) + static_cast<int>(s2) + static_cast<int>(sc));
        } else {
            aoVal[k] = 3;
        }
        const float aoFactor = kAoCurve[aoVal[k]];
        cornerCol[k] = color * faceShade[face] * aoFactor;

        // Vertex normal smoothing: inspect 8 cubes around vertex in 1-cell skirt
        if (mat == 0.0f) {
            Vec3 vGrad(0.0f, 0.0f, 0.0f);
            for (int dxi = 0; dxi < 2; ++dxi) {
                int cdx = (dxi == 0) ? (px - 1) : px;
                float offX = (cdx == px) ? 0.5f : -0.5f;
                for (int dyi = 0; dyi < 2; ++dyi) {
                    int cdy = (dyi == 0) ? (py - 1) : py;
                    float offY = (cdy == py) ? 0.5f : -0.5f;
                    for (int dzi = 0; dzi < 2; ++dzi) {
                        int cdz = (dzi == 0) ? (pz - 1) : pz;
                        float offZ = (cdz == pz) ? 0.5f : -0.5f;
                        if (isVoxelSolidForAo(vc, stats, lx + cdx, ly + cdy, lz + cdz)) {
                            vGrad.x -= offX;
                            vGrad.y -= offY;
                            vGrad.z -= offZ;
                        }
                    }
                }
            }
            if (vGrad.length() > 1e-4f) {
                Vec3 vNorm = vGrad.normalized();
                Vec3 fNorm(N[face][0], N[face][1], N[face][2]);
                if (vNorm.dot(fNorm) > 0.15f) {
                    cornerNorm[k] = (fNorm * 0.35f + vNorm * 0.65f).normalized();
                } else {
                    cornerNorm[k] = fNorm;
                }
            } else {
                cornerNorm[k] = Vec3(N[face][0], N[face][1], N[face][2]);
            }
        } else {
            cornerNorm[k] = Vec3(N[face][0], N[face][1], N[face][2]);
        }
    }

    // Quad triangulation: flip diagonal if ao0 + ao2 > ao1 + ao3 to prevent anisotropic creasing
    int indices[6];
    if (aoVal[0] + aoVal[2] > aoVal[1] + aoVal[3]) {
        indices[0] = 1; indices[1] = 2; indices[2] = 3;
        indices[3] = 1; indices[4] = 3; indices[5] = 0;
    } else {
        indices[0] = 0; indices[1] = 1; indices[2] = 2;
        indices[3] = 0; indices[4] = 2; indices[5] = 3;
    }

    for (int i = 0; i < 6; ++i) {
        int ci = indices[i];
        const float* p = F[face][ci];
        vc.mesh.push_back(view::ViewChunk::Vertex{
            ox + p[0] * kVoxelSize,
            oy + p[1] * kVoxelSize,
            oz + p[2] * kVoxelSize,
            cornerNorm[ci].x, cornerNorm[ci].y, cornerNorm[ci].z,
            cornerCol[ci].x, cornerCol[ci].y, cornerCol[ci].z,
            mat
        });
    }
}

// Build a chunk's mesh from the cells the sim SENT, and nothing else.
//
// Note the signature: there is no sim::World parameter. That absence is the
// point. Every face-exposure question ("is my neighbour empty?") is answered
// from the 1-cell skirt the sim included in the snapshot, so this function
// physically cannot consult occupancy the client was not shown. A view client
// given this struct and its SentCells can produce the identical mesh, and has
// no path to anything else.
inline void meshChunk(view::ViewChunk& chunk, Stats& stats) {
    chunk.mesh.clear();
    chunk.mesh.reserve(4096);

    const int baseX = chunk.cx * wire::kChunkSize;
    const int baseY = chunk.cy * wire::kChunkSize;
    const int baseZ = chunk.cz * wire::kChunkSize;

    for (int ly = 0; ly < wire::kChunkSize; ++ly) {
        for (int lz = 0; lz < wire::kChunkSize; ++lz) {
            for (int lx = 0; lx < wire::kChunkSize; ++lx) {
                const wire::BlockId b = sentBlockAt(chunk, stats, lx, ly, lz);
                if (b == wire::BlockId::Air) continue;
                const int x = baseX + lx, y = baseY + ly, z = baseZ + lz;
                Vec3 col = blockColor(b);

                for (int f = 0; f < 6; ++f) {
                    // Read the neighbour out of the skirt. lx+1 == CHUNK_SIZE
                    // is still inside the snapshot, so this never leaves the
                    // data the sim sent.
                    const wire::BlockId nb = sentBlockAt(
                        chunk, stats, lx + kFaceOX[f], ly + kFaceOY[f], lz + kFaceOZ[f]);
                    // Unit-cube face exposed only against empty grid cells.
                    bool expose = false;
                    if (isWater(b)) {
                        expose = (nb == wire::BlockId::Air) || (!isWater(nb) && nb != wire::BlockId::Air);
                        // show water surface against air only for clearer tide paint
                        expose = (nb == wire::BlockId::Air);
                    } else {
                        expose = (nb == wire::BlockId::Air) || isWater(nb);
                    }
                    if (!expose) continue;
                    rc::RenderClass cls = rc::RenderClass::World;
                    if (isWater(b)) cls = rc::RenderClass::Water;
                    else if (b == wire::BlockId::LightBulb) cls = rc::RenderClass::Bulb;
                    else if (b == wire::BlockId::Moon) cls = rc::RenderClass::Moon;
                    const float mat = rc::attr(cls);
                    emitSmoothedFace(chunk, stats, lx, ly, lz, x, y, z, f, col, mat);
                }
            }
        }
    }
}

// A small persistent pool that meshes a batch of chunks in parallel.
//
// This is safe because meshChunk reads only the chunk it is given (its own
// snapshot and skirt) and writes only that chunk's mesh, so two chunks never
// share state. The result is independent of which thread meshed which chunk:
// each chunk's vertices are a pure function of its snapshot, and Stats are
// per-thread counters summed after the batch, so their total is order-free.
//
// The caller thread works too, and meshAll does not return until every chunk
// is done, so the frame still sees a finished mesh before it uploads. Nothing
// here runs while the simulation ticks; the pool only exists during meshAll.
class Workers {
public:
    // helpers = extra threads besides the caller. 0 runs everything inline.
    explicit Workers(unsigned helpers) : threadStats_(helpers + 1) {
        for (unsigned i = 0; i < helpers; ++i)
            threads_.emplace_back([this, i] { run(i + 1); });
    }
    ~Workers() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            quit_ = true;
        }
        wake_.notify_all();
        for (auto& t : threads_) t.join();
    }
    Workers(const Workers&) = delete;
    Workers& operator=(const Workers&) = delete;

    unsigned helpers() const { return static_cast<unsigned>(threads_.size()); }

    void meshAll(const std::vector<view::ViewChunk*>& chunks, Stats& stats) {
        // Waking helpers costs more than meshing one chunk, so a lone impact
        // stays on the caller.
        if (threads_.empty() || chunks.size() < 2) {
            for (view::ViewChunk* c : chunks) meshChunk(*c, stats);
            return;
        }
        for (auto& s : threadStats_) s = Stats{};
        {
            std::lock_guard<std::mutex> lk(mu_);
            batch_ = &chunks;
            next_.store(0);
            busy_ = static_cast<unsigned>(threads_.size());
            ++generation_;
        }
        wake_.notify_all();
        drain(0);
        {
            std::unique_lock<std::mutex> lk(mu_);
            done_.wait(lk, [this] { return busy_ == 0; });
            batch_ = nullptr;
        }
        for (const auto& s : threadStats_) stats.skirtAccessViolations += s.skirtAccessViolations;
    }

private:
    void drain(unsigned slot) {
        const auto& chunks = *batch_;
        for (;;) {
            const size_t i = next_.fetch_add(1);
            if (i >= chunks.size()) return;
            meshChunk(*chunks[i], threadStats_[slot]);
        }
    }

    void run(unsigned slot) {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(mu_);
                wake_.wait(lk, [&] { return quit_ || generation_ != seen; });
                if (quit_) return;
                seen = generation_;
            }
            drain(slot);
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (--busy_ == 0) done_.notify_one();
            }
        }
    }

    std::vector<std::thread> threads_;
    std::vector<Stats> threadStats_;          // [0] is the caller
    std::mutex mu_;
    std::condition_variable wake_, done_;
    const std::vector<view::ViewChunk*>* batch_ = nullptr;
    std::atomic<size_t> next_{0};
    unsigned busy_ = 0;
    uint64_t generation_ = 0;
    bool quit_ = false;
};

} // namespace meshview