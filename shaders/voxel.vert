#version 450
#extension GL_GOOGLE_include_directive : require
#include "render_class.glsl"
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in float inMat;
layout(location = 4) in vec3 inTex; // texture layer + 1, painted, shade

layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 viewProj;
    vec3 sunDir;
    float timeOfDay;
    vec3 camPos;
    float time;
    vec3 moonDir;
    float moonIntensity;
    vec3 moonColor;
    float ambientScale;
    float muzzleFlash;
    float fireOverlay;
    float damageFlash;
    float healthTint;
    float fisheyeScale; // visuals menu: lens curve multiplier (1 = original)
    float banding;      // visuals menu: colour-step multiplier (1 = original, 0 = off)
    float uboPad0;
    float uboPad1;
    vec4 texParams[16]; // per texture layer: tileCells, tint, coverage, maskFromLuma
    vec4 texGlobal;     // enabled, strength, scale, unused
    vec4 occDims;       // occupancy volume W, H, D (cells), shadows enabled
    vec4 shadowParams;  // max cells crossed, 1, unused, unused
    vec4 lightInfo;     // light count (storage buffer below), unused x3
} ubo;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec3 fragColor;
layout(location = 2) out vec3 fragWorldPos;
layout(location = 3) out float fragMat;
layout(location = 4) out vec2 fragNdc;
layout(location = 5) out float fragViewZ;
layout(location = 6) flat out vec2 fragTexId; // texture layer + 1, painted
layout(location = 7) out float fragShade;

void main() {
    int rc = renderClass(inMat);
    vec3 pos = inPosition;
    if (rc == RC_WATER) {
        float w = sin(pos.x * 200.0 + ubo.time * 2.0) * cos(pos.z * 180.0 + ubo.time * 1.5);
        pos.y += 0.00015 * w;
    }
    fragWorldPos = pos;
    fragNormal = inNormal;
    fragColor = inColor;
    fragMat = inMat;
    fragTexId = inTex.xy;
    fragShade = inTex.z;

    vec4 clip = ubo.viewProj * vec4(pos, 1.0);
    float wclip = max(abs(clip.w), 1e-5);
    vec2 ndc = clip.xy / wclip;

    // Strong true-sky style fisheye (barrel + higher-order terms)
    // Skipped for the inventory lattice ONLY (RULES.md rule 12): it is parented
    // to the camera and must read as a clean 3D projection rather than being
    // bent by a screen-space barrel distortion. World pickups keep the fisheye
    // like every other world object. This is also what makes the CPU
    // screen-space cell picking in main.cpp exact: the lattice is the only
    // geometry whose GPU rasterisation matches its unprojected CPU transform.
    if (rc != RC_INVENTORY_LATTICE) {
    float r = length(ndc);
    float strength = (rc == RC_MOON || rc == RC_SKY) ? 1.35 : 1.0;
    strength *= ubo.fisheyeScale;
    float k1 = 0.55 * strength;
    float k2 = 0.22 * strength;
    float k3 = 0.08 * strength;
    float r2 = r * r;
    float fisheye = 1.0 + k1 * r2 + k2 * r2 * r2 + k3 * r2 * r2 * r2;
    float edgeSoft = smoothstep(1.85, 1.15, r * fisheye);
    ndc *= mix(1.0, fisheye, 0.92 + 0.08 * edgeSoft);
    ndc.y *= 1.04;
    }

    fragNdc = ndc;
    fragViewZ = clip.w;
    clip.xy = ndc * wclip;

    // Sky dome + moon stay at far plane so world wins on depth
    if (rc == RC_MOON || rc == RC_SKY) {
        clip.z = clip.w * 0.999;
    }
    // Muzzle flash cubes: slight depth bias toward camera so they don't z-fight.
    // The inventory lattice needs none: it draws in the overlay pass with a
    // cleared depth buffer.
    if (rc == RC_MUZZLE) {
        clip.z = clip.z * 0.98;
    }

    gl_Position = clip;
}
