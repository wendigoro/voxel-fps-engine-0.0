#pragma once
// Body-authoritative player movement (RULES.md, "Player body, and the
// camera-offset contract").
//
// Ported from the movement work salvaged out of an abandoned worktree (stance,
// gait, slide, wallrun, dash, stamina). The tuning constants and the state
// machine are carried over; the *shape* is not, because the original moved the
// player by writing the camera transform and resolved collision against the
// view's chunk mesh.
//
// What changed, and why:
//
//   - It moves the *body*. The original advanced `g_camPos` and let the next
//     frame's collision resolve from it, which made the camera the authority and
//     made two views with different render settings disagree about the player.
//   - It does not own collision. It reads `sim::World` only to *ask* questions
//     (is there ground, is there a wall, is a stance change still legal) and
//     hands back a velocity. The existing integrator still resolves the body
//     against the grid, so step-up, terminal velocity and fall damage survive.
//   - It reads intent from `SimInput`. The original polled `g_keys` and
//     `GetAsyncKeyState` inside the update and detected the stance toggle with a
//     `static bool prevControl` latch, so the outcome depended on how many
//     frames were sampled and whether a tap was caught.
//   - Stance is a real body height, so the hitbox follows it. A prone body is
//     short, and "can I stand up here" is a grid question, not a camera one.
//   - Wallrun roll, slide lean and eye lifts are emitted as a `CameraOffset` the
//     view applies after the body is settled. `CameraOffset` is never read back
//     here, and nothing re-derives position from it.

#include "sim_input.hpp"
#include "sim_world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace movement {

enum class Stance : uint8_t { Standing = 0, Crouch = 1, Prone = 2 };
enum class Gait : uint8_t { Walk = 0, Run = 1, Sprint = 2 };

// --- stance ---
inline constexpr float STANCE_EYE_HEIGHT[3] = {0.0165f, 0.0085f, 0.0035f};
inline constexpr float STANCE_COLLISION_HEIGHT[3] = {0.0185f, 0.011f, 0.006f};
inline constexpr float STANCE_SPEED_MULT[3] = {1.0f, 0.55f, 0.25f};

// --- gait ---
inline constexpr float GAIT_SPEED_MULT[3] = {0.6f, 1.0f, 1.8f};
inline constexpr float GAIT_STAMINA_DRAIN[3] = {0.0f, 0.0f, 18.0f};
inline constexpr float GAIT_STAMINA_REGEN = 25.0f;

// --- slide ---
inline constexpr float SLIDE_ENTER_MIN_SPEED = 0.12f;
inline constexpr float SLIDE_INITIAL_BOOST = 1.6f;
inline constexpr float SLIDE_FRICTION = 0.85f;
inline constexpr float SLIDE_MIN_SPEED = 0.02f;
inline constexpr float SLIDE_MAX_TIME = 2.0f;

// --- wallrun ---
inline constexpr float WALLRUN_MAX_TIME = 3.0f;
inline constexpr float WALLRUN_GRAVITY_SCALE = 0.15f;
inline constexpr float WALLRUN_JUMP_IMPULSE = 0.06f;
inline constexpr float WALLRUN_CAMERA_TILT = 0.35f;  // radians, presentation only
inline constexpr float WALLRUN_KICK_OFF = 0.15f;
inline constexpr float WALLRUN_STAMINA_FLOOR = 10.0f;

// --- dash ---
inline constexpr float DASH_SPEED = 0.2f;
inline constexpr float DASH_DURATION = 0.18f;
inline constexpr float DASH_COOLDOWN = 1.2f;
inline constexpr float DASH_INVULN_TIME = 0.12f;
inline constexpr float DASH_STAMINA_COST = 25.0f;

// --- stamina ---
inline constexpr float kStaminaMax = 100.0f;
inline constexpr float kStaminaRegenDelay = 0.5f;

// The body convention: `py` is the FEET, not the centre. That is what
// PlayerBody::py means everywhere else in the simulation, and the salvaged
// movement code got it wrong — it subtracted half a body height from a
// feet-height, so its ground probe sat a body height below the floor and the
// player never registered as grounded.

// How far below the feet counts as ground. Matches the integrator's ground
// probe so both agree on the same tick.
inline constexpr float kGroundProbe = 0.15f * kVoxelSize;

// How far the body may be pushed sideways before it counts as touching a wall.
// Roughly the hitbox half-width, so "wall adjacent to the body" means adjacent.
inline constexpr float kWallProbeDistance = 1.2f * kVoxelSize;

// Everything the movement system owns. Simulation state, not view state.
struct MoveState {
    Stance stance = Stance::Standing;
    Gait gait = Gait::Walk;
    bool sliding = false;
    int slideDirX = 0, slideDirZ = 0;   // one of 4 flat cardinals
    float slideSpeed = 0.0f;
    float slideTime = 0.0f;
    bool wallRunning = false;
    int wallRunSide = 0;                // -1 left, +1 right
    float wallRunTime = 0.0f;
    float wallRunYaw = 0.0f;
    bool dashing = false;
    float dashTime = 0.0f;
    float dashCooldown = 0.0f;
    int dashDirX = 0, dashDirZ = 0;
    bool dashInvuln = false;
    float dashInvulnTime = 0.0f;
    float stamina = kStaminaMax;
    float staminaRegenDelay = 0.0f;
    float animTime = 0.0f;
    // Flat speed at the end of the previous tick, kept as *state* rather than a
    // function-local static. The original stashed this in a `static float
    // prevSpeed`, which made slide entry depend on how many times the function
    // had ever been called.
    float prevFlatSpeed = 0.0f;
    // The body, as the movement system last saw it.
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float vx = 0.0f, vy = 0.0f, vz = 0.0f;
    bool onGround = false;
};

// What the movement system wants the integrator to do this tick. The integrator
// owns collision; this only says how fast and in which direction.
struct MoveIntent {
    float speedScale = 1.0f;   // multiplies the base walk speed
    bool overrideHorizontal = false;  // slide / dash / wallrun drive vx,vz directly
    float vx = 0.0f, vz = 0.0f;
    float jumpImpulse = 0.0f;  // > 0 sets vy and leaves the ground
    float gravityScale = 1.0f; // < 1 while wallrunning
    float facingYaw = 0.0f;    // authoritative facing after a wallrun turn
};

// Presentation only. The view applies this on top of the settled eye; the
// simulation never reads it back. Anything that would change where the player
// lands if this were dropped belongs in the body, not here.
struct CameraOffset {
    float roll = 0.0f;         // radians about the view axis
    float eyeLift = 0.0f;      // world units, relative to stance eye height
    float forwardLean = 0.0f;  // world units along facing
    float lateralLean = 0.0f;  // world units along the player's right
};

struct MoveResult {
    MoveIntent intent;
    CameraOffset cam;
    bool dashInvuln = false;  // consumed by the damage intake this tick
};

// Water is never solid for player physics: the ground probe, the wall probe and
// every stance test treat it as passable. Both water flavours count — a current
// is still water to a body.
inline bool solidForPhysics(const sim::World& w, int x, int y, int z) {
    const sim::Block b = w.get(x, y, z);
    return b != sim::Block::Air && b != sim::Block::Water && b != sim::Block::WaterCurrent;
}

inline int cellX(float worldX) { return static_cast<int>(std::floor(worldX / kVoxelSize)); }
inline int cellZ(float worldZ) { return static_cast<int>(std::floor(worldZ / kVoxelSize)); }
inline int cellY(float worldY) { return static_cast<int>(std::floor(worldY / kVoxelSize)); }

// Is there ground under the feet at this stance height? Water is not ground —
// the player floats and swims in it, and a water cell must not read as a floor
// that a slide can terminate on.
inline bool groundUnder(const sim::World& w, float px, float py, float pz, float stanceHeight) {
    (void)stanceHeight;
    const int y = cellY(py - kGroundProbe);
    return sim::World::inBounds(cellX(px), y, cellZ(pz)) && solidForPhysics(w, cellX(px), y, cellZ(pz));
}

// Is there room for a body of this height standing at these feet? This is what
// gates standing up: you may only leave crouch or prone if a standing body fits.
inline bool headroom(const sim::World& w, float px, float py, float pz, float wantHeight) {
    const int top = cellY(py + wantHeight);
    const int foot = cellY(py + kGroundProbe);
    for (int dx = -1; dx <= 1; ++dx) {
        for (int dz = -1; dz <= 1; ++dz) {
            const int x = cellX(px) + dx, z = cellZ(pz) + dz;
            if (!sim::World::inBounds(x, top, z)) return false;
            for (int y = foot; y <= top; ++y) {
                if (!sim::World::inBounds(x, y, z)) return false;
                if (solidForPhysics(w, x, y, z)) return false;
            }
        }
    }
    return true;
}

// -1 wall on the left, +1 on the right, 0 for none. Probes low and high so a
// waist-high ledge does not read as a wall.
inline int checkWallSide(const sim::World& w, float px, float py, float pz,
                         float rx, float rz, float stanceHeight) {
    const float footY = py + kGroundProbe;
    const float headY = py + stanceHeight;
    auto wallAt = [&](float ox, float oz) {
        const int x = cellX(px + rx * ox);
        const int z = cellZ(pz + rz * oz);
        const int yf = cellY(footY), yh = cellY(headY);
        if (sim::World::inBounds(x, yf, z) && solidForPhysics(w, x, yf, z)) return true;
        return yh != yf && sim::World::inBounds(x, yh, z) && solidForPhysics(w, x, yh, z);
    };
    const bool right = wallAt(kWallProbeDistance, kWallProbeDistance);
    const bool left = wallAt(-kWallProbeDistance, -kWallProbeDistance);
    if (right && !left) return +1;
    if (left && !right) return -1;
    return 0;
}

// Advance the movement state machine one tick. Pure with respect to the view:
// it never reads the camera, and the only thing it returns about presentation
// is a `CameraOffset` the caller applies after the body is settled.
//
// `m.px/py/pz`, `m.vx/vy/vz` and `m.onGround` are refreshed by the caller from
// the authoritative body before the call; `baseSpeed` and `jumpSpeed` are the
// body's own tuning.
inline MoveResult update(float dt, const SimInput& in, const sim::World& w, MoveState& m,
                         float yaw, float baseSpeed, float jumpSpeed) {
    MoveResult out;

    // Flat facing and strafe axes, derived from the sim's authoritative yaw.
    const float fx = -std::sin(yaw), fz = -std::cos(yaw);
    const float rx = -fz, rz = fx;

    const bool onGround = groundUnder(w, m.px, m.py, m.pz, STANCE_COLLISION_HEIGHT[static_cast<int>(m.stance)]);

    // --- stance ---
    // An explicit stance edge owns the tick outright. Otherwise crouch behaves
    // as a level: press to go down, release to come back up. The two must not
    // both act, or holding crouch while cycling the stance would fight itself
    // and the state would depend on the order they happened to run in.
    if (in.stanceCycle) {
        const Stance want = m.stance == Stance::Standing ? Stance::Crouch
                            : m.stance == Stance::Crouch  ? Stance::Prone
                                                         : Stance::Standing;
        if (want == Stance::Standing) {
            // Only stand if a standing body actually fits where the body is.
            if (headroom(w, m.px, m.py, m.pz, STANCE_COLLISION_HEIGHT[0])) m.stance = want;
        } else {
            m.stance = want;
        }
    } else if (in.crouch) {
        if (m.stance == Stance::Standing) m.stance = Stance::Crouch;
    } else if (m.stance == Stance::Crouch) {
        if (headroom(w, m.px, m.py, m.pz, STANCE_COLLISION_HEIGHT[0])) m.stance = Stance::Standing;
    }

    // --- dash: an edge, and a body velocity the integrator then collides ---
    if (m.dashCooldown > 0.0f) m.dashCooldown -= dt;
    if (m.dashInvulnTime > 0.0f) {
        m.dashInvulnTime -= dt;
        if (m.dashInvulnTime <= 0.0f) m.dashInvuln = false;
    }
    if (in.dash && !m.dashing && m.dashCooldown <= 0.0f && m.stamina >= DASH_STAMINA_COST) {
        m.dashing = true;
        m.dashTime = DASH_DURATION;
        m.dashCooldown = DASH_COOLDOWN;
        m.dashInvuln = true;
        m.dashInvulnTime = DASH_INVULN_TIME;
        m.stamina -= DASH_STAMINA_COST;
        m.staminaRegenDelay = kStaminaRegenDelay;

        float dx = fx * in.moveForward + rx * in.moveRight;
        float dz = fz * in.moveForward + rz * in.moveRight;
        if (dx * dx + dz * dz < 1e-8f) { dx = fx; dz = fz; }
        if (std::fabs(dx) >= std::fabs(dz)) {
            m.dashDirX = dx >= 0.0f ? 1 : -1;
            m.dashDirZ = 0;
        } else {
            m.dashDirX = 0;
            m.dashDirZ = dz >= 0.0f ? 1 : -1;
        }
    }
    if (m.dashing) {
        m.dashTime -= dt;
        if (m.dashTime <= 0.0f) m.dashing = false;
    }
    out.dashInvuln = m.dashInvuln;

    // --- wallrun detection: needs a wall, air, and stamina ---
    const int wallSide = (!m.dashing && !m.sliding && !onGround && m.stamina > WALLRUN_STAMINA_FLOOR)
                             ? checkWallSide(w, m.px, m.py, m.pz, rx, rz,
                                             STANCE_COLLISION_HEIGHT[static_cast<int>(m.stance)])
                             : 0;
    if (wallSide != 0) {
        if (m.wallRunning && m.wallRunSide != wallSide) m.wallRunTime = 0.0f;
        if (!m.wallRunning) m.wallRunSide = wallSide;
        m.wallRunning = true;
        m.wallRunTime += dt;
        if (m.wallRunTime > WALLRUN_MAX_TIME) m.wallRunning = false;
        // The *body* turns to run along the wall. This is a real facing change,
        // not a camera trick, so it is reported as an authoritative yaw.
        m.wallRunYaw = std::atan2(-rx * static_cast<float>(wallSide), -rz * static_cast<float>(wallSide));
    } else {
        m.wallRunning = false;
        m.wallRunTime = 0.0f;
    }

    // --- gait from intent, not from a device poll ---
    const bool moving = std::fabs(in.moveForward) > 1e-6f || std::fabs(in.moveRight) > 1e-6f;
    if (in.sprint && onGround && moving && !m.sliding && !m.wallRunning && m.stamina > 0.0f) {
        m.gait = Gait::Sprint;
    } else if (moving && !m.sliding && !m.wallRunning) {
        m.gait = m.stance == Stance::Standing ? Gait::Run : Gait::Walk;
    } else if (!moving) {
        m.gait = Gait::Walk;
    }

    // --- stamina ---
    const float drain = GAIT_STAMINA_DRAIN[static_cast<int>(m.gait)];
    if (drain > 0.0f) {
        m.stamina = std::max(0.0f, m.stamina - drain * dt);
        m.staminaRegenDelay = kStaminaRegenDelay;
    } else if (m.staminaRegenDelay > 0.0f) {
        m.staminaRegenDelay -= dt;
    } else {
        m.stamina = std::min(kStaminaMax, m.stamina + GAIT_STAMINA_REGEN * dt);
    }

    // --- slide entry: sprinting and pressing crouch while already fast ---
    // Evaluated before the resolve step below, so the slide actually drives the
    // body on the tick it starts rather than one tick late.
    if (!m.sliding && !m.dashing && !m.wallRunning && onGround &&
        m.gait == Gait::Sprint && in.crouch && m.prevFlatSpeed > SLIDE_ENTER_MIN_SPEED) {
        m.sliding = true;
        m.slideTime = 0.0f;
        m.slideSpeed = m.prevFlatSpeed * SLIDE_INITIAL_BOOST;
        // A slide commits to the cardinal it started on, so it stays predictable.
        if (std::fabs(fx) >= std::fabs(fz)) {
            m.slideDirX = fx >= 0.0f ? 1 : -1;
            m.slideDirZ = 0;
        } else {
            m.slideDirX = 0;
            m.slideDirZ = fz >= 0.0f ? 1 : -1;
        }
        m.stance = Stance::Crouch;
    }

    // --- resolve what the integrator should do ---
    const float stanceMult = STANCE_SPEED_MULT[static_cast<int>(m.stance)];

    if (m.dashing) {
        out.intent.overrideHorizontal = true;
        out.intent.vx = static_cast<float>(m.dashDirX) * DASH_SPEED;
        out.intent.vz = static_cast<float>(m.dashDirZ) * DASH_SPEED;
        if (in.dash) m.dashTime = DASH_DURATION;  // hold the dash while held
    } else if (m.wallRunning) {
        const float wallSpeed = baseSpeed * stanceMult * GAIT_SPEED_MULT[1] * 1.2f;
        out.intent.overrideHorizontal = true;
        out.intent.vx = std::cos(m.wallRunYaw) * wallSpeed;
        out.intent.vz = -std::sin(m.wallRunYaw) * wallSpeed;
        out.intent.gravityScale = WALLRUN_GRAVITY_SCALE;
        out.intent.facingYaw = m.wallRunYaw;
        // Presentation only: roll toward the wall and lean off it. Nothing reads
        // this back, and dropping it changes neither position nor collision.
        out.cam.roll = static_cast<float>(m.wallRunSide) * WALLRUN_CAMERA_TILT;
        out.cam.lateralLean = -static_cast<float>(m.wallRunSide) * kWallProbeDistance * 0.5f;
        if (in.jump) {
            m.wallRunning = false;
            out.intent.jumpImpulse = WALLRUN_JUMP_IMPULSE;
            // A kick-off pushes the body away from the wall, not the camera.
            out.intent.overrideHorizontal = false;
            out.intent.gravityScale = 1.0f;
        }
    } else if (m.sliding) {
        m.slideTime += dt;
        m.slideSpeed *= std::pow(SLIDE_FRICTION, dt);
        const bool finished = m.slideSpeed < SLIDE_MIN_SPEED ||
                              m.slideTime > SLIDE_MAX_TIME || !onGround;
        if (finished) {
            m.sliding = false;
            m.slideSpeed = 0.0f;
            m.slideTime = 0.0f;
        } else {
            out.intent.overrideHorizontal = true;
            out.intent.vx = static_cast<float>(m.slideDirX) * m.slideSpeed;
            out.intent.vz = static_cast<float>(m.slideDirZ) * m.slideSpeed;
            // Presentation only: pitch the eye forward into the slide.
            out.cam.forwardLean = m.slideSpeed * SLIDE_ENTER_MIN_SPEED;
            out.cam.roll = 0.12f;
        }
    }

    if (!out.intent.overrideHorizontal) {
        out.intent.speedScale = stanceMult * GAIT_SPEED_MULT[static_cast<int>(m.gait)];
    }

    // --- jump, from the edge on the wire ---
    // Stance-scaled: a crouched or prone body launches less.
    if (in.jump && out.intent.jumpImpulse <= 0.0f && onGround) {
        out.intent.jumpImpulse = jumpSpeed * stanceMult;
    }

    m.animTime += dt;
    const float wishX = fx * in.moveForward + rx * in.moveRight;
    const float wishZ = fz * in.moveForward + rz * in.moveRight;
    m.prevFlatSpeed = std::sqrt(wishX * wishX + wishZ * wishZ) * baseSpeed;
    return out;
}

}  // namespace movement
