#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragColor;
layout(location = 2) in vec3 fragWorldPos;

layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 viewProj;
    vec3 lightDir;
    float _pad0;
    vec3 camPos;
    float time;
} ubo;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 n = normalize(fragNormal);
    vec3 l = normalize(-ubo.lightDir);
    float ndotl = max(dot(n, l), 0.0);

    // Soft ambient + directional + slight sky tint by normal.y
    float ambient = 0.28 + 0.08 * max(n.y, 0.0);
    float diffuse = ndotl * 0.72;

    // Cheap specular for a bit of "blocky plastic" sheen
    vec3 v = normalize(ubo.camPos - fragWorldPos);
    vec3 h = normalize(l + v);
    float spec = pow(max(dot(n, h), 0.0), 32.0) * 0.18 * ndotl;

    // Fake AO from height (lower voxels slightly darker)
    float heightAo = clamp(0.75 + fragWorldPos.y * 0.03, 0.55, 1.0);

    vec3 lit = fragColor * (ambient + diffuse) * heightAo + vec3(spec);
    // Mild contrast curve
    lit = pow(clamp(lit, 0.0, 1.0), vec3(1.05));
    outColor = vec4(lit, 1.0);
}
