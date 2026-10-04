#version 450
#extension GL_GOOGLE_include_directive : require
#include "render_class.glsl"
layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragColor;
layout(location = 2) in vec3 fragWorldPos;
layout(location = 3) in float fragMat;
layout(location = 4) in vec2 fragNdc;
layout(location = 5) in float fragViewZ;
layout(location = 6) flat in vec2 fragTexId;
layout(location = 7) in float fragShade;
layout(set = 0, binding = 1) uniform sampler2DArray uTex;
// Occupancy of the cells this client was sent (1 = opaque). Built by the view
// from its chunk snapshots, so a hidden cell can never cast a visible shadow.
layout(set = 0, binding = 2) uniform sampler3D uOcc;
// Light sources (environment layer): two vec4 per light,
// (position.xyz, intensity) then (colour.rgb, radius).
layout(std430, set = 0, binding = 3) readonly buffer LightList { vec4 lights[]; };

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
    float fisheyeScale; // visuals menu: lens curve multiplier (1 = original)
    float banding;      // visuals menu: colour-step multiplier (1 = original, 0 = off)
    float uboPad0;
    float uboPad1;
    vec4 texParams[16]; // per texture layer: tileCells, tint, coverage, maskFromLuma
    vec4 texGlobal;     // enabled, strength, scale, unused
    vec4 occDims;       // occupancy volume W, H, D (cells), shadows enabled
    vec4 shadowParams;  // max cells crossed, 1, unused, unused
    vec4 lightInfo;     // light count (storage buffer below), unused x3
} ubo;

// World colour banding, scaled by the visuals menu. banding = 1 gives the
// original steps exactly; 0 turns stepping off; higher is coarser.
// Triplanar sample of one texture layer in world space: a tile spans
// `tileWorld` units, so a texture covers many unit cells rather than repeating
// per cube.
vec3 triplanar(int layer, vec3 p, vec3 n, float tileWorld) {
    vec3 w = pow(abs(n), vec3(4.0));
    w /= (w.x + w.y + w.z + 1e-5);
    vec3 cx = texture(uTex, vec3(p.zy / tileWorld, float(layer))).rgb;
    vec3 cy = texture(uTex, vec3(p.xz / tileWorld, float(layer))).rgb;
    vec3 cz = texture(uTex, vec3(p.xy / tileWorld, float(layer))).rgb;
    return cx * w.x + cy * w.y + cz * w.z;
}

// Surface colour for a textured world cell (src/textures.hpp, "Blending").
// `shadedBase` is the mesh colour, already multiplied by face shade and AO
// (fragShade); the texture replaces/blends the unshaded colour and the shade
// is applied again on top.
vec3 texturedBase(vec3 shadedBase) {
    if (fragTexId.x < 0.5 || ubo.texGlobal.x < 0.5) return shadedBase;
    int layer = int(fragTexId.x + 0.5) - 1;
    vec4 prm = ubo.texParams[layer];
    float tileWorld = max(prm.x * 0.001 * ubo.texGlobal.z, 1e-4);
    vec3 t = triplanar(layer, fragWorldPos, normalize(fragNormal), tileWorld);
    float shade = max(fragShade, 1e-3);
    vec3 surface = t;
    if (fragTexId.y > 0.5) {
        vec3 paint = shadedBase / shade;
        // Filter: the texture pulled toward the paint (x2 so mid grey is neutral).
        vec3 filtered = mix(t, clamp(t * paint * 2.0, 0.0, 1.0), prm.y);
        // Base: the paint shows through where the texture does not cover it;
        // with maskFromLuma, the texture's bright patches reveal the paint.
        float luma = dot(t, vec3(0.299, 0.587, 0.114));
        float cover = prm.z * mix(1.0, 1.0 - smoothstep(0.55, 0.85, luma), prm.w);
        surface = mix(paint, filtered, cover);
    }
    return mix(shadedBase, surface * shade, ubo.texGlobal.y);
}

vec3 bandq(vec3 c, float levels) {
    if (ubo.banding <= 0.0) return c;
    float l = levels / ubo.banding;
    return floor(c * l + 0.5) / l;
}

layout(location = 0) out vec4 outColor;

float hash21(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

// Trace from just off the surface toward a light through the occupancy volume,
// visiting every cell the ray crosses (Amanatides & Woo DDA, as the hitscan
// does). A fixed-step march would slip between cubes that touch only at a
// corner and leak light through a checkered roof; this cannot. A hit darkens
// the light, less when the occluder is far (a cheap stand-in for a penumbra).
// Returns 1 when unshadowed.
float marchShadow(vec3 origin, vec3 L, float maxDist) {
    if (ubo.occDims.w < 0.5) return 1.0;
    const float vs = 0.001;
    vec3 o = (origin + normalize(fragNormal) * 0.0007) / vs; // in cells
    vec3 d = L;
    ivec3 c = ivec3(floor(o));
    ivec3 stp = ivec3(sign(d));
    vec3 invAbs = 1.0 / max(abs(d), vec3(1e-6));
    vec3 next = vec3(c) + max(vec3(stp), vec3(0.0));       // boundary ahead on each axis
    vec3 tMax = (next - o) / d;
    tMax = mix(tMax, vec3(1e30), lessThan(abs(d), vec3(1e-6)));
    vec3 tDelta = invAbs;
    ivec3 dims = ivec3(ubo.occDims.xyz);
    float maxCells = maxDist / vs;
    int budget = int(ubo.shadowParams.x * max(ubo.shadowParams.y, 0.25));
    float t = 0.0;
    for (int i = 0; i < 512; ++i) {
        if (i >= budget || t > maxCells) break;
        if (all(greaterThanEqual(c, ivec3(0))) && all(lessThan(c, dims)) &&
            texelFetch(uOcc, c, 0).r > 0.5)
            return mix(0.08, 0.45, clamp(t / maxCells, 0.0, 1.0));
        // Step to the nearest boundary.
        if (tMax.x < tMax.y && tMax.x < tMax.z) { t = tMax.x; tMax.x += tDelta.x; c.x += stp.x; }
        else if (tMax.y < tMax.z)               { t = tMax.y; tMax.y += tDelta.y; c.y += stp.y; }
        else                                    { t = tMax.z; tMax.z += tDelta.z; c.z += stp.z; }
    }
    return 1.0;
}

float shadowRayDir(vec3 origin, vec3 L, float maxDist, out float edge) {
    edge = 0.0;
    return marchShadow(origin, L, maxDist);
}

float shadowRayPoint(vec3 origin, vec3 lightPos, out float edge) {
    edge = 0.0;
    vec3 toL = lightPos - origin;
    float dist = length(toL);
    if (dist < 1e-5) return 1.0;
    // Stop a cell short of the light so the fixture itself never shadows it.
    return marchShadow(origin, toL / dist, max(dist - 0.0015, 0.0));
}

void applyFireOverlay(inout vec3 lit) {
    float flash = ubo.muzzleFlash;
    float border = ubo.fireOverlay;
    float dmgFlash = ubo.damageFlash;
    float hpTint = ubo.healthTint;

    float r = length(fragNdc);

    if (flash >= 0.001 || border >= 0.001) {
        // Center bloom + warm fill
        float center = 1.0 - smoothstep(0.0, 0.72, r);
        lit += vec3(1.0, 0.78, 0.32) * flash * (0.18 + 0.55 * center);
        // Frame burn / corner vignette pulse
        float edgeX = smoothstep(0.72, 1.08, abs(fragNdc.x));
        float edgeY = smoothstep(0.68, 1.05, abs(fragNdc.y));
        float frame = max(edgeX, edgeY);
        float corner = edgeX * edgeY;
        lit += vec3(1.0, 0.42, 0.12) * border * (frame * 0.55 + corner * 0.85);
        // Slight desat crush on hard flash for "shutter" feel
        float luma = dot(lit, vec3(0.299, 0.587, 0.114));
        lit = mix(lit, vec3(luma) * vec3(1.05, 0.95, 0.85), flash * 0.12);
    }

    // Combat feel: rapid crimson wash on impact damage
    if (dmgFlash > 0.001) {
        float bloom = 1.0 - smoothstep(0.0, 0.95, r);
        lit = mix(lit, vec3(0.85, 0.04, 0.04), dmgFlash * 0.45);
        lit += vec3(0.9, 0.08, 0.08) * dmgFlash * (0.15 + 0.35 * bloom);
    }

    // Low health: dark red vignette pulse around screen border
    if (hpTint > 0.001) {
        float edge = smoothstep(0.55, 1.15, r);
        float pulse = 0.8 + 0.2 * sin(ubo.time * 6.0);
        lit = mix(lit, vec3(0.55, 0.02, 0.02), hpTint * edge * pulse * 0.65);
    }
}

void main() {
    vec3 n = normalize(fragNormal);
    vec3 base = fragColor;
    // Render class of this fragment (src/render_class.hpp). Each branch below
    // tests one class exactly, so their order does not matter. Every branch
    // except Water returns; Water adjusts the base colour and then takes the
    // shared World lighting path at the bottom.
    int rc = renderClass(fragMat);

    // World pickups are real world objects, not overlay UI: they take the World
    // lighting path (shadow rays, height AO, vignette) and sit in the scene.
    if (rc == RC_WORLD_PICKUP) rc = RC_WORLD;

    // Surface layer: textured world cells (pickups carry no texture id).
    if (rc == RC_WORLD) base = texturedBase(base);

    // Inventory lattice unit cubes (RULES.md rule 12). Deliberately skips the
    // World path's world-space shadow rays, world-Y height AO and vignette —
    // none of which mean anything on a lattice parented to the camera. The
    // vertex stage also skips the fisheye for this class, so this is a clean 3D
    // projection. Flat face shading plus a light bitcrush to match the look.
    if (rc == RC_INVENTORY_LATTICE) {
        vec3 key = normalize(vec3(0.42, 0.78, 0.30));
        float ndl = max(dot(n, key), 0.0);
        float fill = 0.42 + 0.58 * max(n.y, 0.0);
        vec3 lit = base * (0.34 + 0.52 * ndl) * fill;
        // Emphasise the cube silhouette so single cells stay legible.
        float face = pow(max(abs(n.x), max(abs(n.y), abs(n.z))), 8.0);
        lit += base * face * 0.14;
        float levels = 20.0;
        lit = bandq(lit, levels);
        applyFireOverlay(lit);
        outColor = vec4(clamp(lit, 0.0, 1.0), 1.0);
        return;
    }

    // Muzzle flash cubes (emissive, no lighting)
    if (rc == RC_MUZZLE) {
        vec3 glow = base * (1.4 + 0.6 * ubo.muzzleFlash);
        float pulse = 0.85 + 0.15 * sin(ubo.time * 90.0);
        glow *= pulse;
        float levels = 20.0;
        glow = bandq(glow, levels);
        vec3 outRgb = glow;
        applyFireOverlay(outRgb);
        outColor = vec4(outRgb, 1.0);
        return;
    }

    // mat 5: cubic debris chips — lit + emissive lift so sub-voxels read clearly
    if (rc == RC_DEBRIS) {
        float night = smoothstep(0.55, 0.8, ubo.timeOfDay);
        vec3 ambientCol = mix(vec3(0.35, 0.38, 0.42), vec3(0.08, 0.09, 0.11), night);
        float ambient = (0.28 + 0.14 * max(n.y, 0.0)) * ubo.ambientScale;
        vec3 moonL = normalize(-ubo.moonDir);
        float moonNdotL = max(dot(n, moonL), 0.0);
        vec3 moonContrib = ubo.moonColor * ubo.moonIntensity * moonNdotL * 1.15;
        vec3 lit = base * (ambientCol * ambient + moonContrib) + base * 0.22;
        // Face bevel: emphasize cube silhouette
        float face = pow(max(abs(n.x), max(abs(n.y), abs(n.z))), 4.0);
        lit += base * face * 0.18;
        float r = length(fragNdc);
        lit *= 1.0 - smoothstep(0.55, 1.45, r) * 0.25;
        float levels = 18.0;
        lit = bandq(lit, levels);
        applyFireOverlay(lit);
        outColor = vec4(clamp(lit, 0.0, 1.0), 1.0);
        return;
    }

    // mat 4: pixel sky tiles
    if (rc == RC_SKY) {
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
        sky = bandq(sky, levels);
        applyFireOverlay(sky);
        outColor = vec4(sky, 1.0);
        return;
    }

    // mat 3: moon sprite light source
    if (rc == RC_MOON) {
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
        glow = bandq(glow, levels);
        applyFireOverlay(glow);
        outColor = vec4(glow, clamp(crescent + halo * 0.65, 0.0, 1.0));
        return;
    }

    if (rc == RC_BULB) {
        vec3 glow = vec3(1.0, 0.72, 0.42) * (1.1 + 0.15 * sin(ubo.time * 6.0));
        float r = length(fragNdc);
        glow *= 1.0 - smoothstep(0.6, 1.4, r) * 0.25;
        applyFireOverlay(glow);
        outColor = vec4(glow, 1.0);
        return;
    }

    if (rc == RC_WATER) {
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
    int lightCount = int(ubo.lightInfo.x);
    for (int i = 0; i < lightCount; ++i) {
        vec4 lpi = lights[i * 2];
        vec4 lcr = lights[i * 2 + 1];
        float inten = lpi.w;
        if (inten <= 0.001) continue;
        vec3 lp = lpi.xyz;
        float radius = max(lcr.w, 0.01);
        vec3 toL = lp - fragWorldPos;
        float dist = length(toL);
        if (dist > radius * 1.2) continue;
        float att = inten / (1.0 + 90.0 * dist * dist);
        att *= smoothstep(radius, radius * 0.12, dist);
        vec3 Ld = toL / max(dist, 1e-5);
        float nd = max(dot(n, Ld), 0.0);
        // Every light within range is shadowed through the occupancy volume.
        float e = 0.0;
        float sh = (nd > 0.0) ? shadowRayPoint(fragWorldPos, lp, e) : 1.0;
        bulbEdge = max(bulbEdge, e);
        vec3 warm = lcr.rgb;
        bulbContrib += warm * att * nd * sh;
        vec3 hh = normalize(Ld + viewDir);
        bulbContrib += warm * pow(max(dot(n, hh), 0.0), 32.0) * att * 0.2 * sh;
    }

    // Muzzle as transient point light near camera
    if (ubo.muzzleFlash > 0.02) {
        vec3 mp = ubo.camPos;
        vec3 toM = mp - fragWorldPos;
        float md = length(toM);
        float matt = ubo.muzzleFlash * 2.4 / (1.0 + 120.0 * md * md);
        matt *= smoothstep(0.08, 0.01, md);
        float mnd = max(dot(n, normalize(toM + vec3(0.0, 0.001, 0.0))), 0.0);
        bulbContrib += vec3(1.0, 0.72, 0.3) * matt * (0.35 + 0.65 * mnd);
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
    lit = bandq(lit, levels);
    lit = pow(clamp(lit, 0.0, 1.0), vec3(1.12));

    applyFireOverlay(lit);
    outColor = vec4(lit, 1.0);
}
