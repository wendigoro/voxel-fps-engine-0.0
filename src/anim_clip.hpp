#pragma once
// View-side authored and referenced animation clip evaluator, player, and blending controller.
//
// What this is:
//   Loads authored skeletal animation clips from data/anims/*.anim.json.
//   Evaluates bone position and rotation tracks using spherical linear interpolation (slerp),
//   supports cross-fade clip transitions and additive overlay tracks, and links transforms
//   to both the humanoid 16 mm character sub-lattice and weapon model parts.
//
// What this is not:
//   It is NOT simulation state (RULES.md, "Authority and the view/sim split").
//   Simulation never reads animation transforms or bone poses.

#include "char_model.hpp"
#include "jsonx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace anim {

// Extended bone channel indices for character + weapon parts
static constexpr int kBoneWeaponRoot = 20;
static constexpr int kBoneWeaponBolt = 21;
static constexpr int kBoneWeaponMag  = 22;
static constexpr int kTotalChannels  = 24;

// 4D Quaternion for rotational interpolation and matrix generation
struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    Quat() = default;
    constexpr Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    static Quat identity() { return Quat(0.0f, 0.0f, 0.0f, 1.0f); }

    float lengthSq() const { return x * x + y * y + z * z + w * w; }
    float length() const { return std::sqrt(lengthSq()); }

    Quat normalized() const {
        const float l = length();
        if (l < 1e-6f) return identity();
        const float inv = 1.0f / l;
        return Quat(x * inv, y * inv, z * inv, w * inv);
    }

    void normalize() {
        *this = normalized();
    }

    float dot(const Quat& o) const {
        return x * o.x + y * o.y + z * o.z + w * o.w;
    }

    // Convert to 4x4 rotation matrix (column-major per char_model::Mat4)
    char_model::Mat4 toMat4() const {
        const float xx = x * x, yy = y * y, zz = z * z;
        const float xy = x * y, xz = x * z, yz = y * z;
        const float wx = w * x, wy = w * y, wz = w * z;

        char_model::Mat4 m = char_model::Mat4::identity();
        m.m[0]  = 1.0f - 2.0f * (yy + zz);
        m.m[1]  = 2.0f * (xy + wz);
        m.m[2]  = 2.0f * (xz - wy);
        m.m[3]  = 0.0f;

        m.m[4]  = 2.0f * (xy - wz);
        m.m[5]  = 1.0f - 2.0f * (xx + zz);
        m.m[6]  = 2.0f * (yz + wx);
        m.m[7]  = 0.0f;

        m.m[8]  = 2.0f * (xz + wy);
        m.m[9]  = 2.0f * (yz - wx);
        m.m[10] = 1.0f - 2.0f * (xx + yy);
        m.m[11] = 0.0f;

        m.m[12] = 0.0f;
        m.m[13] = 0.0f;
        m.m[14] = 0.0f;
        m.m[15] = 1.0f;
        return m;
    }

    // Spherical linear interpolation between two quaternions
    static Quat slerp(const Quat& q0, Quat q1, float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        float cosTheta = q0.dot(q1);

        // If negative dot product, take antipodal representation to travel shortest path
        if (cosTheta < 0.0f) {
            q1.x = -q1.x;
            q1.y = -q1.y;
            q1.z = -q1.z;
            q1.w = -q1.w;
            cosTheta = -cosTheta;
        }

        // If angle is very small, use linear interpolation to prevent division by zero
        if (cosTheta > 0.9995f) {
            return Quat(
                q0.x + (q1.x - q0.x) * t,
                q0.y + (q1.y - q0.y) * t,
                q0.z + (q1.z - q0.z) * t,
                q0.w + (q1.w - q0.w) * t
            ).normalized();
        }

        const float theta = std::acos(cosTheta);
        const float sinTheta = std::sin(theta);
        const float w0 = std::sin((1.0f - t) * theta) / sinTheta;
        const float w1 = std::sin(t * theta) / sinTheta;

        return Quat(
            q0.x * w0 + q1.x * w1,
            q0.y * w0 + q1.y * w1,
            q0.z * w0 + q1.z * w1,
            q0.w * w0 + q1.w * w1
        ).normalized();
    }
};

// Coordinate space conversion helpers between Blender and Voxel Engine:
// Blender: Right-handed, +X Right, +Y Forward, +Z Up
// Voxel Engine / Vulkan: Right-handed, +X Right, +Y Up, +Z Forward
//
// Position mapping:
//   pos_engine  = (pos_blender.x,  pos_blender.z, -pos_blender.y)
//   pos_blender = (pos_engine.x, -pos_engine.z,  pos_engine.y)
//
// Quaternion mapping:
//   quat_engine  = (quat_blender.x,  quat_blender.z, -quat_blender.y, quat_blender.w)
//   quat_blender = (quat_engine.x, -quat_engine.z,  quat_engine.y, quat_engine.w)

inline char_model::Vec3 blenderPosToEngine(const char_model::Vec3& p) {
    return {p.x, p.z, -p.y};
}

inline char_model::Vec3 enginePosToBlender(const char_model::Vec3& p) {
    return {p.x, -p.z, p.y};
}

inline Quat blenderQuatToEngine(const Quat& q) {
    return Quat(q.x, q.z, -q.y, q.w).normalized();
}

inline Quat engineQuatToBlender(const Quat& q) {
    return Quat(q.x, -q.z, q.y, q.w).normalized();
}

// Normalizes bone names across Blender conventions (.L/.R, DEF-, mixamorig:) into canonical engine identifiers
inline std::string normalizeBoneNameForEngine(const std::string& rawName) {
    if (rawName.empty()) return "";

    std::string s;
    s.reserve(rawName.size());
    for (char c : rawName) {
        s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    // Strip common armature/rig prefixes: "mixamorig:", "def-", "def_", "mch-", "org-", "bip01_"
    if (s.rfind("mixamorig:", 0) == 0)      s = s.substr(10);
    else if (s.rfind("def-", 0) == 0)       s = s.substr(4);
    else if (s.rfind("def_", 0) == 0)       s = s.substr(4);
    else if (s.rfind("mch-", 0) == 0)       s = s.substr(4);
    else if (s.rfind("org-", 0) == 0)       s = s.substr(4);
    else if (s.rfind("bip01_", 0) == 0)     s = s.substr(6);

    // Detect lateral side (left / right)
    bool isLeft = false;
    bool isRight = false;

    // Check prefix
    if (s.rfind("left_", 0) == 0)        { isLeft = true; s = s.substr(5); }
    else if (s.rfind("left", 0) == 0)    { isLeft = true; s = s.substr(4); }
    else if (s.rfind("l_", 0) == 0)      { isLeft = true; s = s.substr(2); }
    else if (s.rfind("right_", 0) == 0)  { isRight = true; s = s.substr(6); }
    else if (s.rfind("right", 0) == 0)   { isRight = true; s = s.substr(5); }
    else if (s.rfind("r_", 0) == 0)      { isRight = true; s = s.substr(2); }

    // Check suffix if side was not determined by prefix
    if (!isLeft && !isRight) {
        if (s.size() >= 2 && (s.substr(s.size() - 2) == ".l" || s.substr(s.size() - 2) == "_l" || s.substr(s.size() - 2) == "-l")) {
            isLeft = true;
            s = s.substr(0, s.size() - 2);
        } else if (s.size() >= 2 && (s.substr(s.size() - 2) == ".r" || s.substr(s.size() - 2) == "_r" || s.substr(s.size() - 2) == "-r")) {
            isRight = true;
            s = s.substr(0, s.size() - 2);
        } else if (s.size() >= 5 && s.substr(s.size() - 5) == "_left") {
            isLeft = true;
            s = s.substr(0, s.size() - 5);
        } else if (s.size() >= 6 && s.substr(s.size() - 6) == "_right") {
            isRight = true;
            s = s.substr(0, s.size() - 6);
        }
    }

    // Clean any leading or trailing underscores, dashes, dots
    while (!s.empty() && (s.front() == '_' || s.front() == '-' || s.front() == '.')) s.erase(0, 1);
    while (!s.empty() && (s.back() == '_' || s.back() == '-' || s.back() == '.')) s.pop_back();

    // Normalize synonyms
    if (s == "hips" || s == "hip")                                                  s = "pelvis";
    else if (s == "spine1" || s == "spine_01" || s == "lower_spine")                s = "spine";
    else if (s == "spine2" || s == "spine_02" || s == "upper_spine" || s == "torso") s = "chest";
    else if (s == "arm" || s == "upperarm" || s == "upper_arm")                     s = "upper_arm";
    else if (s == "forearm" || s == "lowerarm" || s == "lower_arm")                 s = "forearm";
    else if (s == "hand" || s == "wrist")                                           s = "hand";
    else if (s == "thigh" || s == "upleg" || s == "up_leg" || s == "upperleg" || s == "upper_leg") s = "thigh";
    else if (s == "calf" || s == "shin" || s == "leg" || s == "lowerleg" || s == "lower_leg") s = "calf";
    else if (s == "foot" || s == "ankle")                                           s = "foot";
    else if (s == "clavicle" || s == "shoulder")                                    s = "shoulder";
    else if (s == "weapon" || s == "weaponroot")                                    s = "weapon_root";
    else if (s == "bolt" || s == "weaponbolt")                                      s = "weapon_bolt";
    else if (s == "mag" || s == "magazine" || s == "weaponmag")                     s = "weapon_mag";

    if (isLeft) return "l_" + s;
    if (isRight) return "r_" + s;
    return s;
}

// Map textual bone identifier from JSON to bone channel index (supports Blender & Mixamo names)
inline int boneNameToChannel(const std::string& name) {
    const std::string norm = normalizeBoneNameForEngine(name);
    if (norm == "root")         return char_model::BoneId::Root;
    if (norm == "pelvis")       return char_model::BoneId::Pelvis;
    if (norm == "spine")        return char_model::BoneId::Spine;
    if (norm == "chest")        return char_model::BoneId::Chest;
    if (norm == "neck")         return char_model::BoneId::Neck;
    if (norm == "head")         return char_model::BoneId::Head;
    if (norm == "l_shoulder")   return char_model::BoneId::LShoulder;
    if (norm == "l_upper_arm")  return char_model::BoneId::LUpperArm;
    if (norm == "l_forearm")    return char_model::BoneId::LForeArm;
    if (norm == "l_hand")       return char_model::BoneId::LHand;
    if (norm == "r_shoulder")   return char_model::BoneId::RShoulder;
    if (norm == "r_upper_arm")  return char_model::BoneId::RUpperArm;
    if (norm == "r_forearm")    return char_model::BoneId::RForeArm;
    if (norm == "r_hand")       return char_model::BoneId::RHand;
    if (norm == "l_thigh")      return char_model::BoneId::LThigh;
    if (norm == "l_calf")       return char_model::BoneId::LCalf;
    if (norm == "l_foot")       return char_model::BoneId::LFoot;
    if (norm == "r_thigh")      return char_model::BoneId::RThigh;
    if (norm == "r_calf")       return char_model::BoneId::RCalf;
    if (norm == "r_foot")       return char_model::BoneId::RFoot;
    if (norm == "weapon_root")  return kBoneWeaponRoot;
    if (norm == "weapon_bolt")  return kBoneWeaponBolt;
    if (norm == "weapon_mag")   return kBoneWeaponMag;
    return -1;
}

inline const char* channelToBoneName(int channel) {
    switch (channel) {
        case char_model::BoneId::Root:       return "root";
        case char_model::BoneId::Pelvis:     return "pelvis";
        case char_model::BoneId::Spine:      return "spine";
        case char_model::BoneId::Chest:      return "chest";
        case char_model::BoneId::Neck:       return "neck";
        case char_model::BoneId::Head:       return "head";
        case char_model::BoneId::LShoulder:  return "l_shoulder";
        case char_model::BoneId::LUpperArm:  return "l_upper_arm";
        case char_model::BoneId::LForeArm:   return "l_forearm";
        case char_model::BoneId::LHand:      return "l_hand";
        case char_model::BoneId::RShoulder:  return "r_shoulder";
        case char_model::BoneId::RUpperArm:  return "r_upper_arm";
        case char_model::BoneId::RForeArm:   return "r_forearm";
        case char_model::BoneId::RHand:      return "r_hand";
        case char_model::BoneId::LThigh:     return "l_thigh";
        case char_model::BoneId::LCalf:      return "l_calf";
        case char_model::BoneId::LFoot:      return "l_foot";
        case char_model::BoneId::RThigh:     return "r_thigh";
        case char_model::BoneId::RCalf:      return "r_calf";
        case char_model::BoneId::RFoot:      return "r_foot";
        case kBoneWeaponRoot:                return "weapon_root";
        case kBoneWeaponBolt:                return "weapon_bolt";
        case kBoneWeaponMag:                 return "weapon_mag";
        default:                             return "unknown";
    }
}

inline const char* channelToBlenderBoneName(int channel) {
    switch (channel) {
        case char_model::BoneId::Root:       return "Root";
        case char_model::BoneId::Pelvis:     return "Pelvis";
        case char_model::BoneId::Spine:      return "Spine";
        case char_model::BoneId::Chest:      return "Chest";
        case char_model::BoneId::Neck:       return "Neck";
        case char_model::BoneId::Head:       return "Head";
        case char_model::BoneId::LShoulder:  return "Shoulder.L";
        case char_model::BoneId::LUpperArm:  return "UpperArm.L";
        case char_model::BoneId::LForeArm:   return "Forearm.L";
        case char_model::BoneId::LHand:      return "Hand.L";
        case char_model::BoneId::RShoulder:  return "Shoulder.R";
        case char_model::BoneId::RUpperArm:  return "UpperArm.R";
        case char_model::BoneId::RForeArm:   return "Forearm.R";
        case char_model::BoneId::RHand:      return "Hand.R";
        case char_model::BoneId::LThigh:     return "Thigh.L";
        case char_model::BoneId::LCalf:      return "Calf.L";
        case char_model::BoneId::LFoot:      return "Foot.L";
        case char_model::BoneId::RThigh:     return "Thigh.R";
        case char_model::BoneId::RCalf:      return "Calf.R";
        case char_model::BoneId::RFoot:      return "Foot.R";
        case kBoneWeaponRoot:                return "Weapon_Root";
        case kBoneWeaponBolt:                return "Weapon_Bolt";
        case kBoneWeaponMag:                 return "Weapon_Mag";
        default:                             return "Unknown";
    }
}

struct Keyframe {
    float time = 0.0f;
    char_model::Vec3 pos{0.0f, 0.0f, 0.0f};
    Quat rot = Quat::identity();
};

struct BoneTrack {
    int channel = -1;
    std::string boneName;
    std::vector<Keyframe> frames;

    void sample(float time, bool loop, float duration, char_model::Vec3& outPos, Quat& outRot) const {
        if (frames.empty()) {
            outPos = {0.0f, 0.0f, 0.0f};
            outRot = Quat::identity();
            return;
        }
        if (frames.size() == 1) {
            outPos = frames[0].pos;
            outRot = frames[0].rot;
            return;
        }

        if (loop && duration > 1e-4f) {
            time = std::fmod(time, duration);
            if (time < 0.0f) time += duration;
        } else {
            time = std::clamp(time, 0.0f, duration);
        }

        if (time <= frames.front().time) {
            outPos = frames.front().pos;
            outRot = frames.front().rot;
            return;
        }
        if (time >= frames.back().time) {
            outPos = frames.back().pos;
            outRot = frames.back().rot;
            return;
        }

        // Binary search for surrounding keyframes
        size_t low = 0;
        size_t high = frames.size() - 1;
        while (low + 1 < high) {
            const size_t mid = (low + high) / 2;
            if (frames[mid].time <= time) low = mid;
            else high = mid;
        }

        const Keyframe& k0 = frames[low];
        const Keyframe& k1 = frames[high];
        const float segDur = k1.time - k0.time;
        const float alpha = (segDur > 1e-6f) ? std::clamp((time - k0.time) / segDur, 0.0f, 1.0f) : 0.0f;

        // Position linear interpolation
        outPos.x = k0.pos.x + (k1.pos.x - k0.pos.x) * alpha;
        outPos.y = k0.pos.y + (k1.pos.y - k0.pos.y) * alpha;
        outPos.z = k0.pos.z + (k1.pos.z - k0.pos.z) * alpha;

        // Rotation spherical linear interpolation
        outRot = Quat::slerp(k0.rot, k1.rot, alpha);
    }
};

struct AnimationClip {
    std::string id;
    std::string name;
    float durationSec = 1.0f;
    bool loop = false;
    float frameRate = 30.0f;
    std::vector<BoneTrack> tracks;

    bool valid() const { return !tracks.empty() && durationSec > 0.0f; }

    const BoneTrack* findTrack(int channel) const {
        for (const auto& t : tracks) {
            if (t.channel == channel) return &t;
        }
        return nullptr;
    }
};

struct PoseSample {
    char_model::Vec3 pos[kTotalChannels]{};
    Quat rot[kTotalChannels];

    PoseSample() {
        for (int i = 0; i < kTotalChannels; ++i) {
            rot[i] = Quat::identity();
        }
    }

    static PoseSample blend(const PoseSample& a, const PoseSample& b, float alpha) {
        alpha = std::clamp(alpha, 0.0f, 1.0f);
        PoseSample out;
        for (int i = 0; i < kTotalChannels; ++i) {
            out.pos[i].x = a.pos[i].x + (b.pos[i].x - a.pos[i].x) * alpha;
            out.pos[i].y = a.pos[i].y + (b.pos[i].y - a.pos[i].y) * alpha;
            out.pos[i].z = a.pos[i].z + (b.pos[i].z - a.pos[i].z) * alpha;
            out.rot[i] = Quat::slerp(a.rot[i], b.rot[i], alpha);
        }
        return out;
    }
};

inline bool loadClipFromText(const std::string& text, AnimationClip& out) {
    out = AnimationClip{};
    if (text.empty()) return false;

    out.id = jsonExtractString(text, "id", "anim");
    out.name = jsonExtractString(text, "name", out.id);
    out.durationSec = jsonExtractFloat(text, "duration_sec", 1.0f);
    out.loop = jsonExtractBool(text, "loop", false);
    out.frameRate = jsonExtractFloat(text, "frame_rate", 30.0f);

    const std::string tracksArray = jsonExtractArrayBody(text, "tracks");
    if (tracksArray.empty()) return false;

    size_t pos = 0;
    size_t start = 0, end = 0;
    while (jsonxNextObject(tracksArray, pos, start, end)) {
        pos = end;
        const std::string trackObj = tracksArray.substr(start, end - start);
        const std::string boneName = jsonExtractString(trackObj, "bone", "");
        if (boneName.empty()) continue;

        const int channel = boneNameToChannel(boneName);
        if (channel < 0 || channel >= kTotalChannels) continue;

        BoneTrack track;
        track.channel = channel;
        track.boneName = boneName;

        const std::string framesArray = jsonExtractArrayBody(trackObj, "frames");
        if (!framesArray.empty()) {
            size_t fPos = 0;
            size_t fStart = 0, fEnd = 0;
            while (jsonxNextObject(framesArray, fPos, fStart, fEnd)) {
                fPos = fEnd;
                const std::string frameObj = framesArray.substr(fStart, fEnd - fStart);
                Keyframe kf;
                kf.time = jsonExtractFloat(frameObj, "time", 0.0f);

                const std::vector<float> posVec = jsonExtractFloatArray(frameObj, "pos");
                if (posVec.size() >= 3) {
                    kf.pos.x = posVec[0];
                    kf.pos.y = posVec[1];
                    kf.pos.z = posVec[2];
                }

                const std::vector<float> rotVec = jsonExtractFloatArray(frameObj, "rot");
                if (rotVec.size() >= 4) {
                    kf.rot.x = rotVec[0];
                    kf.rot.y = rotVec[1];
                    kf.rot.z = rotVec[2];
                    kf.rot.w = rotVec[3];
                    kf.rot.normalize();
                } else {
                    kf.rot = Quat::identity();
                }
                track.frames.push_back(kf);
            }
        }

        if (!track.frames.empty()) {
            std::sort(track.frames.begin(), track.frames.end(), [](const Keyframe& a, const Keyframe& b) {
                return a.time < b.time;
            });
            out.tracks.push_back(std::move(track));
        }
    }

    return out.valid();
}

inline bool loadClipFromFile(const std::string& path, AnimationClip& out) {
    bool readable = false;
    const std::string text = jsonReadText(path, &readable);
    if (!readable || text.empty()) return false;
    return loadClipFromText(text, out);
}

// Higher-level playback controller supporting cross-fade transitions and additive layering
struct AnimController {
    std::vector<AnimationClip> clips;

    // Active base clip
    int activeClipIndex = -1;
    float clipTime = 0.0f;
    float playbackSpeed = 1.0f;
    bool isPlaying = true;

    // Cross-fade transition
    int targetClipIndex = -1;
    float targetTime = 0.0f;
    float transitionAlpha = 1.0f; // 1.0 = fully in active clip
    float transitionDuration = 0.15f;
    float transitionTimer = 0.0f;

    // Additive one-shot layer (e.g. fire recoil)
    int additiveClipIndex = -1;
    float additiveTime = 0.0f;
    float additiveWeight = 0.0f;
    float additiveSpeed = 1.0f;

    int findClipIndex(const std::string& id) const {
        for (size_t i = 0; i < clips.size(); ++i) {
            if (clips[i].id == id) return static_cast<int>(i);
        }
        return -1;
    }

    void addClip(const AnimationClip& clip) {
        for (auto& c : clips) {
            if (c.id == clip.id) {
                c = clip;
                return;
            }
        }
        clips.push_back(clip);
    }

    void playClip(const std::string& id, bool restart = false, float blendTime = 0.15f) {
        const int idx = findClipIndex(id);
        if (idx < 0) return;

        if (activeClipIndex == idx && !restart) return;

        if (activeClipIndex >= 0 && blendTime > 1e-4f) {
            targetClipIndex = idx;
            targetTime = 0.0f;
            transitionDuration = blendTime;
            transitionTimer = 0.0f;
            transitionAlpha = 0.0f;
        } else {
            activeClipIndex = idx;
            clipTime = 0.0f;
            targetClipIndex = -1;
            transitionAlpha = 1.0f;
        }
        isPlaying = true;
    }

    void triggerAdditive(const std::string& id, float weight = 1.0f, float speed = 1.0f) {
        const int idx = findClipIndex(id);
        if (idx < 0) return;
        additiveClipIndex = idx;
        additiveTime = 0.0f;
        additiveWeight = std::clamp(weight, 0.0f, 1.0f);
        additiveSpeed = speed;
    }

    void update(float dt) {
        if (!isPlaying) return;

        // Base clip advance
        if (activeClipIndex >= 0 && activeClipIndex < static_cast<int>(clips.size())) {
            const auto& clip = clips[activeClipIndex];
            clipTime += dt * playbackSpeed;
            if (clip.loop && clip.durationSec > 1e-4f) {
                clipTime = std::fmod(clipTime, clip.durationSec);
            } else if (clipTime > clip.durationSec) {
                clipTime = clip.durationSec;
            }
        }

        // Transition advance
        if (targetClipIndex >= 0 && targetClipIndex < static_cast<int>(clips.size())) {
            const auto& targetClip = clips[targetClipIndex];
            targetTime += dt * playbackSpeed;
            if (targetClip.loop && targetClip.durationSec > 1e-4f) {
                targetTime = std::fmod(targetTime, targetClip.durationSec);
            }

            transitionTimer += dt;
            if (transitionDuration > 1e-4f) {
                transitionAlpha = std::clamp(transitionTimer / transitionDuration, 0.0f, 1.0f);
            } else {
                transitionAlpha = 1.0f;
            }

            if (transitionAlpha >= 1.0f) {
                activeClipIndex = targetClipIndex;
                clipTime = targetTime;
                targetClipIndex = -1;
            }
        }

        // Additive advance
        if (additiveClipIndex >= 0 && additiveClipIndex < static_cast<int>(clips.size())) {
            const auto& addClip = clips[additiveClipIndex];
            additiveTime += dt * additiveSpeed;
            if (additiveTime >= addClip.durationSec) {
                additiveClipIndex = -1;
                additiveTime = 0.0f;
                additiveWeight = 0.0f;
            }
        }
    }

    PoseSample evaluatePose() const {
        PoseSample result;

        if (activeClipIndex < 0 || activeClipIndex >= static_cast<int>(clips.size())) {
            return result;
        }

        const auto& c0 = clips[activeClipIndex];
        PoseSample p0;
        for (const auto& track : c0.tracks) {
            track.sample(clipTime, c0.loop, c0.durationSec, p0.pos[track.channel], p0.rot[track.channel]);
        }

        if (targetClipIndex >= 0 && targetClipIndex < static_cast<int>(clips.size()) && transitionAlpha < 1.0f) {
            const auto& c1 = clips[targetClipIndex];
            PoseSample p1;
            for (const auto& track : c1.tracks) {
                track.sample(targetTime, c1.loop, c1.durationSec, p1.pos[track.channel], p1.rot[track.channel]);
            }
            result = PoseSample::blend(p0, p1, transitionAlpha);
        } else {
            result = p0;
        }

        // Apply additive layer if active
        if (additiveClipIndex >= 0 && additiveClipIndex < static_cast<int>(clips.size()) && additiveWeight > 1e-4f) {
            const auto& addClip = clips[additiveClipIndex];
            for (const auto& track : addClip.tracks) {
                char_model::Vec3 addPos{0.0f, 0.0f, 0.0f};
                Quat addRot = Quat::identity();
                track.sample(additiveTime, false, addClip.durationSec, addPos, addRot);

                // Add position offset scaled by weight
                result.pos[track.channel].x += addPos.x * additiveWeight;
                result.pos[track.channel].y += addPos.y * additiveWeight;
                result.pos[track.channel].z += addPos.z * additiveWeight;

                // Spherical blend rotation on top of base
                result.rot[track.channel] = Quat::slerp(result.rot[track.channel], addRot, additiveWeight);
            }
        }

        return result;
    }
};

// Deterministic self-test verification
inline bool selfTest() {
    // 1. Quat normalization and identity
    const Quat id = Quat::identity();
    if (std::abs(id.length() - 1.0f) > 1e-6f) return false;

    // 2. Slerp midpoint
    const Quat q0 = Quat::identity();
    const Quat q1(0.0f, 0.70710678f, 0.0f, 0.70710678f); // 90 deg around Y
    const Quat mid = Quat::slerp(q0, q1, 0.5f);
    // 45 deg around Y: sin(22.5 deg) ~ 0.38268, cos(22.5 deg) ~ 0.92388
    if (std::abs(mid.y - 0.38268f) > 1e-3f || std::abs(mid.w - 0.92388f) > 1e-3f) return false;

    // 3. Matrix generation
    const char_model::Mat4 mat = id.toMat4();
    if (std::abs(mat.m[0] - 1.0f) > 1e-6f || std::abs(mat.m[5] - 1.0f) > 1e-6f ||
        std::abs(mat.m[10] - 1.0f) > 1e-6f || std::abs(mat.m[15] - 1.0f) > 1e-6f) return false;

    // 4. Sample JSON clip loading
    const std::string sampleJson = R"({
        "format_version": 1,
        "id": "test_clip",
        "name": "Test Clip",
        "duration_sec": 1.0,
        "loop": true,
        "tracks": [
            {
                "bone": "chest",
                "frames": [
                    { "time": 0.0, "pos": [0.0, 0.0, 0.0], "rot": [0.0, 0.0, 0.0, 1.0] },
                    { "time": 0.5, "pos": [0.0, 0.01, 0.0], "rot": [0.0, 0.0, 0.0, 1.0] },
                    { "time": 1.0, "pos": [0.0, 0.0, 0.0], "rot": [0.0, 0.0, 0.0, 1.0] }
                ]
            }
        ]
    })";

    AnimationClip clip;
    if (!loadClipFromText(sampleJson, clip)) return false;
    if (clip.tracks.size() != 1) return false;
    if (clip.tracks[0].channel != char_model::BoneId::Chest) return false;

    char_model::Vec3 p;
    Quat r;
    clip.tracks[0].sample(0.25f, true, 1.0f, p, r);
    if (std::abs(p.y - 0.005f) > 1e-4f) return false;

    // 5. AnimController cross-fade testing
    AnimController ctrl;
    ctrl.addClip(clip);
    ctrl.playClip("test_clip", false, 0.0f);
    ctrl.update(0.5f);
    const PoseSample ps = ctrl.evaluatePose();
    if (std::abs(ps.pos[char_model::BoneId::Chest].y - 0.01f) > 1e-4f) return false;

    // 6. Blender bone normalization and aliasing
    if (boneNameToChannel("UpperArm.L") != char_model::BoneId::LUpperArm) return false;
    if (boneNameToChannel("DEF-forearm.R") != char_model::BoneId::RForeArm) return false;
    if (boneNameToChannel("mixamorig:LeftLeg") != char_model::BoneId::LCalf) return false;
    if (boneNameToChannel("mixamorig:Hips") != char_model::BoneId::Pelvis) return false;
    if (boneNameToChannel("Weapon_Root") != kBoneWeaponRoot) return false;
    if (boneNameToChannel("DEF-thigh.L") != char_model::BoneId::LThigh) return false;

    // 7. Blender coordinate space transformation roundtrip
    const char_model::Vec3 testBlenderPos{1.25f, -0.75f, 2.5f};
    const char_model::Vec3 enginePos = blenderPosToEngine(testBlenderPos);
    // x_e = 1.25, y_e = 2.5, z_e = 0.75
    if (std::abs(enginePos.x - 1.25f) > 1e-5f || std::abs(enginePos.y - 2.5f) > 1e-5f || std::abs(enginePos.z - 0.75f) > 1e-5f) return false;
    const char_model::Vec3 roundtripBlenderPos = enginePosToBlender(enginePos);
    if (std::abs(roundtripBlenderPos.x - testBlenderPos.x) > 1e-5f ||
        std::abs(roundtripBlenderPos.y - testBlenderPos.y) > 1e-5f ||
        std::abs(roundtripBlenderPos.z - testBlenderPos.z) > 1e-5f) return false;

    const Quat testBlenderQuat(0.1f, 0.2f, 0.3f, 0.9f);
    const Quat engineQuat = blenderQuatToEngine(testBlenderQuat);
    const Quat roundtripBlenderQuat = engineQuatToBlender(engineQuat);
    if (std::abs(testBlenderQuat.normalized().dot(roundtripBlenderQuat)) < 0.999f) return false;

    // 8. JSON loading with Blender bone naming
    const std::string blenderJson = R"({
        "format_version": 1,
        "id": "blender_test",
        "name": "Blender Test Clip",
        "duration_sec": 0.5,
        "tracks": [
            {
                "bone": "UpperArm.L",
                "frames": [
                    { "time": 0.0, "pos": [0.0, 0.0, 0.0], "rot": [0.0, 0.0, 0.0, 1.0] }
                ]
            }
        ]
    })";
    AnimationClip bClip;
    if (!loadClipFromText(blenderJson, bClip)) return false;
    if (bClip.tracks.empty() || bClip.tracks[0].channel != char_model::BoneId::LUpperArm) return false;

    return true;
}

} // namespace anim
