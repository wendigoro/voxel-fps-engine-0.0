#version 450
#extension GL_GOOGLE_include_directive : require
#include "render_class.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in float inMat;
layout(location = 4) in uvec4 inBone;
layout(location = 5) in vec4 inWeight;
layout(location = 6) in uint inRegion;

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
    float fisheyeScale;
    float banding;
    float uboPad0;
    float uboPad1;
    vec4 texParams[16];
    vec4 texGlobal;
    vec4 occDims;
    vec4 shadowParams;
    vec4 lightInfo;
} ubo;

layout(std430, set = 0, binding = 4) readonly buffer BonePalette {
    mat4 boneTransforms[];
};

layout(push_constant) uniform PushConstants {
    mat4 modelMatrix;
} push;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec3 fragColor;
layout(location = 2) out vec3 fragWorldPos;
layout(location = 3) out float fragMat;
layout(location = 4) out vec2 fragNdc;
layout(location = 5) out float fragViewZ;
layout(location = 6) flat out vec2 fragTexId;
layout(location = 7) out float fragShade;

void main() {
    mat4 skinMat = inWeight.x * boneTransforms[inBone.x] +
                   inWeight.y * boneTransforms[inBone.y] +
                   inWeight.z * boneTransforms[inBone.z] +
                   inWeight.w * boneTransforms[inBone.w];

    vec4 localPos = skinMat * vec4(inPosition, 1.0);
    vec4 worldPos = push.modelMatrix * localPos;

    mat3 normMat = mat3(push.modelMatrix) * mat3(skinMat);
    vec3 worldNormal = normalize(normMat * inNormal);

    fragWorldPos = worldPos.xyz;
    fragNormal = worldNormal;
    fragColor = inColor;
    fragMat = inMat;
    fragTexId = vec2(0.0, 0.0);
    fragShade = 1.0;

    vec4 clip = ubo.viewProj * worldPos;
    float wclip = max(abs(clip.w), 1e-5);
    vec2 ndc = clip.xy / wclip;

    float r = length(ndc);
    if (r > 1e-6) {
        float rWarp = r * (1.0 + 0.15 * r * r * ubo.fisheyeScale);
        ndc = (ndc / r) * rWarp;
        clip.xy = ndc * wclip;
    }

    gl_Position = clip;
    fragNdc = ndc;
    fragViewZ = clip.w;
}
