#pragma once
// View-only character model: voxel sub-lattice (16 mm) and skeletal hierarchy.
//
// What this is:
//   The Models layer format (RULES.md, "Render layers") for humanoid characters
//   and NPCs. Holds a 16 mm voxel sub-lattice (solid cells tagged with body
//   regions and appearance materials) alongside a 32-bone skeletal rig and
//   bind-pose transforms.
//
// What this is not:
//   It is NOT simulation state and the simulation NEVER reads it. The simulation
//   owns the player/NPC capsule position, velocity, stance, gait, and collision
//   (RULES.md, "Authority and the view/sim split"). This is purely a view-side
//   representation for extracting skinned meshes and retaining per-region gibs.
//
// Determinism:
//   Lattice bounds, proportions, and bone assignments are generated deterministically
//   from integer coordinates and seed parameters.

#include "movement.hpp"
#include "player_body.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace char_model {

// 16 mm cell size for the character sub-lattice (0.016 m).
static constexpr float kCellSize = 0.016f;

// Maximum bone capacity for the vertex skinning buffer (up to 32 bones).
static constexpr int kMaxBones = 32;

// Standard humanoid skeleton bone indices.
enum BoneId : uint8_t {
    Root = 0,
    Pelvis = 1,
    Spine = 2,
    Chest = 3,
    Neck = 4,
    Head = 5,
    // Left arm chain
    LShoulder = 6,
    LUpperArm = 7,
    LForeArm = 8,
    LHand = 9,
    // Right arm chain
    RShoulder = 10,
    RUpperArm = 11,
    RForeArm = 12,
    RHand = 13,
    // Left leg chain
    LThigh = 14,
    LCalf = 15,
    LFoot = 16,
    // Right leg chain
    RThigh = 17,
    RCalf = 18,
    RFoot = 19,
    Count = 20
};

// Named body regions for segmentation, region-derived bone weights, and gib extraction.
enum BodyRegion : uint8_t {
    Region_Pelvis = 0,
    Region_Spine,
    Region_Chest,
    Region_Neck,
    Region_Head,
    Region_LUpperArm,
    Region_LForeArm,
    Region_LHand,
    Region_RUpperArm,
    Region_RForeArm,
    Region_RHand,
    Region_LThigh,
    Region_LCalf,
    Region_LFoot,
    Region_RThigh,
    Region_RCalf,
    Region_RFoot,
    Region_Count
};

// 3D vector for model-space positions and rest coordinates.
struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }

    float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    float lengthSq() const { return x * x + y * y + z * z; }
    float length() const { return std::sqrt(lengthSq()); }

    Vec3 normalized() const {
        const float l = length();
        return (l > 1e-6f) ? Vec3{x / l, y / l, z / l} : Vec3{0.0f, 1.0f, 0.0f};
    }
};

// Column-major 4x4 matrix matching Vulkan / GLSL mat4 conventions:
// m[col * 4 + row].
struct Mat4 {
    float m[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,  // col 0
        0.0f, 1.0f, 0.0f, 0.0f,  // col 1
        0.0f, 0.0f, 1.0f, 0.0f,  // col 2
        0.0f, 0.0f, 0.0f, 1.0f   // col 3
    };

    static Mat4 identity() {
        return Mat4{};
    }

    static Mat4 translation(float tx, float ty, float tz) {
        Mat4 r;
        r.m[12] = tx;
        r.m[13] = ty;
        r.m[14] = tz;
        return r;
    }

    static Mat4 translation(const Vec3& t) {
        return translation(t.x, t.y, t.z);
    }

    static Mat4 scaling(float sx, float sy, float sz) {
        Mat4 r;
        r.m[0] = sx;
        r.m[5] = sy;
        r.m[10] = sz;
        return r;
    }

    static Mat4 scaling(float s) {
        return scaling(s, s, s);
    }

    static Mat4 rotationX(float rad) {
        Mat4 r;
        const float c = std::cos(rad);
        const float s = std::sin(rad);
        r.m[5] = c;
        r.m[6] = s;
        r.m[9] = -s;
        r.m[10] = c;
        return r;
    }

    static Mat4 rotationY(float rad) {
        Mat4 r;
        const float c = std::cos(rad);
        const float s = std::sin(rad);
        r.m[0] = c;
        r.m[2] = -s;
        r.m[8] = s;
        r.m[10] = c;
        return r;
    }

    static Mat4 rotationZ(float rad) {
        Mat4 r;
        const float c = std::cos(rad);
        const float s = std::sin(rad);
        r.m[0] = c;
        r.m[1] = s;
        r.m[4] = -s;
        r.m[5] = c;
        return r;
    }

    static Mat4 multiply(const Mat4& a, const Mat4& b) {
        Mat4 r;
        for (int c = 0; c < 4; ++c) {
            for (int r_idx = 0; r_idx < 4; ++r_idx) {
                r.m[c * 4 + r_idx] = a.m[0 * 4 + r_idx] * b.m[c * 4 + 0] +
                                     a.m[1 * 4 + r_idx] * b.m[c * 4 + 1] +
                                     a.m[2 * 4 + r_idx] * b.m[c * 4 + 2] +
                                     a.m[3 * 4 + r_idx] * b.m[c * 4 + 3];
            }
        }
        return r;
    }

    // Inverse for rigid-body/affine transforms with orthogonal 3x3 rotation.
    static Mat4 inverseRigid(const Mat4& a) {
        Mat4 r;
        // Transpose 3x3 rotation
        r.m[0] = a.m[0]; r.m[1] = a.m[4]; r.m[2] = a.m[8];
        r.m[4] = a.m[1]; r.m[5] = a.m[5]; r.m[6] = a.m[9];
        r.m[8] = a.m[2]; r.m[9] = a.m[6]; r.m[10] = a.m[10];

        // Inverse translation: -R^T * t
        const float tx = a.m[12], ty = a.m[13], tz = a.m[14];
        r.m[12] = -(r.m[0] * tx + r.m[4] * ty + r.m[8] * tz);
        r.m[13] = -(r.m[1] * tx + r.m[5] * ty + r.m[9] * tz);
        r.m[14] = -(r.m[2] * tx + r.m[6] * ty + r.m[10] * tz);
        r.m[15] = 1.0f;
        return r;
    }

    Vec3 transformPoint(const Vec3& p) const {
        return {
            m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
            m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
            m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]
        };
    }

    Vec3 transformVector(const Vec3& v) const {
        return {
            m[0] * v.x + m[4] * v.y + m[8] * v.z,
            m[1] * v.x + m[5] * v.y + m[9] * v.z,
            m[2] * v.x + m[6] * v.y + m[10] * v.z
        };
    }
};

// Voxel sub-lattice cell.
struct LatticeCell {
    uint8_t solid = 0;   // 0 = air, 1 = solid
    uint8_t region = 0;  // BodyRegion enum
    uint8_t mat = 0;     // Material / appearance palette index
    uint8_t flags = 0;   // Reserved
};

// Skeleton bone joint metadata in bind pose.
struct Bone {
    int8_t parent = -1;
    Vec3 modelPos; // Joint anchor in model space (meters)
};

// Region information mapping body regions to primary and secondary bones for skinning.
struct RegionInfo {
    const char* name = "";
    uint8_t primaryBone = BoneId::Root;
    uint8_t secondaryBone = BoneId::Root;
    float blendFactor = 0.5f; // Blend weight along region axis
};

inline RegionInfo getRegionInfo(BodyRegion r) {
    switch (r) {
        case Region_Pelvis:    return {"Pelvis", BoneId::Pelvis, BoneId::Spine, 0.4f};
        case Region_Spine:     return {"Spine", BoneId::Spine, BoneId::Chest, 0.5f};
        case Region_Chest:     return {"Chest", BoneId::Chest, BoneId::Neck, 0.4f};
        case Region_Neck:      return {"Neck", BoneId::Neck, BoneId::Head, 0.5f};
        case Region_Head:      return {"Head", BoneId::Head, BoneId::Neck, 0.2f};
        case Region_LUpperArm: return {"LUpperArm", BoneId::LUpperArm, BoneId::LShoulder, 0.4f};
        case Region_LForeArm:  return {"LForeArm", BoneId::LForeArm, BoneId::LUpperArm, 0.4f};
        case Region_LHand:     return {"LHand", BoneId::LHand, BoneId::LForeArm, 0.3f};
        case Region_RUpperArm: return {"RUpperArm", BoneId::RUpperArm, BoneId::RShoulder, 0.4f};
        case Region_RForeArm:  return {"RForeArm", BoneId::RForeArm, BoneId::RUpperArm, 0.4f};
        case Region_RHand:     return {"RHand", BoneId::RHand, BoneId::RForeArm, 0.3f};
        case Region_LThigh:    return {"LThigh", BoneId::LThigh, BoneId::Pelvis, 0.4f};
        case Region_LCalf:     return {"LCalf", BoneId::LCalf, BoneId::LThigh, 0.4f};
        case Region_LFoot:     return {"LFoot", BoneId::LFoot, BoneId::LCalf, 0.3f};
        case Region_RThigh:    return {"RThigh", BoneId::RThigh, BoneId::Pelvis, 0.4f};
        case Region_RCalf:     return {"RCalf", BoneId::RCalf, BoneId::RThigh, 0.4f};
        case Region_RFoot:     return {"RFoot", BoneId::RFoot, BoneId::RCalf, 0.3f};
        default:               return {"Unknown", BoneId::Root, BoneId::Root, 0.0f};
    }
}

// Complete character model containing sub-lattice and skeleton rig.
struct Model {
    int dimX = 0, dimY = 0, dimZ = 0;
    Vec3 origin; // World-offset of lattice corner (0,0,0) in model space
    std::vector<LatticeCell> grid;
    std::array<Bone, kMaxBones> skeleton{};
    std::array<Mat4, kMaxBones> bindPose{};       // Model-to-bone in rest pose (inverse bind matrix)
    std::array<Mat4, kMaxBones> boneModelPose{};  // Bone-to-model in rest pose
    uint32_t solidCount = 0;

    size_t index(int x, int y, int z) const {
        return static_cast<size_t>(z) * (static_cast<size_t>(dimY) * dimX) +
               static_cast<size_t>(y) * dimX + static_cast<size_t>(x);
    }

    bool inBounds(int x, int y, int z) const {
        return x >= 0 && x < dimX && y >= 0 && y < dimY && z >= 0 && z < dimZ;
    }

    LatticeCell get(int x, int y, int z) const {
        if (!inBounds(x, y, z)) return LatticeCell{};
        return grid[index(x, y, z)];
    }

    void set(int x, int y, int z, LatticeCell cell) {
        if (inBounds(x, y, z)) {
            const size_t idx = index(x, y, z);
            if (!grid[idx].solid && cell.solid) ++solidCount;
            else if (grid[idx].solid && !cell.solid) --solidCount;
            grid[idx] = cell;
        }
    }

    Vec3 cellToModel(int x, int y, int z) const {
        return {
            origin.x + (static_cast<float>(x) + 0.5f) * kCellSize,
            origin.y + (static_cast<float>(y) + 0.5f) * kCellSize,
            origin.z + (static_cast<float>(z) + 0.5f) * kCellSize
        };
    }
};

// Generates a canonical humanoid character model on the 16 mm sub-lattice.
// Proportion canon:
// - Height: ~1.76 m (110 lattice cells)
// - Torso width: ~0.30 m (19 cells)
// - Centered at (0, 0) horizontally, resting on Y = 0 (ground level).
inline Model generateHumanoid(uint32_t seed = 0) {
    (void)seed; // Greenfield default is deterministic; seed reserved for variations
    Model model;
    model.dimX = 48;
    model.dimY = 112;
    model.dimZ = 24;
    model.grid.assign(static_cast<size_t>(model.dimX) * model.dimY * model.dimZ, LatticeCell{});

    // Model space origin: centered horizontally, bottom foot sole resting on Y = 0
    model.origin = {
        -static_cast<float>(model.dimX) * 0.5f * kCellSize,
        0.0f,
        -static_cast<float>(model.dimZ) * 0.5f * kCellSize
    };

    // Helper to fill rectangular volumes with a region and material
    auto fillBox = [&](int x0, int x1, int y0, int y1, int z0, int z1, BodyRegion reg, uint8_t mat) {
        for (int y = y0; y <= y1; ++y) {
            for (int z = z0; z <= z1; ++z) {
                for (int x = x0; x <= x1; ++x) {
                    LatticeCell c;
                    c.solid = 1;
                    c.region = static_cast<uint8_t>(reg);
                    c.mat = mat;
                    model.set(x, y, z, c);
                }
            }
        }
    };

    // Lattice center coords: x = 24, z = 12
    const int cx = 24;
    const int cz = 12;

    // 1. Feet (Y: 2..5)
    // Left Foot
    fillBox(cx - 7, cx - 3, 2, 5, cz - 2, cz + 5, Region_LFoot, 1);
    // Right Foot
    fillBox(cx + 3, cx + 7, 2, 5, cz - 2, cz + 5, Region_RFoot, 1);

    // 2. Calves / Shins (Y: 6..28)
    fillBox(cx - 7, cx - 3, 6, 28, cz - 2, cz + 2, Region_LCalf, 1);
    fillBox(cx + 3, cx + 7, 6, 28, cz - 2, cz + 2, Region_RCalf, 1);

    // 3. Thighs (Y: 29..52)
    fillBox(cx - 7, cx - 2, 29, 52, cz - 2, cz + 3, Region_LThigh, 1);
    fillBox(cx + 2, cx + 7, 29, 52, cz - 2, cz + 3, Region_RThigh, 1);

    // 4. Pelvis (Y: 53..60)
    fillBox(cx - 8, cx + 8, 53, 60, cz - 2, cz + 3, Region_Pelvis, 2);

    // 5. Spine (Y: 61..72)
    fillBox(cx - 7, cx + 7, 61, 72, cz - 2, cz + 2, Region_Spine, 2);

    // 6. Chest / Upper Torso (Y: 73..88)
    fillBox(cx - 9, cx + 9, 73, 88, cz - 3, cz + 4, Region_Chest, 2);

    // 7. Neck (Y: 89..94)
    fillBox(cx - 2, cx + 2, 89, 94, cz - 2, cz + 2, Region_Neck, 3);

    // 8. Head (Y: 95..108)
    fillBox(cx - 5, cx + 5, 95, 108, cz - 4, cz + 4, Region_Head, 3);

    // 9. Arms (Shoulder to Hands)
    // Left Upper Arm (Y: 70..86)
    fillBox(cx - 14, cx - 10, 70, 86, cz - 2, cz + 2, Region_LUpperArm, 1);
    // Left Forearm (Y: 50..69)
    fillBox(cx - 14, cx - 10, 50, 69, cz - 2, cz + 2, Region_LForeArm, 1);
    // Left Hand (Y: 40..49)
    fillBox(cx - 14, cx - 10, 40, 49, cz - 2, cz + 1, Region_LHand, 3);

    // Right Upper Arm (Y: 70..86)
    fillBox(cx + 10, cx + 14, 70, 86, cz - 2, cz + 2, Region_RUpperArm, 1);
    // Right Forearm (Y: 50..69)
    fillBox(cx + 10, cx + 14, 50, 69, cz - 2, cz + 2, Region_RForeArm, 1);
    // Right Hand (Y: 40..49)
    fillBox(cx + 10, cx + 14, 40, 49, cz - 2, cz + 1, Region_RHand, 3);

    // Initialize Skeleton Hierarchy and Rest Joint Positions
    auto& sk = model.skeleton;
    sk[BoneId::Root].parent = -1;
    sk[BoneId::Root].modelPos = {0.0f, 0.0f, 0.0f};

    sk[BoneId::Pelvis].parent = BoneId::Root;
    sk[BoneId::Pelvis].modelPos = {0.0f, 56.0f * kCellSize, 0.0f};

    sk[BoneId::Spine].parent = BoneId::Pelvis;
    sk[BoneId::Spine].modelPos = {0.0f, 66.0f * kCellSize, 0.0f};

    sk[BoneId::Chest].parent = BoneId::Spine;
    sk[BoneId::Chest].modelPos = {0.0f, 80.0f * kCellSize, 0.0f};

    sk[BoneId::Neck].parent = BoneId::Chest;
    sk[BoneId::Neck].modelPos = {0.0f, 91.0f * kCellSize, 0.0f};

    sk[BoneId::Head].parent = BoneId::Neck;
    sk[BoneId::Head].modelPos = {0.0f, 101.0f * kCellSize, 0.0f};

    // Left Arm
    sk[BoneId::LShoulder].parent = BoneId::Chest;
    sk[BoneId::LShoulder].modelPos = {-9.0f * kCellSize, 86.0f * kCellSize, 0.0f};

    sk[BoneId::LUpperArm].parent = BoneId::LShoulder;
    sk[BoneId::LUpperArm].modelPos = {-12.0f * kCellSize, 78.0f * kCellSize, 0.0f};

    sk[BoneId::LForeArm].parent = BoneId::LUpperArm;
    sk[BoneId::LForeArm].modelPos = {-12.0f * kCellSize, 60.0f * kCellSize, 0.0f};

    sk[BoneId::LHand].parent = BoneId::LForeArm;
    sk[BoneId::LHand].modelPos = {-12.0f * kCellSize, 45.0f * kCellSize, 0.0f};

    // Right Arm
    sk[BoneId::RShoulder].parent = BoneId::Chest;
    sk[BoneId::RShoulder].modelPos = {9.0f * kCellSize, 86.0f * kCellSize, 0.0f};

    sk[BoneId::RUpperArm].parent = BoneId::RShoulder;
    sk[BoneId::RUpperArm].modelPos = {12.0f * kCellSize, 78.0f * kCellSize, 0.0f};

    sk[BoneId::RForeArm].parent = BoneId::RUpperArm;
    sk[BoneId::RForeArm].modelPos = {12.0f * kCellSize, 60.0f * kCellSize, 0.0f};

    sk[BoneId::RHand].parent = BoneId::RForeArm;
    sk[BoneId::RHand].modelPos = {12.0f * kCellSize, 45.0f * kCellSize, 0.0f};

    // Left Leg
    sk[BoneId::LThigh].parent = BoneId::Pelvis;
    sk[BoneId::LThigh].modelPos = {-5.0f * kCellSize, 40.0f * kCellSize, 0.0f};

    sk[BoneId::LCalf].parent = BoneId::LThigh;
    sk[BoneId::LCalf].modelPos = {-5.0f * kCellSize, 17.0f * kCellSize, 0.0f};

    sk[BoneId::LFoot].parent = BoneId::LCalf;
    sk[BoneId::LFoot].modelPos = {-5.0f * kCellSize, 3.5f * kCellSize, 1.5f * kCellSize};

    // Right Leg
    sk[BoneId::RThigh].parent = BoneId::Pelvis;
    sk[BoneId::RThigh].modelPos = {5.0f * kCellSize, 40.0f * kCellSize, 0.0f};

    sk[BoneId::RCalf].parent = BoneId::RThigh;
    sk[BoneId::RCalf].modelPos = {5.0f * kCellSize, 17.0f * kCellSize, 0.0f};

    sk[BoneId::RFoot].parent = BoneId::RCalf;
    sk[BoneId::RFoot].modelPos = {5.0f * kCellSize, 3.5f * kCellSize, 1.5f * kCellSize};

    // Calculate Bind-Pose Matrices:
    // bindPose[b] converts model space to bone space: T(-joint.x, -joint.y, -joint.z)
    // boneModelPose[b] converts bone space to model space: T(joint.x, joint.y, joint.z)
    for (int b = 0; b < BoneId::Count; ++b) {
        const Vec3& jp = sk[b].modelPos;
        model.bindPose[b] = Mat4::translation(-jp.x, -jp.y, -jp.z);
        model.boneModelPose[b] = Mat4::translation(jp.x, jp.y, jp.z);
    }
    for (int b = BoneId::Count; b < kMaxBones; ++b) {
        model.bindPose[b] = Mat4::identity();
        model.boneModelPose[b] = Mat4::identity();
    }

    return model;
}

// 64-bit FNV-1a hash over model occupancy and skeleton joints for determinism verification.
inline uint64_t hashModel(const Model& model) {
    uint64_t h = 1469598103934665603ull;
    const auto addByte = [&h](uint8_t b) {
        h ^= b;
        h *= 1099511628211ull;
    };
    const auto addU32 = [&addByte](uint32_t v) {
        for (int i = 0; i < 4; ++i) addByte(static_cast<uint8_t>(v >> (i * 8)));
    };
    addU32(static_cast<uint32_t>(model.dimX));
    addU32(static_cast<uint32_t>(model.dimY));
    addU32(static_cast<uint32_t>(model.dimZ));
    addU32(model.solidCount);

    for (const auto& cell : model.grid) {
        addByte(cell.solid);
        if (cell.solid) {
            addByte(cell.region);
            addByte(cell.mat);
        }
    }
    for (int b = 0; b < BoneId::Count; ++b) {
        uint32_t bx = 0, by = 0, bz = 0;
        std::memcpy(&bx, &model.skeleton[b].modelPos.x, sizeof(float));
        std::memcpy(&by, &model.skeleton[b].modelPos.y, sizeof(float));
        std::memcpy(&bz, &model.skeleton[b].modelPos.z, sizeof(float));
        addU32(bx);
        addU32(by);
        addU32(bz);
    }
    return h;
}

// Returns the canonical humanoid model, generated once and cached.
inline const Model& getCanonicalModel() {
    static const Model s_model = generateHumanoid(0);
    return s_model;
}

// Procedural locomotion baseline for character model posing (view-side only, Models layer).
// Evaluates skeletal bone transforms from player movement intent and body state.
// The simulation never reads these transforms; they are uploaded to the BonePalette SSBO
// for GPU vertex skinning in char.vert.
inline void evaluateProceduralPose(
    const Model& model,
    const movement::MoveState& move,
    const PlayerBody& body,
    float timeSec,
    Mat4 outBones[kMaxBones])
{
    Vec3 localTrans[BoneId::Count]{};
    Mat4 localRot[BoneId::Count];
    for (int b = 0; b < BoneId::Count; ++b) {
        localRot[b] = Mat4::identity();
    }

    // 1. Horizontal speed & movement intent
    const float speedSq = body.vx * body.vx + body.vz * body.vz;
    const float speed = std::sqrt(speedSq);
    const bool isMoving = (speed > 0.001f);

    // 2. Stamina-driven fatigue
    const float stamina = std::clamp(move.stamina, 0.0f, 100.0f);
    const float fatigue = (100.0f - stamina) / 100.0f; // 0 = rested, 1 = exhausted

    // 3. Idle & Stamina Breathing:
    // When idle or low on stamina, apply sinusoidal chest/spine heave and head bob
    // with breathing rate scaled by (100.0f - move.stamina) / 100.0f.
    const float breathFreq = 2.2f + 3.8f * fatigue;
    const float breathAmp = 0.0025f + 0.0050f * fatigue;
    const float breath = std::sin(timeSec * breathFreq) * breathAmp;

    localTrans[BoneId::Chest].y += breath;
    localTrans[BoneId::Spine].y += breath * 0.5f;
    localTrans[BoneId::Head].y += breath * 0.4f;
    localRot[BoneId::Head] = Mat4::rotationX(breath * 4.0f);

    // Subtle idle sway when resting
    if (!isMoving) {
        const float idleSway = std::sin(timeSec * 1.5f) * 0.0015f * (1.0f + fatigue * 0.5f);
        localTrans[BoneId::Pelvis].x += idleSway;
        localRot[BoneId::Pelvis] = Mat4::rotationZ(idleSway * 1.5f);
    }

    // 4. Gait Cycles:
    // When moving horizontally (sqrt(vx^2 + vz^2) > 0.001f), calculate locomotion phase
    // based on gait cycle duration (Walk = 1.0s, Run = 0.7s, Sprint = 0.5s).
    // Rotate thighs and calves in counter-phase around their modelPos anchors, counter-swing
    // shoulders/forearms, and apply vertical hip bob and lateral pelvic sway.
    if (isMoving) {
        float cycleDuration = 1.0f;
        if (move.stance == movement::Stance::Crouch) {
            cycleDuration = 1.3f;
        } else if (move.stance == movement::Stance::Prone) {
            cycleDuration = 1.6f;
        } else if (move.gait == movement::Gait::Sprint) {
            cycleDuration = 0.5f;
        } else if (move.gait == movement::Gait::Run) {
            cycleDuration = 0.7f;
        } else {
            cycleDuration = 1.0f;
        }

        const float animPhase = (timeSec / cycleDuration) * 2.0f * 3.1415926535f;
        const float s = std::sin(animPhase);
        const float c = std::cos(animPhase);

        float legSwingMax = 0.40f;
        float kneeBendMax = 0.55f;
        float armSwingMax = 0.35f;
        float hipBobMax = 0.0035f;
        float pelvisSwayMax = 0.0025f;

        if (move.gait == movement::Gait::Run) {
            legSwingMax = 0.65f;
            kneeBendMax = 0.80f;
            armSwingMax = 0.55f;
            hipBobMax = 0.0060f;
            pelvisSwayMax = 0.0035f;
        } else if (move.gait == movement::Gait::Sprint) {
            legSwingMax = 0.85f;
            kneeBendMax = 1.05f;
            armSwingMax = 0.75f;
            hipBobMax = 0.0085f;
            pelvisSwayMax = 0.0045f;
        }

        // Thighs counter-phase
        localRot[BoneId::LThigh] = Mat4::rotationX(s * legSwingMax);
        localRot[BoneId::RThigh] = Mat4::rotationX(-s * legSwingMax);

        // Calves flex backward when thigh swings forward
        localRot[BoneId::LCalf] = Mat4::rotationX(-std::max(0.0f, s) * kneeBendMax);
        localRot[BoneId::RCalf] = Mat4::rotationX(-std::max(0.0f, -s) * kneeBendMax);

        // Feet counter-flexion to stay level
        localRot[BoneId::LFoot] = Mat4::rotationX(-s * legSwingMax * 0.35f);
        localRot[BoneId::RFoot] = Mat4::rotationX(s * legSwingMax * 0.35f);

        // Arms counter-swing opposite to legs
        localRot[BoneId::LUpperArm] = Mat4::rotationX(-s * armSwingMax);
        localRot[BoneId::RUpperArm] = Mat4::rotationX(s * armSwingMax);

        // Forearms slight resting bend + flexion on forward swing
        localRot[BoneId::LForeArm] = Mat4::rotationX(0.20f + std::max(0.0f, -s) * 0.35f);
        localRot[BoneId::RForeArm] = Mat4::rotationX(0.20f + std::max(0.0f, s) * 0.35f);

        // Pelvis hip bob and lateral sway
        localTrans[BoneId::Pelvis].y -= std::abs(s) * hipBobMax;
        localTrans[BoneId::Pelvis].x += c * pelvisSwayMax;
        localRot[BoneId::Pelvis] = Mat4::rotationZ(c * 0.04f);

        // Torso counter-twist
        localRot[BoneId::Spine] = Mat4::rotationY(-s * 0.07f);
    }

    // 5. Stance Adaptations:
    // In Crouch and Prone, compress spine height and flex thighs/calves to match
    // collision heights (STANCE_COLLISION_HEIGHT).
    if (move.stance == movement::Stance::Crouch) {
        // Lower pelvis by ~0.42m to match crouch height
        localTrans[BoneId::Pelvis].y -= 26.0f * kCellSize;
        localRot[BoneId::LThigh] = Mat4::multiply(localRot[BoneId::LThigh], Mat4::rotationX(0.85f));
        localRot[BoneId::RThigh] = Mat4::multiply(localRot[BoneId::RThigh], Mat4::rotationX(0.85f));
        localRot[BoneId::LCalf] = Mat4::multiply(localRot[BoneId::LCalf], Mat4::rotationX(-1.45f));
        localRot[BoneId::RCalf] = Mat4::multiply(localRot[BoneId::RCalf], Mat4::rotationX(-1.45f));
        localRot[BoneId::LFoot] = Mat4::multiply(localRot[BoneId::LFoot], Mat4::rotationX(0.60f));
        localRot[BoneId::RFoot] = Mat4::multiply(localRot[BoneId::RFoot], Mat4::rotationX(0.60f));
        localRot[BoneId::Spine] = Mat4::multiply(localRot[BoneId::Spine], Mat4::rotationX(0.25f));
        localRot[BoneId::Chest] = Mat4::multiply(localRot[BoneId::Chest], Mat4::rotationX(0.15f));
        localRot[BoneId::Head] = Mat4::multiply(localRot[BoneId::Head], Mat4::rotationX(-0.25f));
    } else if (move.stance == movement::Stance::Prone) {
        // Lower pelvis near floor and rotate horizontal
        localTrans[BoneId::Pelvis].y -= 46.0f * kCellSize;
        localRot[BoneId::Pelvis] = Mat4::multiply(localRot[BoneId::Pelvis], Mat4::rotationX(1.52f));
        localRot[BoneId::Head] = Mat4::multiply(localRot[BoneId::Head], Mat4::rotationX(-1.35f));
        localRot[BoneId::LThigh] = Mat4::rotationX(-1.45f);
        localRot[BoneId::RThigh] = Mat4::rotationX(-1.45f);
        localRot[BoneId::LCalf] = Mat4::rotationX(-0.20f);
        localRot[BoneId::RCalf] = Mat4::rotationX(-0.20f);
        localRot[BoneId::LUpperArm] = Mat4::rotationX(0.60f);
        localRot[BoneId::RUpperArm] = Mat4::rotationX(0.60f);
        localRot[BoneId::LForeArm] = Mat4::rotationX(0.80f);
        localRot[BoneId::RForeArm] = Mat4::rotationX(0.80f);
    }

    // 6. Maneuvers:
    // Pitch torso forward during slide and dash, bank during wallrun.
    if (move.sliding) {
        localTrans[BoneId::Pelvis].y -= 18.0f * kCellSize;
        localRot[BoneId::Pelvis] = Mat4::rotationX(-0.40f);
        localRot[BoneId::Spine] = Mat4::rotationX(-0.15f);
        localRot[BoneId::LThigh] = Mat4::rotationX(1.20f);
        localRot[BoneId::RThigh] = Mat4::rotationX(1.10f);
        localRot[BoneId::LCalf] = Mat4::rotationX(0.15f);
        localRot[BoneId::RCalf] = Mat4::rotationX(0.30f);
        localRot[BoneId::LUpperArm] = Mat4::rotationX(-0.60f);
        localRot[BoneId::RUpperArm] = Mat4::rotationX(-0.60f);
    }

    if (move.dashing) {
        localRot[BoneId::Pelvis] = Mat4::multiply(localRot[BoneId::Pelvis], Mat4::rotationX(0.45f));
        localRot[BoneId::Spine] = Mat4::multiply(localRot[BoneId::Spine], Mat4::rotationX(0.20f));
        localTrans[BoneId::Pelvis].y -= 4.0f * kCellSize;
    }

    if (move.wallRunning) {
        const float bank = static_cast<float>(move.wallRunSide) * 0.35f;
        localRot[BoneId::Pelvis] = Mat4::multiply(localRot[BoneId::Pelvis], Mat4::rotationZ(bank));
        localRot[BoneId::Chest] = Mat4::multiply(localRot[BoneId::Chest], Mat4::rotationZ(bank * 0.4f));
        localRot[BoneId::Head] = Mat4::multiply(localRot[BoneId::Head], Mat4::rotationZ(-bank * 0.3f));
        if (move.wallRunSide > 0) {
            localRot[BoneId::RThigh] = Mat4::rotationX(-0.55f);
            localRot[BoneId::RCalf] = Mat4::rotationX(-0.40f);
        } else {
            localRot[BoneId::LThigh] = Mat4::rotationX(-0.55f);
            localRot[BoneId::LCalf] = Mat4::rotationX(-0.40f);
        }
    }

    // Player lean coupling (Q/E)
    if (std::abs(body.lean) > 0.01f) {
        const float leanRoll = -body.lean * 0.15f;
        localRot[BoneId::Spine] = Mat4::multiply(localRot[BoneId::Spine], Mat4::rotationZ(leanRoll));
        localRot[BoneId::Head] = Mat4::multiply(localRot[BoneId::Head], Mat4::rotationZ(-leanRoll * 0.5f));
    }

    // 7. Forward kinematics pass: propagate joint positions and rotations
    // down the topological skeletal tree (all parents < child index).
    struct JointState {
        Vec3 pos;
        Mat4 rot;
    };
    JointState global[kMaxBones];

    // Bone 0: Root
    global[0].pos = model.skeleton[0].modelPos + localTrans[0];
    global[0].rot = localRot[0];

    for (int b = 1; b < BoneId::Count; ++b) {
        const int p = model.skeleton[b].parent;
        const Vec3 restOffset = model.skeleton[b].modelPos - model.skeleton[p].modelPos;
        const Vec3 rotatedOffset = global[p].rot.transformVector(restOffset + localTrans[b]);
        global[b].pos = global[p].pos + rotatedOffset;
        global[b].rot = Mat4::multiply(global[p].rot, localRot[b]);
    }

    // 8. Compute bone skinning matrices:
    // outBones[b] = T(global[b].pos) * global[b].rot * T(-modelPos[b])
    for (int b = 0; b < BoneId::Count; ++b) {
        const Vec3& restPos = model.skeleton[b].modelPos;
        outBones[b] = Mat4::multiply(
            Mat4::translation(global[b].pos),
            Mat4::multiply(global[b].rot, Mat4::translation(-restPos.x, -restPos.y, -restPos.z))
        );
    }
    for (int b = BoneId::Count; b < kMaxBones; ++b) {
        outBones[b] = Mat4::identity();
    }
}

} // namespace char_model
