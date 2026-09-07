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
#include <stdexcept>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static constexpr int WIDTH = 1280;
static constexpr int HEIGHT = 720;
static constexpr int MAX_FRAMES = 2;
static constexpr int WORLD_W = 48;
static constexpr int WORLD_H = 24;
static constexpr int WORLD_D = 48;

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
};

struct FrameUBO {
    float viewProj[16];
    float lightDir[3];
    float _pad0;
    float camPos[3];
    float time;
};

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
static float g_yaw = 0.6f;
static float g_pitch = 0.45f;
static float g_dist = 42.0f;
static Vec3 g_target(WORLD_W * 0.5f, 6.0f, WORLD_D * 0.5f);

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
static VkRenderPass g_renderPass = VK_NULL_HANDLE;
static VkDescriptorSetLayout g_dsl = VK_NULL_HANDLE;
static VkPipelineLayout g_pipelineLayout = VK_NULL_HANDLE;
static VkPipeline g_pipeline = VK_NULL_HANDLE;
static VkCommandPool g_cmdPool = VK_NULL_HANDLE;
static std::vector<VkCommandBuffer> g_cmdBuffers;
static VkImage g_depthImage = VK_NULL_HANDLE;
static VkDeviceMemory g_depthMem = VK_NULL_HANDLE;
static VkImageView g_depthView = VK_NULL_HANDLE;
static VkBuffer g_vertexBuffer = VK_NULL_HANDLE;
static VkDeviceMemory g_vertexMem = VK_NULL_HANDLE;
static uint32_t g_vertexCount = 0;
static VkBuffer g_uboBuffers[MAX_FRAMES]{};
static VkDeviceMemory g_uboMems[MAX_FRAMES]{};
static void* g_uboMapped[MAX_FRAMES]{};
static VkDescriptorPool g_descPool = VK_NULL_HANDLE;
static VkDescriptorSet g_descSets[MAX_FRAMES]{};
static VkSemaphore g_imageAvailable[MAX_FRAMES]{};
static VkSemaphore g_renderFinished[MAX_FRAMES]{};
static VkFence g_inFlight[MAX_FRAMES]{};
static size_t g_frame = 0;

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

// ---- voxel mesh ----
enum class Block : uint8_t {
    Air = 0,
    Grass,
    Dirt,
    Stone,
    Water,
    Sand,
    Wood,
    Leaves,
    Body,
    Shirt,
    Pants,
    Skin,
    Hair,
    Accent
};

static Vec3 blockColor(Block b) {
    switch (b) {
    case Block::Grass:  return {0.30f, 0.72f, 0.28f};
    case Block::Dirt:   return {0.45f, 0.30f, 0.16f};
    case Block::Stone:  return {0.55f, 0.55f, 0.58f};
    case Block::Water:  return {0.20f, 0.45f, 0.85f};
    case Block::Sand:   return {0.86f, 0.78f, 0.52f};
    case Block::Wood:   return {0.42f, 0.26f, 0.12f};
    case Block::Leaves: return {0.18f, 0.55f, 0.22f};
    case Block::Body:   return {0.25f, 0.45f, 0.85f};
    case Block::Shirt:  return {0.85f, 0.25f, 0.22f};
    case Block::Pants:  return {0.18f, 0.22f, 0.40f};
    case Block::Skin:   return {0.92f, 0.74f, 0.58f};
    case Block::Hair:   return {0.12f, 0.08f, 0.05f};
    case Block::Accent: return {0.95f, 0.80f, 0.15f};
    default:            return {1, 0, 1};
    }
}

static inline int idx(int x, int y, int z) {
    return (y * WORLD_D + z) * WORLD_W + x;
}

static bool inBounds(int x, int y, int z) {
    return x >= 0 && y >= 0 && z >= 0 && x < WORLD_W && y < WORLD_H && z < WORLD_D;
}

static float hashNoise(int x, int z) {
    uint32_t n = static_cast<uint32_t>(x * 374761393 + z * 668265263);
    n = (n ^ (n >> 13)) * 1274126177u;
    n ^= n >> 16;
    return (n & 0xFFFF) / 65535.0f;
}

static void setBlock(std::vector<Block>& w, int x, int y, int z, Block b) {
    if (inBounds(x, y, z)) w[idx(x, y, z)] = b;
}

static Block getBlock(const std::vector<Block>& w, int x, int y, int z) {
    if (!inBounds(x, y, z)) return Block::Air;
    return w[idx(x, y, z)];
}

static void placeTree(std::vector<Block>& w, int x, int z) {
    int base = 0;
    for (int y = WORLD_H - 1; y >= 0; --y) {
        Block b = getBlock(w, x, y, z);
        if (b == Block::Grass || b == Block::Dirt || b == Block::Sand) {
            base = y + 1;
            break;
        }
    }
    if (base <= 0 || base + 6 >= WORLD_H) return;
    int h = 4 + static_cast<int>(hashNoise(x + 3, z + 7) * 3.0f);
    for (int i = 0; i < h; ++i) setBlock(w, x, base + i, z, Block::Wood);
    int top = base + h;
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            for (int dz = -2; dz <= 2; ++dz) {
                if (std::abs(dx) + std::abs(dy) + std::abs(dz) > 4) continue;
                if (dx == 0 && dz == 0 && dy <= 0) continue;
                setBlock(w, x + dx, top + dy, z + dz, Block::Leaves);
            }
        }
    }
}

static void placeCharacter(std::vector<Block>& w, int ox, int oz) {
    // Find ground
    int gy = 1;
    for (int y = WORLD_H - 1; y >= 0; --y) {
        Block b = getBlock(w, ox, y, oz);
        if (b != Block::Air && b != Block::Water && b != Block::Leaves) {
            gy = y + 1;
            break;
        }
    }

    auto put = [&](int x, int y, int z, Block b) { setBlock(w, ox + x, gy + y, oz + z, b); };

    // Legs
    put(0, 0, 0, Block::Pants); put(0, 1, 0, Block::Pants);
    put(2, 0, 0, Block::Pants); put(2, 1, 0, Block::Pants);
    // Torso
    for (int y = 2; y <= 4; ++y)
        for (int x = 0; x <= 2; ++x)
            put(x, y, 0, Block::Shirt);
    put(1, 3, 0, Block::Accent); // belt buckle-ish
    // Arms
    put(-1, 3, 0, Block::Skin); put(-1, 4, 0, Block::Shirt);
    put(3, 3, 0, Block::Skin);  put(3, 4, 0, Block::Shirt);
    // Head
    for (int y = 5; y <= 6; ++y)
        for (int x = 0; x <= 2; ++x)
            for (int z = -1; z <= 0; ++z)
                put(x, y, z, Block::Skin);
    // Hair
    for (int x = 0; x <= 2; ++x)
        for (int z = -1; z <= 0; ++z)
            put(x, 7, z, Block::Hair);
    put(0, 6, -1, Block::Hair);
    put(2, 6, -1, Block::Hair);
    // Eyes
    put(0, 6, -1, Block::Stone);
    put(2, 6, -1, Block::Stone);
}

static std::vector<Block> buildWorld() {
    std::vector<Block> w(WORLD_W * WORLD_H * WORLD_D, Block::Air);

    for (int z = 0; z < WORLD_D; ++z) {
        for (int x = 0; x < WORLD_W; ++x) {
            float n1 = hashNoise(x, z);
            float n2 = hashNoise(x * 3, z * 2);
            float n3 = hashNoise(x + 50, z + 20);
            int h = 3 + static_cast<int>(n1 * 4.0f + n2 * 3.0f + n3 * 2.0f);

            // Gentle basin for a pond near center
            float cx = x - WORLD_W * 0.5f;
            float cz = z - WORLD_D * 0.35f;
            float pond = std::sqrt(cx * cx + cz * cz);
            bool inPond = pond < 7.5f;

            if (inPond) h = 2;

            for (int y = 0; y <= h; ++y) {
                Block b = Block::Stone;
                if (y == h) b = inPond ? Block::Sand : (h < 5 ? Block::Sand : Block::Grass);
                else if (y >= h - 2) b = Block::Dirt;
                else b = Block::Stone;
                setBlock(w, x, y, z, b);
            }
            if (inPond) {
                for (int y = h + 1; y <= 3; ++y) setBlock(w, x, y, z, Block::Water);
            }
        }
    }

    // Scatter trees
    for (int i = 0; i < 28; ++i) {
        int x = 4 + static_cast<int>(hashNoise(i * 17, 9) * (WORLD_W - 8));
        int z = 4 + static_cast<int>(hashNoise(i * 31, 13) * (WORLD_D - 8));
        float cx = x - WORLD_W * 0.5f;
        float cz = z - WORLD_D * 0.35f;
        if (std::sqrt(cx * cx + cz * cz) < 9.0f) continue;
        placeTree(w, x, z);
    }

    // Voxel character near center-front
    placeCharacter(w, WORLD_W / 2 - 1, WORLD_D / 2 + 4);
    return w;
}

static void emitFace(std::vector<Vertex>& out, float x, float y, float z,
                     int face, const Vec3& color) {
    // face: 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z
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
    static const int IDX[6] = {0, 1, 2, 0, 2, 3};

    // Slight per-face shading baked into color for readability even without lights
    float faceShade[6] = {0.85f, 0.70f, 1.0f, 0.55f, 0.90f, 0.75f};
    Vec3 c = color * faceShade[face];

    for (int i = 0; i < 6; ++i) {
        const float* p = F[face][IDX[i]];
        out.push_back(Vertex{
            x + p[0], y + p[1], z + p[2],
            N[face][0], N[face][1], N[face][2],
            c.x, c.y, c.z
        });
    }
}

static std::vector<Vertex> meshWorld(const std::vector<Block>& w) {
    std::vector<Vertex> verts;
    verts.reserve(200000);
    const int ox[6] = {1,-1,0,0,0,0};
    const int oy[6] = {0,0,1,-1,0,0};
    const int oz[6] = {0,0,0,0,1,-1};

    for (int y = 0; y < WORLD_H; ++y) {
        for (int z = 0; z < WORLD_D; ++z) {
            for (int x = 0; x < WORLD_W; ++x) {
                Block b = getBlock(w, x, y, z);
                if (b == Block::Air) continue;
                Vec3 col = blockColor(b);
                // Water slightly darker/translucent look via color only
                if (b == Block::Water) col = col * 0.85f;
                for (int f = 0; f < 6; ++f) {
                    Block nb = getBlock(w, x + ox[f], y + oy[f], z + oz[f]);
                    bool occluded = nb != Block::Air &&
                                    !(b != Block::Water && nb == Block::Water) &&
                                    !(b == Block::Water && nb != Block::Air && nb != Block::Water);
                    // Show face if neighbor is air, or solid next to water, or water surface
                    if (nb == Block::Air || (b != Block::Water && nb == Block::Water)) {
                        emitFace(verts, static_cast<float>(x), static_cast<float>(y),
                                 static_cast<float>(z), f, col);
                    } else if (!occluded && b == Block::Water && nb == Block::Air) {
                        emitFace(verts, static_cast<float>(x), static_cast<float>(y),
                                 static_cast<float>(z), f, col);
                    }
                }
            }
        }
    }
    return verts;
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
            g_width = std::max(1, LOWORD(lParam));
            g_height = std::max(1, HIWORD(lParam));
            g_resized = true;
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam < 256) g_keys[wParam] = true;
        if (wParam == VK_ESCAPE) {
            g_running = false;
            PostQuitMessage(0);
        }
        return 0;
    case WM_KEYUP:
        if (wParam < 256) g_keys[wParam] = false;
        return 0;
    case WM_LBUTTONDOWN:
        g_mouseDown = true;
        g_lastMouseX = LOWORD(lParam);
        g_lastMouseY = HIWORD(lParam);
        SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP:
        g_mouseDown = false;
        ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE:
        g_mouseX = static_cast<short>(LOWORD(lParam));
        g_mouseY = static_cast<short>(HIWORD(lParam));
        if (g_mouseDown) {
            int dx = g_mouseX - g_lastMouseX;
            int dy = g_mouseY - g_lastMouseY;
            g_yaw += dx * 0.005f;
            g_pitch += dy * 0.005f;
            g_pitch = std::max(0.05f, std::min(1.45f, g_pitch));
            g_lastMouseX = g_mouseX;
            g_lastMouseY = g_mouseY;
        }
        return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        g_dist *= (delta > 0) ? 0.9f : 1.1f;
        g_dist = std::max(8.0f, std::min(90.0f, g_dist));
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
        0, wc.lpszClassName, "Voxel Vulkan Engine (Clang)",
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

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = exts;

    if (vkCreateInstance(&ci, nullptr, &g_instance) != VK_SUCCESS)
        fail("vkCreateInstance failed");
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
    for (auto m : modes)
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
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

    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, px)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, nx)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, cr)};

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAtt{};
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

static void uploadMesh(const std::vector<Vertex>& verts) {
    g_vertexCount = static_cast<uint32_t>(verts.size());
    VkDeviceSize size = sizeof(Vertex) * verts.size();

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 staging, stagingMem);
    void* data = nullptr;
    vkMapMemory(g_device, stagingMem, 0, size, 0, &data);
    std::memcpy(data, verts.data(), static_cast<size_t>(size));
    vkUnmapMemory(g_device, stagingMem);

    createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, g_vertexBuffer, g_vertexMem);

    VkCommandBuffer cmd = beginOneTime();
    VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(cmd, staging, g_vertexBuffer, 1, &copy);
    endOneTime(cmd);

    vkDestroyBuffer(g_device, staging, nullptr);
    vkFreeMemory(g_device, stagingMem, nullptr);
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
    clears[0].color = {{0.45f, 0.70f, 0.95f, 1.0f}}; // sky
    clears[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = g_renderPass;
    rp.framebuffer = g_framebuffers[imageIndex];
    rp.renderArea.extent = g_extent;
    rp.clearValueCount = 2;
    rp.pClearValues = clears;

    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline);

    VkViewport viewport{};
    viewport.width = static_cast<float>(g_extent.width);
    viewport.height = static_cast<float>(g_extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = g_extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &g_vertexBuffer, &off);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipelineLayout, 0, 1,
                            &g_descSets[frameIndex], 0, nullptr);
    vkCmdDraw(cmd, g_vertexCount, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
}

static void updateCamera(float dt) {
    float speed = 18.0f * dt;
    Vec3 forward = {
        std::sin(g_yaw) * std::cos(g_pitch),
        0.0f,
        -std::cos(g_yaw) * std::cos(g_pitch)
    };
    forward = forward.normalized();
    Vec3 right = forward.cross({0, 1, 0}).normalized();

    if (g_keys['W'] || g_keys[VK_UP]) g_target = g_target + forward * speed;
    if (g_keys['S'] || g_keys[VK_DOWN]) g_target = g_target - forward * speed;
    if (g_keys['A'] || g_keys[VK_LEFT]) g_target = g_target - right * speed;
    if (g_keys['D'] || g_keys[VK_RIGHT]) g_target = g_target + right * speed;
    if (g_keys[VK_SPACE] || g_keys['E']) g_target.y += speed;
    if (g_keys[VK_CONTROL] || g_keys['Q']) g_target.y -= speed;
}

static Vec3 eyeFromOrbit() {
    return {
        g_target.x + g_dist * std::sin(g_yaw) * std::cos(g_pitch),
        g_target.y + g_dist * std::sin(g_pitch),
        g_target.z + g_dist * std::cos(g_yaw) * std::cos(g_pitch)
    };
}

static void updateUBO(uint32_t frameIndex, float timeSec) {
    Vec3 eye = eyeFromOrbit();
    float aspect = g_extent.height > 0
                       ? static_cast<float>(g_extent.width) / static_cast<float>(g_extent.height)
                       : 1.0f;
    Mat4 proj = Mat4::perspective(50.0f * static_cast<float>(M_PI) / 180.0f, aspect, 0.1f, 250.0f);
    Mat4 view = Mat4::lookAt(eye, g_target, {0, 1, 0});
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
}

static void drawFrame(float timeSec, float dt) {
    updateCamera(dt);

    vkWaitForFences(g_device, 1, &g_inFlight[g_frame], VK_TRUE, UINT64_MAX);

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
}

static void cleanup() {
    if (g_device) vkDeviceWaitIdle(g_device);
    destroySwapchainObjects();
    if (g_pipeline) vkDestroyPipeline(g_device, g_pipeline, nullptr);
    if (g_pipelineLayout) vkDestroyPipelineLayout(g_device, g_pipelineLayout, nullptr);
    if (g_renderPass) vkDestroyRenderPass(g_device, g_renderPass, nullptr);
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
    if (g_vertexBuffer) vkDestroyBuffer(g_device, g_vertexBuffer, nullptr);
    if (g_vertexMem) vkFreeMemory(g_device, g_vertexMem, nullptr);
    if (g_cmdPool) vkDestroyCommandPool(g_device, g_cmdPool, nullptr);
    if (g_device) vkDestroyDevice(g_device, nullptr);
    if (g_surface) vkDestroySurfaceKHR(g_instance, g_surface, nullptr);
    if (g_instance) vkDestroyInstance(g_instance, nullptr);
    if (g_hwnd) DestroyWindow(g_hwnd);
    g_pipeline = VK_NULL_HANDLE;
    g_pipelineLayout = VK_NULL_HANDLE;
    g_renderPass = VK_NULL_HANDLE;
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

// Optional headless-ish smoke test: run N frames then quit if --smoke
static bool g_smoke = false;
static int g_smokeFrames = 120;

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmdLine, int) {
    std::string cmd = cmdLine ? cmdLine : "";
    if (cmd.find("--smoke") != std::string::npos) g_smoke = true;

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
        createDepthResources();
        createFramebuffers();
        createDescriptors();
        createPipeline();
        createSync();

        auto world = buildWorld();
        auto mesh = meshWorld(world);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "Voxel mesh vertices: %zu\n", mesh.size());
        OutputDebugStringA(msg);
        uploadMesh(mesh);

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

            auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;
            float t = std::chrono::duration<float>(now - start).count();

            // Slow auto-orbit so smoke test shows motion without input
            if (g_smoke) g_yaw += dt * 0.35f;

            drawFrame(t, dt);
            ++frames;

            if (g_smoke && frames >= g_smokeFrames) {
                g_running = false;
            }
        }

        vkDeviceWaitIdle(g_device);

        // Write success marker for smoke tests
        if (g_smoke) {
            std::string outPath = g_exeDir + "\\smoke_ok.txt";
            std::ofstream out(outPath);
            out << "frames=" << frames << "\nvertices=" << g_vertexCount << "\n";
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
