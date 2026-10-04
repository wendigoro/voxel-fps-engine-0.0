#pragma once
// View-side skinned character mesh extraction and region-partitioned submeshes.
//
// What this is:
//   Extracts a skinned polygonal surface from the 16 mm character sub-lattice
//   (char_model::Model). Faces between adjacent solid cells are culled so internal
//   voxels produce no geometry. Vertices carry smooth normals and region-derived
//   bone weights for skinning. Submesh index ranges are partitioned per BodyRegion
//   so individual limbs or gibs can be detached or rendered independently.
//
// What this is not:
//   It is NOT simulation state (RULES.md, "Authority and the view/sim split").
//   Sim never reads this mesh, its vertices, or its transforms.
//
// Determinism:
//   Face iteration, vertex corner generation, and normal accumulation follow
//   strictly ordered integer coordinates. Golden hash gating ensures bit-level
//   reproducibility across builds.

#include "char_model.hpp"
#include "render_class.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace char_mesh {

// 64-byte skinned vertex layout matching GPU alignment (16-byte boundary).
struct SkinnedVertex {
    float x = 0.0f, y = 0.0f, z = 0.0f;          // Position in model space (meters)
    float nx = 0.0f, ny = 0.0f, nz = 0.0f;       // Smoothed normal
    float r = 1.0f, g = 1.0f, b = 1.0f;          // Color / tint
    float mat = rc::attr(rc::RenderClass::Character); // RenderClass::Character = 9.0f
    uint8_t bone[4] = {0, 0, 0, 0};              // Bone indices (up to 4 influences)
    float weight[4] = {1.0f, 0.0f, 0.0f, 0.0f};  // Normalized weights (sum = 1.0)
    uint8_t region = 0;                          // BodyRegion index
    uint8_t pad[3] = {0, 0, 0};                  // Alignment padding
};

static_assert(sizeof(SkinnedVertex) == 64, "SkinnedVertex must be 64 bytes");

// Region submesh range for retained gib extraction or per-limb draw calls.
struct RegionRange {
    uint8_t region = 0;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t solidCells = 0;
};

// Complete extracted character mesh.
struct ExtractedMesh {
    std::vector<SkinnedVertex> vertices;
    std::vector<uint32_t> indices;
    std::array<RegionRange, char_model::BodyRegion::Region_Count> regionRanges{};
    uint32_t exposedFaces = 0;
    uint32_t culledFaces = 0;
};

// Six unit-face neighbor offsets: +X, -X, +Y, -Y, +Z, -Z
struct FaceDir {
    int dx, dy, dz;
    // Corners relative to cell min (x0, y0, z0) and cell max (x1, y1, z1)
    // Ordered CCW when viewed from outside the cube
    int corners[4][3];
};

static constexpr FaceDir kFaceDirs[6] = {
    // +X (Right)
    { 1, 0, 0, {{1, 0, 1}, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}} },
    // -X (Left)
    {-1, 0, 0, {{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}} },
    // +Y (Top)
    { 0, 1, 0, {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}} },
    // -Y (Bottom)
    { 0,-1, 0, {{0, 0, 1}, {0, 0, 0}, {1, 0, 0}, {1, 0, 1}} },
    // +Z (Front)
    { 0, 0, 1, {{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}} },
    // -Z (Back)
    { 0, 0,-1, {{1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {1, 1, 0}} }
};

// Calculate normalized bone weights for a vertex position in a body region.
inline void computeBoneWeights(const char_model::Model& model, const char_model::Vec3& p,
                               uint8_t regionId, uint8_t outBones[4], float outWeights[4]) {
    outBones[0] = 0; outBones[1] = 0; outBones[2] = 0; outBones[3] = 0;
    outWeights[0] = 1.0f; outWeights[1] = 0.0f; outWeights[2] = 0.0f; outWeights[3] = 0.0f;

    if (regionId >= char_model::BodyRegion::Region_Count) return;

    const char_model::RegionInfo info = char_model::getRegionInfo(static_cast<char_model::BodyRegion>(regionId));
    outBones[0] = info.primaryBone;
    outBones[1] = info.secondaryBone;

    if (info.primaryBone == info.secondaryBone) {
        return; // Pure 1.0f on primary bone
    }

    const char_model::Vec3& j0 = model.skeleton[info.primaryBone].modelPos;
    const char_model::Vec3& j1 = model.skeleton[info.secondaryBone].modelPos;

    const char_model::Vec3 seg = j1 - j0;
    const float lenSq = seg.lengthSq();
    if (lenSq < 1e-6f) {
        return;
    }

    // Project (p - j0) onto bone axis segment
    const float t = (p - j0).dot(seg) / lenSq;
    const float clampedT = std::clamp(t, 0.0f, 1.0f);

    // Blend towards secondary bone near the joint
    const float w1 = clampedT * info.blendFactor;
    const float w0 = 1.0f - w1;

    outWeights[0] = w0;
    outWeights[1] = w1;
}

// Extracts a skinned mesh from a character model with neighbor face culling and smoothed normals.
inline ExtractedMesh extractMesh(const char_model::Model& model) {
    ExtractedMesh result;

    // Count solid cells per region
    for (int reg = 0; reg < char_model::BodyRegion::Region_Count; ++reg) {
        result.regionRanges[reg].region = static_cast<uint8_t>(reg);
        result.regionRanges[reg].solidCells = 0;
        result.regionRanges[reg].firstIndex = 0;
        result.regionRanges[reg].indexCount = 0;
    }

    for (const auto& c : model.grid) {
        if (c.solid && c.region < char_model::BodyRegion::Region_Count) {
            result.regionRanges[c.region].solidCells++;
        }
    }

    // Hash map to accumulate normals at shared lattice corner vertices:
    // Key: integer corner coordinate (cx, cy, cz) where cx in [0..dimX], cy in [0..dimY], cz in [0..dimZ]
    auto cornerKey = [&](int cx, int cy, int cz) -> uint64_t {
        return (static_cast<uint64_t>(cx) & 0xFFFFull) |
               ((static_cast<uint64_t>(cy) & 0xFFFFull) << 16) |
               ((static_cast<uint64_t>(cz) & 0xFFFFull) << 32);
    };

    struct NormalAccum {
        float nx = 0.0f, ny = 0.0f, nz = 0.0f;
    };
    std::unordered_map<uint64_t, NormalAccum> cornerNormals;

    // Pass 1: Identify all exposed faces and accumulate corner normals
    for (int y = 0; y < model.dimY; ++y) {
        for (int z = 0; z < model.dimZ; ++z) {
            for (int x = 0; x < model.dimX; ++x) {
                const auto cell = model.get(x, y, z);
                if (!cell.solid) continue;

                for (const auto& fd : kFaceDirs) {
                    const int nx = x + fd.dx;
                    const int ny = y + fd.dy;
                    const int nz = z + fd.dz;

                    // If neighbor is in bounds and solid, face is culled
                    if (model.inBounds(nx, ny, nz) && model.get(nx, ny, nz).solid) {
                        result.culledFaces++;
                        continue;
                    }

                    // Face is exposed!
                    result.exposedFaces++;
                    const float fnx = static_cast<float>(fd.dx);
                    const float fny = static_cast<float>(fd.dy);
                    const float fnz = static_cast<float>(fd.dz);

                    for (int i = 0; i < 4; ++i) {
                        const int cx = x + fd.corners[i][0];
                        const int cy = y + fd.corners[i][1];
                        const int cz = z + fd.corners[i][2];
                        const uint64_t key = cornerKey(cx, cy, cz);
                        auto& acc = cornerNormals[key];
                        acc.nx += fnx;
                        acc.ny += fny;
                        acc.nz += fnz;
                    }
                }
            }
        }
    }

    // Material colors for base palette
    auto getMatColor = [](uint8_t mat) -> char_model::Vec3 {
        switch (mat) {
            case 1: return {0.25f, 0.35f, 0.55f}; // Pants / boots
            case 2: return {0.65f, 0.25f, 0.20f}; // Shirt / torso
            case 3: return {0.85f, 0.70f, 0.55f}; // Skin (head, neck, hands)
            default: return {0.70f, 0.70f, 0.70f};
        }
    };

    // Pass 2: Emit vertices and indices grouped by BodyRegion for retained submesh ranges
    for (int reg = 0; reg < char_model::BodyRegion::Region_Count; ++reg) {
        const uint8_t currentReg = static_cast<uint8_t>(reg);
        result.regionRanges[reg].firstIndex = static_cast<uint32_t>(result.indices.size());

        for (int y = 0; y < model.dimY; ++y) {
            for (int z = 0; z < model.dimZ; ++z) {
                for (int x = 0; x < model.dimX; ++x) {
                    const auto cell = model.get(x, y, z);
                    if (!cell.solid || cell.region != currentReg) continue;

                    for (const auto& fd : kFaceDirs) {
                        const int nx = x + fd.dx;
                        const int ny = y + fd.dy;
                        const int nz = z + fd.dz;

                        // Neighbor culling
                        if (model.inBounds(nx, ny, nz) && model.get(nx, ny, nz).solid) {
                            continue;
                        }

                        // Emit 4 vertices for this exposed quad
                        const uint32_t baseVert = static_cast<uint32_t>(result.vertices.size());
                        const char_model::Vec3 col = getMatColor(cell.mat);

                        for (int i = 0; i < 4; ++i) {
                            const int cx = x + fd.corners[i][0];
                            const int cy = y + fd.corners[i][1];
                            const int cz = z + fd.corners[i][2];

                            const char_model::Vec3 pos = {
                                model.origin.x + static_cast<float>(cx) * char_model::kCellSize,
                                model.origin.y + static_cast<float>(cy) * char_model::kCellSize,
                                model.origin.z + static_cast<float>(cz) * char_model::kCellSize
                            };

                            // Normal from accumulator
                            const uint64_t key = cornerKey(cx, cy, cz);
                            const auto& acc = cornerNormals[key];
                            const char_model::Vec3 norm = char_model::Vec3(acc.nx, acc.ny, acc.nz).normalized();

                            SkinnedVertex v;
                            v.x = pos.x; v.y = pos.y; v.z = pos.z;
                            v.nx = norm.x; v.ny = norm.y; v.nz = norm.z;
                            v.r = col.x; v.g = col.y; v.b = col.z;
                            v.mat = rc::attr(rc::RenderClass::Character);
                            v.region = currentReg;

                            computeBoneWeights(model, pos, currentReg, v.bone, v.weight);
                            result.vertices.push_back(v);
                        }

                        // Emit 2 triangles (0, 1, 2) and (0, 2, 3)
                        result.indices.push_back(baseVert + 0);
                        result.indices.push_back(baseVert + 1);
                        result.indices.push_back(baseVert + 2);
                        result.indices.push_back(baseVert + 0);
                        result.indices.push_back(baseVert + 2);
                        result.indices.push_back(baseVert + 3);
                    }
                }
            }
        }

        result.regionRanges[reg].indexCount =
            static_cast<uint32_t>(result.indices.size()) - result.regionRanges[reg].firstIndex;
    }

    return result;
}

// 64-bit FNV-1a hash over extracted mesh vertices and indices for determinism gating.
inline uint64_t hashMesh(const ExtractedMesh& mesh) {
    uint64_t h = 1469598103934665603ull;
    const auto addByte = [&h](uint8_t b) {
        h ^= b;
        h *= 1099511628211ull;
    };
    const auto addU32 = [&addByte](uint32_t v) {
        for (int i = 0; i < 4; ++i) addByte(static_cast<uint8_t>(v >> (i * 8)));
    };

    addU32(static_cast<uint32_t>(mesh.vertices.size()));
    addU32(static_cast<uint32_t>(mesh.indices.size()));
    addU32(mesh.exposedFaces);
    addU32(mesh.culledFaces);

    for (const auto& v : mesh.vertices) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v.x, 4); addU32(bits);
        std::memcpy(&bits, &v.y, 4); addU32(bits);
        std::memcpy(&bits, &v.z, 4); addU32(bits);
        std::memcpy(&bits, &v.nx, 4); addU32(bits);
        std::memcpy(&bits, &v.ny, 4); addU32(bits);
        std::memcpy(&bits, &v.nz, 4); addU32(bits);
        std::memcpy(&bits, &v.r, 4); addU32(bits);
        std::memcpy(&bits, &v.g, 4); addU32(bits);
        std::memcpy(&bits, &v.b, 4); addU32(bits);
        std::memcpy(&bits, &v.mat, 4); addU32(bits);
        addByte(v.bone[0]); addByte(v.bone[1]); addByte(v.bone[2]); addByte(v.bone[3]);
        std::memcpy(&bits, &v.weight[0], 4); addU32(bits);
        std::memcpy(&bits, &v.weight[1], 4); addU32(bits);
        addByte(v.region);
    }

    for (uint32_t idx : mesh.indices) {
        addU32(idx);
    }

    return h;
}

// Golden hash for the canonical humanoid extracted mesh.
static constexpr uint64_t kCharMeshGolden = 0x9d724d3914ad8206ull;

// Self-test report for test suites and smoke gates.
struct SelfTestReport {
    bool solidCellsOk = false;
    bool verticesOk = false;
    bool indicesOk = false;
    bool faceCullingOk = false;
    bool boneWeightsNormalizedOk = false;
    bool boneIndicesValidOk = false;
    bool regionRangesOk = false;
    bool determinismOk = false;
    bool sensitivityOk = false;
    bool goldenMatchOk = false;
    uint32_t solidCells = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t exposedFaces = 0;
    uint32_t culledFaces = 0;
    uint64_t goldenHash = 0;

    bool ok() const {
        return solidCellsOk && verticesOk && indicesOk && faceCullingOk &&
               boneWeightsNormalizedOk && boneIndicesValidOk && regionRangesOk &&
               determinismOk && sensitivityOk && goldenMatchOk;
    }
};

// Full self-test verifying face culling, weight normalization, region partitioning, and determinism.
inline SelfTestReport selfTest() {
    SelfTestReport rep;

    // 1. Generate canonical humanoid
    const char_model::Model m1 = char_model::generateHumanoid(0);
    rep.solidCells = m1.solidCount;
    rep.solidCellsOk = (m1.solidCount > 1000);

    // 2. Extract mesh
    const ExtractedMesh mesh1 = extractMesh(m1);
    rep.vertexCount = static_cast<uint32_t>(mesh1.vertices.size());
    rep.indexCount = static_cast<uint32_t>(mesh1.indices.size());
    rep.exposedFaces = mesh1.exposedFaces;
    rep.culledFaces = mesh1.culledFaces;

    rep.verticesOk = (rep.vertexCount > 0 && rep.vertexCount == rep.exposedFaces * 4);
    rep.indicesOk = (rep.indexCount > 0 && rep.indexCount == rep.exposedFaces * 6);
    rep.faceCullingOk = (rep.culledFaces > 0); // Must cull interior faces

    // 3. Verify bone weights and bone index validity
    bool weightsNorm = true;
    bool bonesValid = true;
    for (const auto& v : mesh1.vertices) {
        const float wSum = v.weight[0] + v.weight[1] + v.weight[2] + v.weight[3];
        if (std::abs(wSum - 1.0f) > 1e-4f) {
            weightsNorm = false;
        }
        for (int b = 0; b < 4; ++b) {
            if (v.bone[b] >= char_model::BoneId::Count) {
                bonesValid = false;
            }
        }
    }
    rep.boneWeightsNormalizedOk = weightsNorm;
    rep.boneIndicesValidOk = bonesValid;

    // 4. Verify region index ranges
    uint32_t sumIndices = 0;
    bool rangesMatch = true;
    for (const auto& rr : mesh1.regionRanges) {
        sumIndices += rr.indexCount;
    }
    rep.regionRangesOk = (sumIndices == rep.indexCount);

    // 5. Determinism: generate and extract again, verify identical hash
    const char_model::Model m2 = char_model::generateHumanoid(0);
    const ExtractedMesh mesh2 = extractMesh(m2);
    const uint64_t h1 = hashMesh(mesh1);
    const uint64_t h2 = hashMesh(mesh2);
    rep.goldenHash = h1;
    rep.determinismOk = (h1 == h2 && h1 != 0);

    // 6. Sensitivity: modifying one cell in model changes hash
    char_model::Model m3 = m1;
    char_model::LatticeCell mod;
    mod.solid = 1;
    mod.region = char_model::BodyRegion::Region_Chest;
    mod.mat = 2;
    m3.set(2, 2, 2, mod); // Add cell
    const ExtractedMesh mesh3 = extractMesh(m3);
    const uint64_t h3 = hashMesh(mesh3);
    rep.sensitivityOk = (h3 != h1);

    // 7. Golden match: verify exact match against golden baseline constant
    rep.goldenMatchOk = (h1 == kCharMeshGolden);

    return rep;
}

} // namespace char_mesh
