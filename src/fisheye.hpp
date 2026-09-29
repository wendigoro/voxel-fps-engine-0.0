// Canonical fisheye projection constants.
//
// shaders/voxel.vert applies a radial NDC expansion after the view-projection.
// The CPU culler has to agree with it, or the two drift apart and chunks either
// pop in or are drawn for nothing. These values are the single source of truth
// for BOTH sides; scripts/check_constants.py fails the build if the shader's
// literals stop matching, exactly as it does for VOXEL_SIZE and the material table.
//
// The shader treats sky/moon geometry (mat 2.5..4.5) with a stronger curve.
#pragma once

// Strength applied to normal world geometry.
static constexpr float kFisheyeStrengthWorld = 1.0f;
// Strength applied to sky tiles and the moon sprite (mat 2.5..4.5).
static constexpr float kFisheyeStrengthSky = 1.35f;

// Polynomial coefficients, scaled by the strength at the point of use.
static constexpr float kFisheyeK1 = 0.55f;
static constexpr float kFisheyeK2 = 0.22f;
static constexpr float kFisheyeK3 = 0.08f;

// Radial expansion floor and the mix weight toward the full curve.
static constexpr float kFisheyeMixBase = 0.92f;
static constexpr float kFisheyeMixEdge = 0.08f;
// smoothstep(hi, lo, r) bounds used to soften the curve near the frame edge.
static constexpr float kFisheyeEdgeHi = 1.85f;
static constexpr float kFisheyeEdgeLo = 1.15f;
// Extra vertical stretch applied to NDC after the radial term.
static constexpr float kFisheyeYSquash = 1.04f;

// Final NDC radius for a vertex at pre-transform radius r.
static inline float fisheyeNdcRadius(float r, float strength) {
    const float k1 = kFisheyeK1 * strength;
    const float k2 = kFisheyeK2 * strength;
    const float k3 = kFisheyeK3 * strength;
    const float r2 = r * r;
    const float f = 1.0f + k1 * r2 + k2 * r2 * r2 + k3 * r2 * r2 * r2;

    // smoothstep(kFisheyeEdgeHi, kFisheyeEdgeLo, r * f)
    float t = (kFisheyeEdgeHi - r * f) / (kFisheyeEdgeHi - kFisheyeEdgeLo);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float edgeSoft = t * t * (3.0f - 2.0f * t);

    return r * f * (kFisheyeMixBase + kFisheyeMixEdge * edgeSoft);
}

// Largest pre-transform NDC radius that still lands on screen. Anything beyond
// this is pushed off the viewport by the vertex shader, so the culler may skip it.
//
// Found by bisection on fisheyeNdcRadius; computed rather than hardcoded so it
// cannot fall out of step with the curve above.
static inline float fisheyeVisibleNdcRadius(float strength) {
    float lo = 0.0f, hi = 4.0f;
    for (int i = 0; i < 48; ++i) {
        const float mid = (lo + hi) * 0.5f;
        if (fisheyeNdcRadius(mid, strength) <= 1.0f) lo = mid; else hi = mid;
    }
    return lo;
}
