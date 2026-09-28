// View -> simulation input contract.
//
// This is the *only* channel by which a view may influence the simulation. Before
// this existed the sim read the Win32 key array directly (g_keys['W'], g_keys['X'])
// and mouse deltas mutated g_yaw/g_pitch inside WndProc. That made a headless sim
// process impossible: it has no keyboard, and its aim depended on how many times
// Windows happened to deliver a WM_MOUSEMOVE.
//
// A view now fills one of these per tick and hands it to sim::tick(). The sim
// never sees a key code, a window handle, a cursor position, or a frame counter.
// What crosses this boundary is player *intent*, which is exactly the quantity a
// network protocol would carry.
//
// Deliberately fixed-width and trivially copyable: no pointers, no size_t, no
// std::string, no constructors that could throw. It is safe to memcpy over a
// socket, a shared-memory slot, or an in-process call. See voxel_wire.hpp for the
// same rules applied to the snapshot side of the boundary.
#pragma once

#include <cstdint>

// One tick of player intent.
//
// Level inputs (move, look-while-held, ads, fire) are the state the view believes
// is true right now. Edge inputs (jump, firePressed, the select/cycle buttons) are
// one-shot and are consumed by the tick that reads them; the view is responsible
// for not re-raising an edge it has already sent.
struct SimInput {
    // --- movement: analog wish axes in the player's facing frame, each -1..+1 ---
    // The sim resolves them against its own authoritative yaw, so a view cannot
    // teleport the player by sending a world-space direction.
    float moveForward = 0.0f;
    float moveRight = 0.0f;
    bool sprint = false;
    bool jump = false;        // edge

    // --- look: accumulated pointer deltas in pixels, consumed and zeroed by the
    // tick. Accumulating here (rather than applying in the message handler) is
    // what makes aim a function of tick count instead of message rate. ---
    float lookDx = 0.0f;
    float lookDy = 0.0f;

    // --- lean: Q/E, clamped by the sim to -1..+1 ---
    float leanAxis = 0.0f;

    bool ads = false;         // hold optic ADS

    // --- weapon fire ---
    bool firePressed = false; // edge: semi/bolt fire one round
    bool fireHeld = false;    // level: auto fire while held

    // --- loadout selection. These are *requests*; the sim validates them
    // against its own inventory and ignores anything out of range. A view may
    // ask for a weapon the player does not have and must not get one. ---
    // -1 means "no request this tick".
    int8_t selectCaliber = -1;   // 0 light, 1 medium, 2 heavy, 3 energy
    bool cycleAmmo = false;      // edge: R
    bool cycleWeapon = false;    // edge: V
    bool cycleFireMode = false;  // edge: B
};

// Reset an input to "no intent". Called after each tick so level state is always
// re-asserted by the view and edges never fire twice.
inline void simInputClearEdges(SimInput& in) {
    in.jump = false;
    in.firePressed = false;
    in.cycleAmmo = false;
    in.cycleWeapon = false;
    in.cycleFireMode = false;
    in.lookDx = 0.0f;
    in.lookDy = 0.0f;
    in.selectCaliber = -1;
}
