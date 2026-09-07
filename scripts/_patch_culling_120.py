from pathlib import Path

p = Path(__file__).resolve().parents[1] / "src" / "main.cpp"
t = p.read_text(encoding="utf-8")

def rep(a, b, label):
    global t
    if a not in t:
        raise SystemExit(f"missing: {label}")
    t = t.replace(a, b, 1)
    print("ok", label)

if "firstVertex" not in t:
    raise SystemExit("chunk ranges missing - run base culling patch first")
if "chunkNotSeen" not in t:
    raise SystemExit("cull helpers missing - run base culling patch first")

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

    // Not-seen rendering: only draw frustum/front-facing chunks
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

# forward decl before recordCommandBuffer
idx_rec = t.find("static void recordCommandBuffer")
idx_fwd = t.find("static Vec3 cameraForward();")
if idx_fwd < 0 or idx_fwd > idx_rec:
    t = t.replace(
        "static void recordCommandBuffer(uint32_t imageIndex, uint32_t frameIndex) {",
        "static Vec3 cameraForward();\nstatic void recordCommandBuffer(uint32_t imageIndex, uint32_t frameIndex) {",
        1,
    )
    print("ok forward cameraForward")

rep(
    """    g_frame = (g_frame + 1) % MAX_FRAMES;
}""",
    """    g_frame = (g_frame + 1) % MAX_FRAMES;
    paceFrame120();
}""",
    "pace",
)

# smoke stats
needle = '<< "\\nrender_scale=" << RENDER_SCALE'
if needle in t and "drawn_chunks" not in t:
    t = t.replace(
        needle,
        needle
        + '\n                << "\\ndrawn_chunks=" << g_drawnChunks'
        + '\n                << "\\nculled_chunks=" << g_culledChunks',
        1,
    )
    print("ok smoke stats")

p.write_text(t, encoding="utf-8")
t = p.read_text(encoding="utf-8")
print("written", p.stat().st_size)
print("multidraw", "vkCmdDraw(cmd, c.vertexCount" in t)
print("pace", "paceFrame120();" in t)
print("fwd_ok", t.find("static Vec3 cameraForward();") < t.find("recordCommandBuffer"))
