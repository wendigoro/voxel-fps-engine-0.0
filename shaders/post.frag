#version 450
// Post chain: the world image (rendered at the menu's render scale) is drawn
// full-screen through these effects, in this order. Each effect is a no-op at
// its neutral value, so with default settings this is an exact pass-through.
// Parameters come from the visuals registry (src/visual_params.hpp) as push
// constants; add an effect by adding a field here, in PostParams, and a
// parameter declaration there.
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(push_constant) uniform PC {
    float posterize; // levels per channel; < 2 = off
    float dither;    // 0..1, noise before posterizing
    float crush;     // gamma; 1 = off
    float time;      // seconds, animates the dither
} pc;

float hash21(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

void main() {
    vec3 col = texture(uScene, vUv).rgb;

    // 1. Posterize, optionally dithered.
    if (pc.posterize >= 2.0) {
        float levels = pc.posterize;
        float d = (hash21(gl_FragCoord.xy + fract(pc.time) * 61.0) - 0.5) * pc.dither;
        col = floor(col * levels + 0.5 + d) / levels;
    }

    // 2. Crush (gamma).
    if (pc.crush != 1.0) {
        col = pow(clamp(col, 0.0, 1.0), vec3(pc.crush));
    }

    outColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
