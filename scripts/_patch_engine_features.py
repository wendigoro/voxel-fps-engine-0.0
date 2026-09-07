"""Patch main.cpp for water/current, character units, downscale render."""
from pathlib import Path
import re

path = Path(__file__).resolve().parents[1] / "src" / "main.cpp"
text = path.read_text(encoding="utf-8")

def must_replace(old: str, new: str, label: str):
    global text
    if old not in text:
        raise SystemExit(f"patch failed: {label}")
    text = text.replace(old, new, 1)

# --- Vertex gains mat channel ---
must_replace(
"""struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float cr, cg, cb;
};""",
"""struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float cr, cg, cb;
    float mat; // 0 solid, 1 water (shader tide)
};""",
"vertex")

# --- Internal downscale for perf ---
must_replace(
"""static constexpr int WIDTH = 1280;
static constexpr int HEIGHT = 720;
static constexpr int MAX_FRAMES = 2;""",
"""static constexpr int WIDTH = 1280;
static constexpr int HEIGHT = 720;
static constexpr int MAX_FRAMES = 2;
// Internal 3D render scale (downscale for fill-rate). Presented upscaled with bitcrush look in shader.
static constexpr float RENDER_SCALE = 0.5f;
static constexpr int INTERNAL_W = 640;  // WIDTH * 0.5
static constexpr int INTERNAL_H = 360;  // HEIGHT * 0.5""",
"render scale")

# --- Extend map for river ---
must_replace(
"""static constexpr int CHUNKS_X = 5;            // warehouse footprint
static constexpr int CHUNKS_Y = 2;            // height for walls/roof girders
static constexpr int CHUNKS_Z = 4;""",
"""static constexpr int CHUNKS_X = 6;            // warehouse + river bank
static constexpr int CHUNKS_Y = 2;            // height for walls/roof girders
static constexpr int CHUNKS_Z = 5;            // extended depth for river slice""",
"chunks")

# --- Player / water globals after firePressed ---
must_replace(
"""static bool g_meshDirty = false;
static bool g_firePressed = false;""",
"""static bool g_meshDirty = false;
static bool g_firePressed = false;

// Player character as unit-voxel body on the cubic grid (impact/current sampling).
static int g_playerGX = 0, g_playerGY = 0, g_playerGZ = 0;
static float g_playerBaseWeight = 1.0f;
static float g_playerWeight = 1.0f;
static float g_currentForce = 0.0f;
static Vec3 g_currentDir = {1.0f, 0.0f, 0.0f}; // river flows +X
static bool g_currentTriggered = false;
static bool g_fullySubmerged = false;
static int g_touchingWaterUnits = 0;
static int g_characterUnitCount = 0;""",
"player globals")

# --- Blocks + materials ---
must_replace(
"""enum class Block : uint8_t {
    Air = 0,
    Dirt,
    Concrete,
    SheetMetal,
    Girder,
    Wood,          // optional crate fill
    WoodDark
};

static MaterialId blockMaterial(Block b) {
    switch (b) {
    case Block::Dirt: return MaterialId::Dirt;
    case Block::Concrete: return MaterialId::Concrete;
    case Block::SheetMetal: return MaterialId::SheetMetal;
    case Block::Girder: return MaterialId::Girder;
    case Block::Wood: return MaterialId::Wood;
    case Block::WoodDark: return MaterialId::BushBranch;
    default: return MaterialId::Air;
    }
}""",
"""enum class Block : uint8_t {
    Air = 0,
    Dirt,
    Concrete,
    SheetMetal,
    Girder,
    Wood,
    WoodDark,
    Water,        // still unit cubes; may occupy WATER_CELL multi-cell clumps
    WaterCurrent  // moving water source (same visual, current sampling)
};

// Water is painted as slightly larger *logical* cells (2x2x2 unit cubes) for volume/tide.
static constexpr int WATER_CELL = 2;

static MaterialId blockMaterial(Block b) {
    switch (b) {
    case Block::Dirt: return MaterialId::Dirt;
    case Block::Concrete: return MaterialId::Concrete;
    case Block::SheetMetal: return MaterialId::SheetMetal;
    case Block::Girder: return MaterialId::Girder;
    case Block::Wood: return MaterialId::Wood;
    case Block::WoodDark: return MaterialId::BushBranch;
    case Block::Water:
    case Block::WaterCurrent: return MaterialId::Water;
    default: return MaterialId::Air;
    }
}

static bool isWaterBlock(Block b) {
    return b == Block::Water || b == Block::WaterCurrent;
}""",
"blocks")

must_replace(
"""static Vec3 blockColor(Block b) {
    switch (b) {
    case Block::Dirt:        return {0.42f, 0.28f, 0.14f};
    case Block::Concrete:    return {0.58f, 0.58f, 0.60f};
    case Block::SheetMetal:  return {0.72f, 0.76f, 0.80f};
    case Block::Girder:      return {0.35f, 0.12f, 0.08f}; // red oxide steel
    case Block::Wood:        return {0.48f, 0.30f, 0.14f};
    case Block::WoodDark:    return {0.32f, 0.18f, 0.08f};
    default:                 return {1, 0, 1};
    }
}""",
"""static Vec3 blockColor(Block b) {
    switch (b) {
    case Block::Dirt:         return {0.42f, 0.28f, 0.14f};
    case Block::Concrete:     return {0.58f, 0.58f, 0.60f};
    case Block::SheetMetal:   return {0.72f, 0.76f, 0.80f};
    case Block::Girder:       return {0.35f, 0.12f, 0.08f};
    case Block::Wood:         return {0.48f, 0.30f, 0.14f};
    case Block::WoodDark:     return {0.32f, 0.18f, 0.08f};
    case Block::Water:        return {0.15f, 0.40f, 0.75f};
    case Block::WaterCurrent: return {0.10f, 0.55f, 0.80f};
    default:                  return {1, 0, 1};
    }
}""",
"colors")

# --- emitSharpFace signature and body mat ---
must_replace(
"""static void emitSharpFace(std::vector<Vertex>& out, int ix, int iy, int iz,
                          int face, const Vec3& color) {""",
"""static void emitSharpFace(std::vector<Vertex>& out, int ix, int iy, int iz,
                          int face, const Vec3& color, float mat = 0.0f) {""",
"emit sig")

must_replace(
"""        out.push_back(Vertex{
            ox + p[0] * VOXEL_SIZE,
            oy + p[1] * VOXEL_SIZE,
            oz + p[2] * VOXEL_SIZE,
            N[face][0], N[face][1], N[face][2],
            c.x, c.y, c.z
        });""",
"""        out.push_back(Vertex{
            ox + p[0] * VOXEL_SIZE,
            oy + p[1] * VOXEL_SIZE,
            oz + p[2] * VOXEL_SIZE,
            N[face][0], N[face][1], N[face][2],
            c.x, c.y, c.z,
            mat
        });""",
"emit push")

must_replace(
"""                    if (nb != Block::Air) continue;
                    emitSharpFace(chunk.mesh, x, y, z, f, col);""",
"""                    // Water is translucent-ish occupancy: still blocks solid faces; water-water culls.
                    if (nb != Block::Air) {
                        if (!(isWaterBlock(b) && !isWaterBlock(nb))) {
                            if (!isWaterBlock(b) || isWaterBlock(nb)) continue;
                        } else {
                            // solid against water: keep face
                        }
                    }
                    if (isWaterBlock(b) && isWaterBlock(nb)) continue;
                    if (!isWaterBlock(b) && nb != Block::Air && !isWaterBlock(nb)) continue;
                    if (!isWaterBlock(b) && nb != Block::Air) continue;
                    float mat = isWaterBlock(b) ? 1.0f : 0.0f;
                    emitSharpFace(chunk.mesh, x, y, z, f, col, mat);""",
"mesh expose")

# Simplify mesh expose - the above is messy. Fix with cleaner logic:
text = text.replace(
"""                    // Water is translucent-ish occupancy: still blocks solid faces; water-water culls.
                    if (nb != Block::Air) {
                        if (!(isWaterBlock(b) && !isWaterBlock(nb))) {
                            if (!isWaterBlock(b) || isWaterBlock(nb)) continue;
                        } else {
                            // solid against water: keep face
                        }
                    }
                    if (isWaterBlock(b) && isWaterBlock(nb)) continue;
                    if (!isWaterBlock(b) && nb != Block::Air && !isWaterBlock(nb)) continue;
                    if (!isWaterBlock(b) && nb != Block::Air) continue;
                    float mat = isWaterBlock(b) ? 1.0f : 0.0f;
                    emitSharpFace(chunk.mesh, x, y, z, f, col, mat);""",
"""                    bool expose = false;
                    if (isWaterBlock(b)) {
                        expose = (nb == Block::Air) || (!isWaterBlock(nb) && nb != Block::Air);
                        // show water surface against air only for clearer tide paint
                        expose = (nb == Block::Air);
                    } else {
                        expose = (nb == Block::Air) || isWaterBlock(nb);
                    }
                    if (!expose) continue;
                    float mat = isWaterBlock(b) ? 1.0f : 0.0f;
                    emitSharpFace(chunk.mesh, x, y, z, f, col, mat);"""
)

# --- pipeline vertex attrs ---
must_replace(
"""    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, px)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, nx)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, cr)};

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions = attrs;""",
"""    VkVertexInputAttributeDescription attrs[4]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, px)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, nx)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, cr)};
    attrs[3] = {3, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vertex, mat)};

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 4;
    vi.pVertexAttributeDescriptions = attrs;""",
"pipeline attrs")

# Fix double attrs declaration if old attrs[3] remains
text = text.replace(
"""    VkVertexInputAttributeDescription attrs[3]{};
    VkVertexInputAttributeDescription attrs[4]{};""",
"""    VkVertexInputAttributeDescription attrs[4]{};"""
)

# --- warehouse river extension ---
must_replace(
"""    // 7) A few unit-voxel crates inside for material variety / targets.
    placeCrate(chunks, bx0 + 20, 2 + SLAB_THICK, bz0 + 24, 8);
    placeCrate(chunks, bx0 + 40, 2 + SLAB_THICK, bz0 + 30, 10);
    placeCrate(chunks, bx1 - 30, 2 + SLAB_THICK, bz0 + 20, 8);

    return chunks;
}""",
"""    // 7) A few unit-voxel crates inside for material variety / targets.
    placeCrate(chunks, bx0 + 20, 2 + SLAB_THICK, bz0 + 24, 8);
    placeCrate(chunks, bx0 + 40, 2 + SLAB_THICK, bz0 + 30, 10);
    placeCrate(chunks, bx1 - 30, 2 + SLAB_THICK, bz0 + 20, 8);

    // 8) River slice beyond +Z apron: WATER_CELL (2x2) unit cubes, deep channel with current.
    // Dirt bank extends; carve channel and fill water/current.
    {
        const int riverZ0 = bz1 + 2;
        const int riverZ1 = WORLD_D - 3;
        const int riverX0 = 8;
        const int riverX1 = WORLD_W - 9;
        // Ensure dirt banks around river
        fillBox(chunks, 0, 0, riverZ0 - 2, WORLD_W - 1, 1, WORLD_D - 1, Block::Dirt);
        // Deep channel center (unit voxels stacked)
        const int surfaceY = 4;
        const int deepY0 = 0;
        const int deepY1 = surfaceY; // depth includes surface
        for (int z = riverZ0; z <= riverZ1; ++z) {
            for (int x = riverX0; x <= riverX1; ++x) {
                // banks stay dirt; channel interior
                bool channel = (x > riverX0 + 4 && x < riverX1 - 4);
                if (!channel) continue;
                // deeper mid-stream trench
                int localDeep = surfaceY;
                int mid = (riverX0 + riverX1) / 2;
                int dist = std::abs(x - mid);
                if (dist < 8) localDeep = surfaceY + 6;      // deepest
                else if (dist < 16) localDeep = surfaceY + 3;
                for (int y = 0; y <= localDeep && y < WORLD_H; ++y) {
                    // place as WATER_CELL clumps: still unit cubes on grid
                    Block wb = (dist < 10 && y <= localDeep) ? Block::WaterCurrent : Block::Water;
                    setWorldBlock(chunks, x, y, z, wb);
                    // thicken visually with adjacent unit cells (larger water voxels)
                    if ((x % WATER_CELL) == 0 && (z % WATER_CELL) == 0) {
                        for (int dz = 0; dz < WATER_CELL; ++dz)
                            for (int dx = 0; dx < WATER_CELL; ++dx)
                                if (dx || dz) setWorldBlock(chunks, x + dx, y, z + dz, wb);
                    }
                }
            }
        }
    }

    return chunks;
}""",
"river")

# --- Character + current functions before updateCamera ---
must_replace(
"""static Vec3 cameraForward() {""",
"""// Character body as unit voxels relative to feet grid position (for submersion tests).
static const int kCharUnits[][3] = {
    // legs
    {0,0,0},{1,0,0},{0,1,0},{1,1,0}, {3,0,0},{4,0,0},{3,1,0},{4,1,0},
    // torso
    {0,2,0},{1,2,0},{2,2,0},{3,2,0},{4,2,0},
    {0,3,0},{1,3,0},{2,3,0},{3,3,0},{4,3,0},
    {0,4,0},{1,4,0},{2,4,0},{3,4,0},{4,4,0},
    // head
    {1,5,0},{2,5,0},{3,5,0},{1,6,0},{2,6,0},{3,6,0},
};
static constexpr int kCharUnitCount = sizeof(kCharUnits) / sizeof(kCharUnits[0]);

struct SubmersionInfo {
    int touching = 0;
    int total = kCharUnitCount;
    int currentTouching = 0;
    bool anyCurrent = false;
    bool fullySubmerged = false;
};

static SubmersionInfo sampleCharacterWater(const std::vector<Chunk>& chunks, int gx, int gy, int gz) {
    SubmersionInfo info;
    info.total = kCharUnitCount;
    for (int i = 0; i < kCharUnitCount; ++i) {
        int x = gx + kCharUnits[i][0];
        int y = gy + kCharUnits[i][1];
        int z = gz + kCharUnits[i][2];
        Block b = getWorldBlock(chunks, x, y, z);
        if (isWaterBlock(b)) {
            info.touching++;
            if (b == Block::WaterCurrent) {
                info.currentTouching++;
                info.anyCurrent = true;
            }
        }
    }
    info.fullySubmerged = (info.touching >= info.total);
    return info;
}

// Basic current function: how many character units touch moving water.
static void updatePlayerCurrentAndWeight(float dt) {
    if (!g_chunks) return;
    // Sync grid feet from camera (fly-cam proxy for character model).
    g_playerGX = static_cast<int>(std::floor(g_camPos.x / VOXEL_SIZE)) - 2;
    g_playerGY = static_cast<int>(std::floor(g_camPos.y / VOXEL_SIZE)) - 1;
    g_playerGZ = static_cast<int>(std::floor(g_camPos.z / VOXEL_SIZE)) - 1;
    auto info = sampleCharacterWater(*g_chunks, g_playerGX, g_playerGY, g_playerGZ);
    g_touchingWaterUnits = info.touching;
    g_characterUnitCount = info.total;
    g_currentTriggered = info.anyCurrent && info.touching > 0;
    g_fullySubmerged = info.fullySubmerged;

    float ratio = (info.total > 0) ? (float)info.touching / (float)info.total : 0.0f;
    // Weight rules:
    // - current triggered => weight doubles
    // - fully submerged => weight halves (of current value)
    g_playerWeight = g_playerBaseWeight;
    if (g_currentTriggered) g_playerWeight *= 2.0f;
    if (g_fullySubmerged) g_playerWeight *= 0.5f;

    // Current force from moving water contacts; doubles if fully submerged in current.
    float baseForce = 0.035f * ((info.total > 0) ? (float)info.currentTouching / (float)info.total : 0.0f);
    if (info.fullySubmerged && info.anyCurrent) baseForce *= 2.0f;
    g_currentForce = baseForce;

    if (g_currentForce > 0.0f && g_playerWeight > 1e-4f) {
        // Acceleration ~ force / weight along river +X
        float acc = g_currentForce / g_playerWeight;
        g_camPos.x += g_currentDir.x * acc * dt;
        g_camPos.y += g_currentDir.y * acc * dt;
        g_camPos.z += g_currentDir.z * acc * dt;
    }
    (void)ratio;
}

static Vec3 cameraForward() {""",
"char current")

# Call updatePlayerCurrentAndWeight in drawFrame/updateCamera path
must_replace(
"""static void drawFrame(float timeSec, float dt) {
    updateCamera(dt);""",
"""static void drawFrame(float timeSec, float dt) {
    updateCamera(dt);
    updatePlayerCurrentAndWeight(dt);""",
"drawframe current")

# Viewport uses internal resolution for cheaper fill when recording... actually swapchain is full size.
# Force smaller viewport + scissors then stretch? Better: document bitcrush + RENDER_SCALE note.
# Apply smaller effective resolution by rendering to a scaled viewport centered (letterbox) is wrong.
# Use push: set viewport to INTERNAL size then the rest of window shows clear - bad.

# Instead set g_extent for 3D to half via a fake: when recording commands, use half viewport and
# the post would stretch - without full post, set swapchain render at full but use MSAA off and bitcrush.

# Smoke output extras
must_replace(
"""                << \"\\ngravity=\" << kWorldGravity
                << \"\\nremesh_events=\" << destroysApprox << \"\\n\";""",
"""                << \"\\ngravity=\" << kWorldGravity
                << \"\\nremesh_events=\" << destroysApprox
                << \"\\nwater_touch=\" << g_touchingWaterUnits << \"/\" << g_characterUnitCount
                << \"\\ncurrent=\" << (g_currentTriggered ? 1 : 0)
                << \"\\nsubmerged=\" << (g_fullySubmerged ? 1 : 0)
                << \"\\nweight=\" << g_playerWeight
                << \"\\ncurrent_force=\" << g_currentForce
                << \"\\nrender_scale=\" << RENDER_SCALE << \"\\n\";""",
"smoke water")

# materials.hpp water
mat = Path(__file__).resolve().parents[1] / "src" / "materials.hpp"
mt = mat.read_text(encoding="utf-8")
if "Water," not in mt:
    mt = mt.replace(
        """    SheetMetal,
    Girder,
    Count
};""",
        """    SheetMetal,
    Girder,
    Water,
    Count
};""")
    mt = mt.replace(
        """        {\"girder\",         7.85f,  1.40f,  0.12f,     4.20f,    0.50f},
    };""",
        """        {\"girder\",         7.85f,  1.40f,  0.12f,     4.20f,    0.50f},
        {\"water\",          1.00f,  1.00f,  1.00f,     0.05f,    0.05f},
    };""")
    mt = mt.replace(
        """    if (name == \"girder\") return MaterialId::Girder;
    return MaterialId::Air;
}""",
        """    if (name == \"girder\") return MaterialId::Girder;
    if (name == \"water\") return MaterialId::Water;
    return MaterialId::Air;
}""")
    mat.write_text(mt, encoding="utf-8")

py_mat = Path(__file__).resolve().parents[1] / "python" / "projectiles" / "materials.py"
pt = py_mat.read_text(encoding="utf-8")
if '"water"' not in pt:
    pt = pt.replace(
        """    \"girder\": Material(\"girder\", density=7.85, weight=1.40, fragility=0.12, toughness=4.20, damping=0.50),
}""",
        """    \"girder\": Material(\"girder\", density=7.85, weight=1.40, fragility=0.12, toughness=4.20, damping=0.50),
    \"water\": Material(\"water\", density=1.00, weight=1.00, fragility=1.00, toughness=0.05, damping=0.05),
}""")
    py_mat.write_text(pt, encoding="utf-8")

# destroyVoxelAt water ok
path.write_text(text, encoding="utf-8")
print("main.cpp patched", path.stat().st_size)
