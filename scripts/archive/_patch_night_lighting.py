from pathlib import Path

path = Path(__file__).resolve().parents[1] / "src" / "main.cpp"
text = path.read_text(encoding="utf-8")

def rep(old, new, label):
    global text
    if old not in text:
        raise SystemExit(f"fail: {label}")
    text = text.replace(old, new, 1)

# FrameUBO struct
rep(
"""struct FrameUBO {
    float viewProj[16];
    float lightDir[3];
    float _pad0;
    float camPos[3];
    float time;
};""",
"""struct FrameUBO {
    float viewProj[16];
    float sunDir[3];
    float timeOfDay;       // 0..1  (night default ~0.88)
    float camPos[3];
    float time;
    float moonDir[3];
    float moonIntensity;
    float moonColor[3];
    float ambientScale;
    float bulbPos[4][4];   // xyz, intensity
    float bulbColor[4][4]; // rgb, radius
};

// Time system
static float g_timeOfDay = 0.88f; // night
static float g_timeScale = 0.0f;  // frozen night unless changed
static bool g_isNight = true;""",
"FrameUBO")

# Sky clear darker
if "clears[0].color = {{0.45f, 0.70f, 0.95f, 1.0f}}" in text:
    rep(
        "clears[0].color = {{0.45f, 0.70f, 0.95f, 1.0f}}; // sky",
        "clears[0].color = {{0.03f, 0.035f, 0.05f, 1.0f}}; // night sky",
        "sky",
    )
elif "0.45f, 0.70f, 0.95f" in text:
    text = text.replace(
        "0.45f, 0.70f, 0.95f, 1.0f",
        "0.03f, 0.035f, 0.05f, 1.0f",
        1,
    )

# Block types for moon/bulb emissive
rep(
"""    Water,        // still unit cubes; may occupy WATER_CELL multi-cell clumps
    WaterCurrent  // moving water source (same visual, current sampling)
};""",
"""    Water,        // still unit cubes; may occupy WATER_CELL multi-cell clumps
    WaterCurrent, // moving water source (same visual, current sampling)
    Moon,         // cool emissive crescent grid
    LightBulb     // warm emissive indoor bulbs
};""",
"blocks emissive")

rep(
"""    case Block::Water:
    case Block::WaterCurrent: return MaterialId::Water;
    default: return MaterialId::Air;
    }
}""",
"""    case Block::Water:
    case Block::WaterCurrent: return MaterialId::Water;
    case Block::Moon:
    case Block::LightBulb: return MaterialId::Air; // emissive, no impact mass
    default: return MaterialId::Air;
    }
}""",
"block mat")

rep(
"""    case Block::Water:        return {0.15f, 0.40f, 0.75f};
    case Block::WaterCurrent: return {0.10f, 0.55f, 0.80f};
    default:                  return {1, 0, 1};
    }
}""",
"""    case Block::Water:        return {0.12f, 0.28f, 0.42f};
    case Block::WaterCurrent: return {0.10f, 0.35f, 0.48f};
    case Block::Moon:         return {0.75f, 0.80f, 0.90f};
    case Block::LightBulb:    return {1.00f, 0.75f, 0.45f};
    default:                  return {1, 0, 1};
    }
}""",
"colors night")

# Neutral darker materials slightly
for a, b in [
    ("{0.42f, 0.28f, 0.14f}", "{0.28f, 0.20f, 0.12f}"),
    ("{0.58f, 0.58f, 0.60f}", "{0.40f, 0.40f, 0.42f}"),
    ("{0.72f, 0.76f, 0.80f}", "{0.48f, 0.50f, 0.52f}"),
    ("{0.35f, 0.12f, 0.08f}", "{0.28f, 0.10f, 0.08f}"),
    ("{0.48f, 0.30f, 0.14f}", "{0.34f, 0.22f, 0.12f}"),
]:
    text = text.replace(a, b, 1)

# emit mat codes for moon/bulb
rep(
"""                    float mat = isWaterBlock(b) ? 1.0f : 0.0f;
                    emitSharpFace(chunk.mesh, x, y, z, f, col, mat);""",
"""                    float mat = 0.0f;
                    if (isWaterBlock(b)) mat = 1.0f;
                    else if (b == Block::LightBulb) mat = 2.0f;
                    else if (b == Block::Moon) mat = 3.0f;
                    emitSharpFace(chunk.mesh, x, y, z, f, col, mat);""",
"emit mat")

# Place moon + bulbs before return chunks
rep(
"""    // 7) A few unit-voxel crates inside for material variety / targets.
    placeCrate(chunks, bx0 + 20, 2 + SLAB_THICK, bz0 + 24, 8);
    placeCrate(chunks, bx0 + 40, 2 + SLAB_THICK, bz0 + 30, 10);
    placeCrate(chunks, bx1 - 30, 2 + SLAB_THICK, bz0 + 20, 8);""",
"""    // 7) A few unit-voxel crates inside for material variety / targets.
    placeCrate(chunks, bx0 + 20, 2 + SLAB_THICK, bz0 + 24, 8);
    placeCrate(chunks, bx0 + 40, 2 + SLAB_THICK, bz0 + 30, 10);
    placeCrate(chunks, bx1 - 30, 2 + SLAB_THICK, bz0 + 20, 8);

    // 7b) Warm light bulbs inside (unit voxels hanging near roof girders).
    {
        const int by = roofY - 2;
        auto bulb = [&](int x, int z) {
            setWorldBlock(chunks, x, by, z, Block::LightBulb);
            setWorldBlock(chunks, x, by - 1, z, Block::LightBulb);
            // small cage
            setWorldBlock(chunks, x + 1, by, z, Block::Girder);
            setWorldBlock(chunks, x - 1, by, z, Block::Girder);
        };
        bulb((bx0 + bx1) / 2, (bz0 + bz1) / 2);
        bulb(bx0 + 28, bz0 + 28);
        bulb(bx1 - 28, bz0 + 32);
        bulb((bx0 + bx1) / 2, bz1 - 18);
    }

    // 7c) Grid-painted crescent moon high in the night sky (unit voxels only).
    {
        const int mx = WORLD_W / 2 + 20;
        const int mz = 8;
        const int my = WORLD_H - 8;
        const int R = 10;
        const int rInner = 7;
        const int ox = 4; // crescent offset
        for (int dz = -R; dz <= R; ++dz) {
            for (int dy = -R; dy <= R; ++dy) {
                int d2 = dz * dz + dy * dy;
                if (d2 > R * R || d2 < 3) continue;
                // subtract offset disk for crescent
                int ez = dz - ox;
                int e2 = ez * ez + dy * dy;
                if (e2 < rInner * rInner) continue;
                setWorldBlock(chunks, mx, my + dy, mz + dz, Block::Moon);
                // thicken one unit for readable grid paint
                if ((dz + dy) & 1)
                    setWorldBlock(chunks, mx + 1, my + dy, mz + dz, Block::Moon);
            }
        }
    }""",
"moon bulbs")

# updateUBO body
rep(
"""static void updateUBO(uint32_t frameIndex, float timeSec) {
    Vec3 eye = g_camPos;
    Vec3 center = g_camPos + cameraForward();
    float aspect = g_extent.height > 0
                       ? static_cast<float>(g_extent.width) / static_cast<float>(g_extent.height)
                       : 1.0f;
    Mat4 proj = Mat4::perspective(70.0f * static_cast<float>(M_PI) / 180.0f, aspect, 0.0005f, 5.0f);
    Mat4 view = Mat4::lookAt(eye, center, {0, 1, 0});
    Mat4 vp = proj * view;

    FrameUBO ubo{};
    std::memcpy(ubo.viewProj, vp.m, sizeof(vp.m));
    ubo.lightDir[0] = -0.45f;
    ubo.lightDir[1] = -1.0f;
    ubo.lightDir[2] = -0.35f;
    ubo.camPos[0] = eye.x;
    ubo.camPos[1] = eye.y;
    ubo.camPos[2] = eye.z;
    ubo.time = timeSec;
    std::memcpy(g_uboMapped[frameIndex], &ubo, sizeof(ubo));
}""",
"""static void updateUBO(uint32_t frameIndex, float timeSec) {
    Vec3 eye = g_camPos;
    Vec3 center = g_camPos + cameraForward();
    float aspect = g_extent.height > 0
                       ? static_cast<float>(g_extent.width) / static_cast<float>(g_extent.height)
                       : 1.0f;
    Mat4 proj = Mat4::perspective(70.0f * static_cast<float>(M_PI) / 180.0f, aspect, 0.0005f, 5.0f);
    Mat4 view = Mat4::lookAt(eye, center, {0, 1, 0});
    Mat4 vp = proj * view;

    if (g_timeScale > 0.0f)
        g_timeOfDay = std::fmod(g_timeOfDay + dt_safe_time(timeSec) * g_timeScale, 1.0f);
    g_isNight = g_timeOfDay > 0.7f || g_timeOfDay < 0.25f;

    FrameUBO ubo{};
    std::memcpy(ubo.viewProj, vp.m, sizeof(vp.m));
    // sun mostly off at night
    ubo.sunDir[0] = 0.2f; ubo.sunDir[1] = -1.0f; ubo.sunDir[2] = 0.15f;
    ubo.timeOfDay = g_timeOfDay;
    ubo.camPos[0] = eye.x; ubo.camPos[1] = eye.y; ubo.camPos[2] = eye.z;
    ubo.time = timeSec;
    // moon light direction (from crescent toward scene)
    ubo.moonDir[0] = 0.25f; ubo.moonDir[1] = -0.85f; ubo.moonDir[2] = 0.45f;
    ubo.moonIntensity = g_isNight ? 0.55f : 0.05f;
    ubo.moonColor[0] = 0.55f; ubo.moonColor[1] = 0.62f; ubo.moonColor[2] = 0.78f;
    ubo.ambientScale = g_isNight ? 0.65f : 1.0f;

    // Warm bulbs (world-space); match warehouse placements roughly
    auto setBulb = [&](int i, float x, float y, float z, float inten, float r, float g, float b, float radius) {
        ubo.bulbPos[i][0] = x; ubo.bulbPos[i][1] = y; ubo.bulbPos[i][2] = z; ubo.bulbPos[i][3] = inten;
        ubo.bulbColor[i][0] = r; ubo.bulbColor[i][1] = g; ubo.bulbColor[i][2] = b; ubo.bulbColor[i][3] = radius;
    };
    // Convert grid guesses to world using VOXEL_SIZE
    setBulb(0, WORLD_W * 0.5f * VOXEL_SIZE, 0.040f, WORLD_D * 0.35f * VOXEL_SIZE, 1.8f, 1.0f, 0.72f, 0.42f, 0.09f);
    setBulb(1, 0.038f, 0.040f, 0.038f, 1.4f, 1.0f, 0.7f, 0.4f, 0.07f);
    setBulb(2, 0.12f, 0.040f, 0.042f, 1.4f, 1.0f, 0.68f, 0.38f, 0.07f);
    setBulb(3, WORLD_W * 0.5f * VOXEL_SIZE, 0.040f, 0.095f, 1.2f, 1.0f, 0.75f, 0.45f, 0.08f);

    std::memcpy(g_uboMapped[frameIndex], &ubo, sizeof(ubo));
}

static float dt_safe_time(float timeSec) {
    static float prev = timeSec;
    float d = timeSec - prev;
    prev = timeSec;
    if (d < 0.0f || d > 0.25f) d = 0.016f;
    return d;
}""",
"updateUBO")

# Fix order: dt_safe_time used before definition - move helper above updateUBO
# The replacement put dt_safe_time after updateUBO which calls it - need forward or reorder.
text = text.replace(
"""    if (g_timeScale > 0.0f)
        g_timeOfDay = std::fmod(g_timeOfDay + dt_safe_time(timeSec) * g_timeScale, 1.0f);
    g_isNight = g_timeOfDay > 0.7f || g_timeOfDay < 0.25f;

    FrameUBO ubo{};""",
"""    g_isNight = g_timeOfDay > 0.7f || g_timeOfDay < 0.25f;

    FrameUBO ubo{};"""
)
# remove trailing dt_safe_time function if present and don't need it
text = text.replace(
"""
static float dt_safe_time(float timeSec) {
    static float prev = timeSec;
    float d = timeSec - prev;
    prev = timeSec;
    if (d < 0.0f || d > 0.25f) d = 0.016f;
    return d;
}
""",
"\n"
)

# smoke night fields
if "render_scale=" in text and "timeOfDay" not in text:
    text = text.replace(
        "<< \"\\nrender_scale=\" << RENDER_SCALE << \"\\n\";",
        "<< \"\\nrender_scale=\" << RENDER_SCALE"
        "<< \"\\ntimeOfDay=\" << g_timeOfDay"
        "<< \"\\nnight=\" << (g_isNight ? 1 : 0) << \"\\n\";",
    )

path.write_text(text, encoding="utf-8")
print("night lighting patched", path.stat().st_size)
print("ubo sizeof note: ensure host/gpu match")
