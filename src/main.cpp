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
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Block identity, grid dimensions, and the sim/view chunk types. These sit at
// the top of the translation unit because the globals below alias them.
#include "sim_world.hpp"
#include "view_chunk.hpp"

#include "debris.hpp"
#include "destruction.hpp"
#include "health.hpp"
#include "inventory.hpp"
#include "fisheye.hpp"
#include "materials.hpp"
#include "map_vox.hpp"
#include "mesh_view.hpp"
#include "render_class.hpp"
#include "post_fx.hpp"
#include "textures.hpp"
#include "visual_params.hpp"

// Dear ImGui (third_party/imgui, MIT): the in-engine menu. View only.
#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include "backends/imgui_impl_win32.h"
// The backend header keeps this behind #if 0 so it need not pull in windows.h.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);
#include "sim_input.hpp"
#include "movement.hpp"
#include "ballistics.hpp"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// The view mesher keeps its own (deliberately sim-free) copy of the voxel
// scale; this is the compile-time bridge proving it never drifts from the
// authority. Same rule as the wire enum asserts in sim_world.hpp.
static_assert(meshview::kVoxelSize == kVoxelSize,
              "mesh_view mesh scale must agree with the sim's voxel scale");

// ---- grid aliases ----
// Block identity, scale, and the authoritative grid now live in sim_world.hpp.
// The engine aliases them so existing call sites keep working; the aliases are
// the seam, not a second definition.
using Block = sim::Block;
using SimChunk = sim::Chunk;
using ViewChunk = view::ViewChunk;
using SentCells = view::SentCells;

static constexpr int CHUNK_SIZE = sim::kChunkSize;
static constexpr int CHUNKS_X = sim::kChunksX;
static constexpr int CHUNKS_Y = sim::kChunksY;
static constexpr int CHUNKS_Z = sim::kChunksZ;
static constexpr int WORLD_W = sim::kWorldW;
static constexpr int WORLD_H = sim::kWorldH;
static constexpr int WORLD_D = sim::kWorldD;
static constexpr int VOXELS_PER_CHUNK = sim::kVoxelsPerChunk;

static constexpr int WIDTH = 1280;
static constexpr int HEIGHT = 720;
static constexpr int MAX_FRAMES = 2;
// Internal 3D render scale (downscale for fill-rate). Presented upscaled with bitcrush look in shader.
static constexpr int INTERNAL_W = 640;  // WIDTH * 0.5
static constexpr int INTERNAL_H = 360;  // HEIGHT * 0.5
static constexpr float DEFAULT_FOV_DEG = 101.5f; // 70 * 1.45 fisheye default

// ---- deterministic simulation clock ----
// The simulation advances only in whole ticks of exactly TICK_DT. Wall clock is
// used to decide HOW MANY ticks to run, never what a tick's dt is. That keeps
// world state a pure function of g_tick, which is the prerequisite for the
// view/sim split, for replay, and for moving systems onto other machines.
static constexpr double TICK_HZ = 120.0;
static constexpr double TICK_DT = 1.0 / TICK_HZ;
// Cap ticks per frame so a long stall cannot spiral. The backlog is dropped
// rather than chased, so a hitch costs time but not determinism.
static constexpr int MAX_TICKS_PER_FRAME = 8;
static uint64_t g_tick = 0;
static double g_tickAccum = 0.0;

// Unit voxel grid: 1000x smaller than original 1.0 blocks. Every solid is 1x1x1 voxels
// (no stretched planes). Impact / destruction use integer grid indices only.
// Single source of truth for the scale is materials.hpp (kVoxelSize); the grid
// dimensions now live in sim_world.hpp alongside the authority rules they serve.
static constexpr float VOXEL_SIZE = kVoxelSize;

// Warehouse layout in unit voxels (grid space)

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

// A light the view shades with: one entry per map light (environment layer).
struct BulbLight {
    Vec3 pos;          // world-space centre
    Vec3 color;
    float intensity;
    float radius;
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
    float mat;            // render class (src/render_class.hpp)
    float texLayer = 0;   // texture layer + 1; 0 = untextured
    float painted = 0;    // 1 = cell has an appearance colour
    float shade = 1;      // face shade x AO already folded into cr,cg,cb
};
// Chunk meshes are built as view::ViewChunk::Vertex and copied into the same
// vertex buffer, so the two layouts must be identical.
static_assert(sizeof(Vertex) == sizeof(view::ViewChunk::Vertex), "vertex layouts must match");

// Fixed light slots in the frame UBO. The shader loops this many, so it is a
// layout constant, not a tuning knob.
// Light sources go to the shaders through a storage buffer (two vec4 each).
static constexpr int kMaxLights = 64;

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
    float damageFlash;     // 0..1 crimson damage intake flash
    float healthTint;      // 0..1 low-health pulsing vignette
    float fisheyeScale;            // visuals menu: lens curve multiplier
    float banding;                 // visuals menu: colour-step multiplier
    float uboPad[2];
    float texParams[tex::kMaxLayers][4]; // per layer: tileCells, tint, coverage, maskFromLuma
    float texGlobal[4];            // enabled, strength, scale, unused
    float occDims[4];              // occupancy volume W, H, D, shadows enabled
    float shadowParams[4];         // max cells crossed, 1, unused, unused
    float lightInfo[4];            // light count, unused x3
};

// Occupancy volume (view side): 1 byte per cell of the whole world, built
// only from the chunk snapshots this client was sent, and uploaded to a 3D
// image the shaders march shadows through. Remeshed chunks are re-uploaded,
// each frame through its own staging buffer.
static VkImage g_occImage = VK_NULL_HANDLE;
static VkDeviceMemory g_occMem = VK_NULL_HANDLE;
static VkImageView g_occView = VK_NULL_HANDLE;
static VkSampler g_occSampler = VK_NULL_HANDLE;
static VkBuffer g_occStage[MAX_FRAMES]{};
static VkDeviceMemory g_occStageMem[MAX_FRAMES]{};
static void* g_occStageMapped[MAX_FRAMES]{};
static bool g_occImageReady = false;       // has been transitioned out of UNDEFINED
static std::vector<uint8_t> g_occCpu;      // the volume as last sent
static std::vector<int> g_occDirty;        // chunk indices waiting for upload
static uint64_t g_occChunkUploads = 0;     // telemetry
// Lights for the shaders, one host-visible buffer per frame in flight.
static VkBuffer g_lightBuf[MAX_FRAMES]{};
static VkDeviceMemory g_lightMem[MAX_FRAMES]{};
static void* g_lightMapped[MAX_FRAMES]{};

// Surface textures (src/textures.hpp): one 2D array image, all mips.
static VkImage g_texImage = VK_NULL_HANDLE;
static VkDeviceMemory g_texMem = VK_NULL_HANDLE;
static VkImageView g_texView = VK_NULL_HANDLE;
static VkSampler g_texSampler = VK_NULL_HANDLE;
static tex::Table g_texTable;
static int g_texLayerCount = 0;   // 0 = no texture file; a 1x1 white fallback is bound
static float g_maxAnisotropy = 1.0f;

// Visual settings (view only): the registry the menu, presets and the settings
// file are built from, and the post chain that consumes most of them.
static vis::Registry g_visReg;
static vis::Settings g_vis;
static postfx::PostFx g_post;

// Time system. g_timeOfDay is frozen: the world ships as permanent night, and
// the dead g_timeScale that was meant to advance it is gone.
static float g_timeOfDay = 0.88f; // night
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
// Previous-tick level of the edge-detected movement keys. These exist only in the
// view, to turn a press into an edge for SimInput; the sim sees the edge and never
// the latch.
static bool g_dashHeldPrev = false;
static bool g_stanceHeldPrev = false;
static bool g_mouseDown = false;
static int g_mouseX = 0, g_mouseY = 0, g_lastMouseX = 0, g_lastMouseY = 0;
// The view's raw device state. The simulation never reads this: buildSimInput()
// translates it into player intent, and that intent is all sim::tick() receives.
static SimInput g_pendingInput;
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

// Movement state (stance, gait, slide, wallrun, dash, stamina) and the
// presentation-only offset it produces. Both are simulation state: the view
// applies the offset after the body is settled and never writes either back.
// See RULES.md, "Player body, and the camera-offset contract".
static movement::MoveState g_move;
static movement::CameraOffset g_camOffset;
// The sim eye as it stood before the most recent tick; renderEye() interpolates
// from here to g_camPos. Written only by the host loop, read only by the view.
static Vec3 g_camPosPrevTick;
// Smoke pause probe (see the main loop).
static bool g_smokePauseDone = false;
static bool g_smokePauseFrozenOk = false;
static uint64_t g_smokePauseTick = 0;
static int g_smokePausedFrames = 0;

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
// Damage attempts refused because the player was inside the dash invulnerability
// window. A non-zero value here is proof the window is actually consulted, which
// the movement smoke's own assertion cannot establish on its own.
static int g_dashInvulnBlocks = 0;

// The two halves of the world, deliberately separate objects:
//
//   g_world  - authoritative occupancy. Read by simulation only.
//   g_views  - per-chunk snapshots + meshes. Read by the render path only.
//
// Nothing in the render path takes g_world, and nothing in the simulation takes
// g_views except to *write* fresh snapshots into it. That asymmetry is the
// enforcement mechanism for the view/sim split; the comments above each are
// load-bearing, so keep them with the declarations.
static sim::World* g_world = nullptr;
static std::vector<ViewChunk>* g_views = nullptr;
// The map palette as the view received it (appearance layer). Sent once with
// the map; every ViewChunk points here.
static wire::Palette g_viewPalette;

static void sendPalette(const sim::World& world, wire::Palette& out) {
    out = wire::Palette{};
    out.used = static_cast<int>(std::min<size_t>(world.palette.size(), wire::kPaletteSize));
    for (int i = 1; i < out.used; ++i) {
        const uint32_t c = world.palette[i];
        out.colors[i] = wire::PaletteColor{uint8_t((c >> 16) & 255), uint8_t((c >> 8) & 255), uint8_t(c & 255)};
    }
}
static std::vector<ProjectileDef> g_projDefs;
static std::vector<AmmoDef> g_ammoDefs;
// Live rounds and the last impact belong to the ballistics module's state.
// g_projectiles is the name the rest of this file uses for the round list.
static ballistics::State g_ballistics;
static std::vector<ProjectileRuntime>& g_projectiles = g_ballistics.projectiles;
// What the loaded map authored for spawn and pickups (empty when the world
// came from the procedural builder).
static mapvox::PlayerSpawn g_mapSpawn;
static std::vector<mapvox::PickupPlacement> g_mapPickups;
static std::string g_mapPath; // file the world was loaded from (empty = procedural)
static int g_mapPrefabsStamped = 0;

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

// Is the player inside the dash invulnerability window?
//
// movement::update owns the flag (movement.hpp sets dashInvuln on the dash edge
// and clears it when DASH_INVULN_TIME expires); this is the single read point.
// Every damage path funnels through here so a dash actually protects the player
// rather than merely reporting that it could. Drowning is excluded on purpose:
// health::updateBreath is not an impact and mutates health directly, and it
// runs before movement::update in the tick, so it cannot be gated by a flag the
// same tick has not set yet.
static bool dashInvulnerable() { return g_move.dashInvuln; }

// Convert an impact's voxel-destruction energy into HP loss on the player's body
// and apply it at `zone`. Returns the HP actually removed.
static float damagePlayerAtZone(float energy, const std::string& effect, ArmorZone zone) {
    if (g_health.dead) return 0.0f;
    if (dashInvulnerable()) { ++g_dashInvulnBlocks; return 0.0f; }
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
static constexpr float kInventoryMatId = rc::attr(rc::RenderClass::InventoryLattice);
static constexpr float kPickupMatId = rc::attr(rc::RenderClass::WorldPickup); // main pass, not the overlay

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
// Vertices currently occupied across all chunk slots. Kept separate from
// g_vertexCount (buffer capacity in vertices) so telemetry reports real geometry.
static uint32_t g_liveVertexCount = 0;
// End of the last allocated chunk slot, in vertices. Everything past it is free,
// so a chunk that outgrows its slot can move there without a full repack.
static uint32_t g_slotCursor = 0;
static int g_meshRelocateCount = 0;   // chunks moved to the tail instead of repacking
static uint64_t g_meshTouchedSum = 0; // chunks remeshed per flush (batch size for the pool)
static int g_meshTouchedMax = 0;
static int g_meshRepackCount = 0;
static double g_meshUploadUsMax = 0.0;
static double g_meshUploadUsSum = 0.0;
static int g_meshUploadSamples = 0;
static int g_ticksSinceRemesh = 99;
static int g_remeshSkipCount = 0;
// Headless modes (declared early — used by debris draw + remesh throttle).
static bool g_smoke = false;
static bool g_stress = false;
// Movement-only headless pass. Implies --smoke so it can reuse the quit path,
// but writes its own marker and prints only the movement report.
static bool g_smokeMovement = false;
static uint64_t g_smokeTicks = 300;
static int g_projLivePeak = 0;
static int g_stressFireCount = 0;
// Real count of occupancy cells destroyed. Previously inferred by diffing the
// rendered vertex count, which made the simulation read render state.
static int g_voxelsDestroyed = 0;
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
static uint32_t g_skyTileVertexCount = 0;
// Eye the mapped sky vertices were last written for. The tile is camera-relative,
// so it is rebuilt only when the eye moves, not on every frame.
static Vec3 g_skyTileEye = {0, 0, 0};
static bool g_skyTileBuilt = false;
static Vec3 g_moonWorldPos = {0, 0, 0};
// Lights from the map's "lights" section (environment layer, not occupancy).
static std::vector<BulbLight> g_bulbs;
static std::vector<mapvox::LightPlacement> g_mapLights;
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

// Per-subsystem latency breakdown (telemetry only).
//
// This is the Phase 1 audit instrument: which subsystem owns the frame cost is
// the input to the Phase 2 split. Scope timers track every section once per
// invocation and aggregate here; the report prints each as us avg/max/count.
// QPC wall-clock is explicitly reserved for telemetry (AGENTS.md), and none of
// these values can ever influence simulation state, so determinism is intact.
enum SectionId {
    SEC_HEALTH,        // health tick + respawn
    SEC_MOVEMENT,      // player body / movement::update
    SEC_FIRE,          // fireProjectile() (spawn, collision cast)
    SEC_PROJECTILES,   // ballistic integration + impacts
    SEC_DEBRIS_SIM,    // debris particle physics (sim side)
    SEC_WAIT,          // vkWaitForFences (GPU backpressure)
    SEC_MESH,          // flushDirtyMesh(): chunk remesh + upload (the three below nest in it)
    SEC_MESH_SNAPSHOT, //   sendChunkSnapshot for stale chunks (sim -> view copy)
    SEC_MESH_BUILD,    //   meshChunk over the stale chunks (pure view work)
    SEC_MESH_COPY,     //   memcpy into the mapped vertex buffer (incl. repack)
    SEC_UBO,           // per-frame UBO write
    SEC_SKY,           // moon/sky tile rebuild
    SEC_DEBRIS_MESH,   // debris + muzzle VBO write (view side)
    SEC_PICKUP,        // pickup hover mesh
    SEC_INVENTORY,     // inventory overlay mesh + input drain
    SEC_RECORD,        // recordCommandBuffer (build + submit)
    SEC_COUNT
};
static const char* kSectionName[SEC_COUNT] = {
    "health", "movement", "fire", "projectiles", "debris_sim",
    "wait", "mesh", "mesh_snapshot", "mesh_build", "mesh_copy", "ubo", "sky", "debris_mesh", "pickup", "inventory", "record"};
static double g_secUsSum[SEC_COUNT] = {};
static double g_secUsMax[SEC_COUNT] = {};
static int g_secCount[SEC_COUNT] = {};
static LARGE_INTEGER g_secStart[SEC_COUNT];

// RAII scope marker for one section. QPC is for telemetry measurement only and
// never feeds a gameplay branch, so sampling here cannot disturb the fixed-tick
// simulation. Uses its own frequency handle so it stands alone (paceFrame120,
// flushDirtyMesh and updateDebrisMesh each keep their local one).
struct ScopedSection {
    SectionId id;
    ScopedSection(SectionId s) : id(s) { QueryPerformanceCounter(&g_secStart[s]); }
    ~ScopedSection() {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        static LARGE_INTEGER freq{};
        if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
        const double us = double(now.QuadPart - g_secStart[id].QuadPart) * 1e6 / double(freq.QuadPart);
        g_secUsSum[id] += us;
        if (us > g_secUsMax[id]) g_secUsMax[id] = us;
        ++g_secCount[id];
    }
};

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

// ---- fine voxel + chunk system (sharp face vertices) ----

// Water is painted as slightly larger *logical* cells (2x2x2 unit cubes) for volume/tide.
static constexpr int WATER_CELL = 2;

// Impact material of a block. Owned by the ballistics module, which is the
// simulation code that asks the question most.
using ballistics::blockMaterial;

static bool isWaterBlock(Block b) {
    return b == Block::Water || b == Block::WaterCurrent;
}

// Collision solids: occupancy that blocks the player hitbox (not water/emissive).
static bool isSolidBlock(Block b) {
    if (b == Block::Air || isWaterBlock(b)) return false;
    if (b == Block::Moon || b == Block::LightBulb) return false;
    return true;
}

// The engine's chunk pair: authoritative occupancy on the sim side, and the
// snapshot + mesh the view derived from it. Kept as one parallel vector pair so
// chunkIndex() addresses both; the two are never merged into a single struct,
// because that merge is the coupling this split removes.

static inline int localIndex(int lx, int ly, int lz) {
    return sim::World::localIndex(lx, ly, lz);
}

static bool worldInBounds(int x, int y, int z) {
    return sim::World::inBounds(x, y, z);
}

// The authoritative occupancy accessors. These are simulation-side reads; view
// code must use ViewChunk::sent instead.
static Block getWorldBlock(const sim::World& w, int x, int y, int z) {
    return w.get(x, y, z);
}

static void setWorldBlock(sim::World& w, int x, int y, int z, Block b) {
    w.set(x, y, z, b);
}


// Fill a solid axis-aligned box with unit voxels (inclusive).
// An all-Air world with every chunk allocated and placed.
static sim::World makeEmptyWorld() {
    sim::World world;
    world.alloc();
    for (int cy = 0; cy < CHUNKS_Y; ++cy)
        for (int cz = 0; cz < CHUNKS_Z; ++cz)
            for (int cx = 0; cx < CHUNKS_X; ++cx) {
                SimChunk& c = world.chunks[sim::World::chunkIndex(cx, cy, cz)];
                c.cx = cx; c.cy = cy; c.cz = cz;
                c.voxels.assign(VOXELS_PER_CHUNK, Block::Air);
            }
    return world;
}


// Skirt isolation tracking: assert that client-side meshing never attempts to
// read outside the supplied visible skirt (RULES.md, "Visibility filtering").
// The view mesher now lives in mesh_view.hpp; this is its telemetry, shared by
// every meshChunk call in this TU.
static meshview::Stats g_meshStats;

// ---- the send path: sim -> view ----
// Everything the view will ever know about occupancy passes through here. This
// is the anti-cheat boundary: a client is sent the cells it is allowed to see
// and nothing else, so it cannot infer or fabricate the rest.

// Copy one chunk's occupancy plus its 1-cell skirt into the client's snapshot.
//
// This is the ONLY writer of ViewChunk::sent. Because it is the only writer,
// the invariant "the view holds exactly the cells the sim chose to send" is
// structural rather than a convention someone has to remember.
//
// The sim::Block -> wire::BlockId conversion happens HERE, at the boundary.
// sim_world.hpp static_asserts the two enums agree, so this crossing is checked
// rather than trusted, and it is the single place world data becomes client
// data.
static void sendChunkSnapshot(const sim::World& world, ViewChunk& vc, bool isVisible = true) {
    if (!vc.hasSnapshot) vc.sent.alloc();
    if (!isVisible) {
        // Anti-cheat: zero out snapshot so client memory contains no hidden world data
        for (auto& cell : vc.sent.cells) {
            cell.id = static_cast<uint8_t>(wire::BlockId::Air);
            cell.appear = 0;
        }
        vc.hasSnapshot = true;
        return;
    }
    const int baseX = vc.cx * CHUNK_SIZE;
    const int baseY = vc.cy * CHUNK_SIZE;
    const int baseZ = vc.cz * CHUNK_SIZE;
    for (int ly = -view::kSkirt; ly < CHUNK_SIZE + view::kSkirt; ++ly) {
        for (int lz = -view::kSkirt; lz < CHUNK_SIZE + view::kSkirt; ++lz) {
            for (int lx = -view::kSkirt; lx < CHUNK_SIZE + view::kSkirt; ++lx) {
                const Block b = world.get(baseX + lx, baseY + ly, baseZ + lz);
                vc.sent.set(lx, ly, lz, static_cast<wire::BlockId>(static_cast<uint8_t>(b)));
                vc.sent.setAppearance(lx, ly, lz, world.getAppearance(baseX + lx, baseY + ly, baseZ + lz));
            }
        }
    }
    vc.hasSnapshot = true;
}

// Per-chunk upload. Each chunk owns a contiguous region of the world vertex
// buffer, so a remesh copies only the chunks whose voxels actually changed
// instead of rebuilding and re-uploading the whole world every time.

// Re-meshing a changed chunk is two phases, and they are split on purpose.
//
// Phase 1 (sendStaleSnapshots) is the sim side: it reads sim::World and
// refreshes the snapshot of every chunk whose version moved. It must run on
// the thread that owns the world, between ticks.
//
// Phase 2 (buildChunkMeshes) is pure view work: it meshes each of those
// chunks from its own snapshot and touches nothing else, so it can fan out
// across the mesh workers. Running phase 1 to completion first is what
// keeps the mesh in step with the world, since meshChunk cannot see the world.

// Refresh the snapshot of every chunk whose occupancy changed. Returns those
// chunks, so the caller meshes and uploads exactly those and nothing else.
static std::vector<ViewChunk*> sendStaleSnapshots(const sim::World& world,
                                                  std::vector<ViewChunk>& views) {
    std::vector<ViewChunk*> touched;
    for (auto& c : views) {
        const sim::Chunk& sc = world.chunks[sim::World::chunkIndex(c.cx, c.cy, c.cz)];
        if (!c.snapshotStale(sc.version)) continue;
        sendChunkSnapshot(world, c);
        c.meshedVersion = sc.version;
        touched.push_back(&c);
    }
    return touched;
}

static meshview::Workers* g_meshWorkers = nullptr;
static int g_meshWorkerHelpers = 0; // telemetry: size of the live pool

// Mesh each touched chunk from its snapshot, then record its size.
static void noteOccupancy(const ViewChunk& c); // shadow volume (see createOccupancyVolume)

static void buildChunkMeshes(const std::vector<ViewChunk*>& touched) {
    if (g_meshWorkers) g_meshWorkers->meshAll(touched, g_meshStats);
    else for (ViewChunk* c : touched) meshview::meshChunk(*c, g_meshStats);
    for (ViewChunk* c : touched) {
        c->vertexCount = static_cast<uint32_t>(c->mesh.size());
        noteOccupancy(*c);
    }
}

// Slot size for a chunk mesh of `verts` vertices. The headroom means ordinary
// destruction (which exposes new interior faces and grows the mesh) does not
// immediately outgrow the slot. A chunk may still shrink freely.
static uint32_t slotWant(uint32_t verts) { return verts + verts / 4 + 1024; }

static void repackChunkSlots(std::vector<ViewChunk>& chunks) {
    uint32_t cursor = 0;
    for (auto& c : chunks) {
        c.firstVertex = cursor;
        c.vertexCount = static_cast<uint32_t>(c.mesh.size());
        const uint32_t want = slotWant(c.vertexCount);
        if (c.slotCapacity < want) c.slotCapacity = want;
        cursor += c.slotCapacity;
    }
    g_slotCursor = cursor;
    g_liveVertexCount = 0;
    for (const auto& c : chunks) g_liveVertexCount += c.vertexCount;
}

// Upload a chunk's mesh into its reserved slot. Returns false when the chunk
// needs a larger buffer, in which case the caller must repack and retry.
static bool uploadChunkRange(const ViewChunk& c) {
    if (c.vertexCount == 0) return true;
    VkDeviceSize offsetBytes = sizeof(Vertex) * c.firstVertex;
    VkDeviceSize size = sizeof(Vertex) * c.vertexCount;
    if (offsetBytes + size > g_vertexCapacity) return false;
    std::memcpy(static_cast<Vertex*>(g_vertexMapped) + c.firstVertex,
                c.mesh.data(), static_cast<size_t>(size));
    return true;
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

static void chunkWorldAABB(const ViewChunk& c, float& minx, float& miny, float& minz,
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
//
// The vertex shader expands NDC radially (see shaders/voxel.vert), so it only
// draws out to about kFisheyeVisibleRadius of the unit disc, while the box test
// below keeps anything inside the unit cube. That makes culling conservative:
// it can hold on to geometry the shader will discard, but it can never drop a
// chunk the shader would have drawn. Tightening this to the shader's real disc
// needs a projected-sphere test rather than a box test, and is left as a later
// optimisation rather than a correctness fix.
static bool chunkNotSeen(const ViewChunk& c, const Frustum& fr, const Vec3& eye, const Vec3& forward) {
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

// Presentation-side frame pacer. Sleeping here shapes how often the view
// presents; it never touches world state, so it cannot affect determinism.
// Whether the sim runs 0, 1 or several ticks this frame is decided separately
// by the accumulator in the main loop.
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
    // Face axis and sign, then its four corners counter-clockwise seen from
    // outside (the same faces and winding as meshview::emitSharpFace).
    // Corner bits: 1 = +x, 2 = +y, 4 = +z.
    struct Face { int axis; int sign; int c[4]; };
    static const Face kFaces[6] = {
        {0, +1, {1, 3, 7, 5}}, // +x
        {0, -1, {4, 6, 2, 0}}, // -x
        {1, +1, {2, 6, 7, 3}}, // +y
        {1, -1, {4, 0, 1, 5}}, // -y
        {2, +1, {5, 7, 6, 4}}, // +z
        {2, -1, {0, 2, 3, 1}}, // -z
    };
    // Two triangles per quad. (This used to be corner(order[t / 2]), i.e.
    // a,a,b,b,c,c: two degenerate triangles per face, so every cube drawn
    // through here rasterised nothing.)
    static const int kTri[6] = {0, 1, 2, 0, 2, 3};
    for (const auto& f : kFaces) {
        const Vec3 nrm = (f.axis == 0 ? axR : (f.axis == 1 ? axU : axF)) * static_cast<float>(f.sign);
        for (int t = 0; t < 6; ++t) {
            Vertex& v = verts[wi++];
            const Vec3 p = corner(f.c[kTri[t]]);
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
            v.texLayer = 0.0f;
            v.painted = 0.0f;
            v.shade = 1.0f;
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

// How far overlay cubes sit from the eye, as a multiple of the original 6 mm.
// Cells are fixed 1 mm cubes, so distance sets their on-screen size: at the
// original 1x the health block was ~250 px wide (sized while emitUnitCube drew
// nothing). Every anchor offset scales with it, so positions on screen are kept.
static constexpr float kOverlayDepthScale = 2.5f;

static uint32_t emitHealthHud(uint32_t wi) {
    if (!g_inventoryMapped) return wi;
    Vec3 fwd = cameraForward();
    Vec3 right = cameraRight();
    if (right.length() < 1e-5f) right = Vec3(1, 0, 0);
    right = right.normalized();
    const Vec3 up = right.cross(fwd).normalized();
    // Same distance as the lattice so both sit on one visual grid; the HUD is
    // anchored below the panel and to its left.
    const float k = kOverlayDepthScale;
    const float dist = 0.006f * k;
    const Vec3 origin = g_camPos + fwd * dist + up * (-0.0062f * k) + right * (0.0009f * k);
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

    // Anchor: down and to the right of the eye. A 3x3x4 cell block is
    // 0.003 x 0.003 x 0.004 world units; the distance sets its screen size.
    const float k = kOverlayDepthScale;
    const float dist = 0.006f * k;
    const Vec3 origin = g_camPos + fwd * dist + up * (-0.0042f * k) + right * (0.0046f * k);

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
    if (!g_mapPickups.empty()) {
        // Map-authored placements: exact cells, no floor search. An unknown
        // item id is skipped, not faked.
        for (const auto& mp : g_mapPickups) {
            const int defIdx = itemIndexById(mp.item.c_str());
            if (defIdx < 0) continue;
            const ItemDef* d = itemDefAt(g_itemDefs, defIdx);
            if (!d || !d->shape.valid()) continue;
            WorldPickup p;
            p.defIndex = defIdx;
            p.rot = mp.rot;
            p.cx = mp.x; p.cy = mp.y; p.cz = mp.z;
            g_pickups.push_back(p);
        }
        g_pickupMeshDirty = true;
        return;
    }
    // A map without "pickups" simply has none.
    g_pickupMeshDirty = true;
}

// Light fixtures (environment layer) are models, not occupancy: built once
// from the map's lights and drawn in the main pass with the Bulb render class.
// A "bulb" is a 1x2x1 stack of unit cubes centred on its light, the same
// silhouette the old light_bulb cells had.
static constexpr uint32_t kFixtureMaxVerts = 36u * 2u * 64u;
static VkBuffer g_fixtureVB = VK_NULL_HANDLE;
static VkDeviceMemory g_fixtureMem = VK_NULL_HANDLE;
static void* g_fixtureMapped = nullptr;
static uint32_t g_fixtureVertexCount = 0;

static void buildFixtureMesh() {
    if (!g_fixtureVB) {
        const VkDeviceSize size = sizeof(Vertex) * kFixtureMaxVerts;
        createBuffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     g_fixtureVB, g_fixtureMem);
        vkMapMemory(g_device, g_fixtureMem, 0, size, 0, &g_fixtureMapped);
    }
    g_fixtureVertexCount = 0;
    if (!g_fixtureMapped) return;
    const Vec3 axR(1, 0, 0), axU(0, 1, 0), axF(0, 0, 1);
    Vertex* verts = reinterpret_cast<Vertex*>(g_fixtureMapped);
    uint32_t wi = 0;
    const float cls = rc::attr(rc::RenderClass::Bulb);
    for (const auto& l : g_mapLights) {
        if (l.kind != "bulb") continue;
        for (int k = 0; k < 2; ++k)
            emitUnitCube(verts, wi, kFixtureMaxVerts, Vec3(0, 0, 0), axR, axU, axF, l.x - 0.5f,
                         l.y - 1.0f + static_cast<float>(k), l.z - 0.5f, VOXEL_SIZE, l.r, l.g, l.b, cls);
    }
    g_fixtureVertexCount = wi;
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
        v.texLayer = 0.0f;
        v.painted = 0.0f;
        v.shade = 1.0f;
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
    // The sky tile is camera-relative (every vertex is eye + dir * radius), so it
    // genuinely moves with the eye — but not with rotation, and not at all while
    // the player stands still. Rebuild only when the eye actually moved, which
    // turns a per-frame 2.4k-vertex rewrite into a no-op when stationary.
    if (g_skyTileBuilt) {
        const Vec3 d = g_camPos - g_skyTileEye;
        if (d.x * d.x + d.y * d.y + d.z * d.z < 1e-12f) return;
    }
    g_skyTileEye = g_camPos;
    g_skyTileBuilt = true;
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
        dst.mat = rc::attr(rc::RenderClass::Sky);
        dst.texLayer = 0.0f;
        dst.painted = 0.0f;
        dst.shade = 1.0f;
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

    // Moon light-source sprite (RenderClass::Moon), camera-facing at moon bearing
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
        dst.mat = rc::attr(rc::RenderClass::Moon);
        dst.texLayer = 0.0f;
        dst.painted = 0.0f;
        dst.shade = 1.0f;
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
// ---- window-side pause and mouse lock ----
// Both are host/view state, never simulation state. Pausing stops the main
// loop scheduling ticks: the simulation is not told anything, it is simply not
// advanced. The lock only changes where look deltas come from (raw mouse input
// instead of a drag); they still reach the simulation as SimInput::lookDx/Dy.
static bool g_paused = false;
static bool g_cursorLocked = false;
static bool g_uiReady = false;     // ImGui context and backends exist
static bool g_menuForced = false;  // capture harness: draw the menu without pausing
static VkRenderPass g_uiRenderPass = VK_NULL_HANDLE;   // menu pass (see initUi)
static std::vector<VkFramebuffer> g_uiFramebuffers;    // one per swapchain image
static void createUiFramebuffers();

static void updateWindowTitle() {
    std::string t =
        "Voxel FPS 0.0 — WASD walk | Space jump | Q/E lean | RMB/F fire | X ADS | 1-4 cal | "
        "R ammo | V weapon | B mode | Tab pack";
    if (g_paused) t += "  —  PAUSED: Esc to resume, Shift+Esc quits";
    else if (g_cursorLocked) t += "  —  Esc pause";
    else t += "  —  click to look, Esc pause";
    SetWindowTextA(g_hwnd, t.c_str());
}

// Confine the cursor to the client area. Re-run whenever the window moves or
// resizes while locked, since the clip rect is in screen coordinates.
static void clipCursorToClient() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    POINT tl{rc.left, rc.top}, br{rc.right, rc.bottom};
    ClientToScreen(g_hwnd, &tl);
    ClientToScreen(g_hwnd, &br);
    const RECT screen{tl.x, tl.y, br.x, br.y};
    ClipCursor(&screen);
}

static void setCursorLock(bool lock) {
    // Smoke and stress run on someone's desktop; they must never take the mouse.
    if (g_smoke) lock = false;
    if (lock == g_cursorLocked) return;
    g_cursorLocked = lock;
    if (lock) {
        clipCursorToClient();
        ShowCursor(FALSE); // ShowCursor is a counter: called once per state change
    } else {
        ClipCursor(nullptr);
        ShowCursor(TRUE);
    }
    updateWindowTitle();
}

static void setPaused(bool paused) {
    if (paused == g_paused) return;
    g_paused = paused;
    // Nothing pressed while paused may reach the first tick after resume: a
    // fire tap during the pause must not shoot the moment play continues.
    simInputClearEdges(g_pendingInput);
    g_pendingInput.fireHeld = false;
    g_mouseDown = false;
    ReleaseCapture();
    if (paused) setCursorLock(false);
    updateWindowTitle();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // The menu sees every message first (it tracks the mouse even when closed).
    if (g_uiReady && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;
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
        if (g_cursorLocked) clipCursorToClient();
        return 0;
    case WM_MOVE:
        if (g_cursorLocked) clipCursorToClient();
        return 0;
    case WM_KILLFOCUS:
        // Alt-Tab away: pause, free the mouse, and forget held keys (their
        // key-up goes to whichever window has focus, so it would never arrive).
        std::memset(g_keys, 0, sizeof(g_keys));
        if (!g_smoke) setPaused(true);
        return 0;
    case WM_INPUT: {
        // Raw mouse motion drives look while locked: unaccelerated counts, and
        // no dependence on where the cursor is, so it never hits a screen edge.
        // Every path returns through DefWindowProc, which must see WM_INPUT to
        // release the raw input buffer.
        if (g_cursorLocked && !g_paused && !g_inventoryOpen) {
            RAWINPUT raw{};
            UINT size = sizeof(raw);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &size,
                                sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1) &&
                raw.header.dwType == RIM_TYPEMOUSE &&
                (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                g_pendingInput.lookDx += static_cast<float>(raw.data.mouse.lLastX);
                g_pendingInput.lookDy += static_cast<float>(raw.data.mouse.lLastY);
            }
        }
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    case WM_KEYDOWN:
        if (wParam < 256) g_keys[wParam] = true;
        if (wParam == VK_ESCAPE) {
            // Esc pauses; Shift+Esc quits. Smoke/stress keep Esc = quit so a run
            // can be aborted (it then fails with smoke_complete=0, exit 7).
            if (g_smoke || (GetKeyState(VK_SHIFT) & 0x8000)) {
                g_running = false;
                PostQuitMessage(0);
            } else if ((lParam & (1 << 30)) == 0) { // ignore auto-repeat
                setPaused(!g_paused);
            }
            return 0;
        }
        if (g_paused) return 0; // nothing else is a request while paused
        // Everything below is a *request* to the simulation. The view does not
        // decide the active caliber, weapon, ammo, or fire mode itself; it says
        // what the player asked for and sim::tick() validates it against the
        // authoritative inventory. This is what stops a modified client from
        // selecting a weapon it was never given.
        if (wParam == 'F') {
            g_pendingInput.firePressed = true;
            g_pendingInput.fireHeld = true;
        }
        if (wParam == VK_SPACE) g_pendingInput.jump = true;
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
                setCursorLock(false);
            } else {
                setCursorLock(true);
            }
        }
        // R: rotate the held item while the lattice is up; otherwise cycle ammo.
        if (wParam == 'R') {
            if (g_inventoryOpen) {
                g_inventoryHandRot = (g_inventoryHandRot + 1) & 3;
            } else {
                g_pendingInput.cycleAmmo = true;
            }
        }
        // G: take the world item under the crosshair (auto-places into the pack)
        if (wParam == 'G' && !g_inventoryOpen) g_pickupPressed = true;
        // 1-4: caliber class (light medium heavy energy)
        if (wParam == '1') g_pendingInput.selectCaliber = 0;
        if (wParam == '2') g_pendingInput.selectCaliber = 1;
        if (wParam == '3') g_pendingInput.selectCaliber = 2;
        if (wParam == '4') g_pendingInput.selectCaliber = 3;
        if (wParam == 'V') g_pendingInput.cycleWeapon = true;
        if (wParam == 'B') g_pendingInput.cycleFireMode = true;
        return 0;
    case WM_KEYUP:
        if (wParam < 256) g_keys[wParam] = false;
        if (wParam == 'F') g_pendingInput.fireHeld = false;
        return 0;
    case WM_LBUTTONDOWN:
        // While paused, clicks belong to the menu. A click while unlocked only
        // locks the mouse; it is not also read as a look drag or a pick.
        if (g_paused) return 0;
        if (!g_cursorLocked && !g_inventoryOpen && !g_smoke) {
            setCursorLock(true);
            return 0;
        }
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
        if (g_paused) return 0;
        if (g_inventoryOpen) {
            g_inventoryStow = true;
            return 0;
        }
        g_pendingInput.firePressed = true;
        g_pendingInput.fireHeld = true;
        return 0;
    case WM_RBUTTONUP:
        g_pendingInput.fireHeld = false;
        return 0;
    case WM_MOUSEMOVE:
        g_mouseX = static_cast<short>(LOWORD(lParam));
        g_mouseY = static_cast<short>(HIWORD(lParam));
        // While the inventory lattice is up, the cursor selects cells instead of
        // turning the camera, so the click is read as a pick/place, never a look.
        // Drag-look is the fallback when the mouse is not locked (raw input
        // drives look while it is).
        if (g_mouseDown && !g_inventoryOpen && !g_cursorLocked && !g_paused) {
            // Accumulate the raw pixel delta; sim::tick() applies it. Applying
            // yaw here instead would make aim depend on how often Windows
            // delivers WM_MOUSEMOVE, which is not reproducible across machines.
            g_pendingInput.lookDx += static_cast<float>(g_mouseX - g_lastMouseX);
            g_pendingInput.lookDy += static_cast<float>(g_mouseY - g_lastMouseY);
        }
        g_lastMouseX = g_mouseX;
        g_lastMouseY = g_mouseY;
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
"Voxel FPS 0.0",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, g_hInstance, nullptr);
    if (!g_hwnd) fail("CreateWindowEx failed");
    updateWindowTitle();
    // Raw mouse input for locked look (HID generic desktop page, mouse usage).
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = 0;
    rid.hwndTarget = g_hwnd;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
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
    {
        // Anisotropic filtering keeps textures on 1 mm cells readable at grazing
        // angles; enabled only where the device supports it.
        VkPhysicalDeviceFeatures avail{};
        vkGetPhysicalDeviceFeatures(g_phys, &avail);
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(g_phys, &props);
        feats.samplerAnisotropy = avail.samplerAnisotropy;
        g_maxAnisotropy = avail.samplerAnisotropy ? std::min(8.0f, props.limits.maxSamplerAnisotropy) : 1.0f;
    }
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
    g_post.destroyTargets(); // its framebuffers reference the swapchain views
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
    // TRANSFER_SRC lets the capture harness copy a finished frame out (--capture).
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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

// The world pass belongs to the post module: the world renders offscreen at the
// menu's render scale and the post pass draws it to the window.
static void createRenderPass() {
    g_post.init(g_device, g_phys, g_swapFormat);
    g_renderPass = g_post.worldPass;
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
    // Cleared, as rule 12 requires. This was DONT_CARE and only worked because
    // the overlay shared the world pass's depth image, so the lattice was in
    // fact depth-tested against leftover world depth. With the world rendered
    // offscreen that image is never written, and DONT_CARE hid every cube.
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
    // The world renders offscreen (g_post); only the overlay pass draws to the
    // swapchain image with a full-window depth buffer (RULES.md rule 12).
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

// Upload build/textures.bin as one RGBA8 2D array with a full mip chain (the
// mips are what keep a texture on far 1 mm cells from shimmering). Without the
// file a 1x1 white layer is bound so the descriptor is always valid, and
// texturing stays off.
static void uploadTextures() {
    const tex::TextureSet set = tex::load(g_exeDir + "\\textures.bin");
    const uint8_t white[4] = {255, 255, 255, 255};
    const bool have = set.ok();
    const uint32_t size = have ? static_cast<uint32_t>(set.size) : 1u;
    const uint32_t layers = have ? static_cast<uint32_t>(set.keys.size()) : 1u;
    const uint8_t* pixels = have ? set.rgba.data() : white;
    uint32_t mips = 1;
    while ((size >> mips) > 0) ++mips;
    if (have) {
        g_texTable = tex::resolve(set);
        g_texLayerCount = static_cast<int>(layers);
    }

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.extent = {size, size, 1};
    ii.mipLevels = mips;
    ii.arrayLayers = layers;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(g_device, &ii, nullptr, &g_texImage) != VK_SUCCESS) fail("texture image failed");
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g_device, g_texImage, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_device, &mai, nullptr, &g_texMem) != VK_SUCCESS) fail("texture memory failed");
    vkBindImageMemory(g_device, g_texImage, g_texMem, 0);

    const VkDeviceSize bytes = VkDeviceSize(size) * size * 4 * layers;
    VkBuffer stg = VK_NULL_HANDLE;
    VkDeviceMemory stgMem = VK_NULL_HANDLE;
    createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stg, stgMem);
    void* mapped = nullptr;
    vkMapMemory(g_device, stgMem, 0, bytes, 0, &mapped);
    std::memcpy(mapped, pixels, static_cast<size_t>(bytes));
    vkUnmapMemory(g_device, stgMem);

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g_cmdPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(g_device, &cai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    auto barrier = [&](uint32_t level, uint32_t count, VkImageLayout from, VkImageLayout to,
                       VkAccessFlags srcA, VkAccessFlags dstA, VkPipelineStageFlags srcS,
                       VkPipelineStageFlags dstS) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = from;
        b.newLayout = to;
        b.srcAccessMask = srcA;
        b.dstAccessMask = dstA;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = g_texImage;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, count, 0, layers};
        vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    barrier(0, mips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
    region.imageExtent = {size, size, 1};
    vkCmdCopyBufferToImage(cmd, stg, g_texImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    // Mip chain: each level is a linear blit of the one above it.
    for (uint32_t m = 1; m < mips; ++m) {
        barrier(m - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        const int32_t src = static_cast<int32_t>(std::max(1u, size >> (m - 1)));
        const int32_t dst = static_cast<int32_t>(std::max(1u, size >> m));
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, layers};
        blit.srcOffsets[1] = {src, src, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, layers};
        blit.dstOffsets[1] = {dst, dst, 1};
        vkCmdBlitImage(cmd, g_texImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_texImage,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    if (mips > 1)
        barrier(0, mips - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    barrier(mips - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(g_graphicsQueue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_graphicsQueue);
    vkFreeCommandBuffers(g_device, g_cmdPool, 1, &cmd);
    vkDestroyBuffer(g_device, stg, nullptr);
    vkFreeMemory(g_device, stgMem, nullptr);

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = g_texImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers};
    if (vkCreateImageView(g_device, &vi, nullptr, &g_texView) != VK_SUCCESS) fail("texture view failed");
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = static_cast<float>(mips);
    sci.anisotropyEnable = g_maxAnisotropy > 1.0f ? VK_TRUE : VK_FALSE;
    sci.maxAnisotropy = g_maxAnisotropy;
    if (vkCreateSampler(g_device, &sci, nullptr, &g_texSampler) != VK_SUCCESS) fail("texture sampler failed");
}

static void createOccupancyVolume() {
    g_occCpu.assign(static_cast<size_t>(WORLD_W) * WORLD_H * WORLD_D, 0);
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_3D;
    ii.extent = {static_cast<uint32_t>(WORLD_W), static_cast<uint32_t>(WORLD_H), static_cast<uint32_t>(WORLD_D)};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.format = VK_FORMAT_R8_UNORM;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(g_device, &ii, nullptr, &g_occImage) != VK_SUCCESS) fail("occupancy image failed");
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g_device, g_occImage, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_device, &mai, nullptr, &g_occMem) != VK_SUCCESS) fail("occupancy memory failed");
    vkBindImageMemory(g_device, g_occImage, g_occMem, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = g_occImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vi.format = VK_FORMAT_R8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(g_device, &vi, nullptr, &g_occView) != VK_SUCCESS) fail("occupancy view failed");
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(g_device, &sci, nullptr, &g_occSampler) != VK_SUCCESS) fail("occupancy sampler failed");
    for (int i = 0; i < MAX_FRAMES; ++i) {
        createBuffer(g_occCpu.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     g_occStage[i], g_occStageMem[i]);
        vkMapMemory(g_device, g_occStageMem[i], 0, g_occCpu.size(), 0, &g_occStageMapped[i]);
        createBuffer(sizeof(float) * 8 * kMaxLights, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     g_lightBuf[i], g_lightMem[i]);
        vkMapMemory(g_device, g_lightMem[i], 0, sizeof(float) * 8 * kMaxLights, 0, &g_lightMapped[i]);
    }
}

// Copy a remeshed chunk's sent occupancy into the CPU volume and queue it.
static void noteOccupancy(const ViewChunk& c) {
    if (g_occCpu.empty() || !c.hasSnapshot) return;
    for (int lz = 0; lz < CHUNK_SIZE; ++lz)
        for (int ly = 0; ly < CHUNK_SIZE; ++ly)
            for (int lx = 0; lx < CHUNK_SIZE; ++lx) {
                const wire::BlockId b = c.sent.get(lx, ly, lz);
                const bool opaque = b != wire::BlockId::Air && b != wire::BlockId::Water &&
                                    b != wire::BlockId::WaterCurrent;
                const size_t x = size_t(c.cx * CHUNK_SIZE + lx), y = size_t(c.cy * CHUNK_SIZE + ly),
                             z = size_t(c.cz * CHUNK_SIZE + lz);
                g_occCpu[x + size_t(WORLD_W) * (y + size_t(WORLD_H) * z)] = opaque ? 255 : 0;
            }
    const int idx = sim::World::chunkIndex(c.cx, c.cy, c.cz);
    if (std::find(g_occDirty.begin(), g_occDirty.end(), idx) == g_occDirty.end()) g_occDirty.push_back(idx);
}

// Record the pending chunk uploads into this frame's command buffer, before
// the world pass. The staging buffer is this frame's own, so a frame still in
// flight never sees it change.
static void recordOccupancyUpload(VkCommandBuffer cmd, uint32_t frameIndex) {
    if (g_occDirty.empty() || !g_occImage) return;
    uint8_t* stage = static_cast<uint8_t*>(g_occStageMapped[frameIndex]);
    std::vector<VkBufferImageCopy> regions;
    for (int idx : g_occDirty) {
        const int cx = idx % CHUNKS_X, cz = (idx / CHUNKS_X) % CHUNKS_Z, cy = idx / (CHUNKS_X * CHUNKS_Z);
        const size_t x0 = size_t(cx) * CHUNK_SIZE, y0 = size_t(cy) * CHUNK_SIZE, z0 = size_t(cz) * CHUNK_SIZE;
        for (int lz = 0; lz < CHUNK_SIZE; ++lz)
            for (int ly = 0; ly < CHUNK_SIZE; ++ly) {
                const size_t o = x0 + size_t(WORLD_W) * ((y0 + ly) + size_t(WORLD_H) * (z0 + lz));
                std::memcpy(stage + o, g_occCpu.data() + o, CHUNK_SIZE);
            }
        VkBufferImageCopy r{};
        r.bufferOffset = x0 + size_t(WORLD_W) * (y0 + size_t(WORLD_H) * z0);
        r.bufferRowLength = WORLD_W;
        r.bufferImageHeight = WORLD_H;
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageOffset = {int32_t(x0), int32_t(y0), int32_t(z0)};
        r.imageExtent = {uint32_t(CHUNK_SIZE), uint32_t(CHUNK_SIZE), uint32_t(CHUNK_SIZE)};
        regions.push_back(r);
    }
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = g_occImageReady ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask = g_occImageReady ? VK_ACCESS_SHADER_READ_BIT : 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = g_occImage;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // A partial upload must keep the rest, so only the very first upload may
    // discard (UNDEFINED); it covers every chunk, as all start dirty.
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    vkCmdCopyBufferToImage(cmd, g_occStage[frameIndex], g_occImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(regions.size()), regions.data());
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
    g_occChunkUploads += regions.size();
    g_occDirty.clear();
    g_occImageReady = true;
}

static void createDescriptors() {
    uploadTextures();
    createOccupancyVolume();
    VkDescriptorSetLayoutBinding bindings[4]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[2] = bindings[1];
    bindings[2].binding = 2; // occupancy volume
    bindings[3].binding = 3; // light list
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 4;
    lci.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(g_device, &lci, nullptr, &g_dsl) != VK_SUCCESS)
        fail("descriptor set layout failed");

    for (int i = 0; i < MAX_FRAMES; ++i) {
        createBuffer(sizeof(FrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     g_uboBuffers[i], g_uboMems[i]);
        vkMapMemory(g_device, g_uboMems[i], 0, sizeof(FrameUBO), 0, &g_uboMapped[i]);
    }

    VkDescriptorPoolSize poolSizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_FRAMES},
                                         {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * MAX_FRAMES},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_FRAMES}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.poolSizeCount = 3;
    pci.pPoolSizes = poolSizes;
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
        VkDescriptorImageInfo ti{g_texSampler, g_texView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet texWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        texWrite.dstSet = g_descSets[i];
        texWrite.dstBinding = 1;
        texWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texWrite.descriptorCount = 1;
        texWrite.pImageInfo = &ti;
        VkDescriptorImageInfo oi{g_occSampler, g_occView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet occWrite = texWrite;
        occWrite.dstBinding = 2;
        occWrite.pImageInfo = &oi;
        VkDescriptorBufferInfo li{g_lightBuf[i], 0, sizeof(float) * 8 * kMaxLights};
        VkWriteDescriptorSet lightWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        lightWrite.dstSet = g_descSets[i];
        lightWrite.dstBinding = 3;
        lightWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        lightWrite.descriptorCount = 1;
        lightWrite.pBufferInfo = &li;
        VkWriteDescriptorSet writes[4] = {write, texWrite, occWrite, lightWrite};
        vkUpdateDescriptorSets(g_device, 4, writes, 0, nullptr);
    }
}

// Shader lookup is by search, never by a baked-in absolute path. A view process
// may be launched from any working directory, and the whole point of the split is
// that this binary is relocatable. KSHADER_DIR overrides the search for
// deployments that keep assets somewhere else entirely.
static std::string resolveShaderPath(const char* name) {
    const std::string file = std::string("shaders\\") + name + ".spv";
    std::vector<std::string> roots;
    if (const char* env = std::getenv("KSHADER_DIR")) {
        if (*env) roots.push_back(env);
    }
    roots.push_back(g_exeDir);
    roots.push_back(g_exeDir + "\\..");
    roots.push_back(g_exeDir + "\\..\\build");
    char cwd[MAX_PATH];
    if (GetCurrentDirectoryA(MAX_PATH, cwd)) roots.push_back(cwd);

    for (const std::string& root : roots) {
        std::string candidate = root + "\\" + file;
        if (GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
            return candidate;
    }
    std::string tried;
    for (const std::string& root : roots) { tried += "\n  " + root + "\\" + file; }
    MessageBoxA(nullptr, ("Could not locate " + file + ". Set KSHADER_DIR to the folder containing shaders\\" + file + ". Tried:" + tried).c_str(),
                "voxel_engine: missing shader", MB_ICONERROR);
    return std::string();
}

static bool createPipeline() {
    const std::string vertPath = resolveShaderPath("voxel.vert");
    const std::string fragPath = resolveShaderPath("voxel.frag");
    if (vertPath.empty() || fragPath.empty()) return false;
    const std::string postVert = resolveShaderPath("post.vert");
    const std::string postFrag = resolveShaderPath("post.frag");
    if (postVert.empty() || postFrag.empty()) return false;
    g_post.createPipeline(readFile(postVert), readFile(postFrag));

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

    VkVertexInputAttributeDescription attrs[5]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, px)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, nx)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, cr)};
    attrs[3] = {3, 0, VK_FORMAT_R32_SFLOAT, offsetof(Vertex, mat)};
    attrs[4] = {4, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, texLayer)};

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 5;
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
    return true;
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
// Incremental chunk updates go through uploadChunkRange; this only sizes and clears
// the buffer. The caller waits on the in-flight fence before touching shared buffers.
static bool ensureVertexCapacity(uint32_t verts) {
    if (verts == 0) return true;
    VkDeviceSize size = sizeof(Vertex) * static_cast<VkDeviceSize>(verts);
    if (g_vertexBuffer && g_vertexCapacity >= size) return true;
    // Grow with headroom so a small repack rarely reallocates.
    VkDeviceSize need = size + size / 8;
    if (need < size) need = size;
    // The caller only waited on THIS frame's fence. With MAX_FRAMES in flight the
    // other frame's command buffer may still bind the old buffer, so drain every
    // in-flight frame before freeing it. Growth is rare (headroom above), so
    // this stall is too.
    VkFence live[MAX_FRAMES];
    uint32_t liveCount = 0;
    for (int i = 0; i < MAX_FRAMES; ++i)
        if (g_inFlight[i]) live[liveCount++] = g_inFlight[i];
    if (g_vertexBuffer && liveCount > 0)
        vkWaitForFences(g_device, liveCount, live, VK_TRUE, UINT64_MAX);
    destroyWorldMeshBuffer();
    createBuffer(need, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 g_vertexBuffer, g_vertexMem);
    vkMapMemory(g_device, g_vertexMem, 0, need, 0, &g_vertexMapped);
    g_vertexCapacity = need;
    g_vertexCount = verts;
    return g_vertexMapped != nullptr;
}

// What ballistics does to the rest of this simulation: debris, remesh flags,
// damage intake and telemetry. The module itself holds none of these.
struct SimBallisticsHooks final : ballistics::Hooks {
    void voxelDestroyed(int x, int y, int z, Block was, MaterialId mat,
                        const ballistics::Impact& imp) override {
        // Visual degradation: spawn 8x8x8 sub-voxel debris chips (occupancy still unit cube).
        g_debris.spawnFromVoxel(x, y, z, mat, imp.dx, imp.dy, imp.dz, imp.energy, VOXEL_SIZE,
                                imp.aoeScale);
        (void)was;
        g_meshDirty = true;
        ++g_voxelsDestroyed;
    }
    void ricochet(int x, int y, int z, MaterialId mat, const ballistics::Impact& imp) override {
        ++g_debris.ricochets;
        g_debris.spawnFromVoxel(x, y, z, mat, imp.dx, imp.dy, imp.dz, imp.energy, VOXEL_SIZE,
                                imp.aoeScale);
    }
    ArmorZone bodySweep(float ax, float ay, float az, float bx, float by, float bz,
                        float radiusCells) override {
        return projectileHitZone(ax, ay, az, bx, by, bz, radiusCells);
    }
    float bodyDistance(float x, float y, float z, ArmorZone& nearest) override {
        const health::BodyCellOrigin org = health::bodyCellOrigin(g_player.px, g_player.py, g_player.pz);
        nearest = health::zoneNearestPoint(org, x, y, z);
        return health::distanceToBody(org, x, y, z);
    }
    void damageBody(float energy, const std::string& effect, ArmorZone zone) override {
        damagePlayerAtZone(energy, effect, zone);
    }
};
static SimBallisticsHooks g_ballisticsHooks;

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
    if (!g_world) return;
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
    g_ballistics.last.aoeScale = impactAoeScale(def);

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
        ballistics::fireHitscan(g_ballistics, *g_world, g_ballisticsHooks, def, 1.0f,
                                g_camPos.x, g_camPos.y, g_camPos.z, aim.x, aim.y, aim.z);
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
                ballistics::spawnProjectile(g_ballistics, def, g_camPos.x, g_camPos.y, g_camPos.z,
                                            dir.x, dir.y, dir.z);
            }
            g_ballisticShots += spawnN;
        } else {
            ballistics::spawnProjectile(g_ballistics, def, g_camPos.x, g_camPos.y, g_camPos.z,
                                        aim.x, aim.y, aim.z);
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

// Movement smoke (RULES.md, "Player body, and the camera-offset contract").
//
// --smoke skips the frame loop, so none of this is reachable by walking around.
// The harness drives the *real* movement seam with scripted SimInput against a
// local grid, which is the point: the old smoke poked g_keys and latched keys
// inside the update, so it proved nothing about what a client would experience.
struct MovementSmokeReport {
    bool stanceCycleOk = false;
    bool stanceHeadroomOk = false;
    bool gaitOk = false;
    bool staminaDrainOk = false;
    bool staminaRegenOk = false;
    bool slideEnters = false;
    bool slideExhausts = false;
    bool dashEnters = false;
    bool dashCooldownOk = false;
    bool dashInvulnOk = false;
    // The invulnerability window is only real if it (a) withholds damage while
    // open, (b) expires on its own, and (c) stops withholding once expired.
    // dashInvulnOk alone proves none of that -- it only reads one tick's flag.
    bool dashBlocksDamageOk = false;
    bool dashInvulnExpiresOk = false;
    bool wallrunDetects = false;
    bool wallrunTimesOut = false;
    bool bodyMovesNotCamera = false;
    bool offsetIsPresentationOnly = false;
    bool waterNotSolid = false;
    bool edgeNotLatchOk = false;
    float staminaAfterSprint = 0.0f;
    float maxSpeed = 0.0f;
};

static MovementSmokeReport g_moveSmoke;

static MovementSmokeReport runMovementSmoke() {
    MovementSmokeReport rep;

    // A local grid: a floor at y=0, a wall at x=20, and a water cell that must
    // never read as ground or as a wall.
    sim::World w;
    w.alloc();
    // alloc() sizes the chunk list only; every chunk still needs its voxel
    // volume, or get()/set() index an empty vector.
    for (int cy = 0; cy < CHUNKS_Y; ++cy)
        for (int cz = 0; cz < CHUNKS_Z; ++cz)
            for (int cx = 0; cx < CHUNKS_X; ++cx) {
                SimChunk& c = w.chunks[sim::World::chunkIndex(cx, cy, cz)];
                c.cx = cx; c.cy = cy; c.cz = cz;
                c.voxels.assign(VOXELS_PER_CHUNK, Block::Air);
            }
    for (int z = 0; z < WORLD_D; ++z)
        for (int x = 0; x < WORLD_W; ++x) w.set(x, 0, z, Block::Concrete);
    for (int y = 1; y < 30; ++y)
        for (int z = 0; z < WORLD_D; ++z) w.set(20, y, z, Block::Concrete);
    for (int y = 1; y < 6; ++y)
        for (int z = 0; z < WORLD_D; ++z) w.set(60, y, z, Block::Water);

    movement::MoveState m;
    // Feet rest on top of the y=0 floor, so the body is one voxel up. `py` is
    // the feet, not the centre.
    constexpr float kFloorTop = 1.0f * kVoxelSize;
    m.px = 10.5f * kVoxelSize;  m.py = kFloorTop;  m.pz = 10.5f * kVoxelSize;
    m.vx = m.vy = m.vz = 0.0f;
    m.onGround = true;

    constexpr float kDt = 1.0f / 60.0f;
    constexpr float kBase = 0.045f;   // g_moveSpeed
    constexpr float kJump = 0.055f;  // g_player.jumpSpeed

    // Advance the body the way updatePlayerPhysics does: integrate, run the
    // state machine, then apply the intent it returns.
    auto integrate = [&](movement::MoveState& s, const movement::MoveIntent& it, float baseScale) {
        const float accel = s.onGround ? 18.0f : 4.0f;
        const float friction = s.onGround ? 12.0f : 1.5f;
        if (it.overrideHorizontal) {
            s.vx = it.vx;
            s.vz = it.vz;
        } else if (baseScale > 0.0f) {
            const float want = baseScale * (s.onGround ? 18.0f : 4.0f);
            s.vx += (want - s.vx) * std::min(1.0f, accel * kDt);
            s.vz += (want - s.vz) * std::min(1.0f, accel * kDt);
        } else {
            const float damp = std::exp(-friction * kDt);
            s.vx *= damp; s.vz *= damp;
        }
        if (it.jumpImpulse > 0.0f) { s.vy = it.jumpImpulse; s.onGround = false; }
        s.vy -= kWorldGravity * it.gravityScale * kDt;
        if (s.vy < -0.25f) s.vy = -0.25f;
        s.px += s.vx * kDt; s.py += s.vy * kDt; s.pz += s.vz * kDt;
        if (s.py < kFloorTop) { s.py = kFloorTop; s.vy = 0.0f; s.onGround = true; }
        else { s.onGround = false; }
    };

    auto step = [&](const SimInput& in, bool advance = true) {
        if (advance) { m.px += m.vx * kDt; m.py += m.vy * kDt; m.pz += m.vz * kDt; }
        const movement::MoveResult r = movement::update(kDt, in, w, m, 0.0f, kBase, kJump);
        if (advance) integrate(m, r.intent, 0.0f);
        return r;
    };

    // --- stance: the cycle is an edge, and it changes the body height ---
    {
        SimInput i;
        i.stanceCycle = true;
        step(i, false);
        const bool wentCrouch = m.stance == movement::Stance::Crouch;
        const float crouchHeight = movement::STANCE_COLLISION_HEIGHT[static_cast<int>(m.stance)];

        step(i, false);
        const bool wentProne = m.stance == movement::Stance::Prone;
        const float proneHeight = movement::STANCE_COLLISION_HEIGHT[static_cast<int>(m.stance)];

        step(i, false);
        const bool wentStanding = m.stance == movement::Stance::Standing;
        const float standHeight = movement::STANCE_COLLISION_HEIGHT[static_cast<int>(m.stance)];

        // Stance must be a real body height change, not a camera offset.
        rep.stanceCycleOk = wentCrouch && wentProne && wentStanding &&
                            crouchHeight < standHeight && proneHeight < crouchHeight;

        // An edge lasts one tick. A frame with no edge must leave the stance
        // alone — the old `static bool prevControl` latch had no such property.
        step(SimInput{}, false);
        rep.edgeNotLatchOk = m.stance == movement::Stance::Standing;
    }

    // --- stance is a body height, and standing up needs headroom ---
    {
        // Under a low ceiling, crouch holds until there is room to stand.
        for (int y = 1; y <= 6; ++y)
            for (int z = 0; z < WORLD_D; ++z) w.set(40, y, z, Block::Concrete);
        movement::MoveState low;
        low.px = 40.5f * kVoxelSize; low.py = kFloorTop; low.pz = 10.5f * kVoxelSize;
        low.stance = movement::Stance::Standing;
        low.onGround = true;
        SimInput hold;
        hold.crouch = true;
        movement::update(kDt, hold, w, low, 0.0f, kBase, kJump);
        const bool didCrouch = low.stance == movement::Stance::Crouch;
        hold.crouch = false;
        movement::update(kDt, hold, w, low, 0.0f, kBase, kJump);
        const bool stayedCrouched = low.stance == movement::Stance::Crouch;
        rep.stanceHeadroomOk = didCrouch && stayedCrouched;
        for (int y = 1; y <= 6; ++y)
            for (int z = 0; z < WORLD_D; ++z) w.set(40, y, z, Block::Air);
    }

    // --- gait and stamina ---
    {
        SimInput run;
        run.moveForward = 1.0f;
        float runScale = 0.0f;
        for (int t = 0; t < 30; ++t) {
            const auto r = step(run);
            runScale = r.intent.speedScale;
        }
        const bool running = m.gait == movement::Gait::Run;
        SimInput sprint;
        sprint.moveForward = 1.0f;
        sprint.sprint = true;
        float sprintScale = 0.0f;
        for (int t = 0; t < 120; ++t) {
            const auto r = step(sprint);
            sprintScale = r.intent.speedScale;
        }
        rep.maxSpeed = sprintScale;
        rep.staminaAfterSprint = m.stamina;
        const bool sprinting = m.gait == movement::Gait::Sprint;
        const bool drained = m.stamina < movement::kStaminaMax;
        // Sprint must actually be the faster gait, or the drain buys nothing.
        rep.gaitOk = running && sprinting && sprintScale > runScale;
        rep.staminaDrainOk = drained;

        // With no intent held, stamina must come back — and only after the
        // regen delay, not on the same tick the drain stopped.
        for (int t = 0; t < 60; ++t) step(SimInput{});
        rep.staminaRegenOk = m.stamina > rep.staminaAfterSprint;
    }

    // --- slide: enter on sprint+crouch while fast, then wear off ---
    {
        movement::MoveState s;
        s.px = 10.5f * kVoxelSize; s.py = kFloorTop; s.pz = 10.5f * kVoxelSize;
        s.onGround = true;
        s.gait = movement::Gait::Sprint;
        s.prevFlatSpeed = 0.5f;   // already moving fast
        SimInput into;
        into.sprint = true;
        into.crouch = true;
        into.moveForward = 1.0f;
        const auto first = movement::update(kDt, into, w, s, 0.0f, kBase, kJump);
        rep.slideEnters = s.sliding && first.intent.overrideHorizontal;
        for (int t = 0; t < 60 * 8; ++t) {
            const auto r = movement::update(kDt, SimInput{}, w, s, 0.0f, kBase, kJump);
            integrate(s, r.intent, 0.0f);
        }
        rep.slideExhausts = !s.sliding;
    }

    // --- dash: an edge, with a cooldown and a damage window ---
    {
        movement::MoveState d;
        d.px = 10.5f * kVoxelSize; d.py = kFloorTop; d.pz = 10.5f * kVoxelSize;
        d.onGround = true;
        SimInput go;
        go.dash = true;
        go.moveForward = 1.0f;
        const auto started = movement::update(kDt, go, w, d, 0.0f, kBase, kJump);
        rep.dashEnters = d.dashing && started.intent.overrideHorizontal;
        rep.dashInvulnOk = started.dashInvuln;

        // The same edge a second later must not dash again: the cooldown, not a
        // view latch, is what gates it.
        const float cool = d.dashCooldown;
        movement::update(kDt, go, w, d, 0.0f, kBase, kJump);
        rep.dashCooldownOk = cool > 0.0f && std::fabs(d.dashCooldown - cool) < movement::DASH_COOLDOWN;

        // The window must withhold real damage, not merely report a flag. Drive
        // the actual intake while the dash is open and assert HP does not move;
        // then let the window lapse and assert the same hit now lands. Without
        // the second half this would pass for a dash that is invulnerable
        // forever, which is the bug this assertion exists to prevent.
        //
        // g_move/g_health are the production objects here on purpose: a local
        // MoveState would only prove the flag reads back, never that the intake
        // consults it. Both are restored so the rest of the smoke is unaffected.
        movement::MoveState savedMove = g_move;
        health::ActorHealth savedHealth = g_health;
        const int savedBlocks = g_dashInvulnBlocks;
        g_move = d;                      // adopt the already-dashing state
        g_health = health::ActorHealth{}; // full HP, no armor
        g_health.health = 100.0f; g_health.maxHealth = 100.0f;

        SimInput idle;
        idle.moveForward = 1.0f;
        // The contract, stated per tick rather than as a hand-computed tick
        // count: on every tick the window reports itself open, the same hit must
        // land no HP. Counting "within DASH_INVULN_TIME/kDt" here would be
        // fragile -- the adopted state has already spent ticks -- and would
        // silently pass a window that expires early. Track the flag instead and
        // require at least one protected tick.
        int protectedTicks = 0;
        bool blockedDuringWindow = true;
        for (int t = 0; t < 240; ++t) {
            const auto r = movement::update(kDt, idle, w, g_move, 0.0f, kBase, kJump);
            const float hpBefore = g_health.health;
            damagePlayerAtZone(500.0f, "kinetic", ArmorZone::Chest);
            if (r.dashInvuln || g_move.dashInvuln) {
                ++protectedTicks;
                if (g_health.health < hpBefore) blockedDuringWindow = false;
            }
            if (!g_move.dashInvuln && protectedTicks > 0) break;
        }
        rep.dashBlocksDamageOk = blockedDuringWindow && protectedTicks > 0;

        // Now outlast the window. The dash itself ends first, so keep ticking
        // with no input; invulnerability must clear and the hit must then land.
        bool cleared = false;
        for (int t = 0; t < 240; ++t) {
            movement::update(kDt, SimInput{}, w, g_move, 0.0f, kBase, kJump);
            if (!g_move.dashInvuln) { cleared = true; break; }
        }
        const float hpBeforeHit = g_health.health;
        damagePlayerAtZone(500.0f, "kinetic", ArmorZone::Chest);
        rep.dashInvulnExpiresOk = cleared && g_health.health < hpBeforeHit;

        g_move = savedMove;
        g_health = savedHealth;
        g_dashInvulnBlocks = savedBlocks;
    }

    // --- wallrun: a wall adjacent to the body while airborne ---
    {
        movement::MoveState wr;
        // Body centred half a cell inside the wall face, so the wall is within
        // the body radius and the probe can legitimately see it.
        wr.px = 19.4f * kVoxelSize; wr.py = 5.0f * kVoxelSize; wr.pz = 10.5f * kVoxelSize;
        wr.onGround = false;
        bool sawWall = false;
        for (int t = 0; t < 10; ++t) {
            const auto r = movement::update(kDt, SimInput{}, w, wr, 0.0f, kBase, kJump);
            if (r.intent.gravityScale < 1.0f) sawWall = true;
        }
        rep.wallrunDetects = sawWall;

        // It must give up on its own rather than running the wall forever.
        for (int t = 0; t < 60 * 5; ++t) movement::update(kDt, SimInput{}, w, wr, 0.0f, kBase, kJump);
        rep.wallrunTimesOut = !wr.wallRunning;
    }

    // --- the body moved; the camera was never touched ---
    // The old bug was that the camera was the thing that moved. Here the only
    // thing with a position is the state we were handed, and it moved under
    // collision: forward intent from a standing start must leave the body
    // measurably further along the floor it is standing on.
    {
        movement::MoveState b;
        b.px = 10.5f * kVoxelSize; b.py = kFloorTop; b.pz = 10.5f * kVoxelSize;
        b.onGround = true;
        SimInput into;
        into.moveForward = 1.0f;
        const float before = b.px;
        for (int t = 0; t < 60; ++t) {
            const auto r = movement::update(kDt, into, w, b, 0.0f, kBase, kJump);
            integrate(b, r.intent, kBase * r.intent.speedScale);
        }
        rep.bodyMovesNotCamera = (b.px - before) > 1e-4f;
    }

    // --- an offset never becomes load-bearing ---
    {
        // Identical body state and identical intent must produce an identical
        // body, whether or not the previous tick produced a roll. If the camera
        // offset fed back into position, the two runs would diverge.
        auto runOnce = [&](bool consumeOffset) {
            movement::MoveState s;
            s.px = 10.5f * kVoxelSize; s.py = kFloorTop; s.pz = 10.5f * kVoxelSize;
            s.onGround = true;
            SimInput go;
            go.dash = true; go.moveForward = 1.0f; go.sprint = true;
            float lastX = s.px;
            for (int t = 0; t < 40; ++t) {
                const auto r = movement::update(kDt, go, w, s, 0.0f, kBase, kJump);
                integrate(s, r.intent, kBase * r.intent.speedScale);
                lastX = s.px;
                if (consumeOffset) {
                    // Deliberately "use" the offset, to prove that doing so
                    // cannot change the body. The movement code never reads it.
                    volatile float sink = r.cam.roll + r.cam.forwardLean +
                                          r.cam.lateralLean + r.cam.eyeLift;
                    (void)sink;
                }
            }
            return lastX;
        };
        rep.offsetIsPresentationOnly = runOnce(false) == runOnce(true);
    }

    // --- water is never solid for the body ---
    {
        movement::MoveState sw;
        sw.px = 60.5f * kVoxelSize; sw.py = 2.0f * kVoxelSize; sw.pz = 10.5f * kVoxelSize;
        sw.onGround = false;
        const bool inWater = !movement::solidForPhysics(w, 60, 2, 10);
        // Water is not ground: a body over water must not read as supported.
        const bool noGround = !movement::groundUnder(w, sw.px, sw.py, sw.pz, 0.0185f);
        rep.waterNotSolid = inWater && noGround;
    }

    return rep;
}

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
            org, 0.0f, 0.0125f, -0.010f, 0.0f, 0.0125f, 0.010f);
        const health::BodyHit legs = health::segmentHitBody(
            org, -0.010f, 0.0035f, 0.0f, 0.010f, 0.0035f, 0.0f);
        const health::BodyHit miss = health::segmentHitBody(
            org, -0.010f, 0.0035f, 0.060f, 0.010f, 0.0035f, 0.060f);
        // Radial pricing: a blast at the feet resolves to legs, one at the
        // camera height to head.
        const health::BodyCellOrigin live =
            health::bodyCellOrigin(g_player.px, g_player.py, g_player.pz);
        const ArmorZone atEye =
            health::zoneNearestPoint(live, g_camPos.x, g_camPos.y, g_camPos.z);
        rep.zoneHitOk = head.zone == ArmorZone::Head && chest.zone == ArmorZone::Chest &&
                        legs.zone == ArmorZone::Legs && miss.zone == ArmorZone::Count &&
                        atEye == ArmorZone::Head;
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

struct SimViewSmokeReport {
    bool visibleSetOk = false;
    bool antiCheatGatingOk = false;
    bool skirtIsolationOk = false;
    bool cornerAoOk = false;
    bool normalSmoothingOk = false;
    bool cubicPreservedOk = false;
    bool meshWorkersEquivOk = false; // pooled meshing == serial meshing, byte for byte
    int meshWorkersHelpers = 0;
    int meshWorkersChunks = 0;
};

static SimViewSmokeReport g_simViewSmoke;

// MAP-mode voxfmt loader smoke. --smoke skips the frame loop, so the loader is
// exercised directly here: the painter-exported data/voxfmt fixture must parse
// with every field intact (including escaped quotes), counters restore per the
// id_counters contract, and the voxel grid stamps into a fresh all-Air world as
// unit cubes. The refusal cases are crafted text, so they prove the reader
// gates without depending on a second file on disk.
struct MapVoxSmokeReport {
    bool fileFound = false;
    bool docOk = false;
    bool formatOk = false;
    bool modeOk = false;
    bool unitOk = false;
    bool voxelSizeOk = false;
    bool dimsOk = false;
    int events = 0, npcs = 0, routes = 0, voxels = 0, dropped = 0;
    bool eventFieldOk = false;   // every scripted_event field, exact
    bool npcFieldOk = false;     // every npc field, exact
    bool routeFieldOk = false;   // every patrol_route/node field, exact
    bool countersRestoredOk = false; // evt==2, npc==2, route==2 on the fixture
    bool materialDropOk = false; // un-representable material is dropped, counted
    bool stampOk = false;        // 1024 cells written, all concrete, no leftovers
    bool appearanceOk = false;   // painter rgb -> appearance -> wire -> mesh colour
    bool prefabOk = false;       // v2 asset parse + quarter-turn stamp of cells and paint
    bool refusalVersionOk = false; // format_version 2 + non-unit docs refuse
    bool refusalUnitOk = false;
    bool refusalVoxelSizeOk = false;
    int stampWritten = 0;
    int stampSkipped = 0;
};

static MapVoxSmokeReport g_mapSmoke;

static MapVoxSmokeReport runMapVoxSmoke() {
    MapVoxSmokeReport rep;

    // The fixture lives in data/voxfmt in the repo and is copied to build/voxfmt
    // by build.ps1, so probe the same g_exeDir ancestors the other loaders use.
    const std::string mapCandidates[] = {
        g_exeDir + "\\voxfmt\\smoke_map.vox.json",
        g_exeDir + "\\..\\data\\voxfmt\\smoke_map.vox.json",
        g_exeDir + "\\..\\..\\data\\voxfmt\\smoke_map.vox.json",
    };
    mapvox::Doc doc;
    bool anyFound = false;
    for (const auto& p : mapCandidates) {
        mapvox::Doc cand;
        mapvox::loadMapVox(p, cand);
        if (cand.fileFound) {
            anyFound = true;
            doc = cand;        // last found candidate wins; parse status kept
            break;
        }
    }
    rep.fileFound = anyFound;

    rep.formatOk = (doc.formatVersion == mapvox::kMaxFormatVersion); // the painter writes the current layout
    rep.modeOk = doc.modeMap;
    rep.unitOk = doc.unitOk;
    rep.voxelSizeOk = doc.voxelSizeOk;
    rep.dimsOk = (doc.sx == 32 && doc.sy == 16 && doc.sz == 32);
    rep.events = static_cast<int>(doc.events.size());
    rep.npcs = static_cast<int>(doc.npcs.size());
    rep.routes = static_cast<int>(doc.routes.size());
    rep.voxels = static_cast<int>(doc.voxels.size()) + doc.rleCells; // v1 list or v2 runs
    rep.dropped = doc.dropped;
    rep.docOk = doc.ok;

    // Event fields: name/condition carry escaped quotes and a quote inside a
    // string that must survive a round trip.
    if (doc.events.size() == 1) {
        const auto& e = doc.events[0];
        rep.eventFieldOk =
            e.x == 10 && e.y == 1 && e.z == 10 && e.id == "evt_1" &&
            e.name == "Test \"Event\"" && e.script == "open_door.ps1" &&
            e.trigger == "on_signal" && e.radius == 2.0f && e.cooldown == 20 &&
            e.requiredSignal == "key_found" && e.emitSignal == "door_open" &&
            e.condition == "count(\"kills\") > 0" && e.repeatSet &&
            e.repeat == 3 && !e.enabled;
    }

    if (doc.npcs.size() == 1) {
        const auto& n = doc.npcs[0];
        rep.npcFieldOk =
            n.x == 15 && n.y == 1 && n.z == 15 && n.id == "npc_1" &&
            n.name == "Guard" && n.npcType == "guard" && n.aiProfile == "patrol" &&
            n.patrolRoute == "route_1" && n.health == 120.0f &&
            n.maxHealth == 150.0f && n.speed == 0.075f && n.viewDist == 24.5f &&
            n.viewAngle == 110.0f && n.faction == "hostile" &&
            n.dialogue == "guard_taunt" && n.inventory.empty() && n.isStatic &&
            n.spawnTick == 120 && n.spawnCondition == "wave_2" && n.enabled;
    }

    if (doc.routes.size() == 1) {
        const auto& r = doc.routes[0];
        rep.routeFieldOk =
            r.id == "route_1" && r.name == "Guard Patrol" && !r.loop &&
            r.nodes.size() == 3 && r.nodes[0].x == 15 && r.nodes[0].y == 1 &&
            r.nodes[0].z == 15 && r.nodes[0].wait == 2.0f &&
            r.nodes[0].action == "idle" && r.nodes[1].x == 20 &&
            r.nodes[1].y == 1 && r.nodes[1].z == 15 && r.nodes[1].wait == 1.0f &&
            r.nodes[1].action == "look" && r.nodes[2].x == 20 &&
            r.nodes[2].y == 1 && r.nodes[2].z == 20 && r.nodes[2].wait == 2.5f &&
            r.nodes[2].action == "interact";
    }

    // id_counters: fixture persists {evt:2, npc:2, route:1} and ids end at _1,
    // so restored is {evt:2, npc:2, route:max(1, 1+1)=2}.
    rep.countersRestoredOk =
        doc.restored.evt == 2 && doc.restored.npc == 2 && doc.restored.route == 2;

    // Crafted refusals: readers must refuse what they don't understand and never
    // load non-cubic grids.
    {
        mapvox::Doc d;
        rep.refusalVersionOk = !mapvox::parseMapVox(
            "{\"format_version\":3,\"unit\":1,\"voxel_size\":0.001,\"mode\":\"map\",\"dims\":[4,4,4]}", d);
        rep.refusalUnitOk = !mapvox::parseMapVox(
            "{\"format_version\":1,\"unit\":2,\"voxel_size\":0.001,\"mode\":\"map\",\"dims\":[4,4,4]}", d);
        rep.refusalVoxelSizeOk = !mapvox::parseMapVox(
            "{\"format_version\":1,\"unit\":1,\"voxel_size\":0.004,\"mode\":\"map\",\"dims\":[4,4,4]}", d);
    }

    // Material drop: a voxel whose painter material has no sim::Block is
    // removed and counted, never stamped as a wrong block. Craft a 2-cell doc:
    // concrete (kept) + plexiglass (dropped).
    {
        mapvox::Doc d;
        const bool parsed = mapvox::parseMapVox(
            "{\"format_version\":1,\"unit\":1,\"voxel_size\":0.001,\"mode\":\"map\","
            "\"dims\":[4,4,4],"
            "\"voxels\":[{\"x\":0,\"y\":0,\"z\":0,\"mat\":\"concrete\",\"rgb\":6710891},"
            "{\"x\":1,\"y\":0,\"z\":0,\"mat\":\"plexiglass\",\"rgb\":16777215}]}",
            d);
        rep.materialDropOk = parsed && d.ok && d.voxels.size() == 1 &&
                             d.dropped == 1 &&
                             d.voxels[0].block == sim::Block::Concrete;
    }

    // Stamp the whole fixture into a fresh all-Air world at origin (8,0,8). The
    // fixture is a 32x32 concrete slab at y=0; the stamp must place 1024 unit
    // cubes (no shrink, no offsets) and leave the rest Air.
    if (rep.fileFound && doc.ok && rep.voxels == 1024) {
        sim::World w;
        w.alloc();
        for (int cy = 0; cy < CHUNKS_Y; ++cy)
            for (int cz = 0; cz < CHUNKS_Z; ++cz)
                for (int cx = 0; cx < CHUNKS_X; ++cx) {
                    SimChunk& c = w.chunks[sim::World::chunkIndex(cx, cy, cz)];
                    c.cx = cx; c.cy = cy; c.cz = cz;
                    c.voxels.assign(VOXELS_PER_CHUNK, Block::Air);
                }
        const mapvox::StampResult sr = mapvox::stampMapVox(doc, w, 8, 0, 8);
        rep.stampWritten = sr.written;
        rep.stampSkipped = sr.skipped;
        int concrete = 0;
        for (int z = 8; z < 40; ++z)
            for (int x = 8; x < 40; ++x) {
                if (w.get(x, 0, z) == Block::Concrete) ++concrete;
            }
        const bool allConcrete = (concrete == 1024);
        const bool aboveClear = (w.get(8, 1, 8) == Block::Air && w.get(39, 20, 39) == Block::Air);
        rep.stampOk = sr.written == 1024 && sr.skipped == 0 && allConcrete && aboveClear;

        // Appearance layer. The fixture carries only its material's own colour,
        // which v2 does not record as paint, so nothing is painted by it.
        const bool stamped = sr.painted == doc.appearCells && w.getAppearance(8, 0, 8) == 0;
        // Breaking a cell takes its paint with it.
        sim::World broken = w;
        broken.set(8, 0, 8, Block::Air);
        broken.setAppearance(9, 0, 8, broken.paletteIndexFor(0x123456u));
        broken.setAppearance(8, 0, 8, broken.paletteIndexFor(0x123456u));
        broken.set(8, 0, 8, Block::Air);
        const bool breakClears = broken.getAppearance(8, 0, 8) == 0 && broken.getAppearance(9, 0, 8) != 0;
        // A cell painted pure red must reach the mesh as red, and only via the
        // palette: the same snapshot meshed without one shows no red at all.
        w.setAppearance(20, 0, 20, w.paletteIndexFor(0xFF0000u));
        wire::Palette pal;
        sendPalette(w, pal);
        ViewChunk vc;
        vc.cx = 0; vc.cy = 0; vc.cz = 0;
        sendChunkSnapshot(w, vc);
        meshview::Stats st;
        auto redVerts = [](const ViewChunk& c) {
            int n = 0;
            for (const auto& v : c.mesh) n += (v.r > 0.3f && v.g < 0.05f && v.b < 0.05f) ? 1 : 0;
            return n;
        };
        vc.palette = &pal;
        meshview::meshChunk(vc, st);
        const int withPalette = redVerts(vc);
        vc.palette = nullptr;
        meshview::meshChunk(vc, st);
        const int withoutPalette = redVerts(vc);
        rep.appearanceOk = stamped && breakClears && withPalette > 0 && withoutPalette == 0 &&
                           vc.sent.appearance(20, 0, 20) == w.getAppearance(20, 0, 20);

        // Prefab: a v2 asset (3x1x2 wood L, one cell painted red, plus a
        // painter-only material that must be dropped) turned a quarter turn.
        // Local (x, z) -> (sz-1-z, x) with sz = 2, stamped at (60, 5, 60).
        mapvox::Doc pf;
        const bool parsed = mapvox::parseAssetVox(
            "{\"format_version\":2,\"unit\":1,\"voxel_size\":0.001,\"mode\":\"model\",\"dims\":[3,1,2],"
            "\"appearance\":{\"palette\":[[255,0,0]],\"runs\":[0,1,0,1,1]},"
            "\"cells_rle\":{\"palette\":[\"wood\",\"plexiglass\"],\"runs\":[0,0,0,3,0,0,1,0,1,0,0,1,1,1,1]}}",
            pf);
        sim::World pw = makeEmptyWorld();
        mapvox::stampMapVox(pf, pw, 60, 5, 60, 1);
        const bool cellsOk = pw.get(61, 5, 60) == Block::Wood && pw.get(61, 5, 61) == Block::Wood &&
                             pw.get(61, 5, 62) == Block::Wood && pw.get(60, 5, 60) == Block::Wood &&
                             pw.get(60, 5, 61) == Block::Air;
        const bool paintOk = pw.getAppearance(60, 5, 60) != 0 &&
                             pw.palette[pw.getAppearance(60, 5, 60)] == 0xFF0000u &&
                             pw.getAppearance(61, 5, 60) == 0;
        rep.prefabOk = parsed && pf.dropped == 1 && pf.rleCells == 4 && cellsOk && paintOk;
    }

    return rep;
}

static SimViewSmokeReport runSimViewSmoke(const sim::World& world) {
    SimViewSmokeReport rep;

    // 1. Authoritative visible set computation (Milestone 3)
    {
        const Vec3 fwd(0.0f, 0.0f, -1.0f);
        sim::visible::VisibleSet vis = sim::visible::computeVisibleSet(
            world, g_camPos.x, g_camPos.y, g_camPos.z, fwd.x, fwd.y, fwd.z);
        const int ecx = std::clamp(static_cast<int>(g_camPos.x / (sim::kChunkSize * VOXEL_SIZE)), 0, sim::kChunksX - 1);
        const int ecy = std::clamp(static_cast<int>(g_camPos.y / (sim::kChunkSize * VOXEL_SIZE)), 0, sim::kChunksY - 1);
        const int ecz = std::clamp(static_cast<int>(g_camPos.z / (sim::kChunkSize * VOXEL_SIZE)), 0, sim::kChunksZ - 1);
        rep.visibleSetOk = vis.isChunkVisible(ecx, ecy, ecz) && vis.visibleCount() > 0 && vis.visibleCount() <= sim::World::chunkCount();
    }

    // 2. Anti-cheat gating: occluded chunks transmit 0 data, and meshing an occluded chunk emits 0 vertices
    {
        ViewChunk testVc;
        testVc.cx = 0; testVc.cy = 0; testVc.cz = 0;
        sendChunkSnapshot(world, testVc, false); // occluded
        bool allAir = true;
        for (const auto& cell : testVc.sent.cells) {
            if (cell.id != static_cast<uint8_t>(wire::BlockId::Air)) {
                allAir = false;
                break;
            }
        }
        meshview::meshChunk(testVc, g_meshStats);
        const bool zeroVerts = testVc.mesh.empty();

        // When visible, non-air data is sent and faces are generated
        sendChunkSnapshot(world, testVc, true);
        meshview::meshChunk(testVc, g_meshStats);
        const bool hasVerts = !testVc.mesh.empty();

        rep.antiCheatGatingOk = allAir && zeroVerts && hasVerts;
    }

    // 3. Skirt isolation: no out-of-skirt access occurred during meshing
    {
        rep.skirtIsolationOk = (g_meshStats.skirtAccessViolations == 0);
    }

    // 4. Corner Ambient Occlusion (AO): corner touching an adjacent solid block receives darker shade (Milestone 4)
    {
        ViewChunk testAo;
        testAo.sent.alloc();
        // Create an inside corner: floor block at (5, 5, 5), wall block at (6, 6, 5)
        testAo.sent.set(5, 5, 5, wire::BlockId::Concrete);
        testAo.sent.set(6, 6, 5, wire::BlockId::Concrete);
        meshview::meshChunk(testAo, g_meshStats);

        bool foundAoDarkening = false;
        // Search vertices of floor block (5, 5, 5) on top face (+Y)
        for (const auto& v : testAo.mesh) {
            if (std::fabs(v.ny - 1.0f) < 0.2f && v.y > (5.0f * VOXEL_SIZE)) {
                const float unoccludedR = meshview::blockColor(wire::BlockId::Concrete).x * 1.0f; // faceShade[2] = 1.0
                if (v.r < unoccludedR * 0.95f) {
                    foundAoDarkening = true;
                    break;
                }
            }
        }
        rep.cornerAoOk = foundAoDarkening;
    }

    // 5. Normal smoothing: an isolated block has corner normals bent outward along corners
    {
        ViewChunk testNorm;
        testNorm.sent.alloc();
        testNorm.sent.set(5, 5, 5, wire::BlockId::Concrete);
        meshview::meshChunk(testNorm, g_meshStats);

        bool foundSmoothedNormal = false;
        for (const auto& v : testNorm.mesh) {
            if (v.ny > 0.4f && (std::fabs(v.nx) > 0.1f || std::fabs(v.nz) > 0.1f)) {
                foundSmoothedNormal = true;
                break;
            }
        }
        rep.normalSmoothingOk = foundSmoothedNormal;
    }

    // 6. Cubic grid preservation (RULES.md): VOXEL_SIZE is strictly 0.001
    {
        rep.cubicPreservedOk = (std::fabs(VOXEL_SIZE - 0.001f) < 1e-7f);
    }

    // 7. Mesh workers: meshing every chunk through the pool must produce the
    // same vertices as meshing them one by one, byte for byte, and the same
    // skirt telemetry. The pool is forced to 3 helpers so the threaded path
    // runs even on a machine where the live pool would be smaller, and the
    // batch runs twice so a reused pool is covered, not just a fresh one.
    {
        const int n = sim::World::chunkCount();
        std::vector<ViewChunk> serial(n), pooled(n);
        for (int cy = 0; cy < CHUNKS_Y; ++cy)
            for (int cz = 0; cz < CHUNKS_Z; ++cz)
                for (int cx = 0; cx < CHUNKS_X; ++cx) {
                    const int i = sim::World::chunkIndex(cx, cy, cz);
                    for (ViewChunk* c : {&serial[i], &pooled[i]}) {
                        c->cx = cx; c->cy = cy; c->cz = cz;
                        sendChunkSnapshot(world, *c);
                    }
                }
        meshview::Stats serialStats, pooledStats;
        for (auto& c : serial) meshview::meshChunk(c, serialStats);

        std::vector<ViewChunk*> batch;
        for (auto& c : pooled) batch.push_back(&c);
        meshview::Workers pool(3);
        bool same = true;
        for (int pass = 0; pass < 2 && same; ++pass) {
            pooledStats = meshview::Stats{};
            pool.meshAll(batch, pooledStats);
            for (int i = 0; i < n && same; ++i) {
                const auto& a = serial[i].mesh;
                const auto& b = pooled[i].mesh;
                same = a.size() == b.size() &&
                       (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0);
            }
            same = same && pooledStats.skirtAccessViolations == serialStats.skirtAccessViolations;
        }
        rep.meshWorkersEquivOk = same && pool.helpers() == 3;
        rep.meshWorkersHelpers = static_cast<int>(pool.helpers());
        rep.meshWorkersChunks = n;
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
    if (!g_world) return;
    ballistics::stepProjectiles(g_ballistics, *g_world, g_ballisticsHooks, dt);
    // Remesh deferred to drawFrame after GPU fence (see flushDirtyMesh).
}

static void recreateSwapchain() {
    vkDeviceWaitIdle(g_device);
    destroySwapchainObjects();
    createSwapchain();
    createDepthResources();
    createFramebuffers();
    g_post.createTargets(g_extent, g_vis.renderScale, g_swapViews);
    if (g_uiRenderPass) createUiFramebuffers();
}

// ---- frame capture (view-only test harness) ----
// --capture <dir> runs the smoke, stops ticking, then renders a fixed list of
// camera shots and writes each finished frame to <dir>/<shot>.ppm. Golden
// images of those shots are how visual changes are checked: a change that is
// meant to be invisible must reproduce them, and one that is meant to be
// visible can be looked at. The copy is recorded into the same command buffer
// that draws the frame, while the image is still ours, i.e. before present.
static std::string g_captureDir;
static bool g_captureActive = false;    // shots are rendering; the sim is no longer ticking
static bool g_captureThisFrame = false; // record a copy of this frame's swapchain image
static VkBuffer g_captureBuf = VK_NULL_HANDLE;
static VkDeviceMemory g_captureMem = VK_NULL_HANDLE;
static void* g_captureMapped = nullptr;
static VkDeviceSize g_captureSize = 0;

static bool ensureCaptureBuffer() {
    const VkDeviceSize need = VkDeviceSize(g_extent.width) * g_extent.height * 4;
    if (g_captureBuf && g_captureSize >= need) return true;
    if (g_captureBuf) {
        vkUnmapMemory(g_device, g_captureMem);
        vkDestroyBuffer(g_device, g_captureBuf, nullptr);
        vkFreeMemory(g_device, g_captureMem, nullptr);
        g_captureBuf = VK_NULL_HANDLE;
        g_captureMem = VK_NULL_HANDLE;
    }
    createBuffer(need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 g_captureBuf, g_captureMem);
    vkMapMemory(g_device, g_captureMem, 0, need, 0, &g_captureMapped);
    g_captureSize = need;
    return g_captureMapped != nullptr;
}

// Both render passes leave the image in PRESENT_SRC_KHR; borrow it for a copy
// and hand it back in the same layout, so presentation is unaffected.
static void recordCaptureCopy(VkCommandBuffer cmd, uint32_t imageIndex) {
    VkImageMemoryBarrier toSrc{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toSrc.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image = g_swapImages[imageIndex];
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {g_extent.width, g_extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, g_swapImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           g_captureBuf, 1, &region);

    VkImageMemoryBarrier back = toSrc;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = 0;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkBufferMemoryBarrier toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = g_captureBuf;
    toHost.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                         nullptr, 1, &toHost, 1, &back);
}

// Write the captured frame as binary PPM (RGB). Callers wait for the GPU first.
static bool writeCapturePpm(const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const uint32_t w = g_extent.width, h = g_extent.height;
    f << "P6\n" << w << " " << h << "\n255\n";
    const bool bgr = g_swapFormat == VK_FORMAT_B8G8R8A8_UNORM || g_swapFormat == VK_FORMAT_B8G8R8A8_SRGB;
    const auto* px = static_cast<const uint8_t*>(g_captureMapped);
    std::vector<uint8_t> row(size_t(w) * 3);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* p = px + (size_t(y) * w + x) * 4;
            row[x * 3 + 0] = bgr ? p[2] : p[0];
            row[x * 3 + 1] = p[1];
            row[x * 3 + 2] = bgr ? p[0] : p[2];
        }
        f.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }
    return static_cast<bool>(f);
}

// ---- in-engine menu (Dear ImGui) ----
// Drawn in its own pass on the swapchain image after the overlay pass, with
// its own pipeline and no world state bound (RULES.md, "Menus, HUD and other
// view overlays"). It is the pause menu: Esc opens it, and it holds the
// visuals panel built from the parameter registry.
static bool g_uiDrawPending = false; // this frame built draw data to record
static int g_uiFrames = 0;           // frames that drew the menu (telemetry)
static std::string g_uiStatus;       // last save/load result, shown in the menu

static void createUiFramebuffers() {
    for (VkFramebuffer fb : g_uiFramebuffers) vkDestroyFramebuffer(g_device, fb, nullptr);
    g_uiFramebuffers.assign(g_swapViews.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < g_swapViews.size(); ++i) {
        VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        ci.renderPass = g_uiRenderPass;
        ci.attachmentCount = 1;
        ci.pAttachments = &g_swapViews[i];
        ci.width = g_extent.width;
        ci.height = g_extent.height;
        ci.layers = 1;
        if (vkCreateFramebuffer(g_device, &ci, nullptr, &g_uiFramebuffers[i]) != VK_SUCCESS)
            fail("ui framebuffer failed");
    }
}

static void initUi() {
    VkAttachmentDescription color{};
    color.format = g_swapFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rci.attachmentCount = 1;
    rci.pAttachments = &color;
    rci.subpassCount = 1;
    rci.pSubpasses = &sub;
    rci.dependencyCount = 1;
    rci.pDependencies = &dep;
    if (vkCreateRenderPass(g_device, &rci, nullptr, &g_uiRenderPass) != VK_SUCCESS)
        fail("ui render pass failed");
    createUiFramebuffers();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window layout is not persisted; settings are
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_2;
    ii.Instance = g_instance;
    ii.PhysicalDevice = g_phys;
    ii.Device = g_device;
    ii.QueueFamily = static_cast<uint32_t>(g_qidx.graphics);
    ii.Queue = g_graphicsQueue;
    ii.DescriptorPoolSize = 64; // the backend makes its own pool
    ii.MinImageCount = 2;
    ii.ImageCount = static_cast<uint32_t>(std::max<size_t>(2, g_swapImages.size()));
    ii.PipelineInfoMain.RenderPass = g_uiRenderPass;
    ii.PipelineInfoMain.Subpass = 0;
    ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (!ImGui_ImplVulkan_Init(&ii)) fail("ImGui Vulkan backend init failed");
    g_uiReady = true;
}

static void shutdownUi() {
    if (!g_device) return;
    if (g_uiReady) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_uiReady = false;
    }
    for (VkFramebuffer fb : g_uiFramebuffers) vkDestroyFramebuffer(g_device, fb, nullptr);
    g_uiFramebuffers.clear();
    if (g_uiRenderPass) vkDestroyRenderPass(g_device, g_uiRenderPass, nullptr);
    g_uiRenderPass = VK_NULL_HANDLE;
}

// One widget per registered parameter, grouped in declaration order. A new
// effect appears here by registering its parameters; nothing below changes.
static void drawVisualsPanel() {
    const std::string settingsPath = g_exeDir + "\\visual_settings.json";
    ImGui::TextUnformatted("Presets");
    for (const auto& pr : g_visReg.presets()) {
        if (ImGui::Button(pr.name)) g_visReg.applyPreset(pr.name);
        ImGui::SameLine();
    }
    if (ImGui::Button("Defaults")) g_visReg.resetDefaults();
    if (ImGui::Button("Save")) {
        std::ofstream f(settingsPath, std::ios::binary);
        f << g_visReg.toJson();
        g_uiStatus = f ? "Saved " + settingsPath : "Could not write " + settingsPath;
    }
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        bool readable = false;
        const std::string text = jsonReadText(settingsPath, &readable);
        g_uiStatus = readable ? "Loaded " + std::to_string(g_visReg.fromJson(text)) + " settings"
                              : "No saved settings yet";
    }
    if (!g_uiStatus.empty()) ImGui::TextDisabled("%s", g_uiStatus.c_str());

    std::vector<std::string> groups;
    for (const auto& p : g_visReg.params())
        if (std::find(groups.begin(), groups.end(), p.group) == groups.end()) groups.push_back(p.group);
    for (const std::string& g : groups) {
        if (!ImGui::CollapsingHeader(g.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
        for (auto& p : g_visReg.params()) {
            if (g != p.group) continue;
            bool changed = false;
            switch (p.type) {
            case vis::ParamType::Float:
                changed = ImGui::SliderFloat(p.label, p.f, p.minV, p.maxV, "%.2f");
                break;
            case vis::ParamType::Int:
                changed = ImGui::SliderInt(p.label, p.i, static_cast<int>(p.minV), static_cast<int>(p.maxV));
                break;
            case vis::ParamType::Bool:
                changed = ImGui::Checkbox(p.label, p.b);
                break;
            }
            if (p.help && *p.help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.help);
            if (changed) {
                p.set(p.get()); // re-clamp (Ctrl+click lets a slider be typed past its range)
                g_visReg.touch();
            }
        }
    }
}

// Build this frame's menu, if it is showing. Recorded later in the UI pass.
static void buildUiFrame() {
    g_uiDrawPending = false;
    if (!g_uiReady || !(g_paused || g_menuForced)) return;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    const ImVec2 center(0.5f * static_cast<float>(g_extent.width), 0.5f * static_cast<float>(g_extent.height));
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Always);
    ImGui::Begin("Paused", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::Button("Resume")) setPaused(false);
    ImGui::SameLine();
    if (ImGui::Button("Quit")) {
        g_running = false;
        PostQuitMessage(0);
    }
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Visuals", ImGuiTreeNodeFlags_DefaultOpen)) drawVisualsPanel();
    ImGui::End();
    ImGui::Render();
    g_uiDrawPending = true;
}

static void recordUiPass(VkCommandBuffer cmd, uint32_t imageIndex) {
    if (!g_uiDrawPending) return;
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = g_uiRenderPass;
    rp.framebuffer = g_uiFramebuffers[imageIndex];
    rp.renderArea.extent = g_extent;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRenderPass(cmd);
    ++g_uiFrames;
}

static void recordCommandBuffer(uint32_t imageIndex, uint32_t frameIndex) {
    VkCommandBuffer cmd = g_cmdBuffers[frameIndex];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &bi);

    recordOccupancyUpload(cmd, frameIndex);

    VkClearValue clears[2]{};
    clears[0].color = {{0.03f, 0.035f, 0.05f, 1.0f}}; // night sky
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = g_renderPass;
    rp.framebuffer = g_post.worldFb;
    rp.renderArea.extent = g_post.extent;
    rp.clearValueCount = 2;
    rp.pClearValues = clears;

    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline);

    // Full-window viewport for the overlay pass; the world pass uses the
    // offscreen target's own size (render scale) below.
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(g_extent.width);
    viewport.height = static_cast<float>(g_extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = g_extent;
    // The world pass draws into the offscreen target at the render scale; the
    // overlay pass below uses the full-window viewport/scissor above.
    {
        VkViewport wv = viewport;
        wv.width = static_cast<float>(g_post.extent.width);
        wv.height = static_cast<float>(g_post.extent.height);
        VkRect2D ws{{0, 0}, g_post.extent};
        vkCmdSetViewport(cmd, 0, 1, &wv);
        vkCmdSetScissor(cmd, 0, 1, &ws);
    }

    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &g_vertexBuffer, &off);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipelineLayout, 0, 1,
                            &g_descSets[frameIndex], 0, nullptr);

    // Not-seen rendering: frustum + behind-camera cull per chunk
    g_drawnChunks = 0;
    g_culledChunks = 0;
    if (g_views && g_vertexBuffer != VK_NULL_HANDLE) {
        Frustum fr = frustumFromVP(g_viewProjCull);
        Vec3 eye = g_camPos;
        Vec3 forward = cameraForward();
        for (auto& c : *g_views) {
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
    } else if (g_liveVertexCount > 0) {
        vkCmdDraw(cmd, g_liveVertexCount, 1, 0, 0);
        g_drawnChunks = 1;
    }

    // World item pickups (mat 8) live in the main pass so they depth-test against
    // the map and cast/receive light like the world does. Only the player's own
    // lattice is overlay-only.
    // Light fixtures (environment layer): same pass, same pipeline.
    if (g_fixtureVB != VK_NULL_HANDLE && g_fixtureVertexCount > 0) {
        VkDeviceSize fOff = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &g_fixtureVB, &fOff);
        vkCmdDraw(cmd, g_fixtureVertexCount, 1, 0, 0);
    }
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

    // Post chain: the world image onto the swapchain (src/post_fx.hpp).
    {
        postfx::PostParams pp;
        pp.posterize = static_cast<float>(g_vis.posterizeLevels);
        pp.dither = g_vis.dither;
        pp.crush = g_vis.crush;
        pp.time = static_cast<float>(g_tick) * static_cast<float>(TICK_DT);
        g_post.record(cmd, imageIndex, g_extent, g_vis.upscaleNearest, pp);
    }

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
        VkClearValue ovClears[2]{};
        ovClears[1].depthStencil = {1.0f, 0}; // colour is LOAD; only depth clears
        ovp.clearValueCount = 2;
        ovp.pClearValues = ovClears;
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

    recordUiPass(cmd, imageIndex);

    if (g_captureThisFrame && g_captureBuf) recordCaptureCopy(cmd, imageIndex);

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

static SubmersionInfo sampleCharacterWater(const sim::World& world, int gx, int gy, int gz) {
    SubmersionInfo info;
    info.total = kCharUnitCount;
    for (int i = 0; i < kCharUnitCount; ++i) {
        int x = gx + kCharUnits[i][0];
        int y = gy + kCharUnits[i][1];
        int z = gz + kCharUnits[i][2];
        Block b = getWorldBlock(world, x, y, z);
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
    if (!g_world) return false;
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
                if (isSolidBlock(getWorldBlock(*g_world, x, y, z))) return true;
            }
    return false;
}

static void spawnPlayerOnMap(const sim::World& world) {
    if (g_mapSpawn.present) {
        // The map says where the player stands; respawn uses the same cell.
        g_spawnCellX = g_mapSpawn.x;
        g_spawnCellY = g_mapSpawn.y;
        g_spawnCellZ = g_mapSpawn.z;
        g_player.px = (g_mapSpawn.x + 0.5f) * VOXEL_SIZE;
        g_player.py = g_mapSpawn.y * VOXEL_SIZE + 0.0002f;
        g_player.pz = (g_mapSpawn.z + 0.5f) * VOXEL_SIZE;
        g_player.vx = g_player.vy = g_player.vz = 0.0f;
        g_player.onGround = true;
        g_player.lean = 0.0f;
        g_player.leanTarget = 0.0f;
        g_yaw = g_mapSpawn.yaw;
        g_pitch = g_mapSpawn.pitch;
        g_camPos = Vec3(g_player.px, g_player.py + g_player.eyeHeight, g_player.pz);
        return;
    }
    (void)world;
    fail("the map has no player_spawn");
}

// Water current + weight sampling from physics feet (not free-fly camera).
static void updatePlayerCurrentAndWeight(float dt) {
    if (!g_world) return;
    g_playerGX = static_cast<int>(std::floor(g_player.px / VOXEL_SIZE)) - 2;
    g_playerGY = static_cast<int>(std::floor(g_player.py / VOXEL_SIZE));
    g_playerGZ = static_cast<int>(std::floor(g_player.pz / VOXEL_SIZE)) - 0;
    auto info = sampleCharacterWater(*g_world, g_playerGX, g_playerGY, g_playerGZ);
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
    Vec3 fwd = flatForward();
    const float leanLat = g_player.lean * 0.0032f;   // lateral peek
    const float leanDrop = std::fabs(g_player.lean) * 0.0009f;
    // The body is the authority. Everything past this line is presentation:
    // lean, the movement system's stance eye height, and the offset it produced
    // this tick. None of it is read back by the simulation.
    const float eye = movement::STANCE_EYE_HEIGHT[static_cast<int>(g_move.stance)];
    const float lat = leanLat + g_camOffset.lateralLean;
    g_camPos.x = g_player.px + right.x * lat + fwd.x * g_camOffset.forwardLean;
    g_camPos.y = g_player.py + eye + g_camOffset.eyeLift - leanDrop;
    g_camPos.z = g_player.pz + right.z * lat + fwd.z * g_camOffset.forwardLean;
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
// Reads the tick's intent, not the keyboard: the sim must behave identically
// whether intent came from a window, a replay script, or a remote client.
static void updatePlayerPhysics(float dt, const SimInput& in) {
    if (!g_world) return;

    // --- lean: intent axis, clamped here so an out-of-range request is ignored ---
    g_player.leanTarget = std::max(-1.0f, std::min(1.0f, in.leanAxis));
    const float leanRate = 8.0f;
    g_player.lean += (g_player.leanTarget - g_player.lean) * (1.0f - std::exp(-leanRate * dt));

    // The movement system advances stance, gait, slide, wallrun, dash and
    // stamina, and it decides how fast the body is allowed to go. It is handed
    // the authoritative body and the authoritative grid, and it returns an
    // intent plus a presentation offset. It never touches the camera: the roll
    // and lean below are applied by syncCameraToPlayer() after this returns.
    g_move.px = g_player.px;  g_move.py = g_player.py;  g_move.pz = g_player.pz;
    g_move.vx = g_player.vx;  g_move.vy = g_player.vy;  g_move.vz = g_player.vz;
    g_move.onGround = g_player.onGround;
    const movement::MoveResult mv = movement::update(dt, in, *g_world, g_move, g_yaw,
                                                       g_moveSpeed, g_player.jumpSpeed);
    g_camOffset = mv.cam;

    // Stance is a real body height, so the hitbox and the eye follow it. This is
    // a body change, not an offset: a prone body is short and can fit somewhere
    // a standing one cannot.
    g_player.height = movement::STANCE_COLLISION_HEIGHT[static_cast<int>(g_move.stance)];
    g_player.radius = std::max(1.0f * kVoxelSize, g_player.height * 0.12f);
    g_player.eyeHeight = movement::STANCE_EYE_HEIGHT[static_cast<int>(g_move.stance)];

    // A wallrun turns the *body*, so the authoritative facing moves with it.
    if (mv.intent.facingYaw != 0.0f) g_yaw = mv.intent.facingYaw;

    // --- desired horizontal velocity (analog wish axes) ---
    float speed = g_moveSpeed * mv.intent.speedScale;
    // Lean slows strafe slightly (shoulder into cover).
    speed *= (1.0f - 0.18f * std::fabs(g_player.lean));

    Vec3 wish(0, 0, 0);
    Vec3 f = flatForward();
    Vec3 r = flatRight();
    if (!mv.intent.overrideHorizontal) {
        if (std::fabs(in.moveForward) > 1e-6f) wish = wish + f * in.moveForward;
        if (std::fabs(in.moveRight) > 1e-6f) wish = wish + r * in.moveRight;
        if (wish.length() > 1e-5f) wish = wish.normalized() * speed;
    }

    // Accelerate / friction on horizontal plane.
    const float accel = g_player.onGround ? 18.0f : 4.0f;
    const float friction = g_player.onGround ? 12.0f : 1.5f;
    if (mv.intent.overrideHorizontal) {
        // Slide, dash and wallrun drive the body directly. The integrator below
        // still collides it, so a dash into a wall stops like anything else.
        g_player.vx = mv.intent.vx;
        g_player.vz = mv.intent.vz;
    } else if (wish.length() > 1e-6f) {
        g_player.vx += (wish.x - g_player.vx) * std::min(1.0f, accel * dt);
        g_player.vz += (wish.z - g_player.vz) * std::min(1.0f, accel * dt);
    } else {
        float damp = std::exp(-friction * dt);
        g_player.vx *= damp;
        g_player.vz *= damp;
        if (std::fabs(g_player.vx) < 1e-5f) g_player.vx = 0.0f;
        if (std::fabs(g_player.vz) < 1e-5f) g_player.vz = 0.0f;
    }

    // Jump (Space) — no free-fly up/down. The impulse comes from the movement
    // system, which owns stance scaling; in.jump is the edge, so there is no
    // cross-frame latch to miss a tap.
    if (mv.intent.jumpImpulse > 0.0f) {
        g_player.vy = mv.intent.jumpImpulse;
        g_player.onGround = false;
    }
    g_wantJump = false;

    // Gravity (same world scale as projectiles), scaled down while wallrunning.
    g_player.vy -= kWorldGravity * mv.intent.gravityScale * dt;
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
            // it like any other energy, and so the dash invulnerability window
            // withholds a landing exactly as it withholds a bullet.
            if (dashInvulnerable()) {
                ++g_dashInvulnBlocks;
            } else {
                health::applyDamage(g_health, dmg, ArmorZone::Legs,
                                    equippedArmorPoints(ArmorZone::Legs));
                ++g_fallDamageEvents;
            }
        }
        g_fallPeakSpeed = 0.0f;
    }
    g_wasOnGround = g_player.onGround;

    syncCameraToPlayer();
}

// Water weight + physics body + locked camera. Renamed from the misleading
// "updateCamera": the camera is a view output, but this advances the body and
// water buoyancy, which are simulation.
static void updatePlayerAndEye(float dt, const SimInput& in) {
    updatePlayerCurrentAndWeight(dt);
    if (!g_smoke) {
        updatePlayerPhysics(dt, in);
    } else {
        // Headless smoke: keep body planted, only yaw/pitch scripted; still lock eye.
        syncCameraToPlayer();
    }
}

// The eye as rendered, between the previous tick's eye and this tick's by how
// far wall time has run into the next tick. The simulation runs at a fixed 120
// Hz and the display at whatever it manages, so drawing the latest tick's eye
// as-is judders whenever the two disagree. Interpolating costs under one tick
// of positional lag; orientation is not interpolated, so aim stays immediate.
//
// View-only: g_camPos (the sim's eye, which is also the fire origin) is never
// written here, so this cannot change what a shot hits.
static constexpr float kEyeSnapDist = 0.02f; // world units per tick; beyond it is a teleport

static Vec3 lerpEye(const Vec3& a, const Vec3& b, float t) {
    const Vec3 d = b - a;
    if (d.dot(d) > kEyeSnapDist * kEyeSnapDist) return b; // respawn/teleport: snap, never sweep
    return a + d * t;
}

static Vec3 renderEye() {
    if (g_paused || g_captureActive) return g_camPos;
    const float alpha = static_cast<float>(std::clamp(g_tickAccum / TICK_DT, 0.0, 1.0));
    return lerpEye(g_camPosPrevTick, g_camPos, alpha);
}

static void updateUBO(uint32_t frameIndex, float timeSec) {
    const Vec3 eye = renderEye();
    Vec3 center = eye + cameraForward();
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
    ubo.damageFlash = g_health.sinceLastHit < health::kDamageFlashSeconds
                          ? std::clamp(1.0f - (g_health.sinceLastHit / health::kDamageFlashSeconds), 0.0f, 1.0f)
                          : 0.0f;
    const float hpFrac = g_health.healthFraction();
    ubo.healthTint = (!g_health.dead && hpFrac < 0.35f)
                         ? std::clamp((0.35f - hpFrac) / 0.35f, 0.0f, 1.0f)
                         : (g_health.dead ? 1.0f : 0.0f);

    // Lights from the map (environment layer) into this frame's light buffer.
    const int lightCount = std::min(static_cast<int>(g_bulbs.size()), kMaxLights);
    if (g_lightMapped[frameIndex]) {
        float* dst = static_cast<float*>(g_lightMapped[frameIndex]);
        for (int i = 0; i < lightCount; ++i) {
            const BulbLight& l = g_bulbs[i];
            const float v[8] = {l.pos.x, l.pos.y, l.pos.z, l.intensity, l.color.x, l.color.y, l.color.z, l.radius};
            std::memcpy(dst + i * 8, v, sizeof(v));
        }
    }
    ubo.lightInfo[0] = static_cast<float>(lightCount);
    ubo.occDims[0] = static_cast<float>(WORLD_W);
    ubo.occDims[1] = static_cast<float>(WORLD_H);
    ubo.occDims[2] = static_cast<float>(WORLD_D);
    ubo.occDims[3] = (g_vis.shadows && g_occImageReady) ? 1.0f : 0.0f;
    ubo.shadowParams[0] = static_cast<float>(g_vis.shadowSteps);
    ubo.shadowParams[1] = 1.0f; // cells per budget unit (exact DDA visits every cell)

    ubo.fisheyeScale = g_vis.fisheye;
    ubo.banding = g_vis.banding;
    std::memcpy(ubo.texParams, g_texTable.params, sizeof(ubo.texParams));
    ubo.texGlobal[0] = (g_vis.textures && g_texLayerCount > 0) ? 1.0f : 0.0f;
    ubo.texGlobal[1] = g_vis.textureStrength;
    ubo.texGlobal[2] = g_vis.textureScale;
    std::memcpy(g_uboMapped[frameIndex], &ubo, sizeof(ubo));
}


static void flushDirtyMesh() {
    if (!g_meshDirty || !g_world || !g_views) return;
    // Under heavy fire, remeshing every frame dominates CPU. Coalesce dirty updates.
    // The gap is measured in simulation ticks, not frames, so it is reproducible.
    const int minGap = g_stress ? 3 : 1;
    if (g_ticksSinceRemesh < minGap) {
        ++g_remeshSkipCount;
        return;
    }
    g_meshDirty = false;
    g_ticksSinceRemesh = 0;

    LARGE_INTEGER t0{}, t1{}, freq{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    // Push fresh snapshots for stale chunks, then mesh each one purely from the
    // snapshot it was sent. This is the whole sim->view contract.
    std::vector<ViewChunk*> touched;
    {
        ScopedSection s(SEC_MESH_SNAPSHOT);
        touched = sendStaleSnapshots(*g_world, *g_views);
    }
    {
        ScopedSection s(SEC_MESH_BUILD);
        buildChunkMeshes(touched);
    }
    ScopedSection copySection(SEC_MESH_COPY);
    g_meshTouchedSum += touched.size();
    g_meshTouchedMax = std::max(g_meshTouchedMax, static_cast<int>(touched.size()));

    // Copy each touched chunk into its slot. Slots are stable, so every other
    // chunk's offset stays valid and is not copied.
    //
    // A chunk that outgrew its slot moves to fresh space past the last slot
    // instead of forcing a full repack: chunks are drawn one by one from their
    // own firstVertex, so nothing else has to move. Only that chunk is copied,
    // and its old slot is left dead until the next repack reclaims it. Only
    // when the free tail runs out does everything repack, which compacts the
    // dead space and grows the buffer. That full copy is the one expensive
    // path (tens of MB), so it should be rare.
    const uint32_t capacityVerts = static_cast<uint32_t>(g_vertexCapacity / sizeof(Vertex));
    bool repack = (g_vertexBuffer == VK_NULL_HANDLE);
    for (ViewChunk* c : touched) {
        if (repack) break;
        if (c->vertexCount > c->slotCapacity) {
            const uint32_t want = slotWant(c->vertexCount);
            if (g_slotCursor + want > capacityVerts) { repack = true; break; }
            c->firstVertex = g_slotCursor;
            c->slotCapacity = want;
            g_slotCursor += want;
            ++g_meshRelocateCount;
        }
        if (!uploadChunkRange(*c)) repack = true;
    }
    if (repack) {
        repackChunkSlots(*g_views);
        if (!ensureVertexCapacity(g_slotCursor)) return;
        for (const auto& c : *g_views) uploadChunkRange(c);
        ++g_meshRepackCount;
    }
    g_vertexCount = g_slotCursor;
    g_liveVertexCount = 0;
    for (const auto& c : *g_views) g_liveVertexCount += c.vertexCount;

    QueryPerformanceCounter(&t1);
    const double us = (double(t1.QuadPart - t0.QuadPart) * 1e6) / double(freq.QuadPart);
    g_meshUploadUsSum += us;
    if (us > g_meshUploadUsMax) g_meshUploadUsMax = us;
    ++g_meshUploadSamples;
}

static void drawFrame(float timeSec) {
    // Presentation only. This must never advance simulation state: physics,
    // destruction and damage all run from simulateOnce(). The render path
    // reads simulation output, never the other way round.

    // Wait for this frame slot's prior GPU work before touching shared mesh buffers.
    {
        ScopedSection s(SEC_WAIT);
        vkWaitForFences(g_device, 1, &g_inFlight[g_frame], VK_TRUE, UINT64_MAX);
    }

    // The render scale changed (visuals menu): rebuild the offscreen target.
    if (std::fabs(g_vis.renderScale - g_post.scale) > 1e-4f) {
        vkDeviceWaitIdle(g_device);
        g_post.createTargets(g_extent, g_vis.renderScale, g_swapViews);
    }

    // Safe to rebuild world VB now (no device-wide idle).
    {
        ScopedSection s(SEC_MESH);
        flushDirtyMesh();
    }

    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(g_device, g_swapchain, UINT64_MAX,
                                         g_imageAvailable[g_frame], VK_NULL_HANDLE, &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) fail("acquire failed");

    vkResetFences(g_device, 1, &g_inFlight[g_frame]);
    {
        ScopedSection s(SEC_UBO);
        updateUBO(static_cast<uint32_t>(g_frame), timeSec);
    }
    {
        ScopedSection s(SEC_SKY);
        updateMoonSkyTile();
    }
    {
        ScopedSection s(SEC_DEBRIS_MESH);
        updateDebrisMesh();
    }
    // Pickup hover drives a brightness tint, so the mesh follows the crosshair.
    const int prevHover = g_pickupHover;
    g_pickupHover = g_inventoryOpen ? -1 : pickupUnderCrosshair();
    if (prevHover != g_pickupHover) g_pickupMeshDirty = true;
    {
        ScopedSection s(SEC_PICKUP);
        updatePickupMesh();
    }
    // Resolve g_invHover first (it needs this frame's camera basis), then act on
    // it, so a click always lands on the cell that was under the cursor.
    {
        ScopedSection s(SEC_INVENTORY);
        updateInventoryMesh();
        drainInventoryInput();
    }
    buildUiFrame();
    {
        ScopedSection s(SEC_RECORD);
        recordCommandBuffer(imageIndex, static_cast<uint32_t>(g_frame));
    }

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

// Fixed camera shots for --capture, in cell coordinates on the current map.
// The sim has stopped ticking when these run; placing the camera here is a test
// harness posing a view, and nothing is simulated afterwards.
// Extras stage view content a shot needs so every render class is on camera:
// the inventory overlay, and a burst of debris chips plus a muzzle flash.
enum CaptureExtras : int { kShotPlain = 0, kShotInventory = 1, kShotEffects = 2, kShotDebris = 4, kShotMenu = 8 };
struct CaptureShot {
    const char* name;
    float cx, cy, cz; // eye position, in cells
    float yaw, pitch;
    int extras;
    const char* preset = nullptr; // visuals preset for this shot (defaults restored after)
};
static const CaptureShot kCaptureShots[] = {
    {"bay_inward",      96.0f, 19.5f, 132.0f, 0.00f, -0.08f, kShotPlain},
    {"interior_crates", 150.0f, 18.0f, 110.0f, -0.90f, -0.20f, kShotPlain},
    {"back_corner",     20.0f, 30.0f, 20.0f, 2.35f, -0.35f, kShotPlain},
    {"exterior_river",  186.0f, 46.0f, 158.0f, -0.85f, -0.42f, kShotPlain},
    {"inventory_open",  96.0f, 19.5f, 132.0f, 0.00f, -0.08f, kShotInventory},
    {"effects_crate",   34.0f, 13.0f, 58.0f, 0.00f, -0.30f, kShotEffects},
    {"debris_crate",    34.0f, 13.0f, 58.0f, 0.00f, -0.30f, kShotDebris},
    {"pause_menu",      96.0f, 19.5f, 132.0f, 0.00f, -0.08f, kShotMenu},
    {"preset_retro",    20.0f, 30.0f, 20.0f, 2.35f, -0.35f, kShotPlain, "Retro"},
    {"preset_clean",    20.0f, 30.0f, 20.0f, 2.35f, -0.35f, kShotPlain, "Clean"},
};

static int runCaptureShots(float timeSec) {
    int written = 0;
    g_captureActive = true;
    g_recoilPitch = g_recoilYaw = 0.0f;
    for (const CaptureShot& shot : kCaptureShots) {
        g_camPos = Vec3(shot.cx * VOXEL_SIZE, shot.cy * VOXEL_SIZE, shot.cz * VOXEL_SIZE);
        g_yaw = shot.yaw;
        g_pitch = shot.pitch;
        g_inventoryOpen = (shot.extras & kShotInventory) != 0;
        g_menuForced = (shot.extras & kShotMenu) != 0;
        if (shot.preset) g_visReg.applyPreset(shot.preset);
        if (shot.extras & (kShotEffects | kShotDebris)) {
            // Chips thrown off the top edge of the first crate (see
            // data/maps/warehouse_v1). The sim is not ticking, so they hold still.
            for (int x = 32; x <= 35; ++x)
                g_debris.spawnFromVoxel(x, 10, 41, MaterialId::Wood, 0.0f, 0.4f, 1.0f, 6.0f,
                                        VOXEL_SIZE, 1.0f);
            g_debris.meshDirty = true;
        }
        if (shot.extras & kShotEffects) g_muzzleFlash = 1.0f;
        // Two frames per shot: per-frame view state (sky dome around the eye,
        // pickup hover) settles on the first, the second is the one kept.
        for (int i = 0; i < 2; ++i) {
            vkDeviceWaitIdle(g_device);
            g_captureThisFrame = (i == 1) && ensureCaptureBuffer();
            drawFrame(timeSec);
        }
        vkDeviceWaitIdle(g_device);
        if (g_captureThisFrame &&
            writeCapturePpm(g_captureDir + "\\" + shot.name + ".ppm"))
            ++written;
        g_captureThisFrame = false;
        g_inventoryOpen = false;
        g_muzzleFlash = 0.0f;
        g_menuForced = false;
        if (shot.preset) g_visReg.resetDefaults();
    }
    g_captureActive = false;
    return written;
}

// --export-map <path>: write the loaded world, with its spawn and pickups, as a
// run-length map document. This is how the procedural warehouse became
// data/maps/warehouse_v1.map.vox.json; the fingerprints proved the round trip.
static bool exportMapDocument(const sim::World& world, const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    char buf[256];
    f << "{\n";
    f << "  \"format_version\": " << mapvox::kMaxFormatVersion
      << ",\n  \"unit\": 1,\n  \"voxel_size\": 0.001,\n  \"mode\": \"map\",\n";
    f << "  \"dims\": [" << WORLD_W << ", " << WORLD_H << ", " << WORLD_D << "],\n";
    std::snprintf(buf, sizeof(buf),
                  "  \"player_spawn\": {\"x\": %d, \"y\": %d, \"z\": %d, \"yaw\": %.4f, \"pitch\": %.4f},\n",
                  g_spawnCellX, g_spawnCellY, g_spawnCellZ, g_yaw, g_pitch);
    f << buf;
    f << "  \"pickups\": [";
    for (size_t i = 0; i < g_pickups.size(); ++i) {
        const ItemDef* d = itemDefAt(g_itemDefs, g_pickups[i].defIndex);
        std::snprintf(buf, sizeof(buf), "%s\n    {\"item\": \"%s\", \"x\": %d, \"y\": %d, \"z\": %d, \"rot\": %d}",
                      i ? "," : "", d ? d->id.c_str() : "", g_pickups[i].cx, g_pickups[i].cy,
                      g_pickups[i].cz, g_pickups[i].rot);
        f << buf;
    }
    f << "\n  ],\n";
    f << "  \"lights\": [";
    for (size_t i = 0; i < g_mapLights.size(); ++i) {
        const auto& l = g_mapLights[i];
        std::snprintf(buf, sizeof(buf),
                      "%s\n    {\"kind\": \"%s\", \"x\": %.4f, \"y\": %.4f, \"z\": %.4f, "
                      "\"color\": [%.4f, %.4f, %.4f], \"intensity\": %.4f, \"radius\": %.4f}",
                      i ? "," : "", l.kind.c_str(), l.x, l.y, l.z, l.r, l.g, l.b, l.intensity, l.radius);
        f << buf;
    }
    f << "\n  ],\n";
    std::snprintf(buf, sizeof(buf), "  \"environment\": {\"moon_dir\": [%.4f, %.4f, %.4f]},\n",
                  g_moonDirWorld.x, g_moonDirWorld.y, g_moonDirWorld.z);
    f << buf;
    const std::string appearance = mapvox::worldAppearanceRle(world);
    if (!appearance.empty()) f << "  \"appearance\": " << appearance << ",\n";
    f << "  \"cells_rle\": " << mapvox::worldToRle(world) << "\n}\n";
    return static_cast<bool>(f);
}

static void cleanup() {
    if (g_device) vkDeviceWaitIdle(g_device);
    shutdownUi();
    if (g_fixtureVB) {
        vkUnmapMemory(g_device, g_fixtureMem);
        vkDestroyBuffer(g_device, g_fixtureVB, nullptr);
        vkFreeMemory(g_device, g_fixtureMem, nullptr);
        g_fixtureVB = VK_NULL_HANDLE;
        g_fixtureMem = VK_NULL_HANDLE;
    }
    if (g_captureBuf) {
        vkUnmapMemory(g_device, g_captureMem);
        vkDestroyBuffer(g_device, g_captureBuf, nullptr);
        vkFreeMemory(g_device, g_captureMem, nullptr);
        g_captureBuf = VK_NULL_HANDLE;
        g_captureMem = VK_NULL_HANDLE;
    }
    destroySwapchainObjects();
    if (g_pipeline) vkDestroyPipeline(g_device, g_pipeline, nullptr);
    if (g_overlayPipeline) vkDestroyPipeline(g_device, g_overlayPipeline, nullptr);
    if (g_pipelineLayout) vkDestroyPipelineLayout(g_device, g_pipelineLayout, nullptr);
    g_post.destroy(); // owns the world render pass (g_renderPass)
    g_renderPass = VK_NULL_HANDLE;
    if (g_overlayRenderPass) vkDestroyRenderPass(g_device, g_overlayRenderPass, nullptr);
    if (g_descPool) vkDestroyDescriptorPool(g_device, g_descPool, nullptr);
    if (g_texSampler) vkDestroySampler(g_device, g_texSampler, nullptr);
    if (g_occSampler) vkDestroySampler(g_device, g_occSampler, nullptr);
    if (g_occView) vkDestroyImageView(g_device, g_occView, nullptr);
    if (g_occImage) vkDestroyImage(g_device, g_occImage, nullptr);
    if (g_occMem) vkFreeMemory(g_device, g_occMem, nullptr);
    for (int i = 0; i < MAX_FRAMES; ++i) {
        if (g_occStage[i]) vkDestroyBuffer(g_device, g_occStage[i], nullptr);
        if (g_occStageMem[i]) vkFreeMemory(g_device, g_occStageMem[i], nullptr);
        if (g_lightBuf[i]) vkDestroyBuffer(g_device, g_lightBuf[i], nullptr);
        if (g_lightMem[i]) vkFreeMemory(g_device, g_lightMem[i], nullptr);
    }
    if (g_texView) vkDestroyImageView(g_device, g_texView, nullptr);
    if (g_texImage) vkDestroyImage(g_device, g_texImage, nullptr);
    if (g_texMem) vkFreeMemory(g_device, g_texMem, nullptr);
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

// ---- one fixed simulation tick ----
// dt is always TICK_DT. It stays a parameter so this remains a callable unit
// (replay, a headless host, or a later remote tick loop) rather than something
// welded to the frame loop. World state after tick N must depend only on
// g_tick and on the input that arrived by then — never on how long the frame
// took to draw.
// Scripted player intent for the functional smoke/stress runs.
//
// This deliberately produces a SimInput rather than poking g_firePressed /
// g_activeCaliberIndex directly. A scripted run is just another *client* of the
// simulation, so exercising the same seam a real view uses keeps the headless
// harness honest: if the intent path breaks, the smoke test breaks with it.
//
// Scripted off g_tick, never the frame count, so the scenario is reproducible
// regardless of render speed.
static SimInput scriptedInput(float dt) {
    SimInput s;
    if (g_stress) {
        // Keep aim into bay; hammer shotgun to max debris/projectile load.
        g_pitch = -0.10f;
        g_yaw += dt * 0.05f;
        s.selectCaliber = 0; // light -> shotgun_light
        // Fire every 3 ticks once warmed - heavy enough without remesh thrash.
        if (g_tick >= 3 && (g_tick % 3) == 0) {
            g_fireCooldown = 0.0f;
            s.firePressed = true;
            s.fireHeld = true;
            ++g_stressFireCount;
        }
    } else {
        g_yaw += dt * 0.20f;
        if (g_tick < 20) {
            g_pitch = -0.12f;
            if (g_tick == 4) {
                s.selectCaliber = 1; // medium ballistic
                g_fireCooldown = 0.0f;
                s.firePressed = true;
            }
            if (g_tick == 8) {
                s.selectCaliber = 0; // shotgun light
                g_fireCooldown = 0.0f;
                s.firePressed = true;
            }
            if (g_tick == 14) {
                s.selectCaliber = 3; // energy hitscan
                g_fireCooldown = 0.0f;
                s.firePressed = true;
            }
        } else {
            g_pitch = 0.42f; // sky tiles + moon
        }
    }
    return s;
}

// Translate this frame's raw device state into one tick of player intent.
//
// This is the whole of the view's authority over the simulation. Everything the
// window handler has already recorded in g_pendingInput (fire edges, loadout
// requests, look deltas) is merged with the currently-held keys, and nothing
// else crosses.
static SimInput buildSimInput() {
    SimInput in = g_pendingInput;
    simInputClearEdges(g_pendingInput);

    // Held movement keys -> analog wish axes.
    float fwd = 0.0f, right = 0.0f;
    if (g_keys['W'] || g_keys[VK_UP]) fwd += 1.0f;
    if (g_keys['S'] || g_keys[VK_DOWN]) fwd -= 1.0f;
    if (g_keys['D'] || g_keys[VK_RIGHT]) right += 1.0f;
    if (g_keys['A'] || g_keys[VK_LEFT]) right -= 1.0f;
    in.moveForward = std::max(-1.0f, std::min(1.0f, fwd));
    in.moveRight = std::max(-1.0f, std::min(1.0f, right));
    in.sprint = g_keys[VK_SHIFT] != 0;
    in.crouch = g_keys[VK_CONTROL] != 0;
    in.ads = in.ads || (g_keys['X'] != 0);

    // Dash and stance-cycle are edges: they are detected here, in the view, where
    // the press actually happened, and consumed by the sim on the next tick. The
    // sim never learns a key is held, only that a press arrived. Q/E stay reserved
    // for lean and E for interact, so dash is C and the stance cycle is F.
    if (g_keys['C'] && !g_dashHeldPrev) g_pendingInput.dash = true;
    g_dashHeldPrev = g_keys['C'] != 0;
    if (g_keys['F'] && !g_stanceHeldPrev) g_pendingInput.stanceCycle = true;
    g_stanceHeldPrev = g_keys['F'] != 0;

    // Held lean keys -> one axis, so a view cannot send contradictory Q and E.
    float lean = 0.0f;
    if (g_keys['E']) lean += 1.0f;
    if (g_keys['Q']) lean -= 1.0f;
    in.leanAxis = lean;
    return in;
}

// Fingerprint of the simulation's end state, for refactors that must not change
// behaviour. It folds in every cell of the authoritative world, every debris
// particle, live projectiles, the player body and health. The sim is fixed-step
// and seeded by nothing, so two runs of the same build must agree, and a
// behaviour-preserving refactor must reproduce the value exactly. Fields are
// hashed one by one (never whole structs) so padding bytes cannot leak in.
struct Fnv64 {
    uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    }
    template <class T> void add(const T& v) { bytes(&v, sizeof(v)); }
};

static uint64_t simFingerprint() {
    Fnv64 f;
    if (g_world)
        for (const auto& c : g_world->chunks)
            if (!c.voxels.empty()) f.bytes(c.voxels.data(), c.voxels.size() * sizeof(c.voxels[0]));
    for (const auto& d : g_debris.particles) {
        f.add(d.alive);
        if (!d.alive) continue;
        f.add(d.px); f.add(d.py); f.add(d.pz);
        f.add(d.vx); f.add(d.vy); f.add(d.vz);
        f.add(d.life); f.add(d.bounces);
    }
    f.add(g_debris.ricochets);
    for (const auto& p : g_projectiles) {
        f.add(p.px); f.add(p.py); f.add(p.pz);
        f.add(p.vx); f.add(p.vy); f.add(p.vz);
        f.add(p.energy); f.add(p.alive);
    }
    f.add(g_player.px); f.add(g_player.py); f.add(g_player.pz);
    f.add(g_player.vx); f.add(g_player.vy); f.add(g_player.vz);
    f.add(g_health.health); f.add(g_health.dead);
    f.add(g_voxelsDestroyed);
    return f.h;
}

static void simulateOnce(float dt, const SimInput& in) {
    // Health tick (RULES.md rule 15), ahead of input so a dead player is
    // frozen out of every control on the same frame.
    {
        ScopedSection s(SEC_HEALTH);
        if (health::updateActorHealth(g_health, dt)) {
            ++g_respawns;
            if (g_world) spawnPlayerOnMap(*g_world);
            g_fallPeakSpeed = 0.0f;
            g_wasOnGround = true;
            g_deathHandled = false;
        }
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

    // --- consume this tick's player intent ---
    // Look is applied here rather than in WndProc so aim depends on the tick
    // count, not on how many WM_MOUSEMOVE messages Windows delivered.
    {
        const float sens = g_lookSens * (in.ads ? 0.55f : 1.0f);
        g_yaw += in.lookDx * sens;
        g_pitch -= in.lookDy * sens; // drag up = look up
        const float lim = static_cast<float>(M_PI) * 0.49f;
        g_pitch = std::max(-lim, std::min(lim, g_pitch));
    }
    g_ads = in.ads;
    g_wantJump = in.jump;
    g_firePressed = in.firePressed;
    g_fireHeld = in.fireHeld;
    // Loadout requests are validated against the authoritative lists here, in
    // the simulation. A view cannot grant itself a weapon by asking for one.
    if (in.selectCaliber >= 0 && in.selectCaliber < 4) {
        g_activeCaliberIndex = in.selectCaliber;
        g_activeAmmoIndex = 0;
    }
    if (in.cycleAmmo) {
        const std::string cal = (g_activeCaliberIndex >= 0 && g_activeCaliberIndex < 4)
                                    ? kCaliberIds[g_activeCaliberIndex] : "medium";
        auto list = ammosForCaliber(g_ammoDefs, cal);
        if (!list.empty())
            g_activeAmmoIndex = (g_activeAmmoIndex + 1) % static_cast<int>(list.size());
    }
    if (in.cycleWeapon && !g_weapons.empty()) {
        g_activeWeaponIndex = (g_activeWeaponIndex + 1) % static_cast<int>(g_weapons.size());
    }
    if (in.cycleFireMode && !g_weapons.empty()) {
        WeaponDef& w = g_weapons[std::min(g_activeWeaponIndex,
                                          static_cast<int>(g_weapons.size()) - 1)];
        if (w.fireMode == "semi") w.fireMode = "auto";
        else if (w.fireMode == "auto") w.fireMode = "bolt";
        else w.fireMode = "semi";
        g_lastFireMode = w.fireMode;
    }

    if (g_fireCooldown > 0.0f) {
        g_fireCooldown -= dt;
        if (g_fireCooldown < 0.0f) g_fireCooldown = 0.0f;
    }

    updateRecoilRecovery(dt);
    // Player body physics + water weight, and lock the eye to the body.
    {
        ScopedSection s(SEC_MOVEMENT);
        updatePlayerAndEye(dt, in);
    }

    // Fire modes: semi/bolt = edge; auto = held (RMB/F) with cooldown cadence.
    {
        ScopedSection s(SEC_FIRE);
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

    g_debris.beginFrame();
    {
        ScopedSection s(SEC_PROJECTILES);
        updateProjectiles(dt);
        if (static_cast<int>(g_projectiles.size()) > g_projLivePeak)
            g_projLivePeak = static_cast<int>(g_projectiles.size());
    }
    {
        ScopedSection s(SEC_DEBRIS_SIM);
        g_debris.update(dt, kWorldGravity);
    }
    // Decay fire VFX (overlay + muzzle cubes).
    if (g_muzzleFlash > 0.0f || g_fireOverlay > 0.0f) {
        g_muzzleFlash = std::max(0.0f, g_muzzleFlash - dt * 6.5f);
        g_fireOverlay = std::max(0.0f, g_fireOverlay - dt * 4.2f);
        g_debris.meshDirty = true;
    }
    // Remeshing is deferred to drawFrame via g_meshDirty.
}

// Optional headless-ish smoke/stress (globals declared near top).

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmdLine, int) {
    std::string cmd = cmdLine ? cmdLine : "";
    if (cmd.find("--smoke") != std::string::npos) g_smoke = true;
    // --smoke-movement is a standalone movement-only pass: it drives the real
    // movement seam with scripted intent and writes its own marker, so a
    // movement regression can be triaged without reading the whole smoke log.
    if (cmd.find("--smoke-movement") != std::string::npos) g_smokeMovement = true;
    if (cmd.find("--stress") != std::string::npos) {
        g_stress = true;
        g_smoke = true; // reuse headless quit path
        g_smokeTicks = 600; // longer soak
    }
    // --mesh-helpers N overrides the mesh pool size (0 = serial), so the pooled
    // and serial mesher can be timed back to back on the same machine state.
    {
        // --capture <dir>: run the smoke, then write the fixed camera shots there.
        const std::string flag = "--capture";
        const size_t at = cmd.find(flag + " ");
        if (at != std::string::npos) {
            size_t b = at + flag.size() + 1;
            while (b < cmd.size() && cmd[b] == ' ') ++b;
            size_t e = b;
            if (b < cmd.size() && cmd[b] == '"') { ++b; e = cmd.find('"', b); }
            else e = cmd.find(' ', b);
            g_captureDir = cmd.substr(b, e == std::string::npos ? std::string::npos : e - b);
            g_smoke = true;
        }
    }
    std::string exportMapPath; // --export-map <path>: write the world as a map and exit
    {
        const std::string flag = "--export-map ";
        const size_t at = cmd.find(flag);
        if (at != std::string::npos) {
            size_t b = at + flag.size();
            while (b < cmd.size() && cmd[b] == ' ') ++b;
            size_t e = b;
            if (b < cmd.size() && cmd[b] == '"') { ++b; e = cmd.find('"', b); }
            else e = cmd.find(' ', b);
            exportMapPath = cmd.substr(b, e == std::string::npos ? std::string::npos : e - b);
        }
    }
    int meshHelpersOverride = -1;
    {
        const std::string flag = "--mesh-helpers";
        const size_t at = cmd.find(flag);
        if (at != std::string::npos)
            meshHelpersOverride = std::clamp(std::atoi(cmd.c_str() + at + flag.size()), 0, 15);
    }

    try {
        g_exeDir = getExeDir();
        // Visual settings: declare every parameter, then apply the saved file.
        // Smoke/stress always run on defaults so their captures and timings
        // do not depend on whatever a player last chose.
        vis::registerEngineParams(g_visReg, g_vis);
        if (!g_smoke) {
            bool readable = false;
            const std::string saved = jsonReadText(g_exeDir + "\\visual_settings.json", &readable);
            if (readable) g_visReg.fromJson(saved);
        }
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
        g_post.createTargets(g_extent, g_vis.renderScale, g_swapViews);
        createDescriptors();
        if (!createPipeline()) {
            // Shader assets are missing; bail out cleanly instead of running a
            // frame loop with a null pipeline.
            g_world = nullptr;
            g_views = nullptr;
            cleanup();
            return 1;
        }
        createSync();
        initUi();

        // The world comes from data/maps/warehouse_v1.map.vox.json (exported
        // from the old procedural builder, with fingerprint parity, then the
        // builder was deleted). --export-map re-saves whatever world loaded.
        sim::World world;
        {
            mapvox::Doc mapDoc;
            const std::string candidates[] = {
                g_exeDir + "\\maps\\warehouse_v1.map.vox.json",
                g_exeDir + "\\..\\data\\maps\\warehouse_v1.map.vox.json",
                g_exeDir + "\\..\\..\\data\\maps\\warehouse_v1.map.vox.json",
            };
            bool loaded = false;
            {
                for (const auto& path : candidates) {
                    if (!mapvox::loadMapVox(path, mapDoc)) continue;
                    if (mapDoc.sx != WORLD_W || mapDoc.sy != WORLD_H || mapDoc.sz != WORLD_D)
                        fail("map " + path + " does not match the world size");
                    world = makeEmptyWorld();
                    mapvox::stampMapVox(mapDoc, world, 0, 0, 0);
                    // Prefab instances: data/prefabs/<id>.vox.json, stamped in
                    // list order (a later prefab overwrites an earlier one).
                    for (const auto& pf : mapDoc.prefabs) {
                        mapvox::Doc pd;
                        bool found = false;
                        for (const std::string dir : {g_exeDir + "\\prefabs\\", g_exeDir + "\\..\\data\\prefabs\\",
                                                      g_exeDir + "\\..\\..\\data\\prefabs\\"}) {
                            if (mapvox::loadAssetVox(dir + pf.id + ".vox.json", pd)) { found = true; break; }
                        }
                        if (!found) fail("map " + path + ": prefab \"" + pf.id + "\" not found or invalid (" + pd.error + ")");
                        mapvox::stampMapVox(pd, world, pf.x, pf.y, pf.z, pf.rot);
                        ++g_mapPrefabsStamped;
                    }
                    g_mapSpawn = mapDoc.spawn;
                    g_mapPickups = mapDoc.pickups;
                    g_mapLights = mapDoc.lights;
                    g_bulbs.clear();
                    for (const auto& ml : mapDoc.lights) {
                        BulbLight l;
                        l.pos = Vec3(ml.x * VOXEL_SIZE, ml.y * VOXEL_SIZE, ml.z * VOXEL_SIZE);
                        l.color = Vec3(ml.r, ml.g, ml.b);
                        l.intensity = ml.intensity;
                        l.radius = ml.radius;
                        g_bulbs.push_back(l);
                    }
                    if (mapDoc.environment.present)
                        g_moonDirWorld = Vec3(mapDoc.environment.moonDir[0], mapDoc.environment.moonDir[1],
                                              mapDoc.environment.moonDir[2]).normalized();
                    g_mapPath = path;
                    loaded = true;
                    break;
                }
            }
            if (!loaded) fail("map not found: data\\maps\\warehouse_v1.map.vox.json");
        }
        g_world = &world;
        spawnPlayerOnMap(world);

        // The view's chunk list. It starts empty of world knowledge: every
        // cell in it arrives via sendChunkSnapshot() below.
        std::vector<ViewChunk> views(sim::World::chunkCount());
        for (int cy = 0; cy < CHUNKS_Y; ++cy)
            for (int cz = 0; cz < CHUNKS_Z; ++cz)
                for (int cx = 0; cx < CHUNKS_X; ++cx) {
                    ViewChunk& c = views[sim::World::chunkIndex(cx, cy, cz)];
                    c.cx = cx; c.cy = cy; c.cz = cz;
                    c.palette = &g_viewPalette;
                    c.texLayerPlus1 = g_texTable.layerPlus1;
                }
        g_views = &views;
        sendPalette(world, g_viewPalette);

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
        // on the apron in front of the player.
        seedPickups();
        ensurePickupBuffer();
        buildFixtureMesh();

        if (!exportMapPath.empty()) {
            const bool ok = exportMapDocument(world, exportMapPath);
            g_world = nullptr;
            g_views = nullptr;
            cleanup();
            return ok ? 0 : 1;
        }

        // Initial build: send every chunk's snapshot, mesh each one from that
        // snapshot, then lay out stable per-chunk slots so later impacts only
        // re-upload the chunk they damaged.
        // Mesh workers: helpers beside the main thread, capped so the pool never
        // crowds out the OS on a small machine. They sleep between batches.
        const unsigned hw = std::thread::hardware_concurrency();
        const unsigned helpers = meshHelpersOverride >= 0
            ? static_cast<unsigned>(meshHelpersOverride)
            : (hw > 1 ? std::min(hw - 1, 3u) : 0u);
        meshview::Workers meshWorkers(helpers);
        g_meshWorkers = &meshWorkers;
        g_meshWorkerHelpers = static_cast<int>(meshWorkers.helpers());
        struct ClearMeshWorkers { ~ClearMeshWorkers() { g_meshWorkers = nullptr; } } clearMeshWorkers;

        buildChunkMeshes(sendStaleSnapshots(world, views));
        repackChunkSlots(views);
        uint32_t slotTotal = 0;
        for (const auto& c : views) slotTotal += c.slotCapacity;
        if (!ensureVertexCapacity(slotTotal)) {
            g_world = nullptr;
            g_views = nullptr;
            cleanup();
            return 1;
        }
        for (const auto& c : views) uploadChunkRange(c);
        char msg[256];
        std::snprintf(msg, sizeof(msg),
                      "Chunks=%dx%dx%d voxel=%.4f verts=%u projs=%zu weapons=%zu\n",
                      CHUNKS_X, CHUNKS_Y, CHUNKS_Z, VOXEL_SIZE, g_liveVertexCount,
                      g_projDefs.size(), g_weapons.size());
        OutputDebugStringA(msg);

        // Prewarm debris VB so first impact does not allocate mid-frame (spike fix).
        ensureDebrisBuffer();
        ensureSkyTileBuffer();

        auto start = std::chrono::steady_clock::now();
        auto last = start;
        int frames = 0;

        MSG msgWin{};
        while (g_running) {
            while (PeekMessageA(&msgWin, nullptr, 0, 0, PM_REMOVE)) {
                if (msgWin.message == WM_QUIT) g_running = false;
                TranslateMessage(&msgWin);
                DispatchMessageA(&msgWin);
            }
            if (!g_running) break;

            // Wall clock decides only HOW MANY fixed ticks to run. It never
            // becomes a dt that touches world state.
            auto now = std::chrono::steady_clock::now();
            double wall = std::chrono::duration<double>(now - last).count();
            last = now;
            if (wall > 0.25) wall = 0.25; // cap catch-up after a long stall
            g_tickAccum += wall;

            // Smoke pause probe: pause for a stretch mid-run. No tick may run
            // while paused, and the end state must still match an unpaused run
            // (sim_fingerprint), which is what proves pause is view-only.
            if (g_smoke && !g_stress) {
                if (!g_smokePauseDone && !g_paused && g_tick >= 150) {
                    setPaused(true);
                    g_smokePauseTick = g_tick;
                }
                if (g_paused && ++g_smokePausedFrames >= 30) {
                    // Ticks frozen, and the pause menu drew while paused.
                    g_smokePauseFrozenOk = (g_tick == g_smokePauseTick) && g_uiFrames > 0;
                    setPaused(false);
                    g_smokePauseDone = true;
                }
            }
            // Paused: the host schedules no ticks and banks no time to catch up.
            if (g_paused) g_tickAccum = 0.0;

            int steps = 0;
            // A smoke/stress run is exactly g_smokeTicks ticks however the
            // frames fall: without this cap a slow last frame running two
            // ticks overshot to 601, and the end state (and sim_fingerprint)
            // depended on frame timing.
            while (g_tickAccum >= TICK_DT && steps < MAX_TICKS_PER_FRAME &&
                   !(g_smoke && g_tick >= g_smokeTicks)) {
                g_tickAccum -= TICK_DT;
                ++g_tick;
                g_camPosPrevTick = g_camPos;
                ++g_ticksSinceRemesh;
                // One tick of intent, produced by whichever client is driving:
                // the local window, the scripted smoke harness, or (later) a
                // remote client. The simulation cannot tell the difference.
                SimInput in;
                if (g_smoke) {
                    in = scriptedInput(static_cast<float>(TICK_DT));
                } else {
                    in = buildSimInput();
                }
                simulateOnce(static_cast<float>(TICK_DT), in);
                ++steps;
            }
            // Hit the per-frame cap: drop the backlog instead of chasing it, so a
            // stall costs elapsed time but never desynchronises the sim clock.
            if (steps == MAX_TICKS_PER_FRAME) g_tickAccum = 0.0;

            // Hold the inventory open for a stretch so the smoke actually
            // records and submits the overlay render pass (RULES.md rule 12).
            if (g_smoke && g_tick >= 100 && g_tick < 200) {
                g_inventoryOpen = true;
            } else if (g_smoke) {
                g_inventoryOpen = false;
            }

            // Presentation only. Shader time is derived from the tick counter so
            // animation is frame-rate independent.
            drawFrame(static_cast<float>(g_tick) * static_cast<float>(TICK_DT));
            ++frames;

            if (g_smoke && g_tick >= g_smokeTicks) {
                g_running = false;
            }
        }

        // The simulation's end state, fingerprinted before anything else runs:
        // capture shots stage view content and the self-tests poke player state.
        const uint64_t simPrint = simFingerprint();

        // Capture harness: the smoke ticks are done; pose and capture the shots.
        int captureShots = 0;
        if (!g_captureDir.empty() && g_running == false && g_tick >= g_smokeTicks) {
            CreateDirectoryA(g_captureDir.c_str(), nullptr);
            captureShots = runCaptureShots(static_cast<float>(g_tick) * static_cast<float>(TICK_DT));
        }

        vkDeviceWaitIdle(g_device);

        // Write success marker for smoke / stress tests
        if (g_smoke) {
            const bool jsonxOk = jsonxSelfTest();
            const ballistics::SelfTestReport ballisticsRep = ballistics::selfTest();
            const bool visualParamsOk = vis::selfTest();
            // Render-eye interpolation: ends exact, midpoint halfway, teleport snaps.
            bool eyeInterpOk = false;
            {
                const Vec3 a(0.10f, 0.02f, 0.30f), b(0.1004f, 0.0203f, 0.2997f);
                const Vec3 m = lerpEye(a, b, 0.5f), snapped = lerpEye(a, Vec3(0.5f, 0.02f, 0.3f), 0.25f);
                const Vec3 e0 = lerpEye(a, b, 0.0f), e1 = lerpEye(a, b, 1.0f);
                eyeInterpOk = e0.x == a.x && e0.y == a.y && e0.z == a.z &&
                              std::fabs(e1.x - b.x) < 1e-7f && std::fabs(e1.y - b.y) < 1e-7f &&
                              std::fabs(e1.z - b.z) < 1e-7f &&
                              std::fabs(m.x - 0.1002f) < 1e-6f && snapped.x == 0.5f;
            }
            g_simViewSmoke = runSimViewSmoke(world);
            g_invSmoke = runInventorySmoke();
            g_healthSmoke = runHealthSmoke();
            g_moveSmoke = runMovementSmoke();
            g_mapSmoke = runMapVoxSmoke();
            std::string outPath = g_exeDir + (g_stress ? "\\stress_ok.txt" : "\\smoke_ok.txt");
            std::ofstream out(outPath);
out << "ticks=" << g_tick << "\nframes=" << frames
                << "\nsmoke_complete=" << (g_tick >= g_smokeTicks ? 1 : 0)
                << "\nsim_hz=" << TICK_HZ
                << "\nvertices=" << g_liveVertexCount
                << "\nvertex_slots=" << g_vertexCount
                << "\nsim_fingerprint=" << std::hex << simPrint << std::dec
                << "\nmap_source=" << (g_mapPath.empty() ? std::string("procedural") : g_mapPath)
                << "\nmap_prefabs_stamped=" << g_mapPrefabsStamped
                << "\nmesh_repacks=" << g_meshRepackCount
                << "\nmesh_relocations=" << g_meshRelocateCount
                << "\nmesh_touched_avg=" << (g_meshUploadSamples > 0 ? double(g_meshTouchedSum) / g_meshUploadSamples : 0.0)
                << "\nmesh_touched_max=" << g_meshTouchedMax
                << "\ncam=" << g_camPos.x << "," << g_camPos.y << "," << g_camPos.z
                << "\nplayer=" << g_player.px << "," << g_player.py << "," << g_player.pz
                << "\non_ground=" << (g_player.onGround ? 1 : 0)
                << "\nlean=" << g_player.lean
                << "\nyaw=" << g_yaw << "\npitch=" << g_pitch
                << "\nprojectiles_loaded=" << g_projDefs.size()
                << "\nammo_loaded=" << g_ammoDefs.size()
                << "\nweapons_loaded=" << g_weapons.size()
                << "\njsonx_ok=" << (jsonxOk ? 1 : 0)
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
                << "\nvoxels_destroyed=" << g_voxelsDestroyed
                << "\nbulbs=" << g_bulbs.size()
                << "\nmap_lights=" << g_mapLights.size()
                << "\ntextures_loaded=" << g_texLayerCount
                << "\nocc_chunk_uploads=" << g_occChunkUploads
                << "\nshadow_volume_ready=" << (g_occImageReady ? 1 : 0)
                << "\nmax_anisotropy=" << g_maxAnisotropy
                << "\nfixture_verts=" << g_fixtureVertexCount
                << "\nwater_touch=" << g_touchingWaterUnits << "/" << g_characterUnitCount
                << "\ncurrent=" << (g_currentTriggered ? 1 : 0)
                << "\nsubmerged=" << (g_fullySubmerged ? 1 : 0)
                << "\nweight=" << g_playerWeight
                << "\ncurrent_force=" << g_currentForce
                << "\nrender_scale=" << g_post.scale
                << "\nrender_target=" << g_post.extent.width << "x" << g_post.extent.height
                << "\nwindow=" << g_extent.width << "x" << g_extent.height
                << "\ndrawn_chunks=" << g_drawnChunks
                << "\nculled_chunks=" << g_culledChunks
                << "\nfisheye_visible_radius=" << fisheyeVisibleNdcRadius(kFisheyeStrengthWorld)
                << "\nfisheye_visible_radius_sky=" << fisheyeVisibleNdcRadius(kFisheyeStrengthSky)
                << "\navg_frame_ms=" << (g_frameMsCount > 15 ? (g_frameMsSum / double(g_frameMsCount - 15)) : -1.0)
                << "\nmin_frame_ms=" << (g_frameMsMin < 1e8 ? g_frameMsMin : -1.0)
                << "\nmax_frame_ms=" << g_frameMsMax
                << "\npace_hits=" << g_framePaceHits
                << "\nsteady_frames=" << (g_frameMsCount > 15 ? (g_frameMsCount - 15) : 0)
                << "\ntarget_hz=" << TARGET_HZ
                << "\nlock_ok=" << ((g_frameMsCount > 40) && ((g_frameMsSum / double(g_frameMsCount - 15)) <= 9.0) ? 1 : 0)
                // Per-subsystem latency breakdown (Phase 1 audit). Each line is
                // the avg/max microseconds a subsystem took per sample plus the
                // sample count; section sums nest under avg_frame_ms because the
                // frame is serial. Reading these is the input to the Phase 2 split.
                << "\nsections=";
            for (int i = 0; i < SEC_COUNT; ++i) out << (i ? "," : "") << kSectionName[i];
            out << std::fixed << std::setprecision(2);
            for (int i = 0; i < SEC_COUNT; ++i) {
                out << "\nsec_" << kSectionName[i] << "_us_avg="
                    << (g_secCount[i] > 0 ? (g_secUsSum[i] / double(g_secCount[i])) : 0.0)
                    << "\nsec_" << kSectionName[i] << "_us_max=" << g_secUsMax[i]
                    << "\nsec_" << kSectionName[i] << "_count=" << g_secCount[i];
            }
            out << std::defaultfloat << std::setprecision(6);
            out
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
                << "\nhealth_dash_invuln_blocks=" << g_dashInvulnBlocks
                << "\nhealth_drown_ticks=" << g_drownDamageTicks
                << "\nhealth_deaths=" << g_deaths
                << "\nhealth_respawns=" << g_respawns
                << "\nmove_stance_cycle_ok=" << (g_moveSmoke.stanceCycleOk ? 1 : 0)
                << "\nmove_stance_headroom_ok=" << (g_moveSmoke.stanceHeadroomOk ? 1 : 0)
                << "\nmove_gait_ok=" << (g_moveSmoke.gaitOk ? 1 : 0)
                << "\nmove_stamina_drain_ok=" << (g_moveSmoke.staminaDrainOk ? 1 : 0)
                << "\nmove_stamina_regen_ok=" << (g_moveSmoke.staminaRegenOk ? 1 : 0)
                << "\nmove_slide_enters=" << (g_moveSmoke.slideEnters ? 1 : 0)
                << "\nmove_slide_exhausts=" << (g_moveSmoke.slideExhausts ? 1 : 0)
                << "\nmove_dash_enters=" << (g_moveSmoke.dashEnters ? 1 : 0)
                << "\nmove_dash_cooldown_ok=" << (g_moveSmoke.dashCooldownOk ? 1 : 0)
                << "\nmove_dash_invuln_ok=" << (g_moveSmoke.dashInvulnOk ? 1 : 0)
                << "\nmove_dash_blocks_damage_ok=" << (g_moveSmoke.dashBlocksDamageOk ? 1 : 0)
                << "\nmove_dash_invuln_expires_ok=" << (g_moveSmoke.dashInvulnExpiresOk ? 1 : 0)
                << "\nmove_wallrun_detects=" << (g_moveSmoke.wallrunDetects ? 1 : 0)
                << "\nmove_wallrun_times_out=" << (g_moveSmoke.wallrunTimesOut ? 1 : 0)
                << "\nmove_body_moves=" << (g_moveSmoke.bodyMovesNotCamera ? 1 : 0)
                << "\nmove_offset_presentation_only=" << (g_moveSmoke.offsetIsPresentationOnly ? 1 : 0)
                << "\nmove_water_not_solid=" << (g_moveSmoke.waterNotSolid ? 1 : 0)
                << "\nmove_edge_not_latch_ok=" << (g_moveSmoke.edgeNotLatchOk ? 1 : 0)
                << "\nmove_stamina_after_sprint=" << g_moveSmoke.staminaAfterSprint
                << "\nmove_stance=" << static_cast<int>(g_move.stance)
                << "\nmove_gait=" << static_cast<int>(g_move.gait)
                << "\nmove_stamina=" << g_move.stamina
                << "\nmove_ok="
                << ((g_moveSmoke.stanceCycleOk && g_moveSmoke.stanceHeadroomOk &&
                     g_moveSmoke.gaitOk && g_moveSmoke.staminaDrainOk &&
                     g_moveSmoke.staminaRegenOk && g_moveSmoke.slideEnters &&
                     g_moveSmoke.slideExhausts && g_moveSmoke.dashEnters &&
                     g_moveSmoke.dashCooldownOk && g_moveSmoke.dashInvulnOk &&
                     g_moveSmoke.dashBlocksDamageOk && g_moveSmoke.dashInvulnExpiresOk &&
                     g_moveSmoke.wallrunDetects && g_moveSmoke.wallrunTimesOut &&
                     g_moveSmoke.bodyMovesNotCamera && g_moveSmoke.offsetIsPresentationOnly &&
                     g_moveSmoke.waterNotSolid && g_moveSmoke.edgeNotLatchOk)
                        ? 1
                        : 0)
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
                << "\nsim_visible_ok=" << (g_simViewSmoke.visibleSetOk ? 1 : 0)
                << "\nanti_cheat_gating_ok=" << (g_simViewSmoke.antiCheatGatingOk ? 1 : 0)
                << "\nskirt_isolation_ok=" << (g_simViewSmoke.skirtIsolationOk ? 1 : 0)
                << "\nview_smoothing_ok=" << ((g_simViewSmoke.cornerAoOk && g_simViewSmoke.normalSmoothingOk) ? 1 : 0)
                << "\nao_corners_ok=" << (g_simViewSmoke.cornerAoOk ? 1 : 0)
                << "\nballistics_hitscan_breaks_ok=" << (ballisticsRep.hitscanBreaksSoft ? 1 : 0)
                << "\nballistics_ricochet_keeps_cell_ok=" << (ballisticsRep.ricochetKeepsCell ? 1 : 0)
                << "\nballistics_one_bullet_one_body_ok=" << (ballisticsRep.oneBulletOneBody ? 1 : 0)
                << "\nballistics_projectile_breaks_ok=" << (ballisticsRep.projectileBreaks ? 1 : 0)
                << "\nballistics_shooter_not_swept_ok=" << (ballisticsRep.shooterNotSwept ? 1 : 0)
                << "\nballistics_ok=" << (ballisticsRep.ok() ? 1 : 0)
                << "\nvisual_params_ok=" << (visualParamsOk ? 1 : 0)
                << "\npause_probe_done=" << (g_smokePauseDone ? 1 : 0)
                << "\npause_frames=" << g_smokePausedFrames
                << "\npause_ticks_frozen_ok=" << (g_smokePauseFrozenOk ? 1 : 0)
                << "\nmenu_frames=" << g_uiFrames
                << "\neye_interp_ok=" << (eyeInterpOk ? 1 : 0)
                << "\ncapture_shots=" << captureShots
                << "\nmesh_workers_equiv_ok=" << (g_simViewSmoke.meshWorkersEquivOk ? 1 : 0)
                << "\nmesh_workers_test_helpers=" << g_simViewSmoke.meshWorkersHelpers
                << "\nmesh_workers_test_chunks=" << g_simViewSmoke.meshWorkersChunks
                << "\nmesh_workers_live_helpers=" << g_meshWorkerHelpers
                // MAP-mode loader (Phase 2b): fixture parse + field fidelity +
                // counter restore + unit-cube stamp, all in one gate.
                << "\nmap_file_found=" << (g_mapSmoke.fileFound ? 1 : 0)
                << "\nmap_doc_ok=" << (g_mapSmoke.docOk ? 1 : 0)
                << "\nmap_format_ok=" << (g_mapSmoke.formatOk ? 1 : 0)
                << "\nmap_mode_ok=" << (g_mapSmoke.modeOk ? 1 : 0)
                << "\nmap_unit_ok=" << (g_mapSmoke.unitOk ? 1 : 0)
                << "\nmap_voxel_size_ok=" << (g_mapSmoke.voxelSizeOk ? 1 : 0)
                << "\nmap_dims_ok=" << (g_mapSmoke.dimsOk ? 1 : 0)
                << "\nmap_events=" << g_mapSmoke.events
                << "\nmap_npcs=" << g_mapSmoke.npcs
                << "\nmap_routes=" << g_mapSmoke.routes
                << "\nmap_voxels=" << g_mapSmoke.voxels
                << "\nmap_dropped=" << g_mapSmoke.dropped
                << "\nmap_event_fields_ok=" << (g_mapSmoke.eventFieldOk ? 1 : 0)
                << "\nmap_npc_fields_ok=" << (g_mapSmoke.npcFieldOk ? 1 : 0)
                << "\nmap_route_fields_ok=" << (g_mapSmoke.routeFieldOk ? 1 : 0)
                << "\nmap_counters_restored_ok=" << (g_mapSmoke.countersRestoredOk ? 1 : 0)
                << "\nmap_material_drop_ok=" << (g_mapSmoke.materialDropOk ? 1 : 0)
                << "\nmap_stamp_ok=" << (g_mapSmoke.stampOk ? 1 : 0)
                << "\nmap_appearance_ok=" << (g_mapSmoke.appearanceOk ? 1 : 0)
                << "\nmap_prefab_ok=" << (g_mapSmoke.prefabOk ? 1 : 0)
                << "\nmap_stamp_written=" << g_mapSmoke.stampWritten
                << "\nmap_stamp_skipped=" << g_mapSmoke.stampSkipped
                << "\nmap_refusal_version_ok=" << (g_mapSmoke.refusalVersionOk ? 1 : 0)
                << "\nmap_refusal_unit_ok=" << (g_mapSmoke.refusalUnitOk ? 1 : 0)
                << "\nmap_refusal_voxel_size_ok=" << (g_mapSmoke.refusalVoxelSizeOk ? 1 : 0)
                << "\nmap_ok="
                << ((g_mapSmoke.fileFound && g_mapSmoke.docOk &&
                     g_mapSmoke.formatOk && g_mapSmoke.modeOk && g_mapSmoke.unitOk &&
                     g_mapSmoke.voxelSizeOk && g_mapSmoke.dimsOk &&
                     g_mapSmoke.events == 1 && g_mapSmoke.npcs == 1 && g_mapSmoke.routes == 1 &&
                     g_mapSmoke.voxels == 1024 && g_mapSmoke.dropped == 0 &&
                     g_mapSmoke.eventFieldOk && g_mapSmoke.npcFieldOk &&
                     g_mapSmoke.routeFieldOk && g_mapSmoke.countersRestoredOk &&
                     g_mapSmoke.materialDropOk && g_mapSmoke.stampOk && g_mapSmoke.appearanceOk &&
                     g_mapSmoke.prefabOk &&
                     g_mapSmoke.refusalVersionOk && g_mapSmoke.refusalUnitOk &&
                     g_mapSmoke.refusalVoxelSizeOk)
                        ? 1
                        : 0)
                << "\n";
            out.close();

            // --smoke-movement gets its own marker so a movement regression can
            // be triaged without diffing the whole smoke log, and its own exit
            // code so a CI step can gate on movement alone.
            if (g_smokeMovement) {
                const bool moveAllOk = g_moveSmoke.stanceCycleOk && g_moveSmoke.stanceHeadroomOk &&
                                       g_moveSmoke.gaitOk && g_moveSmoke.staminaDrainOk &&
                                       g_moveSmoke.staminaRegenOk && g_moveSmoke.slideEnters &&
                                       g_moveSmoke.slideExhausts && g_moveSmoke.dashEnters &&
                                       g_moveSmoke.dashCooldownOk && g_moveSmoke.dashInvulnOk &&
                                       g_moveSmoke.dashBlocksDamageOk &&
                                       g_moveSmoke.dashInvulnExpiresOk &&
                                       g_moveSmoke.wallrunDetects && g_moveSmoke.wallrunTimesOut &&
                                       g_moveSmoke.bodyMovesNotCamera &&
                                       g_moveSmoke.offsetIsPresentationOnly &&
                                       g_moveSmoke.waterNotSolid && g_moveSmoke.edgeNotLatchOk;
                std::ofstream mvOut(g_exeDir + "\\movement_smoke_ok.txt");
                mvOut << "move_stance_cycle_ok=" << (g_moveSmoke.stanceCycleOk ? 1 : 0) << "\n"
                      << "move_stance_headroom_ok=" << (g_moveSmoke.stanceHeadroomOk ? 1 : 0) << "\n"
                      << "move_gait_ok=" << (g_moveSmoke.gaitOk ? 1 : 0) << "\n"
                      << "move_stamina_drain_ok=" << (g_moveSmoke.staminaDrainOk ? 1 : 0) << "\n"
                      << "move_stamina_regen_ok=" << (g_moveSmoke.staminaRegenOk ? 1 : 0) << "\n"
                      << "move_slide_enters=" << (g_moveSmoke.slideEnters ? 1 : 0) << "\n"
                      << "move_slide_exhausts=" << (g_moveSmoke.slideExhausts ? 1 : 0) << "\n"
                      << "move_dash_enters=" << (g_moveSmoke.dashEnters ? 1 : 0) << "\n"
                      << "move_dash_cooldown_ok=" << (g_moveSmoke.dashCooldownOk ? 1 : 0) << "\n"
                      << "move_dash_invuln_ok=" << (g_moveSmoke.dashInvulnOk ? 1 : 0) << "\n"
                      << "move_dash_blocks_damage_ok=" << (g_moveSmoke.dashBlocksDamageOk ? 1 : 0) << "\n"
                      << "move_dash_invuln_expires_ok=" << (g_moveSmoke.dashInvulnExpiresOk ? 1 : 0) << "\n"
                      << "move_wallrun_detects=" << (g_moveSmoke.wallrunDetects ? 1 : 0) << "\n"
                      << "move_wallrun_times_out=" << (g_moveSmoke.wallrunTimesOut ? 1 : 0) << "\n"
                      << "move_body_moves=" << (g_moveSmoke.bodyMovesNotCamera ? 1 : 0) << "\n"
                      << "move_offset_presentation_only="
                      << (g_moveSmoke.offsetIsPresentationOnly ? 1 : 0) << "\n"
                      << "move_water_not_solid=" << (g_moveSmoke.waterNotSolid ? 1 : 0) << "\n"
                      << "move_edge_not_latch_ok=" << (g_moveSmoke.edgeNotLatchOk ? 1 : 0) << "\n"
                      << "move_stamina_after_sprint=" << g_moveSmoke.staminaAfterSprint << "\n"
                      << "move_ok=" << (moveAllOk ? 1 : 0) << "\n";
                mvOut.close();
                if (!moveAllOk) { cleanup(); return 2; }
            }
            if (!jsonxOk) { cleanup(); return 3; }
            // A run cut short (Esc, window closed) still writes its report, and
            // every count in it is then too small but self-consistent. Refuse it
            // rather than let an interrupted smoke read as a pass.
            if (g_tick < g_smokeTicks) { cleanup(); return 7; }
            // Pooled meshing must be indistinguishable from serial meshing.
            if (!g_simViewSmoke.meshWorkersEquivOk) { cleanup(); return 5; }
            // The ballistics module's own contract, independent of the map.
            if (!ballisticsRep.ok()) { cleanup(); return 6; }
            // Visual parameter registry: presets, save/load round trip, clamping.
            if (!visualParamsOk) { cleanup(); return 9; }
            // View-side pause and camera: pause froze the ticks, interpolation holds.
            const bool pauseOk = g_stress || (g_smokePauseDone && g_smokePauseFrozenOk);
            if (!pauseOk || !eyeInterpOk) { cleanup(); return 8; }
            // MAP loader gate (Phase 2b): the painter-exported fixture must
            // parse, restore counters, and stamp as unit cubes, and refusals
            // must refuse. Exit 4 lets CI triage the map contract separately.
            if (!g_mapSmoke.fileFound || !g_mapSmoke.docOk || !g_mapSmoke.formatOk ||
                !g_mapSmoke.modeOk || !g_mapSmoke.unitOk || !g_mapSmoke.voxelSizeOk ||
                !g_mapSmoke.dimsOk || g_mapSmoke.events != 1 || g_mapSmoke.npcs != 1 ||
                g_mapSmoke.routes != 1 || g_mapSmoke.voxels != 1024 ||
                g_mapSmoke.dropped != 0 || !g_mapSmoke.eventFieldOk ||
                !g_mapSmoke.npcFieldOk || !g_mapSmoke.routeFieldOk ||
                !g_mapSmoke.countersRestoredOk || !g_mapSmoke.materialDropOk ||
                !g_mapSmoke.stampOk || !g_mapSmoke.appearanceOk || !g_mapSmoke.prefabOk ||
                !g_mapSmoke.refusalVersionOk ||
                !g_mapSmoke.refusalUnitOk || !g_mapSmoke.refusalVoxelSizeOk) {
                cleanup();
                return 4;
            }
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
