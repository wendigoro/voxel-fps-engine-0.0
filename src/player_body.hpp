#pragma once
// Physics-bound player body capsule (RULES.md).
// Simulation owns position, velocity, and capsule geometry; view reads it.

struct PlayerBody {
    float px = 0.0f, py = 0.0f, pz = 0.0f; // feet center (world)
    float vx = 0.0f, vy = 0.0f, vz = 0.0f;
    bool onGround = false;
    float lean = 0.0f;       // current lean -1..+1 (Q left, E right)
    float leanTarget = 0.0f;
    float eyeHeight = 0.0165f; // ~16.5 unit voxels
    float height = 0.0185f;    // full body height
    float radius = 0.0022f;    // horizontal half-extent (~2.2 unit voxels)
    float jumpSpeed = 0.055f;
};
