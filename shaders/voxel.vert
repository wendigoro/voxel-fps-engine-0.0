#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

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

void main() {
    fragWorldPos = inPosition;
    fragNormal = inNormal;
    fragColor = inColor;
    gl_Position = ubo.viewProj * vec4(inPosition, 1.0);
}
