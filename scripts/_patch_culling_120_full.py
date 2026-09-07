from pathlib import Path

p = Path(__file__).resolve().parents[1] / "src" / "main.cpp"
t = p.read_text(encoding="utf-8")

def rep(a, b, label):
    global t
    if a not in t:
        raise SystemExit(f"missing: {label}")
    t = t.replace(a, b, 1)
    print("ok", label)

rep(
    """struct Chunk {
    int cx = 0, cy = 0, cz = 0; // chunk coords
    std::vector<Block> voxels;  // CHUNK_SIZE^3
    std::vector<Vertex> mesh;   // sharp unique face verts
    bool dirty = true;
};""",
    """struct Chunk {
    int cx = 0, cy = 0, cz = 0; // chunk coords
    std::vector<Block> voxels;  // CHUNK_SIZE^3
    std::vector<Vertex> mesh;   // sharp unique face verts
    bool dirty = true;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    bool wasVisible = true;
};""",
    "chunk",
)

rep(
    """static size_t g_frame = 0;

static std::string g_exeDir;""",
    """static size_t g_frame = 0;
static Mat4 g_viewProjCull{};
static uint32_t g_drawnChunks = 0;
static uint32_t g_culledChunks = 0;
static constexpr double TARGET_HZ = 120.0;
static constexpr double TARGET_FRAME_SEC = 1.0 / TARGET_HZ;
static LARGE_INTEGER g_qpcFreq{};
static LARGE_INTEGER g_qpcLast{};
static bool g_qpcInit = false;

static std::string g_exeDir;""",
    "globals",
)

rep(
    """static VkPresentModeKHR choosePresentMode() {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &count, modes.data());
    for (auto m : modes)
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) return m;
    return VK_PRESENT_MODE_FIFO_KHR;
}""",
    """static VkPresentModeKHR choosePresentMode() {
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
}""",
    "present",
)

rep(
    """static std::vector<Vertex> meshAllChunks(std::vector<Chunk>& chunks) {
    std::vector<Vertex> verts;
    verts.reserve(300000);
    for (auto& c : chunks) {
        if (c.dirty) meshChunk(c, chunks);
        verts.insert(verts.end(), c.mesh.begin(), c.mesh.end());
    }
    return verts;
}""",
    """static std::vector<Vertex> meshAllChunks(std::vector<Chunk>& chunks) {
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
    for (;;) {
        QueryPerformanceCounter(&now);
        double elapsed = double(now.QuadPart - g_qpcLast.QuadPart) / double(g_qpcFreq.QuadPart);
        if (elapsed >= TARGET_FRAME_SEC) break;
        double remain = TARGET_FRAME_SEC - elapsed;
        if (remain > 0.002) {
            DWORD ms = (DWORD)((remain - 0.0008) * 1000.0);
            if (ms > 0) Sleep(ms);
        }
    }
    QueryPerformanceCounter(&g_qpcLast);
}""",
    "mesh cull pace",
)

rep(
    """    g_isNight = g_timeOfDay > 0.7f || g_timeOfDay < 0.25f;

    FrameUBO ubo{};
    std::memcpy(ubo.viewProj, vp.m, sizeof(vp.m));""",
    """    g_isNight = g_timeOfDay > 0.7f || g_timeOfDay < 0.25f;
    g_viewProjCull = vp;

    FrameUBO ubo{};
    std::memcpy(ubo.viewProj, vp.m, sizeof(vp.m));""",
    "vp cull",
)

rep(
    """    vkCmdBindVertexBuffers(cmd, 0, 1, &g_vertexBuffer, &off);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipelineLayout, 0, 1,
                            &g_descSets[frameIndex], 0, nullptr);
    vkCmdDraw(cmd, g_vertexCount, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
}""",
    """    vkCmdBindVertexBuffers(cmd, 0, 1, &g_vertexBuffer, &off);
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

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
}""",
    "multidraw",
)

idx_rec = t.find("static void recordCommandBuffer")
idx_fwd = t.find("static Vec3 cameraForward();")
if idx_fwd < 0 or (idx_fwd > idx_rec >= 0):
    t = t.replace(
        "static void recordCommandBuffer(uint32_t imageIndex, uint32_t frameIndex) {",
        "static Vec3 cameraForward();\nstatic void recordCommandBuffer(uint32_t imageIndex, uint32_t frameIndex) {",
        1,
    )
    print("ok forward")

rep(
    """    g_frame = (g_frame + 1) % MAX_FRAMES;
}""",
    """    g_frame = (g_frame + 1) % MAX_FRAMES;
    paceFrame120();
}""",
    "pace",
)

# smoke stats
for needle in [
    '<< "\\nrender_scale=" << RENDER_SCALE',
    '<< "\\nrender_scale=" << RENDER_SCALE << "\\n";',
]:
    if needle in t and "drawn_chunks" not in t:
        if needle.endswith('";'):
            t = t.replace(
                needle,
                '<< "\\nrender_scale=" << RENDER_SCALE\n'
                '                << "\\ndrawn_chunks=" << g_drawnChunks\n'
                '                << "\\nculled_chunks=" << g_culledChunks << "\\n";',
                1,
            )
        else:
            t = t.replace(
                needle,
                needle
                + '\n                << "\\ndrawn_chunks=" << g_drawnChunks'
                + '\n                << "\\nculled_chunks=" << g_culledChunks',
                1,
            )
        print("ok smoke")
        break

p.write_text(t, encoding="utf-8")
t = p.read_text(encoding="utf-8")
print("size", p.stat().st_size)
assert "chunkNotSeen" in t and "paceFrame120();" in t
assert "vkCmdDraw(cmd, c.vertexCount" in t
print("ALL_OK")
