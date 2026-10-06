#pragma once
// View-side weapon voxel models, first-person viewmodel rig, and animation controller.
//
// What this is:
//   Loads weapon voxel shapes (from data/voxfmt/*.vox.json or procedural fallback)
//   and computes procedural viewmodel animations (idle breathing, movement bobbing,
//   ADS alignment, spring-damper recoil, and equip raise). Renders weapon models
//   in the Models layer (in first-person view anchored to the camera basis, and
//   in third-person attached to BoneId::RHand).
//
// What this is not:
//   It is NOT simulation state (RULES.md, "Authority and the view/sim split").
//   Simulation never reads viewmodel animation state or weapon meshes.
//
// Display Basis:
//   Strictly adheres to RULES.md Rule 12 (orthonormal camera display basis).

#include "jsonx.hpp"
#include "materials.hpp"
#include "destruction.hpp"
#include "movement.hpp"
#include "player_body.hpp"
#include "render_class.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace weapon_view {

struct WeaponVoxel {
    int x = 0, y = 0, z = 0;
    float cr = 0.5f, cg = 0.5f, cb = 0.5f;
    std::string part;
    std::string material;
};

struct WeaponModel {
    std::string id;
    int sx = 0, sy = 0, sz = 0;
    int pivotX = 0, pivotY = 0, pivotZ = 0;
    int sightX = 0, sightY = 0, sightZ = 0;
    std::vector<WeaponVoxel> voxels;

    bool valid() const { return !voxels.empty(); }
};

inline void getMaterialColor(const std::string& matName, float& r, float& g, float& b) {
    MaterialId id = materialFromName(matName);
    switch (id) {
    case MaterialId::TreatedWood: r = 0.50f; g = 0.32f; b = 0.18f; break;
    case MaterialId::SheetMetal:  r = 0.78f; g = 0.82f; b = 0.88f; break;
    case MaterialId::CarbonFiber: r = 0.18f; g = 0.18f; b = 0.22f; break;
    case MaterialId::Girder:      r = 0.52f; g = 0.28f; b = 0.16f; break;
    case MaterialId::Plexiglass:  r = 0.78f; g = 0.92f; b = 1.00f; break;
    case MaterialId::Wood:        r = 0.55f; g = 0.34f; b = 0.16f; break;
    case MaterialId::Concrete:    r = 0.72f; g = 0.70f; b = 0.66f; break;
    case MaterialId::Dirt:        r = 0.48f; g = 0.34f; b = 0.16f; break;
    default:                      r = 0.58f; g = 0.60f; b = 0.64f; break;
    }
}

// Parse weapon voxfmt JSON document (format_version 2 or 1).
inline bool loadWeaponModelFromText(const std::string& text, WeaponModel& out) {
    out = WeaponModel{};
    if (text.empty()) return false;

    out.id = jsonExtractString(text, "id", "");
    if (out.id.empty()) out.id = jsonExtractString(text, "item_id", "weapon");

    const std::vector<int> dims = jsonExtractIntArray(text, "dims");
    if (dims.size() >= 3) {
        out.sx = dims[0]; out.sy = dims[1]; out.sz = dims[2];
    } else {
        out.sx = 24; out.sy = 12; out.sz = 12;
    }

    const std::string anchors = jsonExtractObjectBody(text, "anchors");
    if (!anchors.empty()) {
        const std::vector<int> piv = jsonExtractIntArray(anchors, "pivot");
        if (piv.size() >= 3) {
            out.pivotX = piv[0]; out.pivotY = piv[1]; out.pivotZ = piv[2];
        }
    } else {
        out.pivotX = out.sx / 2; out.pivotY = 0; out.pivotZ = out.sz / 2;
    }

    // Cells RLE
    std::vector<std::string> cellPalette;
    const std::string cellRle = jsonExtractObjectBody(text, "cells_rle");
    if (!cellRle.empty()) {
        const std::string pal = jsonExtractArrayBody(cellRle, "palette");
        size_t i = 0;
        while ((i = pal.find('"', i)) != std::string::npos) {
            const size_t j = pal.find('"', i + 1);
            if (j == std::string::npos) break;
            cellPalette.push_back(pal.substr(i + 1, j - i - 1));
            i = j + 1;
        }
    }

    // Parts RLE
    std::vector<std::string> partPalette;
    const std::string partRle = jsonExtractObjectBody(text, "parts_rle");
    if (!partRle.empty()) {
        const std::string pal = jsonExtractArrayBody(partRle, "palette");
        size_t i = 0;
        while ((i = pal.find('"', i)) != std::string::npos) {
            const size_t j = pal.find('"', i + 1);
            if (j == std::string::npos) break;
            partPalette.push_back(pal.substr(i + 1, j - i - 1));
            i = j + 1;
        }
    }

    // Unpack cells runs: [y, z, x0, length, paletteIndex]
    std::vector<int> cellRuns = jsonExtractIntArray(cellRle, "runs");
    std::vector<int> partRuns = jsonExtractIntArray(partRle, "runs");

    int maxY = -1;
    if (cellRuns.size() % 5 == 0 && !cellPalette.empty()) {
        for (size_t r = 0; r < cellRuns.size(); r += 5) {
            const int y = cellRuns[r + 0];
            const int z = cellRuns[r + 1];
            const int x0 = cellRuns[r + 2];
            const int len = cellRuns[r + 3];
            const int cPi = cellRuns[r + 4];

            std::string matName = (cPi >= 0 && cPi < static_cast<int>(cellPalette.size()))
                                      ? cellPalette[static_cast<size_t>(cPi)]
                                      : "sheet_metal";

            // Find matching part in partRuns if present
            std::string partName = "frame";
            if (partRuns.size() % 5 == 0 && !partPalette.empty()) {
                for (size_t pr = 0; pr < partRuns.size(); pr += 5) {
                    if (partRuns[pr + 0] == y && partRuns[pr + 1] == z && partRuns[pr + 2] == x0) {
                        const int pPi = partRuns[pr + 4];
                        if (pPi >= 0 && pPi < static_cast<int>(partPalette.size())) {
                            partName = partPalette[static_cast<size_t>(pPi)];
                        }
                        break;
                    }
                }
            }

            float cr = 0.5f, cg = 0.5f, cb = 0.5f;
            getMaterialColor(matName, cr, cg, cb);

            for (int lx = 0; lx < len; ++lx) {
                const int x = x0 + lx;
                WeaponVoxel vox;
                vox.x = x; vox.y = y; vox.z = z;
                vox.cr = cr; vox.cg = cg; vox.cb = cb;
                vox.part = partName;
                vox.material = matName;
                out.voxels.push_back(vox);

                if (partName == "sight" || y > maxY) {
                    if (partName == "sight" || maxY < y) {
                        out.sightX = x;
                        out.sightY = y;
                        out.sightZ = z;
                        maxY = y;
                    }
                }
            }
        }
    }

    // Default sight coordinate if none explicitly found
    if (maxY < 0) {
        out.sightX = out.sx / 2;
        out.sightY = out.sy;
        out.sightZ = out.sz / 2;
    }

    return !out.voxels.empty();
}

inline bool loadWeaponModelFromFile(const std::string& path, WeaponModel& out) {
    bool readable = false;
    const std::string text = jsonReadText(path, &readable);
    if (!readable || text.empty()) return false;
    return loadWeaponModelFromText(text, out);
}

// Procedural fallback rifle matching starter_rifle parts
inline WeaponModel buildProceduralRifle(const std::string& id = "starter_rifle") {
    WeaponModel m;
    m.id = id;
    m.sx = 24; m.sy = 12; m.sz = 12;
    m.pivotX = 12; m.pivotY = 0; m.pivotZ = 6;
    m.sightX = 12; m.sightY = 6; m.sightZ = 4;

    auto add = [&](int x, int y, int z, const char* mat, const char* part) {
        float r = 0.5f, g = 0.5f, b = 0.5f;
        getMaterialColor(mat, r, g, b);
        m.voxels.push_back({x, y, z, r, g, b, part, mat});
    };

    // Stock & Grip (treated_wood)
    for (int y = 2; y <= 4; ++y) {
        for (int x = 2; x <= 6; ++x) add(x, y, 4, "treated_wood", "grip_stock");
    }
    // Trigger (sheet_metal)
    add(8, 2, 4, "sheet_metal", "trigger");
    // Action / Receiver (sheet_metal)
    for (int y = 3; y <= 4; ++y) {
        for (int x = 7; x <= 10; ++x) add(x, y, 4, "sheet_metal", "action");
    }
    for (int x = 7; x <= 8; ++x) add(x, 5, 4, "sheet_metal", "action");
    // Bolt chamber (girder)
    add(9, 5, 4, "girder", "bolt_chamber");
    add(10, 5, 4, "girder", "bolt_chamber");
    // Barrel (carbon_fiber)
    for (int x = 11; x <= 18; ++x) add(x, 4, 4, "carbon_fiber", "barrel");
    // Sight (plexiglass)
    add(12, 6, 4, "plexiglass", "sight");
    add(13, 6, 4, "plexiglass", "sight");

    return m;
}

// Procedural fallback pistol matching sidearm_light parts
inline WeaponModel buildProceduralPistol(const std::string& id = "sidearm_light") {
    WeaponModel m;
    m.id = id;
    m.sx = 16; m.sy = 10; m.sz = 8;
    m.pivotX = 6; m.pivotY = 0; m.pivotZ = 4;
    m.sightX = 7; m.sightY = 3; m.sightZ = 3;

    auto add = [&](int x, int y, int z, const char* mat, const char* part) {
        float r = 0.5f, g = 0.5f, b = 0.5f;
        getMaterialColor(mat, r, g, b);
        m.voxels.push_back({x, y, z, r, g, b, part, mat});
    };

    // Grip (treated_wood)
    for (int x = 2; x <= 4; ++x) add(x, 1, 3, "treated_wood", "grip_stock");
    // Trigger (sheet_metal)
    add(5, 1, 3, "sheet_metal", "trigger");
    // Action / Frame (sheet_metal)
    for (int x = 4; x <= 8; ++x) add(x, 2, 3, "sheet_metal", "action");
    // Bolt chamber (girder)
    add(4, 3, 3, "girder", "bolt_chamber");
    add(5, 3, 3, "girder", "bolt_chamber");
    // Barrel (carbon_fiber)
    for (int x = 9; x <= 12; ++x) add(x, 2, 3, "carbon_fiber", "barrel");
    // Sight (plexiglass)
    add(7, 3, 3, "plexiglass", "sight");

    return m;
}

// First-person viewmodel animation and handling controller
struct ViewModelAnim {
    float idleTimer = 0.0f;
    float bobTimer = 0.0f;
    float adsAlpha = 0.0f;       // 0.0 (hip) -> 1.0 (ADS)
    float equipAlpha = 1.0f;     // 0.0 (lowered) -> 1.0 (raised)

    // Reload animation state
    bool isReloading = false;
    float reloadTimer = 0.0f;
    float reloadDuration = 1.5f;
    float reloadDrop = 0.0f;
    float reloadPitch = 0.0f;
    float reloadRoll = 0.0f;

    // Spring-damper recoil dynamics
    float recoilZ = 0.0f;        // Kick backward along line of sight (m)
    float recoilY = 0.0f;        // Kick downward / upward (m)
    float recoilPitch = 0.0f;    // Barrel climb (rad)
    float recoilYaw = 0.0f;      // Lateral kick (rad)
    float recoilRoll = 0.0f;     // Wrist roll kick (rad)

    float recoilVelZ = 0.0f;
    float recoilVelPitch = 0.0f;
    float recoilVelYaw = 0.0f;
    float recoilVelRoll = 0.0f;

    int currentWeaponIndex = 0;

    void triggerRecoil(float recoilStat, float handlingStat) {
        const float kickScale = std::max(0.6f, recoilStat / 12.0f);
        const float handScale = 1.0f / (1.0f + std::max(0.0f, handlingStat) * 0.04f);
        const float p = kickScale * handScale;

        // Punch backward in Z, barrel climb up in pitch, subtle lateral kick
        recoilVelZ += 0.022f * p;
        recoilVelPitch += 0.12f * p;
        const float yawSign = (std::sin(idleTimer * 15.0f) >= 0.0f) ? 1.0f : -1.0f;
        recoilVelYaw += 0.018f * p * yawSign;
        recoilVelRoll += 0.025f * p * yawSign;
    }

    void triggerEquip() {
        equipAlpha = 0.0f;
    }

    void triggerReload() {
        if (!isReloading) {
            isReloading = true;
            reloadTimer = 0.0f;
        }
    }

    void update(float dt, const movement::MoveState& move, const PlayerBody& body, bool adsHeld) {
        idleTimer += dt;

        // Smooth ADS transition (~0.12s)
        const float adsSpeed = 8.5f;
        adsAlpha += ((adsHeld ? 1.0f : 0.0f) - adsAlpha) * std::clamp(dt * adsSpeed, 0.0f, 1.0f);

        // Smooth weapon equip raise transition (~0.22s)
        const float equipSpeed = 4.5f;
        equipAlpha += (1.0f - equipAlpha) * std::clamp(dt * equipSpeed, 0.0f, 1.0f);

        // Reload progress
        if (isReloading) {
            reloadTimer += dt;
            if (reloadTimer >= reloadDuration) {
                isReloading = false;
                reloadTimer = 0.0f;
                reloadDrop = 0.0f;
                reloadPitch = 0.0f;
                reloadRoll = 0.0f;
            } else {
                const float t = reloadTimer / reloadDuration;
                const float s = std::sin(t * 3.14159265f);
                reloadDrop = s * 0.0018f;
                reloadRoll = s * -0.22f;
                reloadPitch = s * 0.14f;
            }
        }

        // Movement bobbing
        const float horizSpeed = std::sqrt(body.vx * body.vx + body.vz * body.vz);
        float cadence = 1.8f;
        if (move.gait == movement::Gait::Sprint) cadence = 3.2f;
        else if (move.gait == movement::Gait::Run) cadence = 2.4f;
        else if (move.stance == movement::Stance::Crouch) cadence = 1.2f;
        else if (move.stance == movement::Stance::Prone) cadence = 0.7f;

        if (horizSpeed > 0.001f && body.onGround) {
            bobTimer += dt * cadence * (horizSpeed / 0.025f) * 6.283185f;
            if (bobTimer > 6283.185f) bobTimer -= 6283.185f;
        }

        // Spring-damper recoil recovery (stiff spring, critical damping)
        const float springK = 280.0f;
        const float dampD = 26.0f;

        float accelZ = -springK * recoilZ - dampD * recoilVelZ;
        recoilVelZ += accelZ * dt;
        recoilZ += recoilVelZ * dt;

        float accelPitch = -springK * recoilPitch - dampD * recoilVelPitch;
        recoilVelPitch += accelPitch * dt;
        recoilPitch += recoilVelPitch * dt;

        float accelYaw = -springK * recoilYaw - dampD * recoilVelYaw;
        recoilVelYaw += accelYaw * dt;
        recoilYaw += recoilVelYaw * dt;

        float accelRoll = -springK * recoilRoll - dampD * recoilVelRoll;
        recoilVelRoll += accelRoll * dt;
        recoilRoll += recoilVelRoll * dt;

        if (std::abs(recoilZ) < 1e-6f && std::abs(recoilVelZ) < 1e-5f) { recoilZ = 0.0f; recoilVelZ = 0.0f; }
        if (std::abs(recoilPitch) < 1e-6f && std::abs(recoilVelPitch) < 1e-5f) { recoilPitch = 0.0f; recoilVelPitch = 0.0f; }
        if (std::abs(recoilYaw) < 1e-6f && std::abs(recoilVelYaw) < 1e-5f) { recoilYaw = 0.0f; recoilVelYaw = 0.0f; }
        if (std::abs(recoilRoll) < 1e-6f && std::abs(recoilVelRoll) < 1e-5f) { recoilRoll = 0.0f; recoilVelRoll = 0.0f; }
    }
};

} // namespace weapon_view
