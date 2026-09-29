from pathlib import Path

root = Path(__file__).resolve().parents[1]
sh = root / "shaders"

vert = r"""#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in float inMat;

layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 viewProj;
    vec3 sunDir;
    float timeOfDay;     // 0..1, night ~0.85
    vec3 camPos;
    float time;
    vec3 moonDir;
    float moonIntensity;
    vec3 moonColor;
    float ambientScale;
    vec4 bulbPos[4];     // xyz, intensity
    vec4 bulbColor[4];   // rgb, radius
} ubo;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec3 fragColor;
layout(location = 2) out vec3 fragWorldPos;
layout(location = 3) out float fragMat;

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
    gl_Position = ubo.viewProj * vec4(pos, 1.0);
}
"""

frag = r"""#version 450
layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragColor;
layout(location = 2) in vec3 fragWorldPos;
layout(location = 3) in float fragMat;

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

layout(location = 0) out vec4 outColor;

// Cheap analytic soft shadow ray toward a directional light (ground + slab occlusion).
float shadowRayDir(vec3 origin, vec3 L, float maxDist) {
    vec3 o = origin + normalize(fragNormal) * 0.0004;
    float t = 0.0008;
    float shadow = 1.0;
    for (int i = 0; i < 10; ++i) {
        vec3 p = o + L * t;
        // Ground / dirt plane occlusion
        if (p.y < 0.0025) { shadow = min(shadow, 0.12 + t * 8.0); break; }
        // Soft warehouse slab volume (approx map slab)
        if (p.y > 0.002 && p.y < 0.006) {
            if (p.x > 0.01 && p.x < 0.15 && p.z > 0.01 && p.z < 0.12)
                shadow = min(shadow, 0.2 + t * 5.0);
        }
        t += 0.0025 + t * 0.15;
        if (t > maxDist) break;
    }
    return clamp(shadow, 0.08, 1.0);
}

float shadowRayPoint(vec3 origin, vec3 lightPos) {
    vec3 toL = lightPos - origin;
    float dist = length(toL);
    if (dist < 1e-5) return 1.0;
    vec3 L = toL / dist;
    vec3 o = origin + normalize(fragNormal) * 0.00035;
    float t = 0.0006;
    float shadow = 1.0;
    for (int i = 0; i < 8; ++i) {
        vec3 p = o + L * t;
        if (p.y < 0.0022) { shadow = 0.15; break; }
        // Simple ceiling blocker for indoor bulbs
        if (p.y > 0.035 && p.y < 0.05) {
            if (p.x > 0.015 && p.x < 0.145 && p.z > 0.015 && p.z < 0.11)
                shadow *= 0.55;
        }
        t += 0.002 + dist * 0.02;
        if (t >= dist) break;
    }
    return clamp(shadow, 0.1, 1.0);
}

void main() {
    vec3 n = normalize(fragNormal);
    vec3 base = fragColor;
    float matId = fragMat;

    // Emissive moon / bulb voxels (mat codes)
    if (matId > 2.5) {
        // moon grid paint (cool)
        vec3 glow = mix(vec3(0.55, 0.62, 0.75), vec3(0.85, 0.9, 1.0), 0.6);
        outColor = vec4(glow, 1.0);
        return;
    }
    if (matId > 1.5) {
        // warm bulb glass/filament proxy
        vec3 glow = vec3(1.0, 0.72, 0.42) * (1.1 + 0.15 * sin(ubo.time * 6.0));
        outColor = vec4(glow, 1.0);
        return;
    }

    // Water tide paint
    if (matId > 0.5) {
        float tide = sin(fragWorldPos.x * 180.0 + ubo.time * 2.2) *
                     cos(fragWorldPos.z * 160.0 - ubo.time * 1.7);
        float foam = smoothstep(0.55, 0.95, abs(tide));
        base = mix(base, base * vec3(0.55, 0.7, 0.95), 0.4 + 0.2 * tide);
        base += vec3(0.05, 0.08, 0.12) * foam;
    }

    // Night-neutral ambient (dark, desaturated)
    float night = smoothstep(0.55, 0.8, ubo.timeOfDay);
    vec3 ambientCol = mix(vec3(0.35, 0.38, 0.42), vec3(0.05, 0.055, 0.07), night);
    float ambient = (0.12 + 0.06 * max(n.y, 0.0)) * ubo.ambientScale;

    // Moon directional (simple RT soft shadow)
    vec3 moonL = normalize(-ubo.moonDir);
    float moonNdotL = max(dot(n, moonL), 0.0);
    float moonSh = shadowRayDir(fragWorldPos, moonL, 0.08);
    vec3 moonContrib = ubo.moonColor * ubo.moonIntensity * moonNdotL * moonSh;

    // Warm bulbs (point lights + soft RT shadow)
    vec3 bulbContrib = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        float inten = ubo.bulbPos[i].w;
        if (inten <= 0.001) continue;
        vec3 lp = ubo.bulbPos[i].xyz;
        float radius = max(ubo.bulbColor[i].w, 0.01);
        vec3 toL = lp - fragWorldPos;
        float dist = length(toL);
        float att = inten / (1.0 + 80.0 * dist * dist);
        att *= smoothstep(radius, radius * 0.15, dist);
        vec3 Ld = toL / max(dist, 1e-5);
        float nd = max(dot(n, Ld), 0.0);
        float sh = shadowRayPoint(fragWorldPos, lp);
        vec3 warm = ubo.bulbColor[i].rgb;
        bulbContrib += warm * att * nd * sh;
        // soft specular
        vec3 view = normalize(ubo.camPos - fragWorldPos);
        vec3 hh = normalize(Ld + view);
        bulbContrib += warm * pow(max(dot(n, hh), 0.0), 48.0) * att * 0.25 * sh;
    }

    // Contact shadow / ground darkening
    float groundDark = smoothstep(0.0, 0.01, fragWorldPos.y) * 0.15;
    float heightAo = clamp(0.55 + fragWorldPos.y * 25.0, 0.4, 0.95);

    vec3 lit = base * (ambientCol * ambient * heightAo + moonContrib * 0.85 + bulbContrib);
    lit *= (1.0 - groundDark);

    // Neutral desat + bitcrush
    float luma = dot(lit, vec3(0.299, 0.587, 0.114));
    lit = mix(vec3(luma), lit, 0.72);
    float levels = 18.0;
    lit = floor(lit * levels + 0.5) / levels;
    lit = pow(clamp(lit, 0.0, 1.0), vec3(1.12));

    outColor = vec4(lit, 1.0);
}
"""

(sh / "voxel.vert").write_text(vert, encoding="utf-8")
(sh / "voxel.frag").write_text(frag, encoding="utf-8")
print("night shaders written")
