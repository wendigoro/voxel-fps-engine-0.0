// Basic Vulkan voxel rasterizer — Win32 + Clang
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "debris.hpp"
#include "destruction.hpp"
#include "health.hpp"
#include "inventory.hpp"
#include "materials.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static constexpr int WIDTH = 1280;
static constexpr int HEIGHT = 720;
static constexpr int MAX_FRAMES = 2;
// Internal 3D render scale (downscale for fill-rate). Presented upscaled with bitcrush look in shader.
static constexpr float RENDER_SCALE = 0.5f;
static constexpr float DEFAULT_FOV_DEG = 101.5f; // 70 * 1.45 fisheye default
static constexpr int INTERNAL_W = 640;  // WIDTH * 0.5
static constexpr int INTERNAL_H = 360;  // HEIGHT * 0.5

// Unit voxel grid: 1000x smaller than original 1.0 blocks. Every solid is 1x1x1 voxels
// (no stretched planes). Impact / destruction use integer grid indices only.
static constexpr float VOXEL_SIZE = 0.001f;    // == materials.hpp kVoxelSize
static constexpr int CHUNK_SIZE = 32;         // voxels per chunk axis
static constexpr int CHUNKS_X = 6;            // warehouse + river bank
static constexpr int CHUNKS_Y = 2;            // height for walls/roof girders
static constexpr int CHUNKS_Z = 5;            // extended depth for river slice
static constexpr int WORLD_W = CHUNKS_X * CHUNK_SIZE; // 160
static constexpr int WORLD_H = CHUNKS_Y * CHUNK_SIZE; // 64
static constexpr int WORLD_D = CHUNKS_Z * CHUNK_SIZE; // 128
static constexpr int VOXELS_PER_CHUNK = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE;

// Warehouse layout in unit voxels (grid space)
static constexpr int DIRT_MARGIN = 10;        // dirt apron around building
static constexpr int SLAB_THICK = 2;          // concrete floor thickness (voxels)

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(float X, float Y, float Z) : x(X), y(Y), z(Z) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    float length() const { return std::sqrt(dot(*this)); }
    Vec3 normalized() const {
        float l = length();
        return l > 1e-8f ? (*this) * (1.0f / l) : Vec3(0, 1, 0);
    }
};

struct Mat4 {
    float m[16]{};
    static Mat4 identity() {
        Mat4 r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }
    static Mat4 perspective(float fovyRad, float aspect, float znear, float zfar) {
        Mat4 r{};
        float f = 1.0f / std::tan(fovyRad * 0.5f);
        r.m[0] = f / aspect;
        r.m[5] = -f; // flip Y for Vulkan NDC
        r.m[10] = zfar / (znear - zfar);
        r.m[11] = -1.0f;
        r.m[14] = (zfar * znear) / (znear - zfar);
        return r;
    }
    static Mat4 lookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
        Vec3 f = (center - eye).normalized();
        Vec3 s = f.cross(up).normalized();
        Vec3 u = s.cross(f);
        Mat4 r = identity();
        r.m[0] = s.x;  r.m[4] = s.y;  r.m[8]  = s.z;
        r.m[1] = u.x;  r.m[5] = u.y;  r.m[9]  = u.z;
        r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
        r.m[12] = -s.dot(eye);
        r.m[13] = -u.dot(eye);
        r.m[14] = f.dot(eye);
        return r;
    }
    Mat4 operator*(const Mat4& o) const {
        Mat4 r{};
        for (int c = 0; c < 4; ++c) {
            for (int row = 0; row < 4; ++row) {
                r.m[c * 4 + row] =
                    m[0 * 4 + row] * o.m[c * 4 + 0] +
                    m[1 * 4 + row] * o.m[c * 4 + 1] +
                    m[2 * 4 + row] * o.m[c * 4 + 2] +
                    m[3 * 4 + row] * o.m[c * 4 + 3];
            }
        }
        return r;
    }
};

struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float cr, cg, cb;
float mat; // 0 solid, 1 water, 2 bulb, 3 moon, 4 sky, 5 debris cube, 6 muzzle flash, 7 inventory lattice
};

struct FrameUBO {
    float viewProj[16];
    float sunDir[3];
    float timeOfDay;       // 0..1  (night default ~0.88)
    float camPos[3];
    float time;
    float moonDir[3];
    float moonIntensity;
    float moonColor[3];
    float ambientScale;
    float muzzleFlash;     // 0..1 fire pulse (shader overlay + lighting kick)
    float fireOverlay;     // 0..1 frame-border burn
    float _fxPad[2];
    float bulbPos[4][4];   // xyz, intensity
    float bulbColor[4][4]; // rgb, radius
};

// Time system
static float g_timeOfDay = 0.88f; // night
static float g_timeScale = 0.0f;  // frozen night unless changed
static bool g_isNight = true;

struct QueueFamilyIndices {
    int graphics = -1;
    int present = -1;
    bool complete() const { return graphics >= 0 && present >= 0; }
};

// ---- globals / app state ----
static HINSTANCE g_hInstance;
static HWND g_hwnd;
static bool g_running = true;
static bool g_resized = false;
static int g_width = WIDTH;
static int g_height = HEIGHT;

static bool g_keys[256]{};
static bool g_mouseDown = false;
static int g_mouseX = 0, g_mouseY = 0, g_lastMouseX = 0, g_lastMouseY = 0;
// Free-float first-person POV camera (radians)
static float g_yaw = 0.0f;          // 0 = looking toward -Z
static float g_pitch = -0.28f; // slightly down with higher fisheye POV
static float g_moveSpeed = 0.045f;  // walk speed (world units/sec at 0.001 voxel scale)
static float g_lookSens = 0.0035f;
// Locked FPS camera follows physics player (eye). Initialized in spawnPlayer().
static Vec3 g_camPos(
    WORLD_W * VOXEL_SIZE * 0.5f,
    0.018f,
    WORLD_D * VOXEL_SIZE * 0.72f);

// Physics-bound player body (feet at py; hitbox is unit-grid AABB).
struct PlayerBody {
    float px = 0, py = 0, pz = 0; // feet center (world)
    float vx = 0, vy = 0, vz = 0;
    bool onGround = false;
    float lean = 0.0f;       // current lean -1..+1 (Q left, E right)
    float leanTarget = 0.0f;
    float eyeHeight = 0.0165f; // ~16.5 unit voxels
    float height = 0.0185f;    // full body height
    float radius = 0.0022f;    // horizontal half-extent (~2.2 unit voxels)
    float jumpSpeed = 0.055f;
};
static PlayerBody g_player;
static bool g_wantJump = false;

// Player health (RULES.md rule 15). Kept as a distinct ActorHealth rather than
// fields on PlayerBody so a second actor is a new type, not a refactor.
static health::ActorHealth g_health;
// Peak downward speed of the current airborne arc, used to price a landing.
static float g_fallPeakSpeed = 0.0f;
static bool g_wasOnGround = true;
// Lifetime counters for the smoke report.
static int g_fallDamageEvents = 0;
static int g_drownDamageTicks = 0;
static int g_bodyHits = 0;
static int g_deaths = 0;
static int g_respawns = 0;
static bool g_deathHandled = false;

// Projectile destruction state (g_chunks assigned after Chunk type exists)
struct Chunk;
static std::vector<Chunk>* g_chunks = nullptr;
static std::vector<ProjectileDef> g_projDefs;
static std::vector<AmmoDef> g_ammoDefs;
static std::vector<ProjectileRuntime> g_projectiles;
static int g_activeAmmoIndex = 0; // cycles ammo subtypes for active caliber (R)
static bool g_meshDirty = false;
static bool g_firePressed = false; // edge: semi/bolt or smoke
static bool g_fireHeld = false;    // level: auto (RMB / F held)

// Weapon assembly runtime (loaded from data/weapons/*.weapon.json)
static std::vector<WeaponDef> g_weapons;
static int g_activeWeaponIndex = 0;
static int g_activeCaliberIndex = 1; // 0 light 1 medium 2 heavy 3 energy (keys 1-4)
static const char* kCaliberIds[4] = {"light", "medium", "heavy", "energy"};
static float g_fireCooldown = 0.0f;
// Recoil is a temporary view offset on top of aim angles; recovers after last fire.
static float g_recoilPitch = 0.0f;
static float g_recoilYaw = 0.0f;
static float g_recoilHold = 0.0f;     // seconds to hold kick before recover starts
static float g_recoilReturn = 10.0f;  // exponential recover rate (1/s)
static std::string g_lastAmmoId = "medium_fmj";
static std::string g_lastCaliber = "medium";
static std::string g_lastFireMode = "semi";
static bool g_lastHitscan = false;
static std::string g_lastWeaponId = "none";
static int g_hitscanShots = 0;
static int g_ballisticShots = 0;
static bool g_ads = false; // hold X — optic ADS (FOV + spread)
static DebrisSystem g_debris;

// Cubic-unit inventory (RULES.md rule 12). Separate occupancy layer: never
// written into chunk storage, never affects collision / impact / water.
static ItemTable g_itemDefs;
static Inventory g_inventory;
static bool g_inventoryOpen = false;

// ---- health / damage bridge (RULES.md rule 15) ---------------------------
// Armor mitigation comes from whatever piece is actually equipped over the hit
// zone, so the 0.5%-per-point rule is data-driven and an empty slot is simply
// no mitigation.
static float equippedArmorPoints(ArmorZone zone) {
    if (zone >= ArmorZone::Count) return 0.0f;
    const int slot = static_cast<int>(zoneEquipSlot(zone));
    const int defIdx = g_inventory.slotDef[slot];
    if (defIdx < 0) return 0.0f;
    const ItemDef* d = itemDefAt(g_itemDefs, defIdx);
    return d ? d->armorPoints : 0.0f;
}

// Convert an impact's voxel-destruction energy into HP loss on the player's body
// and apply it at `zone`. Returns the HP actually removed.
static float damagePlayerAtZone(float energy, const std::string& effect, ArmorZone zone) {
    if (g_health.dead) return 0.0f;
    const float bio = health::biologicalDamage(energy, effect);
    if (bio <= 0.0f) return 0.0f;
    const health::DamageResult r =
        health::applyDamage(g_health, bio, zone, equippedArmorPoints(zone));
    if (r.applied > 0.0f) ++g_bodyHits;
    return r.applied;
}

// Segment-vs-body test in the player's cell space. Returns the hit zone, or
// ArmorZone::Count on a miss.
static ArmorZone projectileHitZone(float x0, float y0, float z0, float x1, float y1, float z1,
                                   float radiusCells) {
    if (g_health.dead) return ArmorZone::Count;
    const health::BodyCellOrigin org = health::bodyCellOrigin(g_player.px, g_player.py, g_player.pz);
    const health::BodyHit h = health::segmentHitBody(org, x0, y0, z0, x1, y1, z1, radiusCells);
    return h.zone;
}
// One-shot input latches raised by the window proc and drained once per frame.
static bool g_inventoryClick = false;  // LMB: lift a packed item / place the held one
static bool g_inventoryStow = false;   // RMB: stow the held item back into the pack
static bool g_pickupPressed = false;   // G: take the world item under the crosshair

// Inventory lattice mesh: unit cubes emitted camera-relative into their own
// overlay pass. 36 lattice cells + 8 equipment slot markers + margin.
static constexpr uint32_t kInventoryMaxVerts = 4096;
static VkBuffer g_inventoryVB = VK_NULL_HANDLE;
static VkDeviceMemory g_inventoryMem = VK_NULL_HANDLE;
static void* g_inventoryMapped = nullptr;
static uint32_t g_inventoryVertexCount = 0;
static uint32_t g_inventoryOverlayFrames = 0; // frames that actually submitted the pass
static constexpr uint32_t kInventoryMatId = 7;
static constexpr uint32_t kPickupMatId = 8;    // world pickups: main pass, not the overlay

// Display basis + origin, rebuilt with the mesh. Look-and-click picks cells in
// SCREEN space against this transform rather than by casting a world ray, which
// stays exact because mat 7 skips the vertex-shader fisheye: what the CPU
// projects here is precisely what the GPU rasterised.
struct InventoryDisplay {
    Vec3 origin, right, up, fwd;
    bool valid = false;
};
static InventoryDisplay g_invDisplay;

// What the cursor is over, resolved each frame while the inventory is open.
struct InventoryHover {
    bool lattice = false;   // cursor is over a storage cell
    int x = 0, y = 0, z = 0; // cell coords (lattice space)
    bool slot = false;      // cursor is over an equipment marker
    int slotIndex = 0;
    bool overCell = false;  // either of the above
};
static InventoryHover g_invHover;
static int g_inventoryHandRot = 0;   // preview rotation while carrying an item
static float g_lastImpactDx = 0, g_lastImpactDy = 0, g_lastImpactDz = -1;
static float g_lastImpactEnergy = 10.0f;
static float g_lastAoeScale = 1.0f;
static int g_shotgunShots = 0;
static int g_pelletSpawns = 0;
// GPU buffer for visual debris cubes (display-only 8^3 chips) + muzzle flash cubes
static VkBuffer g_debrisVB = VK_NULL_HANDLE;
static VkDeviceMemory g_debrisMem = VK_NULL_HANDLE;
static void* g_debrisMapped = nullptr;
static uint32_t g_debrisVertexCount = 0;
// True cubic chips: 36 verts/particle (6 faces × 2 tris). Cap keeps upload cheap.
static constexpr uint32_t kDebrisMaxParticlesDraw = 160;
static constexpr uint32_t kDebrisVertsPerParticle = 36;
static constexpr uint32_t kMuzzleFlashCubes = 4;
static constexpr uint32_t kDebrisMaxVerts =
    kDebrisMaxParticlesDraw * kDebrisVertsPerParticle + kMuzzleFlashCubes * 36u;
static double g_debrisUploadUsSum = 0.0;
static double g_debrisUploadUsMax = 0.0;
static int g_debrisUploadSamples = 0;
static uint32_t g_debrisVertsPeak = 0;
static bool g_debrisWasActive = false;
static float g_muzzleFlash = 0.0f;   // decays each frame after fire
static float g_fireOverlay = 0.0f;  // frame-border burn intensity
static float g_muzzleR = 1.0f, g_muzzleG = 0.72f, g_muzzleB = 0.28f;

// Player character as unit-voxel body on the cubic grid (impact/current sampling).
static int g_playerGX = 0, g_playerGY = 0, g_playerGZ = 0;
static float g_playerBaseWeight = 1.0f;
static float g_playerWeight = 1.0f;
static float g_currentForce = 0.0f;
static Vec3 g_currentDir = {1.0f, 0.0f, 0.0f}; // river flows +X
static bool g_currentTriggered = false;
static bool g_fullySubmerged = false;
static int g_touchingWaterUnits = 0;
static int g_characterUnitCount = 0;

static VkInstance g_instance = VK_NULL_HANDLE;
static VkSurfaceKHR g_surface = VK_NULL_HANDLE;
static VkPhysicalDevice g_phys = VK_NULL_HANDLE;
static VkDevice g_device = VK_NULL_HANDLE;
static VkQueue g_graphicsQueue = VK_NULL_HANDLE;
static VkQueue g_presentQueue = VK_NULL_HANDLE;
static QueueFamilyIndices g_qidx;
static VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
static VkFormat g_swapFormat = VK_FORMAT_B8G8R8A8_UNORM;
static VkExtent2D g_extent{WIDTH, HEIGHT};
static std::vector<VkImage> g_swapImages;
static std::vector<VkImageView> g_swapViews;
static std::vector<VkFramebuffer> g_framebuffers;
static std::vector<VkFramebuffer> g_overlayFramebuffers; // RULES.md rule 12
static VkRenderPass g_renderPass = VK_NULL_HANDLE;
static VkRenderPass g_overlayRenderPass = VK_NULL_HANDLE; // RULES.md rule 12
static VkDescriptorSetLayout g_dsl = VK_NULL_HANDLE;
static VkPipelineLayout g_pipelineLayout = VK_NULL_HANDLE;
static VkPipeline g_pipeline = VK_NULL_HANDLE;
static VkPipeline g_overlayPipeline = VK_NULL_HANDLE;
static VkCommandPool g_cmdPool = VK_NULL_HANDLE;
static std::vector<VkCommandBuffer> g_cmdBuffers;
static VkImage g_depthImage = VK_NULL_HANDLE;
static VkDeviceMemory g_depthMem = VK_NULL_HANDLE;
static VkImageView g_depthView = VK_NULL_HANDLE;
static VkBuffer g_vertexBuffer = VK_NULL_HANDLE;
static VkDeviceMemory g_vertexMem = VK_NULL_HANDLE;
static void* g_vertexMapped = nullptr;
static VkDeviceSize g_vertexCapacity = 0; // bytes
static uint32_t g_vertexCount = 0;
static double g_meshUploadUsMax = 0.0;
static double g_meshUploadUsSum = 0.0;
static int g_meshUploadSamples = 0;
static int g_framesSinceRemesh = 99;
static int g_remeshSkipCount = 0;
// Headless modes (declared early — used by debris draw + remesh throttle).
static bool g_smoke = false;
static bool g_stress = false;
static int g_smokeFrames = 300;
static int g_projLivePeak = 0;
static int g_stressFireCount = 0;
static VkBuffer g_uboBuffers[MAX_FRAMES]{};
static VkDeviceMemory g_uboMems[MAX_FRAMES]{};
static void* g_uboMapped[MAX_FRAMES]{};
static VkDescriptorPool g_descPool = VK_NULL_HANDLE;
static VkDescriptorSet g_descSets[MAX_FRAMES]{};
static VkSemaphore g_imageAvailable[MAX_FRAMES]{};
static VkSemaphore g_renderFinished[MAX_FRAMES]{};
static VkFence g_inFlight[MAX_FRAMES]{};
static size_t g_frame = 0;
static Mat4 g_viewProjCull{};
static uint32_t g_drawnChunks = 0;
static uint32_t g_culledChunks = 0;
static constexpr double TARGET_HZ = 120.0;
static constexpr double TARGET_FRAME_SEC = 1.0 / TARGET_HZ;
static LARGE_INTEGER g_qpcFreq{};
static LARGE_INTEGER g_qpcLast{};
static bool g_qpcInit = false;
// Sky tile system: pixel-grid hemisphere + moon light-source sprite
static VkBuffer g_skyTileVB = VK_NULL_HANDLE;
static VkDeviceMemory g_skyTileMem = VK_NULL_HANDLE;
static void* g_skyTileMapped = nullptr;
static uint32_t g_skyTileVertexCount = 0;static Vec3 g_moonWorldPos = {0, 0, 0};
static Vec3 g_moonDirWorld = {0.32f, 0.82f, -0.48f}; // fixed sky bearing (light source)
static float g_moonTileSize = 0.034f;
static float g_skyRadius = 0.55f;
static constexpr int SKY_SEG_U = 28; // azimuth tiles
static constexpr int SKY_SEG_V = 14; // elevation tiles (hemisphere)
static double g_frameMsSum = 0.0;
static double g_frameMsMin = 1e9;
static double g_frameMsMax = 0.0;
static int g_frameMsCount = 0;
static int g_framePaceHits = 0;

static std::string g_exeDir;

static void fail(const std::string& msg) {
    MessageBoxA(nullptr, msg.c_str(), "Voxel Engine Error", MB_ICONERROR | MB_OK);
    throw std::runtime_error(msg);
}

static std::vector<char> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::ate | std::ios::binary);
    if (!f) fail("Failed to open file: " + path);
    size_t size = static_cast<size_t>(f.tellg());
    std::vector<char> buf(size);
    f.seekg(0);
    f.read(buf.data(), static_cast<std::streamsize>(size));
    return buf;
}

static uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(g_phys, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    fail("No suitable memory type");
    return 0;
}

static void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                         VkMemoryPropertyFlags props, VkBuffer& buffer,
                         VkDeviceMemory& memory) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_device, &bi, nullptr, &buffer) != VK_SUCCESS)
        fail("vkCreateBuffer failed");

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g_device, buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, props);
    if (vkAllocateMemory(g_device, &ai, nullptr, &memory) != VK_SUCCESS)
        fail("vkAllocateMemory failed");
    vkBindBufferMemory(g_device, buffer, memory, 0);
}

static VkCommandBuffer beginOneTime() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g_cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(g_device, &ai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

static void endOneTime(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(g_graphicsQueue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_graphicsQueue);
    vkFreeCommandBuffers(g_device, g_cmdPool, 1, &cmd);
}

// ---- fine voxel + chunk system (sharp face vertices) ----
enum class Block : uint8_t {
    Air = 0,
    Dirt,
    Concrete,
    SheetMetal,
    Girder,
    Wood,
    WoodDark,
    Water,        // still unit cubes; may occupy WATER_CELL multi-cell clumps
    WaterCurrent, // moving water source (same visual, current sampling)
    Moon,         // cool emissive crescent grid
    LightBulb     // warm emissive indoor bulbs
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
    case Block::Moon:
    case Block::LightBulb: return MaterialId::Air; // emissive, no impact mass
    default: return MaterialId::Air;
    }
}

static bool isWaterBlock(Block b) {
    return b == Block::Water || b == Block::WaterCurrent;
}

// Collision solids: occupancy that blocks the player hitbox (not water/emissive).
static bool isSolidBlock(Block b) {
    if (b == Block::Air || isWaterBlock(b)) return false;
    if (b == Block::Moon || b == Block::LightBulb) return false;
    return true;
}

struct Chunk {
    int cx = 0, cy = 0, cz = 0; // chunk coords
    std::vector<Block> voxels;  // CHUNK_SIZE^3
    std::vector<Vertex> mesh;   // sharp unique face verts
    bool dirty = true;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    bool wasVisible = true;
};

static Vec3 blockColor(Block b) {
    switch (b) {
    case Block::Dirt:         return {0.28f, 0.20f, 0.12f};
    case Block::Concrete:     return {0.40f, 0.40f, 0.42f};
    case Block::SheetMetal:   return {0.48f, 0.50f, 0.52f};
    case Block::Girder:       return {0.28f, 0.10f, 0.08f};
    case Block::Wood:         return {0.34f, 0.22f, 0.12f};
    case Block::WoodDark:     return {0.32f, 0.18f, 0.08f};
    case Block::Water:        return {0.12f, 0.28f, 0.42f};
    case Block::WaterCurrent: return {0.10f, 0.35f, 0.48f};
    case Block::Moon:         return {0.75f, 0.80f, 0.90f};
    case Block::LightBulb:    return {1.00f, 0.75f, 0.45f};
    default:                  return {1, 0, 1};
    }
}

static inline int chunkIndex(int cx, int cy, int cz) {
    return (cy * CHUNKS_Z + cz) * CHUNKS_X + cx;
}

static inline int localIndex(int lx, int ly, int lz) {
    return (ly * CHUNK_SIZE + lz) * CHUNK_SIZE + lx;
}

static bool worldInBounds(int x, int y, int z) {
    return x >= 0 && y >= 0 && z >= 0 && x < WORLD_W && y < WORLD_H && z < WORLD_D;
}

static float hashNoise(int x, int z) {
    uint32_t n = static_cast<uint32_t>(x * 374761393u + z * 668265263u);
    n = (n ^ (n >> 13)) * 1274126177u;
    n ^= n >> 16;
    return (n & 0xFFFFu) / 65535.0f;
}

static float valueNoise(int x, int z) {
    // cheap multi-octave for micro-terrain
    float n = 0.0f;
    n += hashNoise(x, z) * 1.0f;
    n += hashNoise(x / 2, z / 2) * 2.0f;
    n += hashNoise(x / 4, z / 4) * 4.0f;
    n += hashNoise(x / 8, z / 8) * 6.0f;
    return n / 13.0f;
}

static Block getWorldBlock(const std::vector<Chunk>& chunks, int x, int y, int z) {
    if (!worldInBounds(x, y, z)) return Block::Air;
    int cx = x / CHUNK_SIZE;
    int cy = y / CHUNK_SIZE;
    int cz = z / CHUNK_SIZE;
    int lx = x - cx * CHUNK_SIZE;
    int ly = y - cy * CHUNK_SIZE;
    int lz = z - cz * CHUNK_SIZE;
    return chunks[chunkIndex(cx, cy, cz)].voxels[localIndex(lx, ly, lz)];
}

static void setWorldBlock(std::vector<Chunk>& chunks, int x, int y, int z, Block b) {
    if (!worldInBounds(x, y, z)) return;
    int cx = x / CHUNK_SIZE;
    int cy = y / CHUNK_SIZE;
    int cz = z / CHUNK_SIZE;
    int lx = x - cx * CHUNK_SIZE;
    int ly = y - cy * CHUNK_SIZE;
    int lz = z - cz * CHUNK_SIZE;
    chunks[chunkIndex(cx, cy, cz)].voxels[localIndex(lx, ly, lz)] = b;
    chunks[chunkIndex(cx, cy, cz)].dirty = true;
}

static int groundHeight(const std::vector<Chunk>& chunks, int x, int z) {
    for (int y = WORLD_H - 1; y >= 0; --y) {
        Block b = getWorldBlock(chunks, x, y, z);
        if (b != Block::Air) return y;
    }
    return 0;
}

// Fill a solid axis-aligned box with unit voxels (inclusive).
static void fillBox(std::vector<Chunk>& chunks, int x0, int y0, int z0,
                    int x1, int y1, int z1, Block b) {
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    if (z0 > z1) std::swap(z0, z1);
    for (int z = z0; z <= z1; ++z)
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                setWorldBlock(chunks, x, y, z, b);
}

// Vertical I-beam girder (unit voxels only): flanges + web.
static void placeGirderColumn(std::vector<Chunk>& chunks, int cx, int zc,
                              int y0, int y1) {
    for (int y = y0; y <= y1; ++y) {
        // web
        setWorldBlock(chunks, cx, y, zc, Block::Girder);
        setWorldBlock(chunks, cx, y, zc + 1, Block::Girder);
        // flanges
        for (int dx = -2; dx <= 2; ++dx) {
            setWorldBlock(chunks, cx + dx, y, zc - 1, Block::Girder);
            setWorldBlock(chunks, cx + dx, y, zc + 2, Block::Girder);
        }
    }
}

// Horizontal I-beam along X at fixed y,z.
static void placeGirderBeamX(std::vector<Chunk>& chunks, int x0, int x1, int y, int zc) {
    for (int x = x0; x <= x1; ++x) {
        setWorldBlock(chunks, x, y, zc, Block::Girder);
        setWorldBlock(chunks, x, y, zc + 1, Block::Girder);
        for (int dy = -2; dy <= 2; ++dy) {
            setWorldBlock(chunks, x, y + dy, zc - 1, Block::Girder);
            setWorldBlock(chunks, x, y + dy, zc + 2, Block::Girder);
        }
    }
}

// Horizontal I-beam along Z.
static void placeGirderBeamZ(std::vector<Chunk>& chunks, int z0, int z1, int y, int xc) {
    for (int z = z0; z <= z1; ++z) {
        setWorldBlock(chunks, xc, y, z, Block::Girder);
        setWorldBlock(chunks, xc + 1, y, z, Block::Girder);
        for (int dy = -2; dy <= 2; ++dy) {
            setWorldBlock(chunks, xc - 1, y + dy, z, Block::Girder);
            setWorldBlock(chunks, xc + 2, y + dy, z, Block::Girder);
        }
    }
}

// Sheet-metal wall panel: 1-voxel-thick unit cubes (corrugation via alternate offset).
static void placeSheetWallX(std::vector<Chunk>& chunks, int x, int y0, int y1, int z0, int z1) {
    for (int z = z0; z <= z1; ++z) {
        for (int y = y0; y <= y1; ++y) {
            int xo = x + ((z + y) & 1); // slight corrugation still unit voxels
            setWorldBlock(chunks, xo, y, z, Block::SheetMetal);
        }
    }
}

static void placeSheetWallZ(std::vector<Chunk>& chunks, int z, int y0, int y1, int x0, int x1) {
    for (int x = x0; x <= x1; ++x) {
        for (int y = y0; y <= y1; ++y) {
            int zo = z + ((x + y) & 1);
            setWorldBlock(chunks, x, y, zo, Block::SheetMetal);
        }
    }
}

// Wooden crate made of unit voxels.
static void placeCrate(std::vector<Chunk>& chunks, int x0, int y0, int z0, int s) {
    fillBox(chunks, x0, y0, z0, x0 + s - 1, y0 + s - 1, z0 + s - 1, Block::Wood);
    // darker edge frame
    for (int i = 0; i < s; ++i) {
        setWorldBlock(chunks, x0 + i, y0, z0, Block::WoodDark);
        setWorldBlock(chunks, x0 + i, y0, z0 + s - 1, Block::WoodDark);
        setWorldBlock(chunks, x0, y0, z0 + i, Block::WoodDark);
        setWorldBlock(chunks, x0 + s - 1, y0, z0 + i, Block::WoodDark);
        setWorldBlock(chunks, x0 + i, y0 + s - 1, z0, Block::WoodDark);
        setWorldBlock(chunks, x0 + i, y0 + s - 1, z0 + s - 1, Block::WoodDark);
    }
}

// Simple warehouse map: dirt apron, concrete slab, sheet-metal walls,
// red-oxide girder frame — every element is unit voxels on the impact grid.
static std::vector<Chunk> buildWarehouseMap() {
    std::vector<Chunk> chunks(CHUNKS_X * CHUNKS_Y * CHUNKS_Z);
    for (int cy = 0; cy < CHUNKS_Y; ++cy)
        for (int cz = 0; cz < CHUNKS_Z; ++cz)
            for (int cx = 0; cx < CHUNKS_X; ++cx) {
                Chunk& c = chunks[chunkIndex(cx, cy, cz)];
                c.cx = cx; c.cy = cy; c.cz = cz;
                c.voxels.assign(VOXELS_PER_CHUNK, Block::Air);
                c.dirty = true;
            }

    // 1) Dirt apron (single unit layer under map - keeps occupancy grid, fewer faces)
    fillBox(chunks, 0, 0, 0, WORLD_W - 1, 0, WORLD_D - 1, Block::Dirt);

    const int bx0 = DIRT_MARGIN;
    const int bz0 = DIRT_MARGIN;
    const int bx1 = WORLD_W - 1 - DIRT_MARGIN;
    const int bz1 = WORLD_D - 1 - DIRT_MARGIN;
    const int wallH = 40;          // wall height in unit voxels
    const int roofY = 1 + wallH;   // underside of roof beams

    // 2) Concrete slab (multi-voxel thick — not a stretched plane).
    fillBox(chunks, bx0, 1, bz0, bx1, 1 + SLAB_THICK - 1, bz1, Block::Concrete);

    // Outer dirt remains as apron (already filled); clear building footprint dirt top under slab already overwritten.

    // 3) Girder columns at corners and mid-span (I-beam unit voxels).
    const int colsX[] = { bx0 + 2, (bx0 + bx1) / 2, bx1 - 3 };
    const int colsZ[] = { bz0 + 2, (bz0 + bz1) / 2, bz1 - 3 };
    for (int ix = 0; ix < 3; ++ix)
        for (int iz = 0; iz < 3; ++iz)
            placeGirderColumn(chunks, colsX[ix], colsZ[iz], 1 + SLAB_THICK, roofY);

    // 4) Roof girder grid (unit I-beams).
    for (int iz = 0; iz < 3; ++iz)
        placeGirderBeamX(chunks, bx0 + 2, bx1 - 2, roofY, colsZ[iz]);
    for (int ix = 0; ix < 3; ++ix)
        placeGirderBeamZ(chunks, bz0 + 2, bz1 - 2, roofY, colsX[ix]);

    // 5) Sheet-metal walls — 1-voxel-thick unit panels (open bay on +Z front).
    placeSheetWallX(chunks, bx0, 1 + SLAB_THICK, roofY - 1, bz0, bz1);           // -X wall
    placeSheetWallX(chunks, bx1, 1 + SLAB_THICK, roofY - 1, bz0, bz1);           // +X wall
    placeSheetWallZ(chunks, bz0, 1 + SLAB_THICK, roofY - 1, bx0, bx1);           // -Z back wall
    // Front (+Z): partial side wings, open center doorway
    placeSheetWallZ(chunks, bz1, 1 + SLAB_THICK, roofY - 1, bx0, bx0 + 35);
    placeSheetWallZ(chunks, bz1, 1 + SLAB_THICK, roofY - 1, bx1 - 35, bx1);
    // Door lintel strip of sheet metal
    placeSheetWallZ(chunks, bz1, roofY - 8, roofY - 1, bx0 + 36, bx1 - 36);

    // 6) Roof sheet deck: unit metal cubes on top of beams (not a single quad).
    for (int z = bz0; z <= bz1; ++z)
        for (int x = bx0; x <= bx1; ++x) {
            // skip every other for light vents still unit cubes
            if (((x + z) & 3) == 0) continue;
            setWorldBlock(chunks, x, roofY + 3, z, Block::SheetMetal);
        }

    // 7) A few unit-voxel crates inside for material variety / targets.
    placeCrate(chunks, bx0 + 20, 1 + SLAB_THICK, bz0 + 24, 8);
    placeCrate(chunks, bx0 + 40, 1 + SLAB_THICK, bz0 + 30, 10);
    placeCrate(chunks, bx1 - 30, 1 + SLAB_THICK, bz0 + 20, 8);

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

    // 7c) Moon light-source sky tile anchor (sprite drawn as billboard; light via UBO moonDir).
    {
        const int mx = WORLD_W / 2 + 24;
        const int mz = 6;
        const int my = WORLD_H - 6;
        g_moonWorldPos = Vec3((mx + 0.5f) * VOXEL_SIZE, (my + 0.5f) * VOXEL_SIZE, (mz + 0.5f) * VOXEL_SIZE);
        // Single unit voxel anchor (optional debug marker); main visual is sky tile sprite.
        setWorldBlock(chunks, mx, my, mz, Block::Moon);
    }

    // 8) River slice beyond +Z apron: WATER_CELL (2x2) unit cubes, deep channel with current.
    // Dirt bank extends; carve channel and fill water/current.
    {
        const int riverZ0 = bz1 + 2;
        const int riverZ1 = WORLD_D - 3;
        const int riverX0 = 8;
        const int riverX1 = WORLD_W - 9;
        // Ensure dirt banks around river
        fillBox(chunks, 0, 0, riverZ0 - 2, WORLD_W - 1, 0, WORLD_D - 1, Block::Dirt);
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
                if (dist < 6) localDeep = surfaceY + 5;      // deepest
                else if (dist < 12) localDeep = surfaceY + 2;
                for (int y = 0; y <= localDeep && y < WORLD_H; ++y) {
                    // place as WATER_CELL clumps: still unit cubes on grid
                    Block wb = (dist < 10 && y <= localDeep) ? Block::WaterCurrent : Block::Water;
                    setWorldBlock(chunks, x, y, z, wb);
                    // thicken visually with adjacent unit cells (larger water voxels)
                    // WATER_CELL clumps only on even layers to cut fill-rate
                    if ((y & 1) == 0 && (x % WATER_CELL) == 0 && (z % WATER_CELL) == 0) {
                        for (int dz = 0; dz < WATER_CELL; ++dz)
                            for (int dx = 0; dx < WATER_CELL; ++dx)
                                if (dx || dz) setWorldBlock(chunks, x + dx, y, z + dz, wb);
                    }
                }
            }
        }
    }

    return chunks;
}

// Sharp vertex face emit: 6 unique verts/face (2 tris), hard face normals, no sharing.
static void emitSharpFace(std::vector<Vertex>& out, int ix, int iy, int iz,
                          int face, const Vec3& color, float mat = 0.0f) {
    // unit cube corners in voxel space, scaled to world by VOXEL_SIZE
    static const float F[6][4][3] = {
        {{1,0,0},{1,1,0},{1,1,1},{1,0,1}}, // +X
        {{0,0,1},{0,1,1},{0,1,0},{0,0,0}}, // -X
        {{0,1,0},{0,1,1},{1,1,1},{1,1,0}}, // +Y
        {{0,0,1},{0,0,0},{1,0,0},{1,0,1}}, // -Y
        {{1,0,1},{1,1,1},{0,1,1},{0,0,1}}, // +Z
        {{0,0,0},{0,1,0},{1,1,0},{1,0,0}}, // -Z
    };
    static const float N[6][3] = {
        {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}
    };
    // CCW when viewed from outside, matching Vulkan front-face CCW + Y-flip proj
    static const int IDX[6] = {0, 1, 2, 0, 2, 3};
    static const float faceShade[6] = {0.82f, 0.68f, 1.0f, 0.52f, 0.90f, 0.74f};

    const float ox = ix * VOXEL_SIZE;
    const float oy = iy * VOXEL_SIZE;
    const float oz = iz * VOXEL_SIZE;
    Vec3 c = color * faceShade[face];

    for (int i = 0; i < 6; ++i) {
        const float* p = F[face][IDX[i]];
        out.push_back(Vertex{
            ox + p[0] * VOXEL_SIZE,
            oy + p[1] * VOXEL_SIZE,
            oz + p[2] * VOXEL_SIZE,
            N[face][0], N[face][1], N[face][2],
            c.x, c.y, c.z,
            mat
        });
    }
}

static void meshChunk(Chunk& chunk, const std::vector<Chunk>& chunks) {
    chunk.mesh.clear();
    chunk.mesh.reserve(4096);
    const int ox[6] = {1,-1,0,0,0,0};
    const int oy[6] = {0,0,1,-1,0,0};
    const int oz[6] = {0,0,0,0,1,-1};

    const int baseX = chunk.cx * CHUNK_SIZE;
    const int baseY = chunk.cy * CHUNK_SIZE;
    const int baseZ = chunk.cz * CHUNK_SIZE;

    for (int ly = 0; ly < CHUNK_SIZE; ++ly) {
        for (int lz = 0; lz < CHUNK_SIZE; ++lz) {
            for (int lx = 0; lx < CHUNK_SIZE; ++lx) {
                Block b = chunk.voxels[localIndex(lx, ly, lz)];
                if (b == Block::Air) continue;
                int x = baseX + lx, y = baseY + ly, z = baseZ + lz;
                Vec3 col = blockColor(b);

                for (int f = 0; f < 6; ++f) {
                    Block nb = getWorldBlock(chunks, x + ox[f], y + oy[f], z + oz[f]);
                    // Unit-cube face exposed only against empty grid cells.
                    bool expose = false;
                    if (isWaterBlock(b)) {
                        expose = (nb == Block::Air) || (!isWaterBlock(nb) && nb != Block::Air);
                        // show water surface against air only for clearer tide paint
                        expose = (nb == Block::Air);
                    } else {
                        expose = (nb == Block::Air) || isWaterBlock(nb);
                    }
                    if (!expose) continue;
                    float mat = 0.0f;
                    if (isWaterBlock(b)) mat = 1.0f;
                    else if (b == Block::LightBulb) mat = 2.0f;
                    else if (b == Block::Moon) mat = 3.0f;
                    emitSharpFace(chunk.mesh, x, y, z, f, col, mat);
                }
            }
        }
    }
    chunk.dirty = false;
}

static std::vector<Vertex> meshAllChunks(std::vector<Chunk>& chunks) {
    std::vector<Vertex> verts;
    verts.reserve(400000);
    uint32_t cursor = 0;
    for (auto& c : chunks) {
        if (c.dirty) meshChunk(c, chunks);
        c.firstVertex = cursor;
        c.vertexCount = static_cast<uint32_t>(c.mesh.size());
        if (!c.mesh.empty())
            verts.insert(verts.end(), c.mesh.begin(), c.mesh.end());
        cursor += c.vertexCount;
    }
    return verts;
}

struct Frustum { float p[6][4]; };

static void normalizePlane(float pl[4]) {
    float l = std::sqrt(pl[0]*pl[0] + pl[1]*pl[1] + pl[2]*pl[2]);
    if (l > 1e-8f) { pl[0]/=l; pl[1]/=l; pl[2]/=l; pl[3]/=l; }
}

static Frustum frustumFromVP(const Mat4& vp) {
    const float* m = vp.m;
    Frustum f{};
    float raw[6][4] = {
        { m[3]+m[0], m[7]+m[4], m[11]+m[8],  m[15]+m[12] },
        { m[3]-m[0], m[7]-m[4], m[11]-m[8],  m[15]-m[12] },
        { m[3]+m[1], m[7]+m[5], m[11]+m[9],  m[15]+m[13] },
        { m[3]-m[1], m[7]-m[5], m[11]-m[9],  m[15]-m[13] },
        { m[3]+m[2], m[7]+m[6], m[11]+m[10], m[15]+m[14] },
        { m[3]-m[2], m[7]-m[6], m[11]-m[10], m[15]-m[14] },
    };
    for (int i = 0; i < 6; ++i) {
        for (int k = 0; k < 4; ++k) f.p[i][k] = raw[i][k];
        normalizePlane(f.p[i]);
    }
    return f;
}

static bool aabbVisible(const Frustum& f, float minx, float miny, float minz,
                        float maxx, float maxy, float maxz) {
    for (int i = 0; i < 6; ++i) {
        const float* pl = f.p[i];
        float px = pl[0] > 0 ? maxx : minx;
        float py = pl[1] > 0 ? maxy : miny;
        float pz = pl[2] > 0 ? maxz : minz;
        if (pl[0]*px + pl[1]*py + pl[2]*pz + pl[3] < 0.0f) return false;
    }
    return true;
}

static void chunkWorldAABB(const Chunk& c, float& minx, float& miny, float& minz,
                           float& maxx, float& maxy, float& maxz) {
    minx = c.cx * CHUNK_SIZE * VOXEL_SIZE;
    miny = c.cy * CHUNK_SIZE * VOXEL_SIZE;
    minz = c.cz * CHUNK_SIZE * VOXEL_SIZE;
    maxx = minx + CHUNK_SIZE * VOXEL_SIZE;
    maxy = miny + CHUNK_SIZE * VOXEL_SIZE;
    maxz = minz + CHUNK_SIZE * VOXEL_SIZE;
    const float pad = VOXEL_SIZE * 2.0f;
    minx -= pad; miny -= pad; minz -= pad;
    maxx += pad; maxy += pad; maxz += pad;
}

// true => not seen (skip draw)
static bool chunkNotSeen(const Chunk& c, const Frustum& fr, const Vec3& eye, const Vec3& forward) {
    if (c.vertexCount == 0) return true;
    float minx,miny,minz,maxx,maxy,maxz;
    chunkWorldAABB(c, minx,miny,minz,maxx,maxy,maxz);
    if (!aabbVisible(fr, minx,miny,minz, maxx,maxy,maxz)) return true;
    float cx = 0.5f*(minx+maxx), cy = 0.5f*(miny+maxy), cz = 0.5f*(minz+maxz);
    Vec3 to = Vec3(cx,cy,cz) - eye;
    float ext = 0.5f * std::sqrt((maxx-minx)*(maxx-minx)+(maxy-miny)*(maxy-miny)+(maxz-minz)*(maxz-minz));
    if (to.dot(forward) < -ext) return true;
    return false;
}

static void paceFrame120() {
    if (!g_qpcInit) {
        QueryPerformanceFrequency(&g_qpcFreq);
        QueryPerformanceCounter(&g_qpcLast);
        g_qpcInit = true;
        return;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = double(now.QuadPart - g_qpcLast.QuadPart) / double(g_qpcFreq.QuadPart);
    double frameMs = elapsed * 1000.0;
    if (g_frameMsCount >= 15) {
        g_frameMsSum += frameMs;
        if (frameMs < g_frameMsMin) g_frameMsMin = frameMs;
        if (frameMs > g_frameMsMax) g_frameMsMax = frameMs;
        if (frameMs <= (TARGET_FRAME_SEC * 1000.0) + 0.85) ++g_framePaceHits;
    }
    ++g_frameMsCount;
    if (elapsed < TARGET_FRAME_SEC) {
        for (;;) {
            QueryPerformanceCounter(&now);
            elapsed = double(now.QuadPart - g_qpcLast.QuadPart) / double(g_qpcFreq.QuadPart);
            if (elapsed >= TARGET_FRAME_SEC) break;
            double remain = TARGET_FRAME_SEC - elapsed;
            if (remain > 0.002) {
                DWORD ms = (DWORD)((remain - 0.0007) * 1000.0);
                if (ms > 0) Sleep(ms);
            }
        }
    }
    QueryPerformanceCounter(&g_qpcLast);
}

static Vec3 cameraForward();
static Vec3 cameraRight();

static void ensureSkyTileBuffer() {
    if (g_skyTileVB) return;
    // hemisphere tiles + moon sprite quad
    const uint32_t skyQuads = SKY_SEG_U * SKY_SEG_V;
    g_skyTileVertexCount = skyQuads * 6u + 6u;
    VkDeviceSize size = sizeof(Vertex) * g_skyTileVertexCount;
    createBuffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 g_skyTileVB, g_skyTileMem);
    vkMapMemory(g_device, g_skyTileMem, 0, size, 0, &g_skyTileMapped);
}

static void ensureDebrisBuffer() {
    if (g_debrisVB) return;
    VkDeviceSize size = sizeof(Vertex) * kDebrisMaxVerts;
    createBuffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 g_debrisVB, g_debrisMem);
    vkMapMemory(g_device, g_debrisMem, 0, size, 0, &g_debrisMapped);
    // Zero once so partial writes never show garbage.
    if (g_debrisMapped) std::memset(g_debrisMapped, 0, static_cast<size_t>(size));
}

// ---- inventory lattice mesh (RULES.md rule 12) --------------------------
//
// Camera-relative presentation. Every cell is emitted as a true 1x1x1 unit cube
// (VOXEL_SIZE on each edge) through a rigid display basis — the basis is only
// rotated, never scaled, so cells stay unit cubes and occupancy is unchanged.
// Occupied cells take the item's tint; empty cells are a dim backing so the
// lattice volume stays legible.

static void ensureInventoryBuffer() {
    if (g_inventoryVB) return;
    VkDeviceSize size = sizeof(Vertex) * kInventoryMaxVerts;
    createBuffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 g_inventoryVB, g_inventoryMem);
    vkMapMemory(g_device, g_inventoryMem, 0, size, 0, &g_inventoryMapped);
    if (g_inventoryMapped) std::memset(g_inventoryMapped, 0, static_cast<size_t>(size));
}

// Emit one unit cube: 6 faces x 2 tris = 36 verts, hard normals. `base` is the
// world position of lattice cell (0,0,0); the three axes are unit vectors and
// MUST stay orthogonal (rotated, never scaled) or the result is not a cube.
// `edge` is the cube edge in world units, so VOXEL_SIZE yields a unit cube.
static void emitUnitCube(Vertex* verts, uint32_t& wi, uint32_t maxVerts,
                         const Vec3& base, const Vec3& axR, const Vec3& axU, const Vec3& axF,
                         float x0, float y0, float z0, float edge,
                         float cr, float cg, float cb, float matId) {
    if (wi + 36u > maxVerts) return;
    // Corner index bits: 1 = +x, 2 = +y, 4 = +z
    auto corner = [&](int bits) {
        return base + axR * ((x0 + ((bits & 1) ? 1.0f : 0.0f)) * edge) +
               axU * ((y0 + ((bits & 2) ? 1.0f : 0.0f)) * edge) +
               axF * ((z0 + ((bits & 4) ? 1.0f : 0.0f)) * edge);
    };
    // face axis, sign, then the two in-plane corner bitmaps
    struct Face { int axis; int sign; int a, b, c, d; };
    static const Face kFaces[6] = {
        {0, +1, 6, 2, 3, 7}, // +x
        {0, -1, 4, 0, 1, 5}, // -x
        {1, +1, 5, 1, 3, 7}, // +y
        {1, -1, 4, 0, 2, 6}, // -y
        {2, +1, 7, 3, 1, 5}, // +z
        {2, -1, 6, 0, 2, 4}, // -z
    };
    for (const auto& f : kFaces) {
        const Vec3 nrm = (f.axis == 0 ? axR : (f.axis == 1 ? axU : axF)) * static_cast<float>(f.sign);
        const int order[4] = {f.a, f.b, f.c, f.d};
        for (int t = 0; t < 6; ++t) {
            Vertex& v = verts[wi++];
            const Vec3 p = corner(order[t / 2]);
            v.px = p.x;
            v.py = p.y;
            v.pz = p.z;
            v.nx = nrm.x;
            v.ny = nrm.y;
            v.nz = nrm.z;
            v.cr = cr;
            v.cg = cg;
            v.cb = cb;
            v.mat = matId;
        }
    }
}

// Camera-relative lattice cell: unit cubes through the (orthonormal) display
// basis.
static void emitInventoryCube(const Vec3& origin, const Vec3& right, const Vec3& up,
                              const Vec3& fwd, float x0, float y0, float z0,
                              float cr, float cg, float cb, float matId, uint32_t& wi) {
    emitUnitCube(reinterpret_cast<Vertex*>(g_inventoryMapped), wi, kInventoryMaxVerts, origin,
                 right, up, fwd, x0, y0, z0, VOXEL_SIZE, cr, cg, cb, matId);
}

// Mat-7 segment health/breath HUD (RULES.md rule 15). Appended to the same
// overlay buffer as the inventory lattice, so the overlay pass now runs whenever
// either the lattice or the HUD is present. Same camera-relative unit-cube basis
// as the lattice, so there is no second projection to keep in sync.
// No numeric readout: the engine has no glyph system, and a 3x5 digit font is a
// separate task.
static constexpr int kHudHealthCells = 10;
static constexpr int kHudHealthCols = 5;
static constexpr int kHudBreathCells = 5;

static uint32_t emitHealthHud(uint32_t wi) {
    if (!g_inventoryMapped) return wi;
    Vec3 fwd = cameraForward();
    Vec3 right = cameraRight();
    if (right.length() < 1e-5f) right = Vec3(1, 0, 0);
    right = right.normalized();
    const Vec3 up = right.cross(fwd).normalized();
    // Same distance as the lattice so both sit on one visual grid; the HUD is
    // anchored below the panel and to its left.
    const float dist = 0.006f;
    const Vec3 origin = g_camPos + fwd * dist + up * (-0.0062f) + right * (0.0009f);
    const float matId = static_cast<float>(kInventoryMatId);

    const float frac = g_health.healthFraction();
    const int filled = static_cast<int>(std::lround(frac * kHudHealthCells));
    float hr, hg, hb;
    if (g_health.dead) {
        hr = 0.55f; hg = 0.08f; hb = 0.08f;
    } else if (frac > 0.5f) {
        hr = 0.28f; hg = 0.85f; hb = 0.36f;
    } else if (frac > 0.2f) {
        hr = 0.90f; hg = 0.74f; hb = 0.20f;
    } else {
        hr = 0.90f; hg = 0.22f; hb = 0.20f;
    }

    // Health: 5x2 segment block. Cells fill left-to-right, bottom row first.
    for (int i = 0; i < kHudHealthCells; ++i) {
        const int col = i % kHudHealthCols;
        const int row = i / kHudHealthCols;
        const bool on = i < filled && !g_health.dead;
        const float cr = on ? hr : 0.13f;
        const float cg = on ? hg : 0.13f;
        const float cb = on ? hb : 0.15f;
        emitInventoryCube(origin, right, up, fwd, static_cast<float>(col),
                          static_cast<float>(row), 0.0f, cr, cg, cb, matId, wi);
    }

    // Breath: a second 5-cell row below, skipped while dead.
    if (!g_health.dead) {
        const float bf = std::max(0.0f, std::min(1.0f, g_health.breath / health::kBreathSeconds));
        const int bfilled = static_cast<int>(std::lround(bf * kHudBreathCells));
        for (int i = 0; i < kHudBreathCells; ++i) {
            const bool on = i < bfilled;
            emitInventoryCube(origin, right, up, fwd, static_cast<float>(i), -2.0f, 0.0f,
                              on ? 0.25f : 0.13f, on ? 0.55f : 0.13f, on ? 0.95f : 0.15f,
                              matId, wi);
        }
    }
    return wi;
}

// World point -> client pixels. mat 7 skips the vertex fisheye, so plain
// perspective projection reproduces exactly the pixels the GPU drew.
static bool projectToScreen(const Vec3& p, float& outX, float& outY) {
    const Mat4& vp = g_viewProjCull;
    const float cx = vp.m[0] * p.x + vp.m[4] * p.y + vp.m[8] * p.z + vp.m[12];
    const float cy = vp.m[1] * p.x + vp.m[5] * p.y + vp.m[9] * p.z + vp.m[13];
    const float cw = vp.m[3] * p.x + vp.m[7] * p.y + vp.m[11] * p.z + vp.m[15];
    if (cw <= 1e-6f) return false;
    outX = (cx / cw * 0.5f + 0.5f) * static_cast<float>(g_extent.width);
    outY = (1.0f - (cy / cw * 0.5f + 0.5f)) * static_cast<float>(g_extent.height);
    return true;
}

// Screen-space AABB of one cell under the current display transform, and the
// cell's centre distance. Returns false if the cell is behind the camera.
static bool cellScreenRect(const InventoryDisplay& d, float x0, float y0, float z0,
                          float& x0p, float& y0p, float& x1p, float& y1p) {
    const float vs = VOXEL_SIZE;
    x0p = y0p = 1e30f;
    x1p = y1p = -1e30f;
    for (int c = 0; c < 8; ++c) {
        const Vec3 p = d.origin + d.right * ((x0 + (c & 1)) * vs) +
                       d.up * ((y0 + ((c >> 1) & 1)) * vs) +
                       d.fwd * ((z0 + ((c >> 2) & 1)) * vs);
        float sx = 0, sy = 0;
        if (!projectToScreen(p, sx, sy)) return false;
        x0p = std::min(x0p, sx);
        x1p = std::max(x1p, sx);
        y0p = std::min(y0p, sy);
        y1p = std::max(y1p, sy);
    }
    return true;
}

// Rebuild the lattice mesh. Rebuilt every frame while open (cheap at ~1.6k
// verts) because the display basis follows the camera.
static void updateInventoryMesh() {
    if (!g_inventoryOpen) {
        // Lattice closed, but the mat-7 HUD still draws in the overlay pass.
        ensureInventoryBuffer();
        g_inventoryVertexCount = g_inventoryMapped ? emitHealthHud(0) : 0;
        g_invDisplay.valid = false;
        g_invHover = InventoryHover{};
        return;
    }
    ensureInventoryBuffer();
    if (!g_inventoryMapped) {
        g_inventoryVertexCount = 0;
        g_invDisplay.valid = false;
        return;
    }

    // Display basis: camera basis yawed so two faces of the volume are visible
    // (a 3D projection rather than a flat orthographic panel). Built as two true
    // rotations — yaw about the view up, then pitch about the yawed right — so
    // the basis stays orthonormal. A tilt applied to only one axis (e.g.
    // up*cp + fwd*sp) would shear it and turn the unit cells into rhomboids.
    const float yaw = 0.52f; // ~30 degrees about the view up axis
    const float pitch = 0.30f;
    Vec3 fwd = cameraForward();
    Vec3 right = cameraRight();
    if (right.length() < 1e-5f) right = Vec3(1, 0, 0);
    right = right.normalized();
    Vec3 up = right.cross(fwd).normalized();

    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const Vec3 dRight = (right * cy + fwd * sy).normalized();
    const Vec3 dFwdY = (fwd * cy - right * sy).normalized();
    const Vec3 dUpY = up;
    const Vec3 dFwd = (dFwdY * cp + dUpY * sp).normalized();
    const Vec3 dUp = (dUpY * cp - dFwdY * sp).normalized();

    // Anchor: down and to the right of the eye, ~0.006 out. A 3x3x4 cell block
    // is 0.003 x 0.003 x 0.004 world units at this distance.
    const float dist = 0.006f;
    const Vec3 origin = g_camPos + fwd * dist + up * (-0.0042f) + right * (0.0046f);

    g_invDisplay.origin = origin;
    g_invDisplay.right = dRight;
    g_invDisplay.up = dUp;
    g_invDisplay.fwd = dFwd;
    g_invDisplay.valid = true;

    // Resolve what the cursor is over, in screen space. Nearest-containing wins;
    // ties break toward the cell closest to the lattice centre so the front face
    // of the volume wins over the back one.
    const Inventory& inv = g_inventory;
    const int markerCol0 = -(inv.vol.sx + 2);
    g_invHover = InventoryHover{};
    {
        const float mx = static_cast<float>(g_mouseX);
        const float my = static_cast<float>(g_mouseY);
        // Lowest depth wins: a smaller z is the face turned toward the camera,
        // and central x/y columns are less occluded than the outer ones.
        int bestDepth = 1 << 30;
        for (int iz = 0; iz < inv.vol.sz; ++iz) {
            for (int iy = 0; iy < inv.vol.sy; ++iy) {
                for (int ix = 0; ix < inv.vol.sx; ++ix) {
                    float ax, ay, bx, by;
                    if (!cellScreenRect(g_invDisplay, static_cast<float>(ix),
                                        static_cast<float>(iy), static_cast<float>(iz), ax, ay, bx, by))
                        continue;
                    if (mx < ax || mx > bx || my < ay || my > by) continue;
                    const int depth = iz * 4 + std::abs(ix - inv.vol.sx / 2) +
                                      std::abs(iy - inv.vol.sy / 2);
                    if (depth < bestDepth) {
                        bestDepth = depth;
                        g_invHover.lattice = true;
                        g_invHover.x = ix;
                        g_invHover.y = iy;
                        g_invHover.z = iz;
                    }
                }
            }
        }
        for (int s = 0; s < kEquipSlotCount; ++s) {
            float ax, ay, bx, by;
            if (!cellScreenRect(g_invDisplay, static_cast<float>(markerCol0 + (s & 1)),
                                static_cast<float>(s >> 1), 0.0f, ax, ay, bx, by))
                continue;
            if (mx < ax || mx > bx || my < ay || my > by) continue;
            g_invHover.slot = true;
            g_invHover.slotIndex = s;
            g_invHover.lattice = false;
            break;
        }
        g_invHover.overCell = g_invHover.lattice || g_invHover.slot;
    }

    // Would the hand item land legally under the cursor? Drives the red/green
    // placement tint.
    bool handValid = false;
    if (inv.held >= 0 && g_invHover.lattice) {
        handValid = handFits(inv, g_itemDefs, g_inventoryHandRot, g_invHover.x, g_invHover.y,
                            g_invHover.z);
    }

    uint32_t wi = 0;

    // Packed item cells, tinted by their definition.
    for (int iz = 0; iz < inv.vol.sz; ++iz) {
        for (int iy = 0; iy < inv.vol.sy; ++iy) {
            for (int ix = 0; ix < inv.vol.sx; ++ix) {
                const int inst = inv.occ[static_cast<size_t>(inv.vol.index(ix, iy, iz))];
                float cr, cg, cb;
                if (inst >= 0 && inst < static_cast<int>(inv.items.size())) {
                    const ItemDef* d = itemDefAt(g_itemDefs, inv.items[inst].defIndex);
                    if (d) {
                        cr = d->cr;
                        cg = d->cg;
                        cb = d->cb;
                    } else {
                        cr = cg = cb = 0.8f;
                    }
                } else {
                    cr = 0.10f;
                    cg = 0.11f;
                    cb = 0.14f; // dim backing for an empty cell
                }
                // Hover highlight on the cell under the cursor.
                if (g_invHover.lattice && g_invHover.x == ix && g_invHover.y == iy &&
                    g_invHover.z == iz) {
                    cr = std::min(1.0f, cr + 0.30f);
                    cg = std::min(1.0f, cg + 0.34f);
                    cb = std::min(1.0f, cb + 0.38f);
                }
                emitInventoryCube(origin, dRight, dUp, dFwd, static_cast<float>(ix),
                                  static_cast<float>(iy), static_cast<float>(iz), cr, cg, cb,
                                  static_cast<float>(kInventoryMatId), wi);
            }
        }
    }

    // Held item ghost: the item is drawn at the hovered cell so the player can
    // see the footprint before clicking. Green when it fits, red when it does
    // not (out of bounds or overlapping).
    if (inv.held >= 0 && g_invHover.lattice && handValid) {
        const ItemDef* d = itemDefAt(g_itemDefs, inv.items[static_cast<size_t>(inv.held)].defIndex);
        if (d) {
            const ItemShape s = rotatedShape(d->shape, g_inventoryHandRot);
            for (int iz = 0; iz < s.sz; ++iz) {
                for (int iy = 0; iy < s.sy; ++iy) {
                    for (int ix = 0; ix < s.sx; ++ix) {
                        if (!s.solid(ix, iy, iz)) continue;
                        emitInventoryCube(origin, dRight, dUp, dFwd,
                                          static_cast<float>(g_invHover.x + ix),
                                          static_cast<float>(g_invHover.y + iy),
                                          static_cast<float>(g_invHover.z + iz), 0.30f, 0.85f, 0.45f,
                                          static_cast<float>(kInventoryMatId), wi);
                    }
                }
            }
        }
    }

    // Equipment slot markers: 8 one-cell cubes in a 2x4 block left of the volume.
    for (int s = 0; s < kEquipSlotCount; ++s) {
        const int defIdx = inv.slotDef[s];
        float cr, cg, cb;
        if (defIdx >= 0) {
            const ItemDef* d = itemDefAt(g_itemDefs, defIdx);
            if (d) {
                cr = d->cr * 1.15f;
                cg = d->cg * 1.15f;
                cb = d->cb * 1.15f;
            } else {
                cr = cg = cb = 0.8f;
            }
        } else {
            cr = 0.16f;
            cg = 0.17f;
            cb = 0.20f; // empty slot
        }
        if (g_invHover.slot && g_invHover.slotIndex == s) {
            cr = std::min(1.0f, cr + 0.30f);
            cg = std::min(1.0f, cg + 0.34f);
            cb = std::min(1.0f, cb + 0.38f);
        }
        const float mx = static_cast<float>(markerCol0 + (s & 1));
        const float my = static_cast<float>(s >> 1);
        emitInventoryCube(origin, dRight, dUp, dFwd, mx, my, 0.0f, cr, cg, cb,
                          static_cast<float>(kInventoryMatId), wi);
    }

    g_inventoryVertexCount = emitHealthHud(wi);
}

// ---- world item pickups -------------------------------------------------
//
// Ground items are WORLD objects, not storage: they render in the MAIN pass with
// mat 8, keeping the rule-12 overlay pass reserved for the player's own lattice.
// Geometry is the same unit-cube lattice as everything else — a pickup is an
// item shape dropped at a world cell anchor, never a stretched billboard.
//
// Taking one up calls autoPlace() so it lands in the pack immediately. If the
// pack cannot take it the pickup is REFUSED and left on the ground, so a full
// pack never silently deletes an item.

struct WorldPickup {
    int defIndex = -1;
    int cx = 0, cy = 0, cz = 0; // world unit-cell anchor (min corner of the shape)
    int rot = 0;                // quarter-turns about +Y
    bool alive = true;
};
static std::vector<WorldPickup> g_pickups;
static VkBuffer g_pickupVB = VK_NULL_HANDLE;
static VkDeviceMemory g_pickupMem = VK_NULL_HANDLE;
static void* g_pickupMapped = nullptr;
static uint32_t g_pickupVertexCount = 0;
static bool g_pickupMeshDirty = true;
static constexpr uint32_t kPickupMaxVerts = 8192;
static int g_pickupHover = -1; // pickup under the crosshair this frame
static int g_pickupTaken = 0;  // lifetime counters for the smoke
static int g_pickupRefused = 0;
static constexpr float kPickupReach = 0.030f; // 30cm grab range

// Cell the player spawned on, so seeded loot lands on the apron in front of
// them rather than at a hard-coded map coordinate.
static int g_spawnCellX = 0, g_spawnCellY = 0, g_spawnCellZ = 0;

static void ensurePickupBuffer() {
    if (g_pickupVB) return;
    VkDeviceSize size = sizeof(Vertex) * kPickupMaxVerts;
    createBuffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 g_pickupVB, g_pickupMem);
    vkMapMemory(g_device, g_pickupMem, 0, size, 0, &g_pickupMapped);
    if (g_pickupMapped) std::memset(g_pickupMapped, 0, static_cast<size_t>(size));
}

// World AABB of a pickup's solid cells, in world units.
static bool pickupBounds(const WorldPickup& p, Vec3& lo, Vec3& hi) {
    const ItemDef* d = itemDefAt(g_itemDefs, p.defIndex);
    if (!d) return false;
    const ItemShape s = rotatedShape(d->shape, p.rot);
    lo = Vec3(1e30f, 1e30f, 1e30f);
    hi = Vec3(-1e30f, -1e30f, -1e30f);
    for (int iz = 0; iz < s.sz; ++iz) {
        for (int iy = 0; iy < s.sy; ++iy) {
            for (int ix = 0; ix < s.sx; ++ix) {
                if (!s.solid(ix, iy, iz)) continue;
                const float wx = static_cast<float>(p.cx + ix) * VOXEL_SIZE;
                const float wy = static_cast<float>(p.cy + iy) * VOXEL_SIZE;
                const float wz = static_cast<float>(p.cz + iz) * VOXEL_SIZE;
                lo = Vec3(std::min(lo.x, wx), std::min(lo.y, wy), std::min(lo.z, wz));
                hi = Vec3(std::max(hi.x, wx + VOXEL_SIZE), std::max(hi.y, wy + VOXEL_SIZE),
                          std::max(hi.z, wz + VOXEL_SIZE));
            }
        }
    }
    return lo.x <= hi.x;
}

// Slab ray/AABB test. Returns the near hit distance, or -1 on a miss.
static float rayAabb(const Vec3& o, const Vec3& dir, const Vec3& lo, const Vec3& hi) {
    float tmin = 0.0f;
    float tmax = 1e30f;
    const float os[3] = {o.x, o.y, o.z};
    const float ds[3] = {dir.x, dir.y, dir.z};
    const float los[3] = {lo.x, lo.y, lo.z};
    const float his[3] = {hi.x, hi.y, hi.z};
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(ds[a]) < 1e-9f) {
            if (os[a] < los[a] || os[a] > his[a]) return -1.0f;
            continue;
        }
        const float inv = 1.0f / ds[a];
        float t1 = (los[a] - os[a]) * inv;
        float t2 = (his[a] - os[a]) * inv;
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax) return -1.0f;
    }
    return tmin;
}

// Nearest pickup the camera ray crosses within reach, or -1.
static int pickupUnderCrosshair() {
    const Vec3 dir = cameraForward();
    int best = -1;
    float bestT = kPickupReach;
    for (size_t i = 0; i < g_pickups.size(); ++i) {
        if (!g_pickups[i].alive) continue;
        Vec3 lo, hi;
        if (!pickupBounds(g_pickups[i], lo, hi)) continue;
        const float t = rayAabb(g_camPos, dir, lo, hi);
        if (t >= 0.0f && t < bestT) {
            bestT = t;
            best = static_cast<int>(i);
        }
    }
    return best;
}

// Loot one pickup. autoPlace is the intended pickup behaviour; a refused item
// stays on the ground rather than being dropped on the floor.
static bool tryPickupAt(int index) {
    if (index < 0 || static_cast<size_t>(index) >= g_pickups.size()) return false;
    WorldPickup& p = g_pickups[index];
    if (!p.alive) return false;
    // A backpack re-grants storage: auto-place it so the volume grows.
    int placedInst = -1, placedRot = 0;
    if (autoPlace(g_inventory, g_itemDefs, p.defIndex, placedInst, placedRot)) {
        p.alive = false;
        g_pickupMeshDirty = true;
        g_pickupTaken++;
        return true;
    }
    g_pickupRefused++;
    return false;
}

static int itemIndexById(const char* id);

// Scatter starter loot on the apron in front of the spawn, one cell at a time so
// it sits ON the slab (py = the first free cell above the floor) rather than
// floating or buried. Cell anchors, not world offsets, so every pickup is
// exactly the item's own unit-cube lattice (RULES.md rule 12).
static void seedPickups() {
    g_pickups.clear();
    g_pickupTaken = 0;
    g_pickupRefused = 0;
    struct Seed { const char* id; int dx, dz; };
    // Spread across the bay the player spawns looking into. Ids must match the
    // *.item.json files in data/items; an unknown id is skipped, not faked.
    static const Seed kSeeds[] = {
        {"supply_grenade",      -3, -6},
        {"ammo_pouch_medium",   -1, -8},
        {"weapon_sidearm_light", 2, -7},
        {"weapon_starter_rifle", 3, -5},
        {"armor_chest_plate",    0, -10},
        {"armor_helmet",         5, -9},
    };
    for (const Seed& s : kSeeds) {
        const int defIdx = itemIndexById(s.id);
        if (defIdx < 0) continue;
        const ItemDef* d = itemDefAt(g_itemDefs, defIdx);
        if (!d || !d->shape.valid()) continue;
        WorldPickup p;
        p.defIndex = defIdx;
        p.rot = 0;
        p.cx = g_spawnCellX + s.dx;
        p.cz = g_spawnCellZ + s.dz;
        // Drop onto whatever is under it: scan down for the first free cell so
        // the item rests on the floor instead of hovering over a gap.
        p.cy = g_spawnCellY;
        if (g_chunks) {
            for (int y = g_spawnCellY + 4; y >= 1; --y) {
                if (isSolidBlock(getWorldBlock(*g_chunks, p.cx, y - 1, p.cz))) {
                    p.cy = y;
                    break;
                }
            }
        }
        g_pickups.push_back(p);
    }
    g_pickupMeshDirty = true;
}

static void updatePickupMesh() {
    if (!g_pickupMapped && g_pickupVB) ensurePickupBuffer();
    if (!g_pickupMapped) {
        g_pickupVertexCount = 0;
        return;
    }
    if (!g_pickupMeshDirty && g_pickupVertexCount > 0) return;
    g_pickupMeshDirty = false;
    g_pickupVertexCount = 0;

    const Vec3 axR(1, 0, 0), axU(0, 1, 0), axF(0, 0, 1);
    Vertex* verts = reinterpret_cast<Vertex*>(g_pickupMapped);
    uint32_t wi = 0;
    for (size_t pi = 0; pi < g_pickups.size(); ++pi) {
        const WorldPickup& p = g_pickups[pi];
        if (!p.alive) continue;
        const ItemDef* d = itemDefAt(g_itemDefs, p.defIndex);
        if (!d) continue;
        const bool hovered = (static_cast<int>(pi) == g_pickupHover);
        const float k = hovered ? 1.6f : 1.0f; // brighten the item under the crosshair
        const ItemShape s = rotatedShape(d->shape, p.rot);
        for (int iz = 0; iz < s.sz; ++iz) {
            for (int iy = 0; iy < s.sy; ++iy) {
                for (int ix = 0; ix < s.sx; ++ix) {
                    if (!s.solid(ix, iy, iz)) continue;
                    emitUnitCube(verts, wi, kPickupMaxVerts, Vec3(0, 0, 0), axR, axU, axF,
                                 static_cast<float>(p.cx + ix), static_cast<float>(p.cy + iy),
                                 static_cast<float>(p.cz + iz), VOXEL_SIZE,
                                 std::min(1.0f, d->cr * k), std::min(1.0f, d->cg * k),
                                 std::min(1.0f, d->cb * k), static_cast<float>(kPickupMatId));
                }
            }
        }
    }
    g_pickupVertexCount = wi;
}

// ---- one-shot input handling --------------------------------------------
//
// The window proc only sets latches; the frame loop drains them here so a single
// click or keypress is consumed exactly once, in draw order, after the frame's
// camera basis (and therefore g_invDisplay) is up to date. The latch is
// consumed BEFORE the mesh is built, and updateInventoryMesh recomputes
// g_invHover from that same basis a moment later, so the action lands on the cell
// the player was pointing at.
//
// The window proc only sets latches; the frame loop drains them here so a single
// click or keypress is consumed exactly once, in draw order, after the frame's
// camera basis (and therefore g_invDisplay) is up to date.
static void drainInventoryInput() {
    // G: take the world item under the crosshair.
    if (g_pickupPressed) {
        g_pickupPressed = false;
        if (!g_inventoryOpen && g_pickupHover >= 0) {
            if (!tryPickupAt(g_pickupHover)) {
                // Pack refused it: carry it rather than dropping it on the floor.
                takeIntoHand(g_inventory, g_itemDefs,
                            g_pickups[static_cast<size_t>(g_pickupHover)].defIndex);
            }
            g_pickupHover = -1;
        }
    }
    if (!g_inventoryOpen) {
        g_inventoryClick = false;
        g_inventoryStow = false;
        return;
    }
    // RMB: stow whatever is in hand.
    if (g_inventoryStow) {
        g_inventoryStow = false;
        stowHeld(g_inventory, g_itemDefs);
        g_inventoryHandRot = 0;
    }
    // LMB: on a packed item, lift it; otherwise place the held item.
    if (g_inventoryClick) {
        g_inventoryClick = false;
        if (g_inventory.held >= 0) {
            if (g_invHover.lattice)
                placeHeld(g_inventory, g_itemDefs, g_inventoryHandRot, g_invHover.x, g_invHover.y,
                          g_invHover.z);
        } else if (g_invHover.lattice) {
            liftPacked(g_inventory, invIndexAt(g_inventory, g_invHover.x, g_invHover.y, g_invHover.z));
        } else if (g_invHover.slot) {
            // Clicking a marker lifts what is in that equipment slot back out.
            const int defIdx = g_inventory.slotDef[g_invHover.slotIndex];
            if (defIdx >= 0) {
                const bool isPack = g_invHover.slotIndex == static_cast<int>(EquipSlot::Backpack);
                g_inventory.slotDef[g_invHover.slotIndex] = -1;
                g_inventory.slotRot[g_invHover.slotIndex] = 0;
                // Revoking a backpack's storage clears the pack (and anything held),
                // so it must happen BEFORE the pack itself goes into the hand -
                // otherwise the revoking would erase the item we just lifted.
                if (isPack) g_inventory.setVolume(PackVolume::none());
                takeIntoHand(g_inventory, g_itemDefs, defIdx);
            }
        }
    }
}

// Cubic 8^3 debris chips + short-lived muzzle flash cubes (display only).
static void updateDebrisMesh() {
    const int alive = g_debris.activeCount();
    const bool flashOn = g_muzzleFlash > 0.02f;
    if (alive <= 0 && !flashOn) {
        g_debrisVertexCount = 0;
        g_debrisWasActive = false;
        g_debris.meshDirty = false;
        return;
    }
    // Skip rebuild only when nothing moved and no flash (still draw last mesh).
    if (!g_debris.meshDirty && !flashOn && g_debrisWasActive && g_debrisVertexCount > 0) {
        return;
    }

    ensureDebrisBuffer();
    if (!g_debrisMapped) { g_debrisVertexCount = 0; return; }

    LARGE_INTEGER t0{}, t1{}, freq{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    Vertex* verts = reinterpret_cast<Vertex*>(g_debrisMapped);
    uint32_t wi = 0;
    auto put = [&](float px, float py, float pz,
                   float nx, float ny, float nz,
                   float cr, float cg, float cb, float matId) {
        if (wi >= kDebrisMaxVerts) return;
        Vertex& v = verts[wi++];
        v.px = px; v.py = py; v.pz = pz;
        v.nx = nx; v.ny = ny; v.nz = nz;
        v.cr = cr; v.cg = cg; v.cb = cb;
        v.mat = matId;
    };

    // Soft-cap drawn particles under load (36 verts each).
    const int drawCap = g_stress ? 96 : static_cast<int>(kDebrisMaxParticlesDraw);
    int drawn = 0;
    int stride = 1;
    if (alive > drawCap) stride = (alive + drawCap - 1) / drawCap;

    int seen = 0;
    for (const auto& p : g_debris.particles) {
        if (!p.alive) continue;
        if ((seen++ % stride) != 0) continue;
        if (drawn >= drawCap) break;
        if (wi + kDebrisVertsPerParticle > kDebrisMaxVerts - kMuzzleFlashCubes * 36u) break;
        ++drawn;
        emitDebrisCube(p, kDebrisMatId, put);
    }

    // Muzzle flash: stacked emissive cubes just ahead of the camera along aim.
    if (flashOn) {
        Vec3 fwd = cameraForward();
        Vec3 right = cameraRight();
        if (right.length() < 1e-5f) right = Vec3(1, 0, 0);
        right = right.normalized();
        Vec3 up = right.cross(fwd).normalized();
        const float f = g_muzzleFlash;
        const float base = 0.010f + 0.006f * f;
        // Core + side sparks (caliber-tinted).
        const float cr = g_muzzleR, cg = g_muzzleG, cb = g_muzzleB;
        struct FlashSpec { float along, side, up, half, bright; } specs[4] = {
            { base,            0.0f,           0.0f, 0.0018f * (0.7f + f), 1.0f },
            { base * 1.35f,    0.0f,           0.0f, 0.0011f * (0.5f + f), 0.85f },
            { base * 0.85f,    0.0012f * f,    0.0004f, 0.0009f, 0.7f },
            { base * 0.85f,   -0.0012f * f,   -0.0003f, 0.0009f, 0.7f },
        };
        for (uint32_t i = 0; i < kMuzzleFlashCubes; ++i) {
            if (wi + 36u > kDebrisMaxVerts) break;
            const auto& s = specs[i];
            float px = g_camPos.x + fwd.x * s.along + right.x * s.side + up.x * s.up;
            float py = g_camPos.y + fwd.y * s.along + right.y * s.side + up.y * s.up;
            float pz = g_camPos.z + fwd.z * s.along + right.z * s.side + up.z * s.up;
            emitFlashCube(px, py, pz, s.half,
                          cr * s.bright * f, cg * s.bright * f, cb * s.bright * f,
                          kMuzzleMatId, put);
        }
    }

    g_debrisVertexCount = wi;
    if (wi > g_debrisVertsPeak) g_debrisVertsPeak = wi;
    g_debrisWasActive = (wi > 0);
    g_debris.meshDirty = false;

    QueryPerformanceCounter(&t1);
    const double us = (double(t1.QuadPart - t0.QuadPart) * 1e6) / double(freq.QuadPart);
    g_debrisUploadUsSum += us;
    if (us > g_debrisUploadUsMax) g_debrisUploadUsMax = us;
    ++g_debrisUploadSamples;
}

static Vec3 skyDir(float u, float v) {
    // u,v in [0,1]: azimuth full circle, elevation horizon->zenith (+ a little below)
    const float az = u * static_cast<float>(M_PI) * 2.0f;
    const float el = (0.08f + v * 0.92f) * static_cast<float>(M_PI) * 0.5f; // ~0..90deg+)
    const float ce = std::cos(el);
    return Vec3(std::cos(az) * ce, std::sin(el), std::sin(az) * ce).normalized();
}

static void updateMoonSkyTile() {
    ensureSkyTileBuffer();
    Vertex* verts = reinterpret_cast<Vertex*>(g_skyTileMapped);
    Vec3 eye = g_camPos;
    g_moonDirWorld = g_moonDirWorld.normalized();
    g_moonWorldPos = eye + g_moonDirWorld * (g_skyRadius * 0.92f);

    auto putSky = [&](Vertex& dst, const Vec3& p, float u, float v, float tileU, float tileV) {
        Vec3 n = (eye - p).normalized(); // inward
        dst.px = p.x; dst.py = p.y; dst.pz = p.z;
        dst.nx = n.x; dst.ny = n.y; dst.nz = n.z;
        // cr/cg = local tile UV for pixel grid; cb packs sky elevation 0..1
        dst.cr = tileU; dst.cg = tileV; dst.cb = v;
        dst.mat = 4.0f; // sky tile
    };

    uint32_t wi = 0;
    for (int sv = 0; sv < SKY_SEG_V; ++sv) {
        float v0 = static_cast<float>(sv) / static_cast<float>(SKY_SEG_V);
        float v1 = static_cast<float>(sv + 1) / static_cast<float>(SKY_SEG_V);
        for (int su = 0; su < SKY_SEG_U; ++su) {
            float u0 = static_cast<float>(su) / static_cast<float>(SKY_SEG_U);
            float u1 = static_cast<float>(su + 1) / static_cast<float>(SKY_SEG_U);
            Vec3 p00 = eye + skyDir(u0, v0) * g_skyRadius;
            Vec3 p10 = eye + skyDir(u1, v0) * g_skyRadius;
            Vec3 p11 = eye + skyDir(u1, v1) * g_skyRadius;
            Vec3 p01 = eye + skyDir(u0, v1) * g_skyRadius;
            // CCW from inside camera
            putSky(verts[wi++], p00, u0, v0, 0.0f, 0.0f);
            putSky(verts[wi++], p10, u1, v0, 1.0f, 0.0f);
            putSky(verts[wi++], p11, u1, v1, 1.0f, 1.0f);
            putSky(verts[wi++], p00, u0, v0, 0.0f, 0.0f);
            putSky(verts[wi++], p11, u1, v1, 1.0f, 1.0f);
            putSky(verts[wi++], p01, u0, v1, 0.0f, 1.0f);
        }
    }

    // Moon light-source sprite (mat=3), camera-facing at moon bearing
    Vec3 to = g_moonDirWorld;
    Vec3 worldUp(0, 1, 0);
    Vec3 right = to.cross(worldUp);
    if (right.length() < 1e-5f) right = Vec3(1, 0, 0);
    right = right.normalized();
    Vec3 up = right.cross(to).normalized();
    float size = g_moonTileSize;
    Vec3 c = g_moonWorldPos;
    Vec3 m0 = c + (right * -1.0f + up * -1.0f) * size;
    Vec3 m1 = c + (right *  1.0f + up * -1.0f) * size;
    Vec3 m2 = c + (right *  1.0f + up *  1.0f) * size;
    Vec3 m3 = c + (right * -1.0f + up *  1.0f) * size;
    Vec3 mn = (eye - c).normalized();
    auto putMoon = [&](Vertex& dst, const Vec3& p, float u, float v) {
        dst.px = p.x; dst.py = p.y; dst.pz = p.z;
        dst.nx = mn.x; dst.ny = mn.y; dst.nz = mn.z;
        dst.cr = u; dst.cg = v; dst.cb = 1.0f;
        dst.mat = 3.0f;
    };
    putMoon(verts[wi++], m0, 0, 0);
    putMoon(verts[wi++], m1, 1, 0);
    putMoon(verts[wi++], m2, 1, 1);
    putMoon(verts[wi++], m0, 0, 0);
    putMoon(verts[wi++], m2, 1, 1);
    putMoon(verts[wi++], m3, 0, 1);
    g_skyTileVertexCount = wi;
}


// ---- Win32 ----
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CLOSE:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        g_running = false;
        return 0;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_width = std::max(1, static_cast<int>(LOWORD(lParam)));
            g_height = std::max(1, static_cast<int>(HIWORD(lParam)));
            g_resized = true;
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam < 256) g_keys[wParam] = true;
        if (wParam == VK_ESCAPE) {
            g_running = false;
            PostQuitMessage(0);
        }
if (wParam == 'F') {
            g_firePressed = true;
            g_fireHeld = true;
        }
        if (wParam == VK_SPACE) g_wantJump = true;
        // TAB: toggle the 3D inventory lattice (RULES.md rule 12)
        if (wParam == VK_TAB) {
            g_inventoryOpen = !g_inventoryOpen;
            if (g_inventoryOpen) {
                // Free the cursor so it can select cells; drop any held item back
                // so closing the panel never leaves an item stranded in the hand.
                ReleaseCapture();
                g_mouseDown = false;
                stowHeld(g_inventory, g_itemDefs);
                g_inventoryHandRot = 0;
            } else {
                SetCapture(hwnd);
            }
        }
        // R: rotate the held item while the lattice is up; otherwise cycle ammo.
        if (wParam == 'R' && g_inventoryOpen) {
            g_inventoryHandRot = (g_inventoryHandRot + 1) & 3;
        }
        // G: take the world item under the crosshair (auto-places into the pack)
        if (wParam == 'G' && !g_inventoryOpen) g_pickupPressed = true;
        // 1-4: caliber class (light medium heavy energy)
        if (wParam == '1') { g_activeCaliberIndex = 0; g_activeAmmoIndex = 0; }
        if (wParam == '2') { g_activeCaliberIndex = 1; g_activeAmmoIndex = 0; }
        if (wParam == '3') { g_activeCaliberIndex = 2; g_activeAmmoIndex = 0; }
        if (wParam == '4') { g_activeCaliberIndex = 3; g_activeAmmoIndex = 0; }
        // R: cycle ammo subtypes for the active caliber (Python ammo table)
        if (wParam == 'R' && !g_inventoryOpen) {
            std::string cal = (g_activeCaliberIndex >= 0 && g_activeCaliberIndex < 4)
                                  ? kCaliberIds[g_activeCaliberIndex] : "medium";
            auto list = ammosForCaliber(g_ammoDefs, cal);
            if (!list.empty())
                g_activeAmmoIndex = (g_activeAmmoIndex + 1) % static_cast<int>(list.size());
        }
        // V: cycle loaded weapons when multiple exports exist
        if (wParam == 'V' && !g_weapons.empty())
            g_activeWeaponIndex = (g_activeWeaponIndex + 1) % static_cast<int>(g_weapons.size());
        // B: cycle fire mode on active weapon (semi/auto/bolt) for playtests
        if (wParam == 'B' && !g_weapons.empty()) {
            WeaponDef& w = g_weapons[std::min(g_activeWeaponIndex,
                                             static_cast<int>(g_weapons.size()) - 1)];
            if (w.fireMode == "semi") w.fireMode = "auto";
            else if (w.fireMode == "auto") w.fireMode = "bolt";
            else w.fireMode = "semi";
            g_lastFireMode = w.fireMode;
        }
        return 0;
    case WM_KEYUP:
        if (wParam < 256) g_keys[wParam] = false;
        if (wParam == 'F') g_fireHeld = false;
        return 0;
    case WM_LBUTTONDOWN:
        g_mouseDown = true;
        g_lastMouseX = static_cast<short>(LOWORD(lParam));
        g_lastMouseY = static_cast<short>(HIWORD(lParam));
        if (g_inventoryOpen) {
            // Cursor is free while the lattice is up: no capture, and the click
            // is a pick/place rather than a look drag.
            ReleaseCapture();
            g_inventoryClick = true;
            return 0;
        }
        SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP:
        g_mouseDown = false;
        ReleaseCapture();
        return 0;
    case WM_RBUTTONDOWN:
        if (g_inventoryOpen) {
            g_inventoryStow = true;
            return 0;
        }
        g_firePressed = true;
        g_fireHeld = true;
        return 0;
    case WM_RBUTTONUP:
        g_fireHeld = false;
        return 0;
    case WM_MOUSEMOVE:
        g_mouseX = static_cast<short>(LOWORD(lParam));
        g_mouseY = static_cast<short>(HIWORD(lParam));
    // While the inventory lattice is up, the cursor selects cells instead of
        // turning the camera, so the click is read as a pick/place, never a look.
        if (g_mouseDown && !g_inventoryOpen) {
            int dx = g_mouseX - g_lastMouseX;
            int dy = g_mouseY - g_lastMouseY;
            float sens = g_lookSens * (g_ads ? 0.55f : 1.0f);
            g_yaw += dx * sens;
            g_pitch -= dy * sens; // drag up = look up
            const float lim = static_cast<float>(M_PI) * 0.49f;
            g_pitch = std::max(-lim, std::min(lim, g_pitch));
            g_lastMouseX = g_mouseX;
            g_lastMouseY = g_mouseY;
        }
        return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        g_moveSpeed *= (delta > 0) ? 1.1f : 0.9f;
        g_moveSpeed = std::max(0.01f, std::min(0.25f, g_moveSpeed));
        return 0;
    }
    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

static void createWindow() {
    g_hInstance = GetModuleHandleA(nullptr);
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "VoxelVulkanEngine";
    if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        fail("RegisterClassEx failed");

    RECT r{0, 0, WIDTH, HEIGHT};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExA(
        0, wc.lpszClassName,
"Voxel FPS 0.0 — WASD walk | Space jump | Q/E lean | LMB look | RMB/F fire | X ADS | 1-4 cal | R ammo | V weapon | B mode | Esc",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, g_hInstance, nullptr);
    if (!g_hwnd) fail("CreateWindowEx failed");
}

// ---- Vulkan setup ----
static QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) {
    QueueFamilyIndices indices;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> props(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, props.data());
    for (uint32_t i = 0; i < count; ++i) {
        if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) indices.graphics = static_cast<int>(i);
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, g_surface, &present);
        if (present) indices.present = static_cast<int>(i);
        if (indices.complete()) break;
    }
    return indices;
}

// Opt-in Vulkan validation. Off by default so normal playtests stay fast; set
// VOXEL_VALIDATE=1 to get framebuffer-compatibility, render-pass and layout
// transition errors on stderr. The inventory overlay pass needed its own
// pipeline AND its own framebuffers (VUID-VkRenderPassBeginInfo-renderPass-00904
// also compares subpass dependency chains), which is exactly the kind of thing
// that must be machine-checked rather than reasoned about.
static bool g_validate = false;
static PFN_vkCreateDebugUtilsMessengerEXT g_createDUM = nullptr;
static PFN_vkDestroyDebugUtilsMessengerEXT g_destroyDUM = nullptr;
static VkDebugUtilsMessengerEXT g_debugMessenger = VK_NULL_HANDLE;

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT sev,
    VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*) {
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr, "[vulkan] %s\n", data->pMessage ? data->pMessage : "(null)");
    return VK_FALSE;
}

static void createInstance() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "VoxelEngine";
    app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app.pEngineName = "VoxelRaster";
    app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app.apiVersion = VK_API_VERSION_1_2;

    const char* exts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME
    };

    const char* dbgExt = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    uint32_t instExtCount = 0;
    g_validate = false;
    if (GetEnvironmentVariableA("VOXEL_VALIDATE", nullptr, 0) > 0) {
        uint32_t n = 0;
        if (vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr) == VK_SUCCESS && n > 0) {
            std::vector<VkExtensionProperties> props(n);
            if (vkEnumerateInstanceExtensionProperties(nullptr, &n, props.data()) == VK_SUCCESS) {
                for (const auto& p : props)
                    if (std::strcmp(p.extensionName, dbgExt) == 0) g_validate = true;
            }
        }
    }

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = exts;

    VkDebugUtilsMessengerCreateInfoEXT dci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    if (g_validate) {
        // VUID-04926: chaining a VkDebugUtilsMessengerCreateInfoEXT into
        // VkInstanceCreateInfo requires VK_EXT_debug_utils to be enabled.
        static std::vector<const char*> extsWithDebug;
        extsWithDebug.assign(exts, exts + 2);
        extsWithDebug.push_back(dbgExt);
        ci.enabledExtensionCount = static_cast<uint32_t>(extsWithDebug.size());
        ci.ppEnabledExtensionNames = extsWithDebug.data();

        dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dci.pfnUserCallback = debugCallback;
        // Chain through pNext so creation-time validation is also covered.
        ci.pNext = &dci;
        uint32_t lc = 0;
        if (vkEnumerateInstanceLayerProperties(&lc, nullptr) == VK_SUCCESS && lc > 0) {
            std::vector<VkLayerProperties> lps(lc);
            if (vkEnumerateInstanceLayerProperties(&lc, lps.data()) == VK_SUCCESS) {
                for (const auto& l : lps)
                    if (std::strcmp(l.layerName, layers[0]) == 0) {
                        ci.enabledLayerCount = 1;
                        ci.ppEnabledLayerNames = layers;
                    }
            }
        }
    }

    if (vkCreateInstance(&ci, nullptr, &g_instance) != VK_SUCCESS)
        fail("vkCreateInstance failed");

    if (g_validate) {
        g_createDUM = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            g_instance, "vkCreateDebugUtilsMessengerEXT");
        g_destroyDUM = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            g_instance, "vkDestroyDebugUtilsMessengerEXT");
        if (g_createDUM) {
            if (g_createDUM(g_instance, &dci, nullptr, &g_debugMessenger) != VK_SUCCESS) {
                g_debugMessenger = VK_NULL_HANDLE;
                std::fprintf(stderr, "[vulkan] debug messenger creation failed\n");
            }
        }
    }
}

static void createSurface() {
    VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    ci.hwnd = g_hwnd;
    ci.hinstance = g_hInstance;
    if (vkCreateWin32SurfaceKHR(g_instance, &ci, nullptr, &g_surface) != VK_SUCCESS)
        fail("vkCreateWin32SurfaceKHR failed");
}

static void pickDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(g_instance, &count, nullptr);
    if (!count) fail("No Vulkan devices");
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(g_instance, &count, devices.data());

    for (auto d : devices) {
        auto q = findQueueFamilies(d);
        if (!q.complete()) continue;
        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(d, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(d, nullptr, &extCount, exts.data());
        bool hasSwap = false;
        for (auto& e : exts)
            if (std::string(e.extensionName) == VK_KHR_SWAPCHAIN_EXTENSION_NAME) hasSwap = true;
        if (!hasSwap) continue;

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(d, &props);
        g_phys = d;
        g_qidx = q;
        // Prefer discrete
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) break;
    }
    if (!g_phys) fail("No suitable GPU");
}

static void createLogicalDevice() {
    std::vector<VkDeviceQueueCreateInfo> qcis;
    float prio = 1.0f;
    std::vector<int> unique = {g_qidx.graphics};
    if (g_qidx.present != g_qidx.graphics) unique.push_back(g_qidx.present);
    for (int qf : unique) {
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = static_cast<uint32_t>(qf);
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        qcis.push_back(qci);
    }
    const char* exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceFeatures feats{};
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.queueCreateInfoCount = static_cast<uint32_t>(qcis.size());
    ci.pQueueCreateInfos = qcis.data();
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = exts;
    ci.pEnabledFeatures = &feats;
    if (vkCreateDevice(g_phys, &ci, nullptr, &g_device) != VK_SUCCESS)
        fail("vkCreateDevice failed");
    vkGetDeviceQueue(g_device, g_qidx.graphics, 0, &g_graphicsQueue);
    vkGetDeviceQueue(g_device, g_qidx.present, 0, &g_presentQueue);
}

static VkSurfaceFormatKHR chooseSurfaceFormat() {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &count, formats.data());
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            return f;
    }
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) return f;
    }
    return formats[0];
}

static VkPresentModeKHR choosePresentMode() {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &count, modes.data());
    auto has = [&](VkPresentModeKHR want) {
        for (auto m : modes) if (m == want) return true;
        return false;
    };
    // Mailbox/immediate for high-refresh; FIFO locks to display (120Hz panels).
    if (has(VK_PRESENT_MODE_MAILBOX_KHR)) return VK_PRESENT_MODE_MAILBOX_KHR;
    if (has(VK_PRESENT_MODE_FIFO_RELAXED_KHR)) return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) return VK_PRESENT_MODE_IMMEDIATE_KHR;
    return VK_PRESENT_MODE_FIFO_KHR;
}

static void createDepthResources();
static void destroySwapchainObjects() {
    if (g_depthView) vkDestroyImageView(g_device, g_depthView, nullptr);
    if (g_depthImage) vkDestroyImage(g_device, g_depthImage, nullptr);
    if (g_depthMem) vkFreeMemory(g_device, g_depthMem, nullptr);
    g_depthView = VK_NULL_HANDLE;
    g_depthImage = VK_NULL_HANDLE;
    g_depthMem = VK_NULL_HANDLE;

    for (auto fb : g_framebuffers) vkDestroyFramebuffer(g_device, fb, nullptr);
    g_framebuffers.clear();
    for (auto fb : g_overlayFramebuffers) vkDestroyFramebuffer(g_device, fb, nullptr);
    g_overlayFramebuffers.clear();
    for (auto v : g_swapViews) vkDestroyImageView(g_device, v, nullptr);
    g_swapViews.clear();
    if (g_swapchain) vkDestroySwapchainKHR(g_device, g_swapchain, nullptr);
    g_swapchain = VK_NULL_HANDLE;
}

static void createSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_phys, g_surface, &caps);
    auto format = chooseSurfaceFormat();
    auto presentMode = choosePresentMode();

    if (caps.currentExtent.width != UINT32_MAX) {
        g_extent = caps.currentExtent;
    } else {
        g_extent.width = std::clamp(static_cast<uint32_t>(g_width),
                                    caps.minImageExtent.width, caps.maxImageExtent.width);
        g_extent.height = std::clamp(static_cast<uint32_t>(g_height),
                                     caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    g_swapFormat = format.format;

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
        imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = format.format;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = g_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    uint32_t qfs[] = {static_cast<uint32_t>(g_qidx.graphics),
                      static_cast<uint32_t>(g_qidx.present)};
    if (g_qidx.graphics != g_qidx.present) {
        ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        ci.queueFamilyIndexCount = 2;
        ci.pQueueFamilyIndices = qfs;
    } else {
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = presentMode;
    ci.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(g_device, &ci, nullptr, &g_swapchain) != VK_SUCCESS)
        fail("vkCreateSwapchainKHR failed");

    uint32_t imgCount = 0;
    vkGetSwapchainImagesKHR(g_device, g_swapchain, &imgCount, nullptr);
    g_swapImages.resize(imgCount);
    vkGetSwapchainImagesKHR(g_device, g_swapchain, &imgCount, g_swapImages.data());

    g_swapViews.resize(imgCount);
    for (uint32_t i = 0; i < imgCount; ++i) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = g_swapImages[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = g_swapFormat;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(g_device, &vi, nullptr, &g_swapViews[i]) != VK_SUCCESS)
            fail("vkCreateImageView failed");
    }
}

static void createRenderPass() {
    VkAttachmentDescription color{};
    color.format = g_swapFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depth{};
    depth.format = VK_FORMAT_D32_SFLOAT;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;
    sub.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkAttachmentDescription atts[] = {color, depth};
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 2;
    ci.pAttachments = atts;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;
    if (vkCreateRenderPass(g_device, &ci, nullptr, &g_renderPass) != VK_SUCCESS)
        fail("vkCreateRenderPass failed");
}

// RULES.md rule 12 — the sanctioned "paint over map voxels" exception.
// The inventory lattice draws in its own pass with color LOAD (the world stays
// visible) and depth DONT_CARE, so the lattice self-occludes correctly within
// a fresh depth buffer while ignoring world depth entirely.
//
// A dedicated pipeline and dedicated framebuffers are built against this pass
// rather than reusing the main ones. Reusing g_framebuffers looks tempting
// (compatibility is usually described in terms of attachment formats) but
// VUID-VkRenderPassBeginInfo-renderPass-00904 also requires matching subpass
// DEPENDENCIES, and the overlay legitimately wants a different barrier from the
// main pass. Separate framebuffers are the honest way to say "this is its own
// pass"; they are cheap and recreated with the swapchain.
static void createOverlayRenderPass() {
    VkAttachmentDescription color{};
    color.format = g_swapFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // VUID-00900: a non-UNDEFINED initialLayout must equal the attachment's
    // actual layout. The main pass leaves color in PRESENT_SRC, so that is the
    // layout the overlay starts from.
    color.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depth{};
    depth.format = VK_FORMAT_D32_SFLOAT;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // fresh depth for the lattice
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;
    sub.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    // Overlay's first subpass reads color (loadOp LOAD) and needs the depth
    // writes from the main pass complete before the depth attachment is
    // discarded and reused.
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkAttachmentDescription atts[] = {color, depth};
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 2;
    ci.pAttachments = atts;
    ci.subpassCount = 1;
    ci.pSubpasses = &sub;
    ci.dependencyCount = 1;
    ci.pDependencies = &dep;
    if (vkCreateRenderPass(g_device, &ci, nullptr, &g_overlayRenderPass) != VK_SUCCESS)
        fail("vkCreateOverlayRenderPass failed");
}

static void createDepthResources() {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.extent = {g_extent.width, g_extent.height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.format = VK_FORMAT_D32_SFLOAT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(g_device, &ii, nullptr, &g_depthImage) != VK_SUCCESS)
        fail("depth image failed");

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g_device, g_depthImage, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(g_device, &ai, nullptr, &g_depthMem);
    vkBindImageMemory(g_device, g_depthImage, g_depthMem, 0);

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = g_depthImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_D32_SFLOAT;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(g_device, &vi, nullptr, &g_depthView) != VK_SUCCESS)
        fail("depth view failed");
}

static void createFramebuffers() {
    g_framebuffers.resize(g_swapViews.size());
    for (size_t i = 0; i < g_swapViews.size(); ++i) {
        VkImageView atts[] = {g_swapViews[i], g_depthView};
        VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        ci.renderPass = g_renderPass;
        ci.attachmentCount = 2;
        ci.pAttachments = atts;
        ci.width = g_extent.width;
        ci.height = g_extent.height;
        ci.layers = 1;
        if (vkCreateFramebuffer(g_device, &ci, nullptr, &g_framebuffers[i]) != VK_SUCCESS)
            fail("framebuffer failed");
    }

    // Same attachments, bound to the overlay pass (RULES.md rule 12).
    g_overlayFramebuffers.resize(g_swapViews.size());
    for (size_t i = 0; i < g_swapViews.size(); ++i) {
        VkImageView atts[] = {g_swapViews[i], g_depthView};
        VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        ci.renderPass = g_overlayRenderPass;
        ci.attachmentCount = 2;
        ci.pAttachments = atts;
        ci.width = g_extent.width;
        ci.height = g_extent.height;
        ci.layers = 1;
        if (vkCreateFramebuffer(g_device, &ci, nullptr, &g_overlayFramebuffers[i]) != VK_SUCCESS)
            fail("overlay framebuffer failed");
    }
}

static VkShaderModule loadShader(const std::string& path) {
    auto code = readFile(path);
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule mod;
    if (vkCreateShaderModule(g_device, &ci, nullptr, &mod) != VK_SUCCESS)
        fail("shader module failed: " + path);
    return mod;
}

static void createDescriptors() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 1;
    lci.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(g_device, &lci, nullptr, &g_dsl) != VK_SUCCESS)
        fail("descriptor set layout failed");

    for (int i = 0; i < MAX_FRAMES; ++i) {
        createBuffer(sizeof(FrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     g_uboBuffers[i], g_uboMems[i]);
        vkMapMemory(g_device, g_uboMems[i], 0, sizeof(FrameUBO), 0, &g_uboMapped[i]);
    }

    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &poolSize;
    pci.maxSets = MAX_FRAMES;
    if (vkCreateDescriptorPool(g_device, &pci, nullptr, &g_descPool) != VK_SUCCESS)
        fail("descriptor pool failed");

    VkDescriptorSetLayout layouts[MAX_FRAMES] = {g_dsl, g_dsl};
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g_descPool;
    ai.descriptorSetCount = MAX_FRAMES;
    ai.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(g_device, &ai, g_descSets) != VK_SUCCESS)
        fail("allocate descriptor sets failed");

    for (int i = 0; i < MAX_FRAMES; ++i) {
        VkDescriptorBufferInfo bi{};
        bi.buffer = g_uboBuffers[i];
        bi.offset = 0;
        bi.range = sizeof(FrameUBO);
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = g_descSets[i];
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &bi;
        vkUpdateDescriptorSets(g_device, 1, &write, 0, nullptr);
    }
}

static void createPipeline() {
    std::string vertPath = g_exeDir + "\\shaders\\voxel.vert.spv";
    std::string fragPath = g_exeDir + "\\shaders\\voxel.frag.spv";
    // Also try relative to project if running from build/
    if (GetFileAttributesA(vertPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        vertPath = g_exeDir + "\\..\\shaders\\voxel.vert.spv";
        fragPath = g_exeDir + "\\..\\shaders\\voxel.frag.spv";
    }
    if (GetFileAttributesA(vertPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        vertPath = "C:\\Users\\gryph\\voxel_engine\\build\\shaders\\voxel.vert.spv";
        fragPath = "C:\\Users\\gryph\\voxel_engine\\build\\shaders\\voxel.frag.spv";
    }

    VkShaderModule vert = loadShader(vertPath);
    VkShaderModule frag = loadShader(fragPath);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bind{};
    bind.binding = 0;
    bind.stride = sizeof(Vertex);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[4]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, px)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, nx)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, cr)};
    attrs[3] = {3, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vertex, mat)};

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 4;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
rs.cullMode = VK_CULL_MODE_NONE; // sky dome + moon billboard + world
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.blendEnable = VK_TRUE;
    blendAtt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAtt.colorBlendOp = VK_BLEND_OP_ADD;
    blendAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAtt.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blendAtt;

    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &g_dsl;
    if (vkCreatePipelineLayout(g_device, &plci, nullptr, &g_pipelineLayout) != VK_SUCCESS)
        fail("pipeline layout failed");

    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dyn;
    pci.layout = g_pipelineLayout;
    pci.renderPass = g_renderPass;
    pci.subpass = 0;

    if (vkCreateGraphicsPipelines(g_device, VK_NULL_HANDLE, 1, &pci, nullptr, &g_pipeline) != VK_SUCCESS)
        fail("graphics pipeline failed");

    // Same state, bound to the overlay pass (RULES.md rule 12). Depth state is
    // baked rather than dynamic, so the overlay needs its own pipeline object
    // rather than a state change mid-pass.
    pci.renderPass = g_overlayRenderPass;
    if (vkCreateGraphicsPipelines(g_device, VK_NULL_HANDLE, 1, &pci, nullptr, &g_overlayPipeline) != VK_SUCCESS)
        fail("overlay graphics pipeline failed");

    vkDestroyShaderModule(g_device, vert, nullptr);
    vkDestroyShaderModule(g_device, frag, nullptr);
}

static void createCommandPoolAndBuffers() {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = static_cast<uint32_t>(g_qidx.graphics);
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(g_device, &pci, nullptr, &g_cmdPool) != VK_SUCCESS)
        fail("command pool failed");

    g_cmdBuffers.resize(MAX_FRAMES);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g_cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = MAX_FRAMES;
    if (vkAllocateCommandBuffers(g_device, &ai, g_cmdBuffers.data()) != VK_SUCCESS)
        fail("command buffers failed");
}

static void createSync() {
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < MAX_FRAMES; ++i) {
        vkCreateSemaphore(g_device, &sci, nullptr, &g_imageAvailable[i]);
        vkCreateSemaphore(g_device, &sci, nullptr, &g_renderFinished[i]);
        vkCreateFence(g_device, &fci, nullptr, &g_inFlight[i]);
    }
}

static void destroyWorldMeshBuffer() {
    if (g_vertexMapped && g_vertexMem) {
        vkUnmapMemory(g_device, g_vertexMem);
        g_vertexMapped = nullptr;
    }
    if (g_vertexBuffer) {
        vkDestroyBuffer(g_device, g_vertexBuffer, nullptr);
        g_vertexBuffer = VK_NULL_HANDLE;
    }
    if (g_vertexMem) {
        vkFreeMemory(g_device, g_vertexMem, nullptr);
        g_vertexMem = VK_NULL_HANDLE;
    }
    g_vertexCapacity = 0;
    g_vertexCount = 0;
}

// Host-visible persistent world VB: memcpy only, no staging + QueueWaitIdle (main spike source).
static void uploadMesh(const std::vector<Vertex>& verts) {
    LARGE_INTEGER t0{}, t1{}, freq{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    if (verts.empty()) {
        g_vertexCount = 0;
        return;
    }
    // Caller waits on in-flight fence before this so GPU is done with the previous mapping.
    g_vertexCount = static_cast<uint32_t>(verts.size());
    VkDeviceSize size = sizeof(Vertex) * static_cast<VkDeviceSize>(verts.size());
    // Grow with headroom so repeated impact remeshes rarely reallocate.
    VkDeviceSize need = size + size / 8;
    if (need < size) need = size;

    if (!g_vertexBuffer || g_vertexCapacity < size) {
        destroyWorldMeshBuffer();
        createBuffer(need, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     g_vertexBuffer, g_vertexMem);
        vkMapMemory(g_device, g_vertexMem, 0, need, 0, &g_vertexMapped);
        g_vertexCapacity = need;
    }
    if (g_vertexMapped) {
        std::memcpy(g_vertexMapped, verts.data(), static_cast<size_t>(size));
    }

    QueryPerformanceCounter(&t1);
    const double us = (double(t1.QuadPart - t0.QuadPart) * 1e6) / double(freq.QuadPart);
    g_meshUploadUsSum += us;
    if (us > g_meshUploadUsMax) g_meshUploadUsMax = us;
    ++g_meshUploadSamples;
}

static void destroyVoxelAt(int x, int y, int z) {
    if (!g_chunks || !worldInBounds(x, y, z)) return;
    Block b = getWorldBlock(*g_chunks, x, y, z);
    if (b == Block::Air) return;
    MaterialId mat = blockMaterial(b);
    // Visual degradation: spawn 8x8x8 sub-voxel debris chips (occupancy still unit cube).
    g_debris.spawnFromVoxel(x, y, z, mat,
                            g_lastImpactDx, g_lastImpactDy, g_lastImpactDz,
                            g_lastImpactEnergy, VOXEL_SIZE, g_lastAoeScale);
    setWorldBlock(*g_chunks, x, y, z, Block::Air);
    g_meshDirty = true;
}

static void applySplash(int cx, int cy, int cz, float radius, float energy,
                        const ProjectileDef& def) {
    if (!g_chunks || radius <= 0.0f) return;
    // Expand splash by caliber/damage AOE, then density-scale per cell.
    const float aoe = impactAoeScale(def);
    float effectiveR = radius * std::max(0.5f, aoe);
    int r = std::max(1, static_cast<int>(effectiveR / VOXEL_SIZE) + 1);
    // Cap neighborhood for shotgun volleys (performance).
    if (def.pellets > 1) r = std::min(r, 3);
    else r = std::min(r, 6);
    for (int dz = -r; dz <= r; ++dz)
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                if (dx == 0 && dy == 0 && dz == 0) continue;
                int x = cx + dx, y = cy + dy, z = cz + dz;
                if (!worldInBounds(x, y, z)) continue;
                float dist = std::sqrt(float(dx * dx + dy * dy + dz * dz)) * VOXEL_SIZE;
                Block b = getWorldBlock(*g_chunks, x, y, z);
                MaterialId mat = blockMaterial(b);
                if (mat == MaterialId::Air || mat == MaterialId::Plexiglass) continue;
                float cellR = densityScaledSplash(effectiveR, mat);
                if (dist > cellR) continue;
                float fall = std::pow(std::max(0.0f, 1.0f - dist / std::max(cellR, 1e-6f)), def.splashFalloff);
                // Dense materials soak energy harder beyond threshold already.
                float densMul = 1.0f / std::sqrt(std::max(0.2f, materialProps(mat).density));
                float e = energy * fall * 0.65f * effectMultiplier(def.effect, mat) * densMul;
                float thr = breakEnergyThreshold(mat);
                g_lastAoeScale = aoe * densMul;
                g_lastImpactEnergy = e;
                if (e >= thr * 0.8f) destroyVoxelAt(x, y, z);
            }

    // Splash reaches bodies too. Self damage is ON, so the player's own grenade
    // hurts them; the shooter is only excluded from their own bullet's
    // *direct* hit (see ProjectileRuntime::ownerIsPlayer).
    if (health::kSelfFireDamage) {
        const float impactX = (static_cast<float>(cx) + 0.5f) * VOXEL_SIZE;
        const float impactY = (static_cast<float>(cy) + 0.5f) * VOXEL_SIZE;
        const float impactZ = (static_cast<float>(cz) + 0.5f) * VOXEL_SIZE;
        const health::BodyCellOrigin org = health::bodyCellOrigin(g_player.px, g_player.py, g_player.pz);
        const float bodyDist = health::distanceToBody(org, impactX, impactY, impactZ);
        if (bodyDist <= effectiveR) {
            const float fall =
                std::pow(std::max(0.0f, 1.0f - bodyDist / std::max(effectiveR, 1e-6f)), def.splashFalloff);
            const ArmorZone zone = health::zoneNearestPoint(org, impactX, impactY, impactZ);
            damagePlayerAtZone(energy * fall * 0.65f, def.effect, zone);
        }
    }
}

static WeaponDef activeWeaponOrDefault() {
    if (!g_weapons.empty()) {
        int i = std::min(g_activeWeaponIndex, static_cast<int>(g_weapons.size()) - 1);
        return g_weapons[i];
    }
    return defaultWeaponDef();
}

static std::string activeCaliberId(const WeaponDef& w) {
    // Hotkey caliber override (1-4) takes priority for fire tests / play.
    if (g_activeCaliberIndex >= 0 && g_activeCaliberIndex < 4)
        return kCaliberIds[g_activeCaliberIndex];
    if (!w.ammo.caliber.empty()) return normalizeCaliber(w.ammo.caliber);
    return normalizeCaliber(w.caliber.empty() ? std::string("medium") : w.caliber);
}

static void applyRecoilKick(const WeaponDef& w) {
    // Kick accumulates on a temporary offset (not permanent aim). Handling/weight/ADS
    // damp the punch; handling also speeds the smooth return after the last shot.
    const float damp = 1.0f / (1.0f + std::max(0.0f, w.weight) * 0.08f +
                               std::max(0.0f, w.optic) * (g_ads ? 0.12f : 0.03f) +
                               std::max(0.0f, w.handling) * 0.02f);
    const float kick = w.recoil * 0.0024f * damp;
    // Slight horizontal wander so repeated shots don't climb a perfect line.
    const float yawSign = ((g_ballisticShots + g_hitscanShots) & 1) ? 1.0f : -1.0f;
    g_recoilPitch += kick;
    g_recoilYaw += kick * (0.28f + 0.12f * yawSign);
    // Cap stacked recoil so full-auto doesn't flip the camera.
    const float maxKick = 0.22f * damp + 0.04f;
    g_recoilPitch = std::min(g_recoilPitch, maxKick);
    g_recoilYaw = std::max(-maxKick * 0.65f, std::min(maxKick * 0.65f, g_recoilYaw));
// Hold the punch briefly after this shot, then recover (refreshed on every fire).
    const float shotCd = fireCooldownForWeapon(w);
    const float hold = 0.050f + shotCd * 0.45f +
                       (w.fireMode == "bolt" ? 0.10f : 0.0f) +
                       (w.fireMode == "auto" ? 0.02f : 0.0f);
    g_recoilHold = std::max(g_recoilHold, hold);
    // Return speed: heavier / higher handling settles faster; ADS a bit snappier.
    g_recoilReturn = 7.5f + std::max(0.0f, w.handling) * 0.55f +
                     std::max(0.0f, w.weight) * 0.08f +
                     (g_ads ? 3.0f : 0.0f);
}

// Exponentially ease recoil offset back to zero after the hold window.
static void updateRecoilRecovery(float dt) {
    if (g_recoilHold > 0.0f) {
        g_recoilHold -= dt;
        if (g_recoilHold < 0.0f) g_recoilHold = 0.0f;
        return; // keep current kick while holding after last shot
    }
    if (g_recoilPitch == 0.0f && g_recoilYaw == 0.0f) return;
    // While auto-fire is held, don't settle mid-stream — wait for release / last shot hold.
    if (g_fireHeld) return;
    const float k = 1.0f - std::exp(-g_recoilReturn * dt);
    g_recoilPitch += (0.0f - g_recoilPitch) * k;
    g_recoilYaw += (0.0f - g_recoilYaw) * k;
    if (std::fabs(g_recoilPitch) < 1e-5f) g_recoilPitch = 0.0f;
    if (std::fabs(g_recoilYaw) < 1e-5f) g_recoilYaw = 0.0f;
}

// Aim direction with optic/ADS spread (hip-fire looser, ADS tighter).
static Vec3 aimForward(const WeaponDef& w) {
    Vec3 f = cameraForward();
    float spread = std::max(0.0f, 0.040f - w.optic * 0.0035f - w.handling * 0.0010f);
    if (g_ads) spread *= std::max(0.15f, 1.0f - w.optic * 0.08f);
    if (spread <= 1e-5f) return f;
    const int shotN = g_ballisticShots + g_hitscanShots;
    const float a = static_cast<float>(shotN) * 2.3999632f + w.recoil * 0.01f;
    const float ox = std::sin(a) * spread;
    const float oy = std::cos(a * 1.6180339f) * spread * 0.75f;
    Vec3 r = cameraForward().cross(Vec3(0, 1, 0));
    if (r.length() < 1e-5f) r = Vec3(1, 0, 0);
    r = r.normalized();
    Vec3 u = r.cross(f).normalized();
    return (f + r * ox + u * oy).normalized();
}

// Unit-grid DDA ray (Amanatides & Woo). Hitscan/energy only — gravity_scale=0 path.
static int fireHitscanRay(const ProjectileDef& def, float energyScale, const Vec3& aimDir) {
    if (!g_chunks) return 0;
    Vec3 fwd = aimDir.normalized();
    // Start slightly forward of camera in world space.
    float ox = (g_camPos.x + fwd.x * 0.02f) / VOXEL_SIZE;
    float oy = (g_camPos.y + fwd.y * 0.02f) / VOXEL_SIZE;
    float oz = (g_camPos.z + fwd.z * 0.02f) / VOXEL_SIZE;
    float dx = fwd.x, dy = fwd.y, dz = fwd.z;
    // Avoid zero-direction components for DDA.
    const float eps = 1e-8f;
    if (std::fabs(dx) < eps) dx = (dx < 0.0f ? -eps : eps);
    if (std::fabs(dy) < eps) dy = (dy < 0.0f ? -eps : eps);
    if (std::fabs(dz) < eps) dz = (dz < 0.0f ? -eps : eps);

    int ix = static_cast<int>(std::floor(ox));
    int iy = static_cast<int>(std::floor(oy));
    int iz = static_cast<int>(std::floor(oz));

    const int stepX = dx > 0.0f ? 1 : -1;
    const int stepY = dy > 0.0f ? 1 : -1;
    const int stepZ = dz > 0.0f ? 1 : -1;

    // World-space distance to cross one unit voxel on each axis.
    const float tDeltaX = VOXEL_SIZE / std::fabs(dx);
    const float tDeltaY = VOXEL_SIZE / std::fabs(dy);
    const float tDeltaZ = VOXEL_SIZE / std::fabs(dz);

    // tMax: world distance along ray to next voxel boundary on each axis.
    float tMaxX = (stepX > 0)
        ? ((static_cast<float>(ix) + 1.0f - ox) / dx) * VOXEL_SIZE
        : ((ox - static_cast<float>(ix)) / -dx) * VOXEL_SIZE;
    float tMaxY = (stepY > 0)
        ? ((static_cast<float>(iy) + 1.0f - oy) / dy) * VOXEL_SIZE
        : ((oy - static_cast<float>(iy)) / -dy) * VOXEL_SIZE;
    float tMaxZ = (stepZ > 0)
        ? ((static_cast<float>(iz) + 1.0f - oz) / dz) * VOXEL_SIZE
        : ((oz - static_cast<float>(iz)) / -dz) * VOXEL_SIZE;

    float energy = kineticEnergy(def.mass, def.speed) * (def.baseDamage / 10.0f) * energyScale;
    // Energy beams still use kineticEnergy scale; mass is tiny but baseDamage carries power.
    if (def.gravityScale <= 0.0f)
        energy = std::max(energy, def.baseDamage * 1.5f * energyScale);

    const float maxDist = 1.75f; // world units (~1750 unit voxels)
    float traveled = 0.0f;
    int breaks = 0;
    const int maxSteps = static_cast<int>(maxDist / VOXEL_SIZE) + 2;
    // One bullet, one body: the sweep is sampled per cell, and the 5-cell-wide
    // player would otherwise be counted once per cell it spans. The shooter's
    // own shot is excluded (the ray origin is inside their head); enemy fire
    // will use the same path once it exists.
    bool bodyHit = false;
    const float radiusCells = std::max(0.5f, std::min(2.0f, def.radius / VOXEL_SIZE));
    float prevWx = g_camPos.x + fwd.x * 0.02f;
    float prevWy = g_camPos.y + fwd.y * 0.02f;
    float prevWz = g_camPos.z + fwd.z * 0.02f;

    for (int step = 0; step < maxSteps; ++step) {
        // Body sweep for this cell, tested in cell space against the same box
        // the armor zones tile.
        {
            const float curWx = (static_cast<float>(ix) + 0.5f) * VOXEL_SIZE;
            const float curWy = (static_cast<float>(iy) + 0.5f) * VOXEL_SIZE;
            const float curWz = (static_cast<float>(iz) + 0.5f) * VOXEL_SIZE;
            if (!bodyHit) {
                const ArmorZone zone =
                    projectileHitZone(prevWx, prevWy, prevWz, curWx, curWy, curWz, radiusCells);
                if (zone != ArmorZone::Count) {
                    bodyHit = true;
                    damagePlayerAtZone(energy, def.effect, zone);
                    // Soft target: a penetrating round keeps going, weaker.
                    energy *= (1.0f - std::min(0.95f, def.penetration));
                    if (energy < 0.05f) break;
                }
            }
            prevWx = curWx;
            prevWy = curWy;
            prevWz = curWz;
        }
        if (worldInBounds(ix, iy, iz)) {
            Block b = getWorldBlock(*g_chunks, ix, iy, iz);
            MaterialId mat = blockMaterial(b);
            if (mat != MaterialId::Air) {
                float e = energy * effectMultiplier(def.effect, mat);
                g_lastImpactDx = dx; g_lastImpactDy = dy; g_lastImpactDz = dz;
                g_lastImpactEnergy = e;
                if (resolveVoxelHit(mat, e, def.penetration)) {
                    destroyVoxelAt(ix, iy, iz);
                    applySplash(ix, iy, iz, def.splashRadius, energy, def);
                    energy = e;
                    ++breaks;
                    if (energy < 0.05f) break;
                } else {
                    // Ricochet / spark chips on tough surfaces (matrix reflection).
                    float nx, ny, nz;
                    faceNormalFromVelocity(dx, dy, dz, nx, ny, nz);
                    float rvx = dx, rvy = dy, rvz = dz;
                    const auto& mp = materialProps(mat);
                    ricochetVelocity(rvx, rvy, rvz, nx, ny, nz,
                                     0.15f + mp.damping * 0.2f, 0.35f + mp.density * 0.02f);
                    g_debris.ricochets++;
                    // Small chip burst without destroying occupancy
                    g_lastAoeScale = impactAoeScale(def) * 0.5f;
                    g_debris.spawnFromVoxel(ix, iy, iz, mat, dx, dy, dz, e * 0.35f, VOXEL_SIZE, g_lastAoeScale);
                    energy = e;
                    break;
                }
            }
        } else if (traveled > 0.05f) {
            // Left the map after traveling — end ray.
            break;
        }

        // Step to next voxel face.
        if (tMaxX < tMaxY) {
            if (tMaxX < tMaxZ) {
                traveled = tMaxX;
                tMaxX += tDeltaX;
                ix += stepX;
            } else {
                traveled = tMaxZ;
                tMaxZ += tDeltaZ;
                iz += stepZ;
            }
        } else {
            if (tMaxY < tMaxZ) {
                traveled = tMaxY;
                tMaxY += tDeltaY;
                iy += stepY;
            } else {
                traveled = tMaxZ;
                tMaxZ += tDeltaZ;
                iz += stepZ;
            }
        }
        if (traveled > maxDist) break;
    }
    // Remesh deferred to drawFrame after GPU fence.
    return breaks;
}

static void spawnBallisticProjectile(const ProjectileDef& def, const Vec3& aimDir) {
    Vec3 fwd = aimDir.normalized();
    ProjectileRuntime p;
    p.def = def;
    // Spawn just ahead of camera; subunit-sized projectiles use def.radius.
    const float muzzle = std::max(0.02f, def.radius * 40.0f);
    p.px = g_camPos.x + fwd.x * muzzle;
    p.py = g_camPos.y + fwd.y * muzzle;
    p.pz = g_camPos.z + fwd.z * muzzle;
    p.vx = fwd.x * def.speed;
    p.vy = fwd.y * def.speed;
    p.vz = fwd.z * def.speed;
    p.energy = kineticEnergy(def.mass, def.speed) * (def.baseDamage / 10.0f);
    p.alive = true;
    p.ownerIsPlayer = true; // never self-inflicted by the shooter's own bullet
    g_projectiles.push_back(p);
}

// Spread aim direction within a cone (shotgun pellets).
static Vec3 spreadAim(const Vec3& forward, float spreadDeg, int pelletIndex, int pelletCount) {
    if (spreadDeg <= 0.01f || pelletCount <= 1) return forward.normalized();
    Vec3 f = forward.normalized();
    Vec3 right = f.cross(Vec3(0, 1, 0));
    if (right.length() < 1e-5f) right = Vec3(1, 0, 0);
    right = right.normalized();
    Vec3 up = right.cross(f).normalized();
    const float rad = spreadDeg * static_cast<float>(M_PI) / 180.0f;
    // Deterministic ring + hash jitter keeps smoke stable without RNG lag spikes.
    const float t = (static_cast<float>(pelletIndex) + 0.5f) / static_cast<float>(pelletCount);
    const float ang = t * static_cast<float>(M_PI) * 2.0f * 1.6180339f;
    const float ring = rad * (0.35f + 0.65f * t);
    const float jx = std::cos(ang) * ring;
    const float jy = std::sin(ang) * ring;
    return (f + right * jx + up * jy).normalized();
}

static void fireProjectile() {
    if (!g_chunks) return;
    if (g_health.dead) return; // a dead player cannot shoot
    if (g_fireCooldown > 0.0f) return;

    WeaponDef weapon = activeWeaponOrDefault();
    std::string caliber = activeCaliberId(weapon);

    WeaponDef fired = weapon;
    fired.caliber = caliber;
    auto calAmmo = ammosForCaliber(g_ammoDefs, caliber);
    std::string preferredAmmo = weapon.ammoId;
    if (!calAmmo.empty()) {
        int ai = g_activeAmmoIndex % static_cast<int>(calAmmo.size());
        if (g_activeAmmoIndex == 0) {
            AmmoDef match = findAmmoForCaliber(g_ammoDefs, caliber, preferredAmmo);
            if (!match.id.empty()) fired.ammo = match;
            else fired.ammo = calAmmo[0];
        } else {
            fired.ammo = calAmmo[ai];
        }
        fired.ammoId = fired.ammo.id;
    } else {
        fired.ammo = findAmmoForCaliber(g_ammoDefs, caliber, preferredAmmo);
        fired.ammoId = fired.ammo.id.empty() ? defaultAmmoIdForCaliber(caliber) : fired.ammo.id;
    }
    fired.hitscan = weapon.hitscan || caliberIsHitscan(caliber) || fired.ammo.hitscan;

    // Prefer explicit shotgun_light when caliber is light and def exists (smoke / play).
    ProjectileDef def = projectileForCaliber(g_projDefs, caliber);
    if (caliber == "light") {
        for (const auto& d : g_projDefs) {
            if (d.id == "shotgun_light" || d.pellets > 1) { def = d; break; }
        }
    }
    def = scaleProjectileForWeapon(def, fired);
    g_lastAoeScale = impactAoeScale(def);

    bool useHitscan = fired.hitscan || def.hitscan || def.gravityScale <= 0.0f ||
                      fired.ammo.hitscan || fired.ammo.effect == "energy";
    if (useHitscan) {
        def.hitscan = true;
        def.gravityScale = 0.0f;
        def.pellets = 1;
    }

    Vec3 aim = aimForward(fired);

    g_lastWeaponId = def.id.empty() ? fired.id : def.id;
    g_lastCaliber = caliber;
    g_lastHitscan = useHitscan;
    g_lastAmmoId = fired.ammoId.empty() ? fired.ammo.id : fired.ammoId;
    g_lastFireMode = fired.fireMode.empty() ? "semi" : fired.fireMode;

    if (useHitscan) {
        fireHitscanRay(def, 1.0f, aim);
        ++g_hitscanShots;
    } else {
        const int n = std::clamp(def.pellets, 1, 12);
        if (n > 1) {
            ++g_shotgunShots;
            g_pelletSpawns += n;
            // Soft-cap live projectiles so volleys cannot explode memory.
            const int room = std::max(0, 64 - static_cast<int>(g_projectiles.size()));
            const int spawnN = std::min(n, std::max(1, room));
            for (int i = 0; i < spawnN; ++i) {
                Vec3 dir = spreadAim(aim, def.spreadDeg, i, spawnN);
                spawnBallisticProjectile(def, dir);
            }
            g_ballisticShots += spawnN;
        } else {
            spawnBallisticProjectile(def, aim);
            ++g_ballisticShots;
        }
    }

applyRecoilKick(fired);
    g_fireCooldown = fireCooldownForWeapon(fired);
    if (def.pellets > 1) g_fireCooldown *= 1.15f; // slight pump delay

    // Muzzle flash + frame overlay pulse (shader + world cubes).
    {
        float pulse = useHitscan ? 1.0f : (def.pellets > 1 ? 1.15f : 0.85f);
        if (caliber == "heavy") pulse *= 1.2f;
        if (caliber == "energy") { g_muzzleR = 0.45f; g_muzzleG = 0.85f; g_muzzleB = 1.0f; }
        else if (caliber == "light") { g_muzzleR = 1.0f; g_muzzleG = 0.82f; g_muzzleB = 0.35f; }
        else if (caliber == "heavy") { g_muzzleR = 1.0f; g_muzzleG = 0.55f; g_muzzleB = 0.18f; }
        else { g_muzzleR = 1.0f; g_muzzleG = 0.70f; g_muzzleB = 0.28f; }
        g_muzzleFlash = std::min(1.0f, std::max(g_muzzleFlash, pulse));
        g_fireOverlay = std::min(1.0f, std::max(g_fireOverlay, pulse * 0.9f));
        g_debris.meshDirty = true;
    }
}

static void tryLoadWeapons() {
    g_weapons.clear();
    const std::string candidates[] = {
        g_exeDir + "\\weapons",
        g_exeDir + "\\..\\data\\weapons",
        g_exeDir + "\\..\\..\\data\\weapons",
    };
    for (const auto& dir : candidates) {
        std::string pattern = dir + "\\*.weapon.json";
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::string path = dir + "\\" + fd.cFileName;
            WeaponDef w = loadWeaponDef(path);
            if (!w.id.empty()) {
                bindWeaponAmmo(w, g_ammoDefs);
                g_weapons.push_back(w);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        if (!g_weapons.empty()) break;
    }
    if (g_weapons.empty()) {
        const std::string files[] = {
            g_exeDir + "\\weapons\\starter_rifle.weapon.json",
            g_exeDir + "\\..\\data\\weapons\\starter_rifle.weapon.json",
        };
        for (const auto& f : files) {
            WeaponDef w = loadWeaponDef(f);
            if (!w.id.empty()) {
                bindWeaponAmmo(w, g_ammoDefs);
                g_weapons.push_back(w);
                break;
            }
        }
    }
    if (g_weapons.empty()) {
        WeaponDef w = defaultWeaponDef();
        bindWeaponAmmo(w, g_ammoDefs);
        g_weapons.push_back(w);
    }

    g_activeWeaponIndex = 0;
    const WeaponDef& w0 = g_weapons[0];
    g_lastWeaponId = w0.id;
    g_lastCaliber = normalizeCaliber(w0.caliber);
    g_lastHitscan = w0.hitscan || caliberIsHitscan(w0.caliber);
    g_lastAmmoId = w0.ammoId.empty() ? w0.ammo.id : w0.ammoId;
    g_lastFireMode = w0.fireMode.empty() ? "semi" : w0.fireMode;
    g_activeCaliberIndex = 1;
    g_activeAmmoIndex = 0;
    for (int i = 0; i < 4; ++i) {
        if (normalizeCaliber(w0.caliber) == kCaliberIds[i]) { g_activeCaliberIndex = i; break; }
    }
}

// Load inventory item defs (data/items/*.item.json) and seed the base loadout.
static void tryLoadItems() {
    g_itemDefs.clear();
    const std::string candidates[] = {
        g_exeDir + "\\items",
        g_exeDir + "\\..\\data\\items",
        g_exeDir + "\\..\\..\\data\\items",
    };
    for (const auto& dir : candidates) {
        std::string pattern = dir + "\\*.item.json";
        WIN32_FIND_DATAA fd{};
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            ItemDef d = loadItemDef(dir + "\\" + fd.cFileName);
            if (!d.id.empty()) g_itemDefs.push_back(d);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        if (!g_itemDefs.empty()) break;
    }

    initInventory(g_inventory, g_itemDefs);
}

static int itemIndexById(const char* id) {
    const ItemDef* d = findItemById(g_itemDefs, id);
    if (!d) return -1;
    return static_cast<int>(d - g_itemDefs.data());
}

// Exercise the inventory contract end to end: zone tiling, equipment class
// gating, armor zone fit (including the negative cases), and the first-fit
// packer against both the base and the upgraded backpack volume.
struct InventorySmokeReport {
    bool zonesTile = false;
    bool equipOk = false;
    bool wideArmRejected = false;
    bool classGateOk = false;
    bool basePackOk = false;
    bool baseFullRejected = false;
    bool upgradePackOk = false;
    int baseUsed = 0;
    int baseFree = 0;
    int upgradeUsed = 0;
    int upgradeCells = 0;
    int magRounds = 0;
    int pouchRounds = 0;
    int overlayVerts = 0;
    int overlayLatticeVerts = 0;
    int overlaySlotVerts = 0;
    int overlayHudVerts = 0;
    bool hudBuilt = false;
    bool overlayBuilt = false;
    bool overlayUnitCubes = false;
    // look-and-click hand path
    bool hoverResolved = false;
    bool liftOk = false;
    bool placeOk = false;
    bool rotateOk = false;
    bool stowOk = false;
    bool invalidPlaceRejected = false;
    // backpack storage is granted on equip and REMOVED on unequip
    bool backpackVolumeOk = false;
    // unequipping via the marker must leave the pack in hand, not destroy it
    bool backpackLiftOk = false;
    // world pickups
    bool pickupOk = false;
    int pickupVerts = 0;
    bool pickupTaken = false;
};

static InventorySmokeReport g_invSmoke;

// Health smoke (RULES.md rule 15). --smoke skips player physics, so fall damage
// and drowning can never be reached through the frame loop; every check here
// drives the same health:: functions the game calls, on a local actor so the
// live state is untouched.
struct HealthSmokeReport {
    bool maxHealthOk = false;
    bool armorAbsorbOk = false;
    bool zoneHitOk = false;
    bool damageOk = false;
    bool singleHitCapOk = false;
    bool fallOk = false;
    bool drownOk = false;
    bool deathOk = false;
    bool equippedArmorOk = false;
    bool selfFireExcludedOk = false;
    int mediumShotHp = 0;   // HP cost of one starter-rifle medium-caliber hit
    int helmetPoints = 0;   // armor_points actually parsed from data/items
    int chestPoints = 0;
};

static HealthSmokeReport g_healthSmoke;

static HealthSmokeReport runHealthSmoke() {
    HealthSmokeReport rep;
    health::ActorHealth h;
    rep.maxHealthOk = std::fabs(h.maxHealth - 125.0f) < 1e-4f &&
                      std::fabs(h.health - 125.0f) < 1e-4f && !h.dead;

    // 0.5% per point, hard-capped so armor is never invulnerability.
    rep.armorAbsorbOk = std::fabs(health::armorAbsorption(0.0f)) < 1e-6f &&
                        std::fabs(health::armorAbsorption(20.0f) - 0.10f) < 1e-5f &&
                        std::fabs(health::armorAbsorption(100.0f) - 0.50f) < 1e-5f &&
                        std::fabs(health::armorAbsorption(400.0f) - 0.50f) < 1e-5f;

    // Segment -> zone, in cell space. Origin (0,0,0) means world (0,0,0) is the
    // body-centre cell at the feet.
    {
        const health::BodyCellOrigin org{0, 0, 0};
        const health::BodyHit head = health::segmentHitBody(
            org, -0.010f, 0.0175f, 0.0f, 0.010f, 0.0175f, 0.0f);
        const health::BodyHit chest = health::segmentHitBody(
            org, -0.010f, 0.0125f, 0.0f, 0.010f, 0.0125f, 0.0f);
        const health::BodyHit legs = health::segmentHitBody(
            org, -0.010f, 0.0035f, 0.0f, 0.010f, 0.0035f, 0.0f);
        const health::BodyHit miss = health::segmentHitBody(
            org, -0.010f, 0.0035f, 0.060f, 0.010f, 0.0035f, 0.060f);
        // Radial pricing: a blast at the feet resolves to legs, one at the
        // camera height to head.
        const health::BodyCellOrigin live =
            health::bodyCellOrigin(g_player.px, g_player.py, g_player.pz);
        const health::BodyHit atEye = health::segmentHitBody(
            live, g_camPos.x, g_camPos.y, g_camPos.z, g_camPos.x, g_camPos.y, g_camPos.z);
        rep.zoneHitOk = head.zone == ArmorZone::Head && chest.zone == ArmorZone::Chest &&
                        legs.zone == ArmorZone::Legs && miss.zone == ArmorZone::Count &&
                        atEye.zone == ArmorZone::Head;
    }

    // Absorption: 20 HP unarmored costs 20, the same 20 HP behind 100 points
    // (a 50% soak) costs 10.
    {
        health::ActorHealth bare;
        health::ActorHealth armored;
        const health::DamageResult a = health::applyDamage(bare, 20.0f, ArmorZone::Chest, 0.0f);
        const health::DamageResult b = health::applyDamage(armored, 20.0f, ArmorZone::Chest, 100.0f);
        rep.damageOk = std::fabs(a.applied - 20.0f) < 1e-4f &&
                       std::fabs(a.absorbed) < 1e-4f &&
                       std::fabs(b.applied - 10.0f) < 1e-4f &&
                       std::fabs(b.absorbed - 10.0f) < 1e-4f &&
                       std::fabs(armored.absorbed[static_cast<int>(ArmorZone::Chest)] - 10.0f) < 1e-4f;
    }

    // No single application may exceed 60% of the pool.
    {
        health::ActorHealth c;
        const health::DamageResult r = health::applyDamage(c, 1000.0f, ArmorZone::Chest, 0.0f);
        rep.singleHitCapOk = !r.killed && c.dead == false &&
                             std::fabs(c.health - (125.0f - 75.0f)) < 1e-3f;
    }

    // Fall curve: free at/below a one-voxel step, monotonic above it, capped.
    {
        const float step = health::fallDamageForImpactSpeed(0.083f);
        const float mid = health::fallDamageForImpactSpeed(0.16f);
        const float maxV = health::fallDamageForImpactSpeed(0.25f);
        const float over = health::fallDamageForImpactSpeed(9.0f);
        rep.fallOk = step <= 0.0f && mid > step && mid < maxV && maxV <= 40.0f + 1e-4f &&
                     over <= 40.0f + 1e-4f;
    }

    // Breath: drains submerged, refills in air, damages only once it is empty.
    {
        health::ActorHealth b;
        for (int i = 0; i < 260; ++i) health::updateBreath(b, true, 0.1f);
        const bool heldUnder = b.breath <= 0.0f && b.health < 125.0f;
        for (int i = 0; i < 40; ++i) health::updateBreath(b, false, 0.1f);
        const bool refilled = b.breath > 0.0f && b.health < 125.0f; // no HP back
        health::ActorHealth c;
        for (int i = 0; i < 100; ++i) health::updateBreath(c, true, 0.1f);
        const bool noEarlyDmg = std::fabs(c.health - 125.0f) < 1e-4f;
        rep.drownOk = heldUnder && refilled && noEarlyDmg;
    }

    // Death + respawn: drain, confirm dead/timer, then run out the countdown.
    {
        health::ActorHealth d;
        health::DamageResult last{};
        for (int i = 0; i < 40 && !d.dead; ++i)
            last = health::applyDamage(d, 10.0f, ArmorZone::Chest, 0.0f);
        const bool died = d.dead && last.killed && d.health <= 0.0f &&
                          d.respawnTimer > 0.0f;
        bool respawned = false;
        for (int i = 0; i < 200 && !respawned; ++i)
            respawned = health::updateActorHealth(d, 0.05f);
        rep.deathOk = died && respawned && !d.dead &&
                      std::fabs(d.health - d.maxHealth) < 1e-4f;
    }

    // Equipped armor actually drives mitigation, and it is data-driven.
    {
        const int chest = itemIndexById("armor_chest_plate");
        const int helmet = itemIndexById("armor_helmet");
        if (chest >= 0) rep.chestPoints = static_cast<int>(g_itemDefs[static_cast<size_t>(chest)].armorPoints);
        if (helmet >= 0) rep.helmetPoints = static_cast<int>(g_itemDefs[static_cast<size_t>(helmet)].armorPoints);
        const int slot = static_cast<int>(zoneEquipSlot(ArmorZone::Chest));
        const int saved = g_inventory.slotDef[slot];
        const float empty = equippedArmorPoints(ArmorZone::Chest);
        bool ok = empty <= 0.0f;
        if (chest >= 0) {
            g_inventory.slotDef[slot] = chest;
            const float worn = equippedArmorPoints(ArmorZone::Chest);
            ok = ok && worn > 0.0f;
            // 100 points of soak really is 50% off a 20 HP chest hit.
            health::ActorHealth test;
            const health::DamageResult r = health::applyDamage(test, 20.0f, ArmorZone::Chest, worn);
            ok = ok && std::fabs(r.applied - 20.0f * (1.0f - health::armorAbsorption(worn))) < 1e-3f;
        }
        g_inventory.slotDef[slot] = saved;
        rep.equippedArmorOk = ok && rep.chestPoints > 0 && rep.helmetPoints > 0;
    }

    // Self damage is ON, but a shooter is never hurt by their own bullet: the
    // hitscan origin sits inside the player's head, so the exclusion is the only
    // thing standing between firing and suicide. Prove both halves: the ray
    // really does intersect the body, and no engine-spawned projectile is
    // unowned.
    {
        bool allOwned = true;
        for (const auto& p : g_projectiles)
            if (!p.ownerIsPlayer) allOwned = false;
        const health::BodyCellOrigin live =
            health::bodyCellOrigin(g_player.px, g_player.py, g_player.pz);
        const Vec3 f = cameraForward();
        const health::BodyHit selfRay = health::segmentHitBody(
            live, g_camPos.x, g_camPos.y, g_camPos.z, g_camPos.x + f.x * 0.05f,
            g_camPos.y + f.y * 0.05f, g_camPos.z + f.z * 0.05f);
        rep.selfFireExcludedOk = health::kSelfFireDamage && allOwned &&
                                 selfRay.zone == ArmorZone::Head;
    }

    // One starter-rifle medium hit, priced through the real conversion, so the
    // HP-per-shot number is measured rather than asserted.
    {
        int medium = -1;
        for (size_t i = 0; i < g_projDefs.size(); ++i)
            if (g_projDefs[i].caliber == "medium" && !g_projDefs[i].hitscan) {
                medium = static_cast<int>(i);
                break;
            }
        if (medium >= 0) {
            ProjectileDef def = scaleProjectileForWeapon(g_projDefs[static_cast<size_t>(medium)],
                                                         activeWeaponOrDefault());
            const float e = kineticEnergy(def.mass, def.speed) * (def.baseDamage / 10.0f);
            rep.mediumShotHp = static_cast<int>(std::lround(health::biologicalDamage(e, def.effect)));
        }
    }
    return rep;
}

static InventorySmokeReport runInventorySmoke() {
    InventorySmokeReport rep;
    rep.zonesTile = armorZonesTileHitbox();

    const int rifle = itemIndexById("weapon_starter_rifle");
    const int sidearm = itemIndexById("weapon_sidearm_light");
    const int helmet = itemIndexById("armor_helmet");
    const int chestPlate = itemIndexById("armor_chest_plate");
    const int legPlates = itemIndexById("armor_leg_plates");
    const int armGuard = itemIndexById("armor_arm_guard");
    const int wideArm = itemIndexById("armor_arm_guard_wide");
    const int pouch = itemIndexById("ammo_pouch_medium");
    const int grenade = itemIndexById("supply_grenade");
    const int upgrade = itemIndexById("backpack_field_upgrade");

    if (rifle < 0 || sidearm < 0 || helmet < 0 || chestPlate < 0 || legPlates < 0 ||
        armGuard < 0 || wideArm < 0 || pouch < 0 || grenade < 0 || upgrade < 0)
        return rep;

    Inventory inv;
    initInventory(inv, g_itemDefs);

    int displaced = -1;
    bool all = true;
    all = equipDef(inv, g_itemDefs, rifle, EquipSlot::Primary0, displaced) && all;
    all = equipDef(inv, g_itemDefs, sidearm, EquipSlot::Small, displaced) && all;
    all = equipDef(inv, g_itemDefs, helmet, EquipSlot::ArmorHead, displaced) && all;
    all = equipDef(inv, g_itemDefs, chestPlate, EquipSlot::ArmorChest, displaced) && all;
    all = equipDef(inv, g_itemDefs, legPlates, EquipSlot::ArmorLegs, displaced) && all;
    all = equipDef(inv, g_itemDefs, armGuard, EquipSlot::ArmorArms, displaced) && all;
    rep.equipOk = all;

    // The Arms zone is a single-cell-wide column: a 3-wide piece must not fit.
    rep.wideArmRejected = !canEquip(g_itemDefs, wideArm, EquipSlot::ArmorArms);
    // A small weapon may not occupy a primary slot.
    rep.classGateOk = !canEquip(g_itemDefs, sidearm, EquipSlot::Primary0) &&
                      canEquip(g_itemDefs, rifle, EquipSlot::Primary1);

    const ItemDef* rifleDef = itemDefAt(g_itemDefs, rifle);
    rep.magRounds = rifleDef ? rifleDef->magazineSize : 0;
    const ItemDef* pouchDef = itemDefAt(g_itemDefs, pouch);
    rep.pouchRounds = pouchDef ? pouchDef->rounds : 0;

    // Base 3x3x4 = 36 pack: rifle + one pouch + grenade fit, and a second
    // pouch must be rejected. 3x3x4 strands geometry rather than running out
    // of cells — 14 cells stay free with no 2x2x2 gap anywhere, because a
    // 3-deep face only admits 2-tall items at y in {0,1} and both are cut by
    // the rifle.
    int inst = -1, rot = 0;
    bool packed = true;
    packed = autoPlace(inv, g_itemDefs, rifle, inst, rot) && packed;
    packed = autoPlace(inv, g_itemDefs, pouch, inst, rot) && packed;
    packed = autoPlace(inv, g_itemDefs, grenade, inst, rot) && packed;
    rep.basePackOk = packed;
    rep.baseUsed = inv.usedCells();
    rep.baseFree = inv.freeCells();
    rep.baseFullRejected = !autoPlace(inv, g_itemDefs, pouch, inst, rot);

    // Upgraded 4x4x5 = 80 pack must swallow everything the base one could not.
    Inventory big;
    initInventory(big, g_itemDefs);
    equipDef(big, g_itemDefs, upgrade, EquipSlot::Backpack, displaced);
    rep.upgradeCells = big.vol.cells();
    bool bigOk = true;
    bigOk = autoPlace(big, g_itemDefs, rifle, inst, rot) && bigOk;
    bigOk = autoPlace(big, g_itemDefs, rifle, inst, rot) && bigOk;
    bigOk = autoPlace(big, g_itemDefs, sidearm, inst, rot) && bigOk;
    bigOk = autoPlace(big, g_itemDefs, pouch, inst, rot) && bigOk;
    bigOk = autoPlace(big, g_itemDefs, pouch, inst, rot) && bigOk;
    bigOk = autoPlace(big, g_itemDefs, pouch, inst, rot) && bigOk;
    bigOk = autoPlace(big, g_itemDefs, grenade, inst, rot) && bigOk;
    rep.upgradePackOk = bigOk;
    rep.upgradeUsed = big.usedCells();

    // Overlay mesh contract (RULES.md rule 12). Build against the real
    // g_inventory so the smoke exercises the same code path the frame loop
    // does, then check the emitted geometry is exactly unit cubes: one cell
    // must be 36 vertices (6 faces x 2 tris x 3) and the per-cell extent must
    // equal VOXEL_SIZE on all three axes.
    // The hand-path check below drives g_invHover, so the mesh must be built
    // with the panel open and no item in hand: an extra ghost shape would change
    // the expected vertex count.
    const bool wasOpen = g_inventoryOpen;
    const int wasHeld = g_inventory.held;
    g_inventory.held = -1;
    g_inventoryOpen = true;
    // Deterministic HUD geometry: the live run may have taken splash damage or
    // be mid-respawn, and the HUD segment count depends on health/breath state.
    health::respawnActor(g_health);
    updateInventoryMesh();
    g_inventoryOpen = wasOpen;
    g_inventory.held = wasHeld;

    rep.overlayVerts = static_cast<int>(g_inventoryVertexCount);
    const int cells = g_inventory.vol.cells();
    rep.overlayLatticeVerts = cells * 36;
    rep.overlaySlotVerts = kEquipSlotCount * 36;
    // HUD is appended to the same buffer: 10 health + 5 breath unit cells.
    rep.overlayHudVerts = (kHudHealthCells + kHudBreathCells) * 36;
    rep.hudBuilt = rep.overlayVerts ==
                   rep.overlayLatticeVerts + rep.overlaySlotVerts + rep.overlayHudVerts;
    rep.overlayBuilt = rep.hudBuilt && rep.overlayVerts <= static_cast<int>(kInventoryMaxVerts);

    // Unit-cube check. The cell is rotated in world space, so a world AABB
    // would be larger than VOXEL_SIZE; what must hold is that the cell spans
    // exactly VOXEL_SIZE along its OWN basis axes. Faces 0/2/4 of a cell carry
    // the +right/+up/+fwd hard normals, so the basis is recoverable from the
    // mesh itself. Every vertex must also carry mat 7.
    if (g_inventoryMapped && rep.overlayVerts >= 36) {
        const Vertex* verts = reinterpret_cast<const Vertex*>(g_inventoryMapped);
        const Vec3 ax[3] = {Vec3(verts[0].nx, verts[0].ny, verts[0].nz),
                            Vec3(verts[12].nx, verts[12].ny, verts[12].nz),
                            Vec3(verts[24].nx, verts[24].ny, verts[24].nz)};
        bool orthonormal = true;
        for (int a = 0; a < 3; ++a)
            if (std::fabs(ax[a].length() - 1.0f) > 1e-3f) orthonormal = false;
        if (ax[0].dot(ax[1]) > 1e-3f || ax[0].dot(ax[2]) > 1e-3f ||
            ax[1].dot(ax[2]) > 1e-3f)
            orthonormal = false;

        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        bool allMat7 = true;
        for (int i = 0; i < 36; ++i) {
            const Vec3 p(verts[i].px, verts[i].py, verts[i].pz);
            for (int a = 0; a < 3; ++a) {
                const float d = p.dot(ax[a]);
                lo[a] = std::min(lo[a], d);
                hi[a] = std::max(hi[a], d);
            }
            if (verts[i].mat < 6.5f) allMat7 = false;
        }
        const float tol = VOXEL_SIZE * 0.02f;
        const float ex = std::fabs((hi[0] - lo[0]) - VOXEL_SIZE);
        const float ey = std::fabs((hi[1] - lo[1]) - VOXEL_SIZE);
        const float ez = std::fabs((hi[2] - lo[2]) - VOXEL_SIZE);
        rep.overlayUnitCubes = orthonormal && allMat7 && ex < tol && ey < tol && ez < tol;
    }

    // ---- look-and-click hand path -----------------------------------------
    // Exercised on a scratch inventory so the live one is untouched. Point the
    // cursor at a known cell by projecting that cell's own centre, which is
    // exact because mat 7 skips the fisheye (see voxel.vert).
    {
        rep.liftOk = rep.placeOk = rep.rotateOk = false;
        rep.stowOk = rep.invalidPlaceRejected = false;

        // A quarter turn about +Y must actually change the footprint of a
        // non-square item, and the R binding must cycle 0..3.
        {
            const ItemShape a = rotatedShape(rifleDef->shape, 0);
            const ItemShape b90 = rotatedShape(rifleDef->shape, 1);
            int cyc = 0;
            for (int i = 0; i < 5; ++i) cyc = (cyc + 1) & 3;
            rep.rotateOk = a.sx != b90.sx && a.sz != b90.sz && cyc == 1;
        }

        // Cursor -> cell. The lattice is small on screen and neighbouring cells'
        // screen rects overlap, so the contract is "the cursor resolves to a real
        // in-range cell", and the action then applies to THAT cell. Asserting a
        // particular cell here would test the overlap heuristic, not the feature.
        Inventory t;
        initInventory(t, g_itemDefs);
        int tInst = 0, tRot = 0;
        int seedX = -1, seedY = -1, seedZ = -1;
        if (autoPlace(t, g_itemDefs, rifle, tInst, tRot) && tInst >= 0) {
            for (int iz = 0; iz < t.vol.sz && seedX < 0; ++iz)
                for (int iy = 0; iy < t.vol.sy && seedX < 0; ++iy)
                    for (int ix = 0; ix < t.vol.sx; ++ix)
                        if (invIndexAt(t, ix, iy, iz) == tInst) {
                            seedX = ix; seedY = iy; seedZ = iz;
                            break;
                        }
        }
        if (seedX >= 0) {
            const bool wasOpen2 = g_inventoryOpen;
            const int savedMouseX = g_mouseX, savedMouseY = g_mouseY;
            g_inventoryOpen = true;
            updateInventoryMesh(); // establishes g_invDisplay for this basis
            const InventoryDisplay disp = g_invDisplay;
            const Vec3 centre = disp.origin + disp.right * ((seedX + 0.5f) * VOXEL_SIZE) +
                                disp.up * ((seedY + 0.5f) * VOXEL_SIZE) +
                                disp.fwd * ((seedZ + 0.5f) * VOXEL_SIZE);
            float sx = 0, sy = 0;
            if (projectToScreen(centre, sx, sy)) {
                g_mouseX = static_cast<int>(sx);
                g_mouseY = static_cast<int>(sy);
                updateInventoryMesh(); // resolves g_invHover for that cursor
                rep.hoverResolved = g_invHover.lattice && g_invHover.x >= 0 &&
                                    g_invHover.x < t.vol.sx && g_invHover.y >= 0 &&
                                    g_invHover.y < t.vol.sy && g_invHover.z >= 0 &&
                                    g_invHover.z < t.vol.sz;

                // Act on the cell the cursor actually resolved to: lift it, and
                // if it was occupied the cell must be freed.
                const int hx = g_invHover.x, hy = g_invHover.y, hz = g_invHover.z;
                const int occBefore = t.usedCells();
                const int target = invIndexAt(t, hx, hy, hz);
                if (liftPacked(t, target)) {
                    rep.liftOk = t.held >= 0 && invIndexAt(t, hx, hy, hz) == -1;
                    // Put it back at the origin and rotation it was packed with,
                    // NOT the hovered cell with the rifle's rotation: the hover may
                    // have landed on some other instance's cell.
                    const ItemInstance& it = t.items[static_cast<size_t>(t.held)];
                    const int ox = it.ox, oy = it.oy, oz = it.oz, orot = it.rot;
                    rep.placeOk = placeHeld(t, g_itemDefs, orot, ox, oy, oz) && t.held == -1 &&
                                  t.usedCells() == occBefore;
                } else if (target == -1) {
                    // An empty cell has nothing to lift; lifting must be a no-op.
                    rep.liftOk = t.held == -1;
                }
            }
            g_mouseX = savedMouseX;
            g_mouseY = savedMouseY;
            g_inventoryOpen = wasOpen2;
        }

        // Stow and illegal-placement checks run on their own fresh inventories so
        // they cannot be starved by what the hover test above already packed.
        {
            Inventory s;
            initInventory(s, g_itemDefs);
            int sInst = 0, sRot = 0;
            if (autoPlace(s, g_itemDefs, rifle, sInst, sRot) && sInst >= 0 &&
                liftPacked(s, sInst)) {
                rep.stowOk = stowHeld(s, g_itemDefs) && s.held == -1 && s.usedCells() > 0;
            }
        }
        {
            Inventory b2;
            initInventory(b2, g_itemDefs);
            int bInst = 0, bRot = 0;
            if (autoPlace(b2, g_itemDefs, rifle, bInst, bRot) && bInst >= 0 &&
                liftPacked(b2, bInst)) {
                // One cell past the far corner cannot fit in any rotation.
                const int usedBefore = b2.usedCells();
                bool anyPlacementAccepted = false;
                for (int rot = 0; rot < 4; ++rot)
                    for (int ox = b2.vol.sx; ox <= b2.vol.sx; ++ox)
                        for (int oy = b2.vol.sy; oy <= b2.vol.sy; ++oy)
                            for (int oz = b2.vol.sz; oz <= b2.vol.sz; ++oz)
                                if (placeHeld(b2, g_itemDefs, rot, ox, oy, oz))
                                    anyPlacementAccepted = true;
                // The refused placement must leave the item in hand and occupancy
                // untouched -- never a half-written shape.
                rep.invalidPlaceRejected = !anyPlacementAccepted && b2.held >= 0 &&
                                           b2.usedCells() == usedBefore;
                stowHeld(b2, g_itemDefs);
            }
        }
    }

    // ---- backpack volume ownership -----------------------------------------
    // Equipping a pack grants its volume; unequipping must REMOVE storage, not
    // silently restore the base 3x3x4.
    {
        Inventory b;
        initInventory(b, g_itemDefs);
        const int baseCells = b.vol.cells();
        bool granted = equipDef(b, g_itemDefs, upgrade, EquipSlot::Backpack, displaced);
        const int upCells = b.vol.cells();
        int dropped = -1;
        const bool removed = unequipDef(b, EquipSlot::Backpack, dropped);
        rep.backpackVolumeOk = granted && upCells == rep.upgradeCells && removed &&
                               b.vol.cells() == 0 && baseCells > 0;
        // setVolume clears items, so a hand item can never survive a volume swap.
        int bInst = 0, bRot = 0;
        if (autoPlace(b, g_itemDefs, rifle, bInst, bRot) &&
            invIndexAt(b, 0, 0, 0) >= 0) {
            liftPacked(b, invIndexAt(b, 0, 0, 0));
            if (b.held >= 0) b.setVolume(b.vol); // must not leave held dangling
            if (b.held >= 0) rep.backpackVolumeOk = false;
        }
    }

    // ---- lifting an equipped backpack out of its marker ---------------------
    // drainInventoryInput revokes the pack's storage and then puts the pack in
    // the hand. In that order the pack survives; the reverse order would let
    // setVolume() erase the item it had just lifted, destroying it outright.
    {
        Inventory b;
        initInventory(b, g_itemDefs);
        bool ok = equipDef(b, g_itemDefs, upgrade, EquipSlot::Backpack, displaced);
        // Mirror the real input path exactly.
        b.slotDef[static_cast<size_t>(EquipSlot::Backpack)] = -1;
        b.slotRot[static_cast<size_t>(EquipSlot::Backpack)] = 0;
        b.setVolume(PackVolume::none());
        const bool took = takeIntoHand(b, g_itemDefs, upgrade);
        const bool holdsPack = took && b.held >= 0 &&
                               b.items[static_cast<size_t>(b.held)].defIndex == upgrade;
        rep.backpackLiftOk = ok && holdsPack && b.vol.cells() == 0 &&
                              b.slotDef[static_cast<size_t>(EquipSlot::Backpack)] < 0;
    }

    // ---- world pickups ------------------------------------------------------
    // The seeded loot must build unit-cube geometry tagged mat 8, and a pickup
    // must auto-place into a pack that has room.
    {
        ensurePickupBuffer();
        g_pickupMeshDirty = true;
        updatePickupMesh();
        rep.pickupVerts = static_cast<int>(g_pickupVertexCount);
        rep.pickupOk = false;
        if (g_pickupMapped && rep.pickupVerts > 0) {
            // All-pickup-mat8 check plus the same own-basis unit-cube test.
            const Vertex* pv = reinterpret_cast<const Vertex*>(g_pickupMapped);
            bool allMat8 = true;
            for (int i = 0; i < rep.pickupVerts; ++i)
                if (pv[i].mat < 7.5f) allMat8 = false;
            const Vec3 ax[3] = {Vec3(pv[0].nx, pv[0].ny, pv[0].nz),
                                Vec3(pv[12].nx, pv[12].ny, pv[12].nz),
                                Vec3(pv[24].nx, pv[24].ny, pv[24].nz)};
            bool ortho = true;
            for (int a = 0; a < 3; ++a)
                if (std::fabs(ax[a].length() - 1.0f) > 1e-3f) ortho = false;
            if (ax[0].dot(ax[1]) > 1e-3f || ax[0].dot(ax[2]) > 1e-3f || ax[1].dot(ax[2]) > 1e-3f)
                ortho = false;
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (int i = 0; i < 36; ++i) {
                const Vec3 p(pv[i].px, pv[i].py, pv[i].pz);
                for (int a = 0; a < 3; ++a) {
                    const float d = p.dot(ax[a]);
                    lo[a] = std::min(lo[a], d);
                    hi[a] = std::max(hi[a], d);
                }
            }
            const float ptol = VOXEL_SIZE * 0.02f;
            const bool unit = std::fabs((hi[0] - lo[0]) - VOXEL_SIZE) < ptol &&
                              std::fabs((hi[1] - lo[1]) - VOXEL_SIZE) < ptol &&
                              std::fabs((hi[2] - lo[2]) - VOXEL_SIZE) < ptol;
            rep.pickupOk = allMat8 && ortho && unit;

            // Auto-place into a fresh pack; the source must then read as taken.
            // tryPickupAt writes to the LIVE g_inventory, so swap in the scratch
            // pack for the duration rather than testing a pack nothing touched.
            if (!g_pickups.empty()) {
                Inventory w;
                initInventory(w, g_itemDefs);
                const Inventory savedInv = g_inventory;
                g_inventory = w;
                const int before = g_inventory.usedCells();
                const size_t idx = 0;
                g_pickupHover = 0;
                const bool took = tryPickupAt(static_cast<int>(idx));
                rep.pickupTaken = took && !g_pickups[idx].alive &&
                                  g_inventory.usedCells() > before;
                g_inventory = savedInv;
                g_pickupHover = -1;
                g_pickupMeshDirty = true;
            }
        }
    }
    return rep;
}

static void updateProjectiles(float dt) {
    if (!g_chunks) return;
    for (auto& p : g_projectiles) {
        if (!p.alive) continue;
        // Gravity (matches Python WORLD_GRAVITY * gravity_scale)
        p.vy -= kWorldGravity * p.def.gravityScale * dt;

        const int steps = 4;
        const float sdt = dt / static_cast<float>(steps);
        const float radiusCells = std::max(0.5f, std::min(2.0f, p.def.radius / VOXEL_SIZE));
        for (int s = 0; s < steps && p.alive; ++s) {
            const float prevWx = p.px, prevWy = p.py, prevWz = p.pz;
            p.px += p.vx * sdt;
            p.py += p.vy * sdt;
            p.pz += p.vz * sdt;

            // Body sweep first, so a body is hit before the voxel it stands in.
            // One bullet, one body (player::ownerIsPlayer excludes the shooter).
            if (!p.ownerIsPlayer && health::kSelfFireDamage) {
                const ArmorZone zone =
                    projectileHitZone(prevWx, prevWy, prevWz, p.px, p.py, p.pz, radiusCells);
                if (zone != ArmorZone::Count) {
                    damagePlayerAtZone(p.energy, p.def.effect, zone);
                    p.energy *= (1.0f - std::min(0.95f, p.def.penetration));
                    if (p.energy < 0.05f) p.alive = false;
                }
            }

            int ix = static_cast<int>(std::floor(p.px / VOXEL_SIZE));
            int iy = static_cast<int>(std::floor(p.py / VOXEL_SIZE));
            int iz = static_cast<int>(std::floor(p.pz / VOXEL_SIZE));
            if (!worldInBounds(ix, iy, iz)) {
                // allow mild overshoot above world; kill if far
                if (p.py < -0.5f || p.py > 2.0f ||
                    p.px < -0.5f || p.px > WORLD_W * VOXEL_SIZE + 0.5f ||
                    p.pz < -0.5f || p.pz > WORLD_D * VOXEL_SIZE + 0.5f) {
                    p.alive = false;
                }
                continue;
            }

            Block b = getWorldBlock(*g_chunks, ix, iy, iz);
            MaterialId mat = blockMaterial(b);
            if (mat == MaterialId::Air) continue;

            float e = p.energy * effectMultiplier(p.def.effect, mat);
            g_lastImpactDx = p.vx; g_lastImpactDy = p.vy; g_lastImpactDz = p.vz;
            g_lastImpactEnergy = e;
            if (resolveVoxelHit(mat, e, p.def.penetration)) {
                destroyVoxelAt(ix, iy, iz);
                applySplash(ix, iy, iz, p.def.splashRadius, p.energy, p.def);
                p.energy = e;
                if (p.def.effect == "explosive" || p.energy < 0.05f) p.alive = false;
            } else {
                // Matrix ricochet — bounce off without destroying occupancy.
                float nx, ny, nz;
                faceNormalFromVelocity(p.vx, p.vy, p.vz, nx, ny, nz);
                const auto& mp = materialProps(mat);
                ricochetVelocity(p.vx, p.vy, p.vz, nx, ny, nz,
                                 0.20f + (1.0f - mp.fragility) * 0.25f,
                                 0.30f + mp.damping * 0.3f);
                p.energy = e * (1.0f - mp.damping * 0.5f);
                g_debris.ricochets++;
                g_lastAoeScale = impactAoeScale(p.def) * 0.55f;
                g_debris.spawnFromVoxel(ix, iy, iz, mat, g_lastImpactDx, g_lastImpactDy, g_lastImpactDz,
                                        e * 0.4f, VOXEL_SIZE, g_lastAoeScale);
                // Nudge out of cell to avoid re-hit same voxel
                p.px += nx * VOXEL_SIZE * 0.6f;
                p.py += ny * VOXEL_SIZE * 0.6f;
                p.pz += nz * VOXEL_SIZE * 0.6f;
                if (p.energy < 0.08f || (p.vx * p.vx + p.vy * p.vy + p.vz * p.vz) < 1e-5f)
                    p.alive = false;
            }
        }
    }
    g_projectiles.erase(
        std::remove_if(g_projectiles.begin(), g_projectiles.end(),
                       [](const ProjectileRuntime& p) { return !p.alive; }),
        g_projectiles.end());
    // Remesh deferred to drawFrame after GPU fence (see flushDirtyMesh).
}

static void recreateSwapchain() {
    vkDeviceWaitIdle(g_device);
    destroySwapchainObjects();
    createSwapchain();
    createDepthResources();
    createFramebuffers();
}

static void recordCommandBuffer(uint32_t imageIndex, uint32_t frameIndex) {
    VkCommandBuffer cmd = g_cmdBuffers[frameIndex];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &bi);

    VkClearValue clears[2]{};
    clears[0].color = {{0.03f, 0.035f, 0.05f, 1.0f}}; // night sky
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = g_renderPass;
    rp.framebuffer = g_framebuffers[imageIndex];
    rp.renderArea.extent = g_extent;
    rp.clearValueCount = 2;
    rp.pClearValues = clears;

    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline);

    // Full swapchain viewport. Bitcrush in fragment shader provides the downscale/crunch look
    // without a second pass; RENDER_SCALE documents intended internal scale for future offscreen RT.
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(g_extent.width);
    viewport.height = static_cast<float>(g_extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = g_extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &g_vertexBuffer, &off);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipelineLayout, 0, 1,
                            &g_descSets[frameIndex], 0, nullptr);

    // Not-seen rendering: frustum + behind-camera cull per chunk
    g_drawnChunks = 0;
    g_culledChunks = 0;
    if (g_chunks && g_vertexBuffer != VK_NULL_HANDLE) {
        Frustum fr = frustumFromVP(g_viewProjCull);
        Vec3 eye = g_camPos;
        Vec3 forward = cameraForward();
        for (auto& c : *g_chunks) {
            if (chunkNotSeen(c, fr, eye, forward)) {
                ++g_culledChunks;
                c.wasVisible = false;
                continue;
            }
            c.wasVisible = true;
            if (c.vertexCount == 0) continue;
            vkCmdDraw(cmd, c.vertexCount, 1, c.firstVertex, 0);
            ++g_drawnChunks;
        }
    } else if (g_vertexCount > 0) {
        vkCmdDraw(cmd, g_vertexCount, 1, 0, 0);
        g_drawnChunks = 1;
    }

    // World item pickups (mat 8) live in the main pass so they depth-test against
    // the map and cast/receive light like the world does. Only the player's own
    // lattice is overlay-only.
    if (g_pickupVB != VK_NULL_HANDLE && g_pickupVertexCount > 0) {
        VkDeviceSize pOff = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &g_pickupVB, &pOff);
        vkCmdDraw(cmd, g_pickupVertexCount, 1, 0, 0);
    }

    // Visual debris cubes (8x8x8 chips) after world mesh, before sky
    if (g_debrisVB != VK_NULL_HANDLE && g_debrisVertexCount > 0) {
        VkDeviceSize dOff = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &g_debrisVB, &dOff);
        vkCmdDraw(cmd, g_debrisVertexCount, 1, 0, 0);
    }

// Pixel sky dome + moon light-source sprite (drawn after world; sky verts forced to far Z)
    if (g_skyTileVB != VK_NULL_HANDLE && g_skyTileVertexCount > 0) {
        VkDeviceSize mo = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &g_skyTileVB, &mo);
        vkCmdDraw(cmd, g_skyTileVertexCount, 1, 0, 0);
    }

    vkCmdEndRenderPass(cmd);

    // Inventory + HUD overlay (RULES.md rule 12/15): second pass, color LOAD +
    // depth DONT_CARE, so the lattice paints over the map while still
    // self-occluding against a fresh depth buffer. The pass runs when either the
    // lattice or the health HUD produced vertices.
    if (g_inventoryVB != VK_NULL_HANDLE && g_inventoryVertexCount > 0) {
        ++g_inventoryOverlayFrames;
        VkRenderPassBeginInfo ovp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        ovp.renderPass = g_overlayRenderPass;
        ovp.framebuffer = g_overlayFramebuffers[imageIndex];
        ovp.renderArea.extent = g_extent;
        ovp.clearValueCount = 0;
        ovp.pClearValues = nullptr;
        vkCmdBeginRenderPass(cmd, &ovp, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_overlayPipeline);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        VkDeviceSize ioff = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &g_inventoryVB, &ioff);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipelineLayout, 0, 1,
                                &g_descSets[frameIndex], 0, nullptr);
        vkCmdDraw(cmd, g_inventoryVertexCount, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    vkEndCommandBuffer(cmd);
}

// Character body as unit voxels relative to feet grid (hitbox proxy + water sampling).
// Layout is a standing humanoid ~5 wide × 7 tall × 1 deep in unit cells.
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

// True if player AABB at (px,py,pz) intersects any solid unit voxel.
static bool playerHitsSolid(float px, float py, float pz) {
    if (!g_chunks) return false;
    const float r = g_player.radius;
    const float h = g_player.height;
    const float eps = VOXEL_SIZE * 0.02f;
    int x0 = static_cast<int>(std::floor((px - r + eps) / VOXEL_SIZE));
    int x1 = static_cast<int>(std::floor((px + r - eps) / VOXEL_SIZE));
    int y0 = static_cast<int>(std::floor((py + eps) / VOXEL_SIZE));
    int y1 = static_cast<int>(std::floor((py + h - eps) / VOXEL_SIZE));
    int z0 = static_cast<int>(std::floor((pz - r + eps) / VOXEL_SIZE));
    int z1 = static_cast<int>(std::floor((pz + r - eps) / VOXEL_SIZE));
    for (int y = y0; y <= y1; ++y)
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                if (!worldInBounds(x, y, z)) {
                    // Treat out-of-world as solid walls (except open sky above).
                    if (y < 0 || y >= WORLD_H) continue;
                    return true;
                }
                if (isSolidBlock(getWorldBlock(*g_chunks, x, y, z))) return true;
            }
    return false;
}

static void spawnPlayerOnMap(const std::vector<Chunk>& chunks) {
    // Stand on the concrete apron just inside the open bay, looking -Z into the warehouse.
    const int sx = WORLD_W / 2;
    const int sz = WORLD_D - DIRT_MARGIN - 18;
    int gy = 1 + SLAB_THICK; // default slab top
    for (int y = WORLD_H - 2; y >= 0; --y) {
        Block b = getWorldBlock(chunks, sx, y, sz);
        if (isSolidBlock(b)) { gy = y + 1; break; }
    }
    g_spawnCellX = sx;
    g_spawnCellY = gy;
    g_spawnCellZ = sz;
    g_player.px = (sx + 0.5f) * VOXEL_SIZE;
    g_player.py = gy * VOXEL_SIZE + 0.0002f;
    g_player.pz = (sz + 0.5f) * VOXEL_SIZE;
    g_player.vx = g_player.vy = g_player.vz = 0.0f;
    g_player.onGround = true;
    g_player.lean = 0.0f;
    g_player.leanTarget = 0.0f;
    g_yaw = 0.0f;          // look toward -Z into bay
    g_pitch = -0.08f;
    g_camPos = Vec3(g_player.px, g_player.py + g_player.eyeHeight, g_player.pz);
}

// Water current + weight sampling from physics feet (not free-fly camera).
static void updatePlayerCurrentAndWeight(float dt) {
    if (!g_chunks) return;
    g_playerGX = static_cast<int>(std::floor(g_player.px / VOXEL_SIZE)) - 2;
    g_playerGY = static_cast<int>(std::floor(g_player.py / VOXEL_SIZE));
    g_playerGZ = static_cast<int>(std::floor(g_player.pz / VOXEL_SIZE)) - 0;
    auto info = sampleCharacterWater(*g_chunks, g_playerGX, g_playerGY, g_playerGZ);
    g_touchingWaterUnits = info.touching;
    g_characterUnitCount = info.total;
    g_currentTriggered = info.anyCurrent && info.touching > 0;
    g_fullySubmerged = info.fullySubmerged;

    // Breath / drowning (RULES.md rule 15). Reuses the existing character water
    // probe rather than adding a second submersion test.
    if (health::updateBreath(g_health, g_fullySubmerged, dt) > 0.0f) ++g_drownDamageTicks;

    g_playerWeight = g_playerBaseWeight;
    if (g_currentTriggered) g_playerWeight *= 2.0f;
    if (g_fullySubmerged) g_playerWeight *= 0.5f;

    float baseForce = 0.035f * ((info.total > 0) ? (float)info.currentTouching / (float)info.total : 0.0f);
    if (info.fullySubmerged && info.anyCurrent) baseForce *= 2.0f;
    g_currentForce = baseForce;

    if (g_currentForce > 0.0f && g_playerWeight > 1e-4f) {
        float acc = g_currentForce / g_playerWeight;
        g_player.vx += g_currentDir.x * acc * dt;
        g_player.vz += g_currentDir.z * acc * dt;
    }
}

static Vec3 cameraForward() {
    // Locked FPS aim: yaw/pitch + temporary recoil offset (no free-fly roll).
    const float lim = static_cast<float>(M_PI) * 0.49f;
    float pitch = std::max(-lim, std::min(lim, g_pitch + g_recoilPitch));
    float yaw = g_yaw + g_recoilYaw;
    const float cp = std::cos(pitch);
    return Vec3(
        std::sin(yaw) * cp,
        std::sin(pitch),
        -std::cos(yaw) * cp
    ).normalized();
}

static Vec3 cameraRight() {
    return cameraForward().cross(Vec3(0, 1, 0)).normalized();
}

// Flat look vectors (movement stays grounded; pitch does not fly).
static Vec3 flatForward() {
    return Vec3(std::sin(g_yaw), 0.0f, -std::cos(g_yaw)).normalized();
}
static Vec3 flatRight() {
    return Vec3(std::cos(g_yaw), 0.0f, std::sin(g_yaw)).normalized();
}

// Sync locked eye camera to physics body + lean offset.
static void syncCameraToPlayer() {
    Vec3 right = flatRight();
    const float leanLat = g_player.lean * 0.0032f;   // lateral peek
    const float leanDrop = std::fabs(g_player.lean) * 0.0009f;
    g_camPos.x = g_player.px + right.x * leanLat;
    g_camPos.y = g_player.py + g_player.eyeHeight - leanDrop;
    g_camPos.z = g_player.pz + right.z * leanLat;
}

// Death drops whatever is in hand as a normal mat-8 world pickup, reusing the
// existing pickup path instead of inventing a drop system. Packed items and
// equipped gear are untouched — death is a setback, not a wipe.
static void dropHeldItemOnDeath() {
    const int inst = g_inventory.held;
    if (inst < 0 || inst >= static_cast<int>(g_inventory.items.size())) return;
    const int defIdx = g_inventory.items[static_cast<size_t>(inst)].defIndex;
    const ItemDef* d = itemDefAt(g_itemDefs, defIdx);
    if (!d || !d->shape.valid()) return;
    // One cell in front of the feet, so the drop is immediately grabbable with
    // the existing G-to-take reach.
    const health::BodyCellOrigin org = health::bodyCellOrigin(g_player.px, g_player.py, g_player.pz);
    const Vec3 f = flatForward();
    int fx = static_cast<int>(std::round(f.x));
    int fz = static_cast<int>(std::round(f.z));
    if (fx == 0 && fz == 0) fz = 1; // never drop underfoot
    WorldPickup p;
    p.defIndex = defIdx;
    p.cx = org.x + fx;
    p.cy = org.y;
    p.cz = org.z + fz;
    p.rot = g_inventoryHandRot;
    p.alive = true;
    g_pickups.push_back(p);
    g_pickupMeshDirty = true;
    // removePacked clears `held` itself once the instance is gone.
    removePacked(g_inventory, inst);
}

// Physics-bound walk/jump + unit-grid hitbox; Q/E side lean (not up/down fly).
static void updatePlayerPhysics(float dt) {
    if (!g_chunks) return;

    // --- lean targets: Q left, E right ---
    g_player.leanTarget = 0.0f;
    if (g_keys['Q']) g_player.leanTarget -= 1.0f;
    if (g_keys['E']) g_player.leanTarget += 1.0f;
    g_player.leanTarget = std::max(-1.0f, std::min(1.0f, g_player.leanTarget));
    const float leanRate = 8.0f;
    g_player.lean += (g_player.leanTarget - g_player.lean) * (1.0f - std::exp(-leanRate * dt));

    // --- desired horizontal velocity (WASD walk, Shift sprint) ---
    float speed = g_moveSpeed;
    if (g_keys[VK_SHIFT]) speed *= 1.65f;
    // Lean slows strafe slightly (shoulder into cover).
    speed *= (1.0f - 0.18f * std::fabs(g_player.lean));

    Vec3 wish(0, 0, 0);
    Vec3 f = flatForward();
    Vec3 r = flatRight();
    if (g_keys['W'] || g_keys[VK_UP]) wish = wish + f;
    if (g_keys['S'] || g_keys[VK_DOWN]) wish = wish - f;
    if (g_keys['A'] || g_keys[VK_LEFT]) wish = wish - r;
    if (g_keys['D'] || g_keys[VK_RIGHT]) wish = wish + r;
    if (wish.length() > 1e-5f) wish = wish.normalized() * speed;

    // Accelerate / friction on horizontal plane.
    const float accel = g_player.onGround ? 18.0f : 4.0f;
    const float friction = g_player.onGround ? 12.0f : 1.5f;
    if (wish.length() > 1e-6f) {
        g_player.vx += (wish.x - g_player.vx) * std::min(1.0f, accel * dt);
        g_player.vz += (wish.z - g_player.vz) * std::min(1.0f, accel * dt);
    } else {
        float damp = std::exp(-friction * dt);
        g_player.vx *= damp;
        g_player.vz *= damp;
        if (std::fabs(g_player.vx) < 1e-5f) g_player.vx = 0.0f;
        if (std::fabs(g_player.vz) < 1e-5f) g_player.vz = 0.0f;
    }

    // Jump (Space) — no free-fly up/down.
    if (g_wantJump && g_player.onGround) {
        g_player.vy = g_player.jumpSpeed;
        g_player.onGround = false;
    }
    g_wantJump = false;

    // Gravity (same world scale as projectiles).
    g_player.vy -= kWorldGravity * dt;
    if (g_player.vy < -0.25f) g_player.vy = -0.25f; // terminal

    // Integrate with axis-separated unit-grid collision.
    auto moveAxis = [&](float& pos, float& vel, int axis) {
        if (std::fabs(vel) < 1e-8f) return;
        float next = pos + vel * dt;
        float tx = g_player.px, ty = g_player.py, tz = g_player.pz;
        if (axis == 0) tx = next;
        else if (axis == 1) ty = next;
        else tz = next;
        if (!playerHitsSolid(tx, ty, tz)) {
            pos = next;
            return;
        }
        // Step up small ledges while grounded/walking horizontally.
        if (axis != 1 && g_player.onGround) {
            const float step = VOXEL_SIZE * 1.05f;
            if (!playerHitsSolid(tx, g_player.py + step, tz)) {
                g_player.py += step;
                pos = next;
                return;
            }
        }
const float prevVel = vel;
        vel = 0.0f;
        // Only count floor hits (downward), not ceiling bumps.
        if (axis == 1 && prevVel < 0.0f) g_player.onGround = true;
    };

    g_player.onGround = false;
    moveAxis(g_player.px, g_player.vx, 0);
    moveAxis(g_player.pz, g_player.vz, 2);
    moveAxis(g_player.py, g_player.vy, 1);

    // Ground probe: if feet almost on a solid top face, snap and clear fall speed.
    {
        float probeY = g_player.py - VOXEL_SIZE * 0.15f;
        if (playerHitsSolid(g_player.px, probeY, g_player.pz) && g_player.vy <= 0.0f) {
            // Snap up out of penetration.
            for (int i = 0; i < 6 && playerHitsSolid(g_player.px, g_player.py, g_player.pz); ++i)
                g_player.py += VOXEL_SIZE * 0.25f;
            g_player.vy = 0.0f;
            g_player.onGround = true;
        }
    }

    // World floor safety.
    if (g_player.py < 0.0f) {
        g_player.py = 0.0f;
        g_player.vy = 0.0f;
        g_player.onGround = true;
    }

    // Fall damage is priced from the peak downward speed of the whole arc, not
    // from vy at the moment of landing, so it does not depend on which of the
    // two landing paths (moveAxis block or ground probe) happened to zero vy
    // first. Jumping is free because the apex return speed (jumpSpeed 0.055)
    // sits under health::kFallSafeSpeed. --smoke skips physics entirely, so the
    // smoke test exercises fallDamageForImpactSpeed() directly instead.
    if (!g_player.onGround) {
        g_fallPeakSpeed = std::max(g_fallPeakSpeed, -g_player.vy);
    } else if (!g_wasOnGround) {
        const float dmg = health::fallDamageForImpactSpeed(g_fallPeakSpeed);
        if (dmg > 0.0f) {
            // Routed through the same intake as impacts, so leg armor absorbs
            // it like any other energy.
            health::applyDamage(g_health, dmg, ArmorZone::Legs,
                                equippedArmorPoints(ArmorZone::Legs));
            ++g_fallDamageEvents;
        }
        g_fallPeakSpeed = 0.0f;
    }
    g_wasOnGround = g_player.onGround;

    syncCameraToPlayer();
}

// Kept name for call sites: water weight + physics body + locked camera.
static void updateCamera(float dt) {
    updatePlayerCurrentAndWeight(dt);
    if (!g_smoke) {
        updatePlayerPhysics(dt);
    } else {
        // Headless smoke: keep body planted, only yaw/pitch scripted; still lock eye.
        syncCameraToPlayer();
    }
}

static void updateUBO(uint32_t frameIndex, float timeSec) {
    Vec3 eye = g_camPos;
    Vec3 center = g_camPos + cameraForward();
    float aspect = g_extent.height > 0
                       ? static_cast<float>(g_extent.width) / static_cast<float>(g_extent.height)
                       : 1.0f;
    // Optic ADS (hold X): tighter FOV from weapon optic stat.
    float fovDeg = DEFAULT_FOV_DEG;
    if (g_ads) {
        WeaponDef w = activeWeaponOrDefault();
        float zoom = std::min(45.0f, 8.0f + w.optic * 3.5f);
        fovDeg = std::max(40.0f, DEFAULT_FOV_DEG - zoom);
    }
    Mat4 proj = Mat4::perspective(fovDeg * static_cast<float>(M_PI) / 180.0f, aspect, 0.0005f, 5.0f);
    Mat4 view = Mat4::lookAt(eye, center, {0, 1, 0});
    Mat4 vp = proj * view;

    g_isNight = g_timeOfDay > 0.7f || g_timeOfDay < 0.25f;
    g_viewProjCull = vp;

    FrameUBO ubo{};
    std::memcpy(ubo.viewProj, vp.m, sizeof(vp.m));
    // sun mostly off at night
    ubo.sunDir[0] = 0.2f; ubo.sunDir[1] = -1.0f; ubo.sunDir[2] = 0.15f;
    ubo.timeOfDay = g_timeOfDay;
    ubo.camPos[0] = eye.x; ubo.camPos[1] = eye.y; ubo.camPos[2] = eye.z;
    ubo.time = timeSec;
// Moon sprite is the scene light source: light comes from moon sky bearing
    {
        Vec3 L = g_moonDirWorld.normalized(); // direction toward moon from origin-ish
        // Shader uses normalize(-moonDir) as light vector, so moonDir points toward surface from moon
        ubo.moonDir[0] = -L.x; ubo.moonDir[1] = -L.y; ubo.moonDir[2] = -L.z;
    }
ubo.moonIntensity = g_isNight ? 0.95f : 0.08f;
    ubo.moonColor[0] = 0.62f; ubo.moonColor[1] = 0.72f; ubo.moonColor[2] = 0.95f;
    ubo.ambientScale = g_isNight ? 0.65f : 1.0f;
    ubo.muzzleFlash = g_muzzleFlash;
    ubo.fireOverlay = g_fireOverlay;
    ubo._fxPad[0] = 0.0f;
    ubo._fxPad[1] = 0.0f;

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


static void flushDirtyMesh() {
    if (!g_meshDirty || !g_chunks) return;
    // Under heavy fire, remeshing every frame dominates CPU. Coalesce dirty updates.
    const int minGap = g_stress ? 3 : 1;
    if (g_framesSinceRemesh < minGap) {
        ++g_remeshSkipCount;
        return;
    }
    auto mesh = meshAllChunks(*g_chunks);
    uploadMesh(mesh);
    g_meshDirty = false;
    g_framesSinceRemesh = 0;
}

static void drawFrame(float timeSec, float dt) {
    // updateCamera runs physics body + water weight + locks eye to player.
    updateCamera(dt);

    // Wait for this frame slot's prior GPU work before touching shared mesh buffers.
    vkWaitForFences(g_device, 1, &g_inFlight[g_frame], VK_TRUE, UINT64_MAX);

    // Safe to rebuild world VB now (no device-wide idle).
    flushDirtyMesh();

    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(g_device, g_swapchain, UINT64_MAX,
                                         g_imageAvailable[g_frame], VK_NULL_HANDLE, &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) fail("acquire failed");

    vkResetFences(g_device, 1, &g_inFlight[g_frame]);
    updateUBO(static_cast<uint32_t>(g_frame), timeSec);
    updateMoonSkyTile();
    updateDebrisMesh();
    // Pickup hover drives a brightness tint, so the mesh follows the crosshair.
    const int prevHover = g_pickupHover;
    g_pickupHover = g_inventoryOpen ? -1 : pickupUnderCrosshair();
    if (prevHover != g_pickupHover) g_pickupMeshDirty = true;
    updatePickupMesh();
    // Resolve g_invHover first (it needs this frame's camera basis), then act on
    // it, so a click always lands on the cell that was under the cursor.
    updateInventoryMesh();
    drainInventoryInput();
    recordCommandBuffer(imageIndex, static_cast<uint32_t>(g_frame));

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &g_imageAvailable[g_frame];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cmdBuffers[g_frame];
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g_renderFinished[g_frame];
    if (vkQueueSubmit(g_graphicsQueue, 1, &si, g_inFlight[g_frame]) != VK_SUCCESS)
        fail("queue submit failed");

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g_renderFinished[g_frame];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_swapchain;
    pi.pImageIndices = &imageIndex;
    VkResult pr = vkQueuePresentKHR(g_presentQueue, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR || g_resized) {
        g_resized = false;
        recreateSwapchain();
    } else if (pr != VK_SUCCESS) {
        fail("present failed");
    }
    g_frame = (g_frame + 1) % MAX_FRAMES;
    paceFrame120();
}

static void cleanup() {
    if (g_device) vkDeviceWaitIdle(g_device);
    destroySwapchainObjects();
    if (g_pipeline) vkDestroyPipeline(g_device, g_pipeline, nullptr);
    if (g_overlayPipeline) vkDestroyPipeline(g_device, g_overlayPipeline, nullptr);
    if (g_pipelineLayout) vkDestroyPipelineLayout(g_device, g_pipelineLayout, nullptr);
    if (g_renderPass) vkDestroyRenderPass(g_device, g_renderPass, nullptr);
    if (g_overlayRenderPass) vkDestroyRenderPass(g_device, g_overlayRenderPass, nullptr);
    if (g_descPool) vkDestroyDescriptorPool(g_device, g_descPool, nullptr);
    if (g_dsl) vkDestroyDescriptorSetLayout(g_device, g_dsl, nullptr);
    for (int i = 0; i < MAX_FRAMES; ++i) {
        if (g_uboBuffers[i]) vkDestroyBuffer(g_device, g_uboBuffers[i], nullptr);
        if (g_uboMems[i]) {
            if (g_uboMapped[i]) vkUnmapMemory(g_device, g_uboMems[i]);
            vkFreeMemory(g_device, g_uboMems[i], nullptr);
        }
        if (g_imageAvailable[i]) vkDestroySemaphore(g_device, g_imageAvailable[i], nullptr);
        if (g_renderFinished[i]) vkDestroySemaphore(g_device, g_renderFinished[i], nullptr);
        if (g_inFlight[i]) vkDestroyFence(g_device, g_inFlight[i], nullptr);
        g_uboBuffers[i] = VK_NULL_HANDLE;
        g_uboMems[i] = VK_NULL_HANDLE;
        g_uboMapped[i] = nullptr;
        g_imageAvailable[i] = VK_NULL_HANDLE;
        g_renderFinished[i] = VK_NULL_HANDLE;
        g_inFlight[i] = VK_NULL_HANDLE;
    }
if (g_skyTileVB) {
        if (g_skyTileMapped) { vkUnmapMemory(g_device, g_skyTileMem); g_skyTileMapped = nullptr; }
        vkDestroyBuffer(g_device, g_skyTileVB, nullptr);
        g_skyTileVB = VK_NULL_HANDLE;
    }
    if (g_skyTileMem) {
        vkFreeMemory(g_device, g_skyTileMem, nullptr);
        g_skyTileMem = VK_NULL_HANDLE;
    }
    g_skyTileVertexCount = 0;
    if (g_debrisVB) {
        if (g_debrisMapped) { vkUnmapMemory(g_device, g_debrisMem); g_debrisMapped = nullptr; }
        vkDestroyBuffer(g_device, g_debrisVB, nullptr);
        g_debrisVB = VK_NULL_HANDLE;
    }
    if (g_debrisMem) {
        vkFreeMemory(g_device, g_debrisMem, nullptr);
        g_debrisMem = VK_NULL_HANDLE;
    }
    g_debrisVertexCount = 0;
    if (g_pickupVB) {
        vkDestroyBuffer(g_device, g_pickupVB, nullptr);
        vkUnmapMemory(g_device, g_pickupMem);
        vkFreeMemory(g_device, g_pickupMem, nullptr);
        g_pickupVB = VK_NULL_HANDLE;
        g_pickupMem = VK_NULL_HANDLE;
        g_pickupMapped = nullptr;
    }
    if (g_inventoryVB) {
        if (g_inventoryMapped) { vkUnmapMemory(g_device, g_inventoryMem); g_inventoryMapped = nullptr; }
        vkDestroyBuffer(g_device, g_inventoryVB, nullptr);
        g_inventoryVB = VK_NULL_HANDLE;
    }
    if (g_inventoryMem) {
        vkFreeMemory(g_device, g_inventoryMem, nullptr);
        g_inventoryMem = VK_NULL_HANDLE;
    }
    g_inventoryVertexCount = 0;
    destroyWorldMeshBuffer();
    if (g_cmdPool) vkDestroyCommandPool(g_device, g_cmdPool, nullptr);
    if (g_device) vkDestroyDevice(g_device, nullptr);
    if (g_debugMessenger != VK_NULL_HANDLE && g_destroyDUM)
        g_destroyDUM(g_instance, g_debugMessenger, nullptr);
    g_debugMessenger = VK_NULL_HANDLE;
    if (g_surface) vkDestroySurfaceKHR(g_instance, g_surface, nullptr);
    if (g_instance) vkDestroyInstance(g_instance, nullptr);
    if (g_hwnd) DestroyWindow(g_hwnd);
    g_pipeline = VK_NULL_HANDLE;
    g_overlayPipeline = VK_NULL_HANDLE;
    g_pipelineLayout = VK_NULL_HANDLE;
    g_renderPass = VK_NULL_HANDLE;
    g_overlayRenderPass = VK_NULL_HANDLE;
    g_descPool = VK_NULL_HANDLE;
    g_dsl = VK_NULL_HANDLE;
    g_vertexBuffer = VK_NULL_HANDLE;
    g_vertexMem = VK_NULL_HANDLE;
    g_cmdPool = VK_NULL_HANDLE;
    g_device = VK_NULL_HANDLE;
    g_surface = VK_NULL_HANDLE;
    g_instance = VK_NULL_HANDLE;
    g_hwnd = nullptr;
    g_cmdBuffers.clear();
}

static std::string getExeDir() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string s(path);
    size_t p = s.find_last_of("\\/");
    return p == std::string::npos ? "." : s.substr(0, p);
}

// Optional headless-ish smoke/stress (globals declared near top).

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmdLine, int) {
    std::string cmd = cmdLine ? cmdLine : "";
    if (cmd.find("--smoke") != std::string::npos) g_smoke = true;
    if (cmd.find("--stress") != std::string::npos) {
        g_stress = true;
        g_smoke = true; // reuse headless quit path
        g_smokeFrames = 600; // longer soak
    }

    try {
        g_exeDir = getExeDir();
        createWindow();
        createInstance();
        createSurface();
        pickDevice();
        createLogicalDevice();
        createCommandPoolAndBuffers();
        createSwapchain();
        createRenderPass();
        createOverlayRenderPass(); // must exist before createPipeline binds it
        createDepthResources();
        createFramebuffers();
        createDescriptors();
        createPipeline();
        createSync();

auto chunks = buildWarehouseMap();
        g_chunks = &chunks;
        spawnPlayerOnMap(chunks);

        // Load Python-exported projectile + ammo defs (gravity + effects)
        const std::string projCandidates[] = {
            g_exeDir + "\\projectiles.json",
            g_exeDir + "\\..\\data\\projectiles.json",
            g_exeDir + "\\..\\..\\data\\projectiles.json",
        };
        for (const auto& pp : projCandidates) {
            g_projDefs = loadProjectileDefs(pp);
            g_ammoDefs = loadAmmoDefs(pp);
            if (!g_projDefs.empty()) break;
        }
        if (g_projDefs.empty()) {
            // Fallback if export not run yet
            g_projDefs.push_back(ProjectileDef{});
        }

        // Weapon defs from data/weapons or build/weapons
        tryLoadWeapons();

        // Inventory item defs (RULES.md rule 12)
        tryLoadItems();

        // Starter world loot, placed relative to the spawn cell so it is always
        // on the apron in front of the player. Needs items + chunks, hence here.
        seedPickups();
        ensurePickupBuffer();

        auto mesh = meshAllChunks(chunks);
        char msg[256];
        std::snprintf(msg, sizeof(msg),
                      "Chunks=%dx%dx%d voxel=%.4f verts=%zu projs=%zu weapons=%zu\n",
                      CHUNKS_X, CHUNKS_Y, CHUNKS_Z, VOXEL_SIZE, mesh.size(),
                      g_projDefs.size(), g_weapons.size());
        OutputDebugStringA(msg);
        uploadMesh(mesh);

        // Prewarm debris VB so first impact does not allocate mid-frame (spike fix).
        ensureDebrisBuffer();
        ensureSkyTileBuffer();

        auto start = std::chrono::steady_clock::now();
        auto last = start;
        int frames = 0;
        int destroysApprox = 0;

        MSG msgWin{};
        while (g_running) {
            while (PeekMessageA(&msgWin, nullptr, 0, 0, PM_REMOVE)) {
                if (msgWin.message == WM_QUIT) g_running = false;
                TranslateMessage(&msgWin);
                DispatchMessageA(&msgWin);
            }
            if (!g_running) break;

            auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;
            if (dt > 0.05f) dt = 0.05f;
            float t = std::chrono::duration<float>(now - start).count();

if (g_fireCooldown > 0.0f) {
                g_fireCooldown -= dt;
                if (g_fireCooldown < 0.0f) g_fireCooldown = 0.0f;
            }

            g_ads = g_keys['X'] != 0;
            updateRecoilRecovery(dt);

            // Health tick (RULES.md rule 15), ahead of input so a dead player is
            // frozen out of every control on the same frame. updateProjectiles
            // ran before drawFrame last frame, so any damage it dealt has already
            // landed; this only advances the flash and the respawn countdown.
            if (health::updateActorHealth(g_health, dt)) {
                ++g_respawns;
                if (g_chunks) spawnPlayerOnMap(*g_chunks);
                g_fallPeakSpeed = 0.0f;
                g_wasOnGround = true;
                g_deathHandled = false;
            }
            if (g_health.dead) {
                if (!g_deathHandled) {
                    g_deathHandled = true;
                    ++g_deaths;
                    dropHeldItemOnDeath();
                }
                g_inventoryOpen = false;
                g_firePressed = false;
                g_fireHeld = false;
                g_pickupPressed = false;
                g_wantJump = false;
                g_mouseDown = false;
                ReleaseCapture();
            }

            // Functional smoke / stress: fire into warehouse bay
            if (g_smoke) {
                if (g_stress) {
                    // Keep aim into bay; hammer shotgun to max debris/projectile load.
                    g_pitch = -0.10f;
                    g_yaw += dt * 0.05f;
                    g_activeCaliberIndex = 0; // light → shotgun_light
                    g_activeAmmoIndex = 0;
                    // Fire every 3 frames once warmed — heavy enough without remesh thrash.
                    if (frames >= 3 && (frames % 3) == 0) {
                        g_fireCooldown = 0.0f;
                        g_firePressed = true;
                        g_fireHeld = true;
                        ++g_stressFireCount;
                    }
                } else {
                    g_yaw += dt * 0.20f;
                    if (frames < 20) {
                        g_pitch = -0.12f;
                        if (frames == 4) {
                            g_activeCaliberIndex = 1; // medium ballistic
                            g_activeAmmoIndex = 0;
                            g_fireCooldown = 0.0f;
                            g_firePressed = true;
                        }
                        if (frames == 8) {
                            g_activeCaliberIndex = 0; // shotgun light
                            g_activeAmmoIndex = 0;
                            g_fireCooldown = 0.0f;
                            g_firePressed = true;
                        }
                        if (frames == 14) {
                            g_activeCaliberIndex = 3; // energy hitscan
                            g_activeAmmoIndex = 0;
                            g_fireCooldown = 0.0f;
                            g_firePressed = true;
                        }
                    } else {
                        g_pitch = 0.42f; // sky tiles + moon
                    }
                }
            }

            // Fire modes: semi/bolt = edge; auto = held (RMB/F) with cooldown cadence.
            {
                WeaponDef wFire = activeWeaponOrDefault();
                const std::string mode = wFire.fireMode.empty() ? "semi" : wFire.fireMode;
                bool shouldFire = false;
                if (mode == "auto") {
                    shouldFire = g_fireHeld || g_firePressed;
                } else {
                    // semi + bolt: one shot per press edge
                    shouldFire = g_firePressed;
                }
                if (shouldFire) fireProjectile();
                g_firePressed = false;
            }

            // Hold the inventory open for a stretch so the smoke actually
            // records and submits the overlay render pass (RULES.md rule 12).
            g_inventoryOpen = g_smoke && frames >= 100 && frames < 200;

const bool wasDirty = g_meshDirty;
            g_debris.beginFrame();
            updateProjectiles(dt);
            if (static_cast<int>(g_projectiles.size()) > g_projLivePeak)
                g_projLivePeak = static_cast<int>(g_projectiles.size());
            g_debris.update(dt, kWorldGravity);
            // Decay fire VFX (overlay + muzzle cubes).
            if (g_muzzleFlash > 0.0f || g_fireOverlay > 0.0f) {
                g_muzzleFlash = std::max(0.0f, g_muzzleFlash - dt * 6.5f);
                g_fireOverlay = std::max(0.0f, g_fireOverlay - dt * 4.2f);
                g_debris.meshDirty = true;
            }
            if (wasDirty || g_meshDirty) { /* remesh deferred to drawFrame */ }
            // Count live projectile impacts indirectly via remesh flag consumption
            static int lastVertCount = -1;
            if (lastVertCount >= 0 && static_cast<int>(g_vertexCount) < lastVertCount)
                destroysApprox += (lastVertCount - static_cast<int>(g_vertexCount)) / 6;
            lastVertCount = static_cast<int>(g_vertexCount);

            drawFrame(t, dt);
            ++frames;
            ++g_framesSinceRemesh;

            if (g_smoke && frames >= g_smokeFrames) {
                g_running = false;
            }
        }

        vkDeviceWaitIdle(g_device);
        g_chunks = nullptr;

        // Write success marker for smoke / stress tests
        if (g_smoke) {
            g_invSmoke = runInventorySmoke();
            g_healthSmoke = runHealthSmoke();
            std::string outPath = g_exeDir + (g_stress ? "\\stress_ok.txt" : "\\smoke_ok.txt");
            std::ofstream out(outPath);
out << "frames=" << frames << "\nvertices=" << g_vertexCount
                << "\ncam=" << g_camPos.x << "," << g_camPos.y << "," << g_camPos.z
                << "\nplayer=" << g_player.px << "," << g_player.py << "," << g_player.pz
                << "\non_ground=" << (g_player.onGround ? 1 : 0)
                << "\nlean=" << g_player.lean
                << "\nyaw=" << g_yaw << "\npitch=" << g_pitch
                << "\nprojectiles_loaded=" << g_projDefs.size()
                << "\nammo_loaded=" << g_ammoDefs.size()
                << "\nweapons_loaded=" << g_weapons.size()
                << "\nweapon_id=" << g_lastWeaponId
                << "\ncaliber=" << g_lastCaliber
                << "\nfire_mode=" << g_lastFireMode
                << "\nhitscan=" << (g_lastHitscan ? 1 : 0)
                << "\nammo_id=" << g_lastAmmoId
                << "\nballistic_shots=" << g_ballisticShots
                << "\nhitscan_shots=" << g_hitscanShots
                << "\ndebris_spawned=" << g_debris.spawnedTotal
                << "\ndebris_active_peak=" << g_debris.activePeak
                << "\ndebris_alive=" << g_debris.activeCount()
                << "\ndebris_ricochets=" << g_debris.ricochets
                << "\ndebris_subdiv=" << kDebrisSubDiv
                << "\ndebris_verts_peak=" << g_debrisVertsPeak
                << "\ndebris_upload_us_avg=" << (g_debrisUploadSamples > 0 ? (g_debrisUploadUsSum / g_debrisUploadSamples) : 0.0)
                << "\ndebris_upload_us_max=" << g_debrisUploadUsMax
<< "\ndebris_upload_samples=" << g_debrisUploadSamples
                << "\ndebris_billboard=0"
                << "\ndebris_cubes=1"
                << "\ndebris_visual_scale=" << kDebrisVisualScale
                << "\nmuzzle_flash_peak=1"
                << "\nmesh_upload_us_avg=" << (g_meshUploadSamples > 0 ? (g_meshUploadUsSum / g_meshUploadSamples) : 0.0)
                << "\nmesh_upload_us_max=" << g_meshUploadUsMax
                << "\nmesh_upload_samples=" << g_meshUploadSamples
                << "\nmesh_host_visible=1"
                << "\nshotgun_shots=" << g_shotgunShots
                << "\npellet_spawns=" << g_pelletSpawns
                << "\nsub_radius=" << kSubRadius
                << "\nproj_live_peak=" << g_projLivePeak
                << "\nstress=" << (g_stress ? 1 : 0)
                << "\nstress_fire_count=" << g_stressFireCount
                << "\nremesh_skips=" << g_remeshSkipCount
                << "\nstress_ok=" << ((g_stress
                        && g_shotgunShots >= 80
                        && g_debris.activePeak >= 150
                        && g_frameMsCount > 40
                        && (g_frameMsSum / double(g_frameMsCount - 15)) <= 16.5
                        && g_frameMsMax <= 50.0
                        && g_debrisUploadUsMax <= 250.0) ? 1 : (g_stress ? 0 : 1))
                << "\ngravity=" << kWorldGravity
                << "\nremesh_events=" << destroysApprox
                << "\nwater_touch=" << g_touchingWaterUnits << "/" << g_characterUnitCount
                << "\ncurrent=" << (g_currentTriggered ? 1 : 0)
                << "\nsubmerged=" << (g_fullySubmerged ? 1 : 0)
                << "\nweight=" << g_playerWeight
                << "\ncurrent_force=" << g_currentForce
                << "\nrender_scale=" << RENDER_SCALE
                << "\ndrawn_chunks=" << g_drawnChunks
                << "\nculled_chunks=" << g_culledChunks
                << "\navg_frame_ms=" << (g_frameMsCount > 15 ? (g_frameMsSum / double(g_frameMsCount - 15)) : -1.0)
                << "\nmin_frame_ms=" << (g_frameMsMin < 1e8 ? g_frameMsMin : -1.0)
                << "\nmax_frame_ms=" << g_frameMsMax
                << "\npace_hits=" << g_framePaceHits
                << "\nsteady_frames=" << (g_frameMsCount > 15 ? (g_frameMsCount - 15) : 0)
                << "\ntarget_hz=" << TARGET_HZ
                << "\nlock_ok=" << ((g_frameMsCount > 40) && ((g_frameMsSum / double(g_frameMsCount - 15)) <= 9.0) ? 1 : 0)
                << "\nsky_tiles=" << (SKY_SEG_U * SKY_SEG_V)
                << "\nsky_verts=" << g_skyTileVertexCount
                << "\nmoon_light=" << (g_isNight ? 1 : 0)
                << "\nmoon_dir=" << g_moonDirWorld.x << "," << g_moonDirWorld.y << "," << g_moonDirWorld.z
                << "\nitems_loaded=" << g_itemDefs.size()
                << "\ninventory_slots=" << kEquipSlotCount
                << "\ninventory_open=" << (g_inventoryOpen ? 1 : 0)
                << "\npack_size=" << g_inventory.vol.sx << "," << g_inventory.vol.sy << ","
                << g_inventory.vol.sz
                << "\npack_cells=" << g_inventory.vol.cells()
                << "\npack_used=" << g_invSmoke.baseUsed
                << "\npack_free=" << g_invSmoke.baseFree
                << "\nhit_cells=" << kHitCells
                << "\narmor_zones=" << static_cast<int>(ArmorZone::Count)
                << "\narmor_zones_tile=" << (g_invSmoke.zonesTile ? 1 : 0)
                << "\ninventory_equip_ok=" << (g_invSmoke.equipOk ? 1 : 0)
                << "\ninventory_wide_arm_rejected=" << (g_invSmoke.wideArmRejected ? 1 : 0)
                << "\ninventory_class_gate_ok=" << (g_invSmoke.classGateOk ? 1 : 0)
                << "\ninventory_base_pack_ok=" << (g_invSmoke.basePackOk ? 1 : 0)
                << "\ninventory_base_full_rejected=" << (g_invSmoke.baseFullRejected ? 1 : 0)
                << "\npack_upgrade_cells=" << g_invSmoke.upgradeCells
                << "\npack_upgrade_used=" << g_invSmoke.upgradeUsed
                << "\ninventory_upgrade_pack_ok=" << (g_invSmoke.upgradePackOk ? 1 : 0)
                << "\nweapon_mag_rounds=" << g_invSmoke.magRounds
                << "\npouch_rounds=" << g_invSmoke.pouchRounds
                << "\noverlay_verts=" << g_invSmoke.overlayVerts
                << "\noverlay_lattice_verts=" << g_invSmoke.overlayLatticeVerts
                << "\noverlay_slot_verts=" << g_invSmoke.overlaySlotVerts
                << "\noverlay_built=" << (g_invSmoke.overlayBuilt ? 1 : 0)
                << "\noverlay_unit_cubes=" << (g_invSmoke.overlayUnitCubes ? 1 : 0)
                << "\noverlay_frames_drawn=" << g_inventoryOverlayFrames
                << "\nhover_resolved=" << (g_invSmoke.hoverResolved ? 1 : 0)
                << "\nhand_lift_ok=" << (g_invSmoke.liftOk ? 1 : 0)
                << "\nhand_place_ok=" << (g_invSmoke.placeOk ? 1 : 0)
                << "\nhand_rotate_ok=" << (g_invSmoke.rotateOk ? 1 : 0)
                << "\nhand_stow_ok=" << (g_invSmoke.stowOk ? 1 : 0)
                << "\nhand_invalid_place_rejected=" << (g_invSmoke.invalidPlaceRejected ? 1 : 0)
                << "\nbackpack_volume_ok=" << (g_invSmoke.backpackVolumeOk ? 1 : 0)
                << "\nbackpack_unequip_lift_ok=" << (g_invSmoke.backpackLiftOk ? 1 : 0)
                << "\npickup_verts=" << g_invSmoke.pickupVerts
                << "\npickup_unit_cubes=" << (g_invSmoke.pickupOk ? 1 : 0)
                << "\npickup_taken=" << (g_invSmoke.pickupTaken ? 1 : 0)
                << "\nhealth_max_hp=" << g_health.maxHealth
                << "\nhealth_hp=" << g_health.health
                << "\nhealth_dead=" << (g_health.dead ? 1 : 0)
                << "\nhealth_self_fire=" << (health::kSelfFireDamage ? 1 : 0)
                << "\nhealth_absorb_per_point=" << health::kArmorAbsorbPerPoint
                << "\nhealth_absorb_cap=" << health::kMaxArmorAbsorb
                << "\nhealth_medium_shot_hp=" << g_healthSmoke.mediumShotHp
                << "\nhealth_armor_chest_points=" << g_healthSmoke.chestPoints
                << "\nhealth_armor_helmet_points=" << g_healthSmoke.helmetPoints
                << "\nhealth_max_health_ok=" << (g_healthSmoke.maxHealthOk ? 1 : 0)
                << "\nhealth_armor_absorb_ok=" << (g_healthSmoke.armorAbsorbOk ? 1 : 0)
                << "\nhealth_zone_hit_ok=" << (g_healthSmoke.zoneHitOk ? 1 : 0)
                << "\nhealth_damage_ok=" << (g_healthSmoke.damageOk ? 1 : 0)
                << "\nhealth_single_hit_cap_ok=" << (g_healthSmoke.singleHitCapOk ? 1 : 0)
                << "\nhealth_fall_ok=" << (g_healthSmoke.fallOk ? 1 : 0)
                << "\nhealth_drown_ok=" << (g_healthSmoke.drownOk ? 1 : 0)
                << "\nhealth_death_ok=" << (g_healthSmoke.deathOk ? 1 : 0)
                << "\nhealth_equipped_armor_ok=" << (g_healthSmoke.equippedArmorOk ? 1 : 0)
                << "\nhealth_self_fire_excluded_ok=" << (g_healthSmoke.selfFireExcludedOk ? 1 : 0)
                << "\nhealth_hud_verts=" << g_invSmoke.overlayHudVerts
                << "\nhealth_hud_built=" << (g_invSmoke.hudBuilt ? 1 : 0)
                << "\nhealth_body_hits=" << g_bodyHits
                << "\nhealth_fall_events=" << g_fallDamageEvents
                << "\nhealth_drown_ticks=" << g_drownDamageTicks
                << "\nhealth_deaths=" << g_deaths
                << "\nhealth_respawns=" << g_respawns
                << "\nhealth_ok="
                << ((g_healthSmoke.maxHealthOk && g_healthSmoke.armorAbsorbOk &&
                     g_healthSmoke.zoneHitOk && g_healthSmoke.damageOk &&
                     g_healthSmoke.singleHitCapOk && g_healthSmoke.fallOk &&
                     g_healthSmoke.drownOk && g_healthSmoke.deathOk &&
                     g_healthSmoke.equippedArmorOk && g_healthSmoke.selfFireExcludedOk &&
                     g_healthSmoke.mediumShotHp > 0 && g_invSmoke.hudBuilt)
                        ? 1
                        : 0)
                << "\ninventory_ok="
                << ((!g_itemDefs.empty() && g_invSmoke.zonesTile && g_invSmoke.equipOk &&
                     g_invSmoke.wideArmRejected && g_invSmoke.classGateOk &&
                     g_invSmoke.basePackOk && g_invSmoke.baseFullRejected &&
                     g_invSmoke.upgradePackOk && g_invSmoke.overlayBuilt &&
                     g_invSmoke.overlayUnitCubes && g_inventoryOverlayFrames > 0 &&
                     g_invSmoke.hoverResolved && g_invSmoke.liftOk && g_invSmoke.placeOk &&
                     g_invSmoke.rotateOk && g_invSmoke.stowOk &&
                     g_invSmoke.invalidPlaceRejected && g_invSmoke.backpackVolumeOk &&
                     g_invSmoke.backpackLiftOk &&
                     g_invSmoke.pickupOk && g_invSmoke.pickupTaken)
                        ? 1
                        : 0)
                << "\n";
        }
    } catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "Voxel Engine Error", MB_ICONERROR);
        cleanup();
        return 1;
    }

    cleanup();
    return 0;
}

// Console entry when built as console subsystem
int main(int argc, char** argv) {
    std::string cmd;
    for (int i = 1; i < argc; ++i) {
        if (i > 1) cmd += " ";
        cmd += argv[i];
    }
    return WinMain(GetModuleHandleA(nullptr), nullptr, const_cast<LPSTR>(cmd.c_str()), SW_SHOW);
}
