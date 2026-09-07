#version 450
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(push_constant) uniform PC {
    float bits;       // color quantization levels (e.g. 16, 32)
    float dither;     // 0..1
    float time;
} pc;

float hash21(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

void main() {
    // Nearest-neighbor sample for chunky downscale look
    vec2 texSize = vec2(textureSize(uScene, 0));
    vec2 uv = (floor(vUv * texSize) + 0.5) / texSize;
    vec3 col = texture(uScene, uv).rgb;

    float levels = max(pc.bits, 2.0);
    float d = (hash21(gl_FragCoord.xy + pc.time) - 0.5) * pc.dither / levels;
    col = floor(col * levels + d) / levels;

    // Mild crush curve keeps darks heavy (cheap "bitcrunch")
    col = pow(clamp(col, 0.0, 1.0), vec3(1.15));
    outColor = vec4(col, 1.0);
}
