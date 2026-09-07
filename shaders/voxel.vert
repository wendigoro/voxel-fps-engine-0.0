#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in float inMat;

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
    vec4 bulbPos[4];
    vec4 bulbColor[4];
} ubo;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec3 fragColor;
layout(location = 2) out vec3 fragWorldPos;
layout(location = 3) out float fragMat;
layout(location = 4) out vec2 fragNdc;
layout(location = 5) out float fragViewZ;

void main() {
    vec3 pos = inPosition;
    if (inMat > 0.5 && inMat < 1.5) {
        float w = sin(pos.x * 200.0 + ubo.time * 2.0) * cos(pos.z * 180.0 + ubo.time * 1.5);
        pos.y += 0.00015 * w;
    }
    fragWorldPos = pos;
    fragNormal = inNormal;
    fragColor = inColor;
    fragMat = inMat;

    vec4 clip = ubo.viewProj * vec4(pos, 1.0);
    float wclip = max(abs(clip.w), 1e-5);
    vec2 ndc = clip.xy / wclip;

    // Strong true-sky style fisheye (barrel + higher-order terms)
    float r = length(ndc);
    float strength = (inMat > 2.5) ? 1.35 : 1.0;
    float k1 = 0.55 * strength;
    float k2 = 0.22 * strength;
    float k3 = 0.08 * strength;
    float r2 = r * r;
    float fisheye = 1.0 + k1 * r2 + k2 * r2 * r2 + k3 * r2 * r2 * r2;
    float edgeSoft = smoothstep(1.85, 1.15, r * fisheye);
    ndc *= mix(1.0, fisheye, 0.92 + 0.08 * edgeSoft);
    ndc.y *= 1.04;

    fragNdc = ndc;
    fragViewZ = clip.w;
    clip.xy = ndc * wclip;

    // Sky dome + moon stay at far plane so world wins on depth
    if (inMat > 2.5) {
        clip.z = clip.w * 0.999;
    }

    gl_Position = clip;
}
