from pathlib import Path

root = Path(__file__).resolve().parents[1]
sh = root / "shaders"

(sh / "voxel.vert").write_text(
    """#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in float inMat;
layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 viewProj;
    vec3 lightDir;
    float _pad0;
    vec3 camPos;
    float time;
} ubo;
layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec3 fragColor;
layout(location = 2) out vec3 fragWorldPos;
layout(location = 3) out float fragMat;
void main() {
    vec3 pos = inPosition;
    if (inMat > 0.5) {
        float w = sin(pos.x * 200.0 + ubo.time * 2.0) * cos(pos.z * 180.0 + ubo.time * 1.5);
        pos.y += 0.00015 * w;
    }
    fragWorldPos = pos;
    fragNormal = inNormal;
    fragColor = inColor;
    fragMat = inMat;
    gl_Position = ubo.viewProj * vec4(pos, 1.0);
}
""",
    encoding="utf-8",
)

(sh / "voxel.frag").write_text(
    """#version 450
layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragColor;
layout(location = 2) in vec3 fragWorldPos;
layout(location = 3) in float fragMat;
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
    float ambient = 0.28 + 0.08 * max(n.y, 0.0);
    float diffuse = ndotl * 0.72;
    vec3 v = normalize(ubo.camPos - fragWorldPos);
    vec3 h = normalize(l + v);
    float spec = pow(max(dot(n, h), 0.0), 32.0) * 0.18 * ndotl;
    float heightAo = clamp(0.75 + fragWorldPos.y * 30.0, 0.55, 1.0);
    vec3 base = fragColor;
    if (fragMat > 0.5) {
        float tide = sin(fragWorldPos.x * 180.0 + ubo.time * 2.2) *
                     cos(fragWorldPos.z * 160.0 - ubo.time * 1.7);
        float foam = smoothstep(0.55, 0.95, abs(tide));
        base = mix(base, base * vec3(0.75, 0.9, 1.15), 0.35 + 0.25 * tide);
        base += vec3(0.12, 0.18, 0.22) * foam;
        spec *= 1.8;
        ambient += 0.05;
    }
    vec3 lit = base * (ambient + diffuse) * heightAo + vec3(spec);
    // Forced bitcrush for look + slightly cheaper shading continuity
    float levels = 20.0;
    lit = floor(lit * levels + 0.5) / levels;
    lit = pow(clamp(lit, 0.0, 1.0), vec3(1.1));
    outColor = vec4(lit, 1.0);
}
""",
    encoding="utf-8",
)
print("voxel shaders written")
