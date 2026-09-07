#version 450
layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragColor;
layout(location = 2) in vec3 fragWorldPos;
layout(location = 3) in float fragMat;
layout(location = 4) in vec2 fragNdc;
layout(location = 5) in float fragViewZ;

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

float hash21(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float shadowRayDir(vec3 origin, vec3 L, float maxDist, out float edge) {
    vec3 o = origin + normalize(fragNormal) * 0.0004;
    float t = 0.0008;
    float shadow = 1.0;
    edge = 0.0;
    float prev = 1.0;
    for (int i = 0; i < 4; ++i) {
        vec3 p = o + L * t;
        float occ = 1.0;
        if (p.y < 0.0025) occ = 0.12 + t * 8.0;
        else if (p.y > 0.002 && p.y < 0.006) {
            if (p.x > 0.01 && p.x < 0.15 && p.z > 0.01 && p.z < 0.12)
                occ = 0.2 + t * 5.0;
        }
        edge = max(edge, abs(prev - occ));
        prev = occ;
        shadow = min(shadow, occ);
        t += 0.0035 + t * 0.18;
        if (t > maxDist) break;
        if (shadow < 0.15) break;
    }
    edge = clamp(edge * 2.5, 0.0, 1.0);
    return clamp(shadow, 0.08, 1.0);
}

float shadowRayPoint(vec3 origin, vec3 lightPos, out float edge) {
    vec3 toL = lightPos - origin;
    float dist = length(toL);
    edge = 0.0;
    if (dist < 1e-5) return 1.0;
    vec3 L = toL / dist;
    vec3 o = origin + normalize(fragNormal) * 0.00035;
    float t = 0.0006;
    float shadow = 1.0;
    float prev = 1.0;
    for (int i = 0; i < 3; ++i) {
        vec3 p = o + L * t;
        float occ = 1.0;
        if (p.y < 0.0022) occ = 0.15;
        else if (p.y > 0.035 && p.y < 0.05) {
            if (p.x > 0.015 && p.x < 0.145 && p.z > 0.015 && p.z < 0.11)
                occ = 0.55;
        }
        edge = max(edge, abs(prev - occ));
        prev = occ;
        shadow = min(shadow, occ);
        t += 0.0025 + dist * 0.025;
        if (t >= dist) break;
        if (shadow < 0.2) break;
    }
    edge = clamp(edge * 2.0, 0.0, 1.0);
    return clamp(shadow, 0.1, 1.0);
}

void main() {
    vec3 n = normalize(fragNormal);
    vec3 base = fragColor;
    float matId = fragMat;

    // mat 4: pixel sky tiles
    if (matId > 3.5) {
        vec2 tuv = fragColor.rg;
        float elev = fragColor.b;
        vec2 cell = floor(tuv * 8.0);
        float cellN = hash21(cell + floor(fragWorldPos.xz * 40.0));

        vec3 zenith = vec3(0.02, 0.03, 0.08);
        vec3 horizon = vec3(0.08, 0.07, 0.12);
        vec3 sky = mix(horizon, zenith, smoothstep(0.0, 1.0, elev));

        vec2 fuv = fract(tuv * 8.0);
        float grid = step(0.92, max(fuv.x, fuv.y)) * 0.08;
        sky += vec3(0.04, 0.05, 0.09) * grid;

        float star = step(0.97, hash21(cell * 1.7 + 3.1)) * step(0.35, elev);
        star *= smoothstep(0.35, 0.05, length(fuv - 0.5));
        sky += vec3(0.85, 0.9, 1.0) * star * (0.55 + 0.45 * cellN);

        vec3 toMoon = normalize(-ubo.moonDir);
        vec3 viewDir = normalize(fragWorldPos - ubo.camPos);
        float moonFacing = pow(max(dot(viewDir, normalize(-toMoon)), 0.0), 8.0);
        sky += ubo.moonColor * ubo.moonIntensity * moonFacing * 0.35;
        sky += ubo.moonColor * ubo.moonIntensity * 0.04 * (0.3 + elev);

        float r = length(fragNdc);
        sky *= 1.0 - smoothstep(0.55, 1.55, r) * 0.55;

        float levels = 14.0;
        sky = floor(sky * levels + 0.5) / levels;
        outColor = vec4(sky, 1.0);
        return;
    }

    // mat 3: moon sprite light source
    if (matId > 2.5) {
        vec2 uv = fragColor.rg;
        vec2 p = uv * 2.0 - 1.0;
        float d1 = length(p);
        float disk = smoothstep(1.02, 0.78, d1);
        float d2 = length(p - vec2(0.55, 0.08));
        float cut = smoothstep(0.95, 0.62, d2);
        float crescent = clamp(disk * (1.0 - cut), 0.0, 1.0);
        float halo = smoothstep(1.45, 0.12, d1) * 0.55;
        if (crescent + halo < 0.015) discard;

        vec3 moonCore = vec3(0.88, 0.92, 1.0);
        vec3 moonEdge = vec3(0.40, 0.50, 0.72);
        vec3 glow = mix(moonEdge, moonCore, smoothstep(0.15, 0.95, crescent));
        glow *= (0.5 + 0.5 * crescent) * (0.75 + 0.55 * ubo.moonIntensity);
        glow += ubo.moonColor * halo * ubo.moonIntensity * 1.1;

        float nnoise = fract(sin(dot(uv, vec2(41.2, 27.7))) * 529.7);
        glow *= 1.0 - 0.10 * nnoise * crescent;

        float r = length(fragNdc);
        glow *= 1.0 - smoothstep(0.7, 1.5, r) * 0.25;

        float levels = 16.0;
        glow = floor(glow * levels + 0.5) / levels;
        outColor = vec4(glow, clamp(crescent + halo * 0.65, 0.0, 1.0));
        return;
    }

    if (matId > 1.5) {
        vec3 glow = vec3(1.0, 0.72, 0.42) * (1.1 + 0.15 * sin(ubo.time * 6.0));
        float r = length(fragNdc);
        glow *= 1.0 - smoothstep(0.6, 1.4, r) * 0.25;
        outColor = vec4(glow, 1.0);
        return;
    }

    if (matId > 0.5) {
        float tide = sin(fragWorldPos.x * 180.0 + ubo.time * 2.2) *
                     cos(fragWorldPos.z * 160.0 - ubo.time * 1.7);
        float foam = smoothstep(0.55, 0.95, abs(tide));
        base = mix(base, base * vec3(0.55, 0.7, 0.95), 0.4 + 0.2 * tide);
        base += vec3(0.05, 0.08, 0.12) * foam;
    }

    float night = smoothstep(0.55, 0.8, ubo.timeOfDay);
    vec3 ambientCol = mix(vec3(0.35, 0.38, 0.42), vec3(0.05, 0.055, 0.07), night);
    float ambient = (0.12 + 0.06 * max(n.y, 0.0)) * ubo.ambientScale;

    vec3 moonL = normalize(-ubo.moonDir);
    float moonNdotL = max(dot(n, moonL), 0.0);
    float moonEdgeS = 0.0;
    float moonSh = shadowRayDir(fragWorldPos, moonL, 0.08, moonEdgeS);
    vec3 moonContrib = ubo.moonColor * ubo.moonIntensity * moonNdotL * moonSh;

    vec3 bulbContrib = vec3(0.0);
    float bulbEdge = 0.0;
    vec3 viewDir = normalize(ubo.camPos - fragWorldPos);
    for (int i = 0; i < 4; ++i) {
        float inten = ubo.bulbPos[i].w;
        if (inten <= 0.001) continue;
        vec3 lp = ubo.bulbPos[i].xyz;
        float radius = max(ubo.bulbColor[i].w, 0.01);
        vec3 toL = lp - fragWorldPos;
        float dist = length(toL);
        if (dist > radius * 1.2) continue;
        float att = inten / (1.0 + 90.0 * dist * dist);
        att *= smoothstep(radius, radius * 0.12, dist);
        vec3 Ld = toL / max(dist, 1e-5);
        float nd = max(dot(n, Ld), 0.0);
        float sh = 1.0;
        if (i < 2) {
            float e = 0.0;
            sh = shadowRayPoint(fragWorldPos, lp, e);
            bulbEdge = max(bulbEdge, e);
        }
        vec3 warm = ubo.bulbColor[i].rgb;
        bulbContrib += warm * att * nd * sh;
        if (i < 2) {
            vec3 hh = normalize(Ld + viewDir);
            bulbContrib += warm * pow(max(dot(n, hh), 0.0), 32.0) * att * 0.2 * sh;
        }
    }

    float groundDark = smoothstep(0.0, 0.01, fragWorldPos.y) * 0.15;
    float heightAo = clamp(0.55 + fragWorldPos.y * 25.0, 0.4, 0.95);

    vec3 lit = base * (ambientCol * ambient * heightAo + moonContrib * 0.95 + bulbContrib);
    lit *= (1.0 - groundDark);

    float r = length(fragNdc);
    float radialVig = smoothstep(0.30, 1.35, r);
    float depthVig = clamp(fragViewZ * 0.15, 0.0, 0.35) * radialVig;
    float shadowEdgeVig = max(moonEdgeS, bulbEdge) * 0.45;
    vec2 pix = floor(fragNdc * 90.0) / 90.0;
    float pixR = length(pix);
    float pixelVig = smoothstep(0.35, 1.25, pixR);
    float vig = clamp(radialVig * 0.62 + depthVig + shadowEdgeVig * 0.35 + pixelVig * 0.28, 0.0, 0.88);
    lit *= (1.0 - vig);

    float luma = dot(lit, vec3(0.299, 0.587, 0.114));
    lit = mix(vec3(luma), lit, 0.72);
    float levels = 18.0;
    lit = floor(lit * levels + 0.5) / levels;
    lit = pow(clamp(lit, 0.0, 1.0), vec3(1.12));

    outColor = vec4(lit, 1.0);
}
