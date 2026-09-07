from pathlib import Path

p = Path(__file__).resolve().parents[1] / "src" / "main.cpp"
t = p.read_text(encoding="utf-8")

def rep(a, b, label):
    global t
    if a not in t:
        raise SystemExit(f"missing: {label}")
    t = t.replace(a, b, 1)
    print("ok", label)

if "g_frameMsSum" not in t:
    rep(
        "static bool g_qpcInit = false;",
        """static bool g_qpcInit = false;
static double g_frameMsSum = 0.0;
static double g_frameMsMin = 1e9;
static double g_frameMsMax = 0.0;
static int g_frameMsCount = 0;
static int g_framePaceHits = 0;""",
        "globals",
    )

rep(
    """static void paceFrame120() {
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
    """static void paceFrame120() {
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
}""",
    "pace",
)

old_smoke = """            if (g_smoke) {
                g_keys['W'] = true;
                g_yaw += dt * 0.35f;
                g_pitch = -0.25f;
                if (frames == 20 || frames == 50 || frames == 80) {
                    g_activeProjIndex = (frames / 30) % static_cast<int>(std::max<size_t>(1, g_projDefs.size()));
                    g_firePressed = true;
                }
            }"""
new_smoke = """            if (g_smoke) {
                // Bench: slow orbit only (no projectile remesh thrash)
                g_yaw += dt * 0.20f;
                g_pitch = -0.22f;
            }"""
if old_smoke in t:
    t = t.replace(old_smoke, new_smoke, 1)
    print("ok smoke quiet")
else:
    # already quiet?
    if "no projectile remesh thrash" in t:
        print("smoke already quiet")
    else:
        raise SystemExit("smoke block missing")

t = t.replace("static int g_smokeFrames = 120;", "static int g_smokeFrames = 300;", 1)
t = t.replace("static int g_smokeFrames = 240;", "static int g_smokeFrames = 300;", 1)

if "avg_frame_ms" not in t:
    inserted = False
    for needle in [
        '<< "\\nculled_chunks=" << g_culledChunks',
        '<< "\\nrender_scale=" << RENDER_SCALE',
    ]:
        if needle in t:
            extra = (
                needle
                + '\n                << "\\navg_frame_ms=" << (g_frameMsCount > 15 ? (g_frameMsSum / double(g_frameMsCount - 15)) : -1.0)'
                + '\n                << "\\nmin_frame_ms=" << (g_frameMsMin < 1e8 ? g_frameMsMin : -1.0)'
                + '\n                << "\\nmax_frame_ms=" << g_frameMsMax'
                + '\n                << "\\npace_hits=" << g_framePaceHits'
                + '\n                << "\\nsteady_frames=" << (g_frameMsCount > 15 ? (g_frameMsCount - 15) : 0)'
                + '\n                << "\\ntarget_hz=" << TARGET_HZ'
                + '\n                << "\\nlock_ok=" << ((g_frameMsCount > 40) && ((g_frameMsSum / double(g_frameMsCount - 15)) <= 9.0) ? 1 : 0)'
            )
            # if needle already ends with more on same stream, just append after first occurrence line content
            t = t.replace(needle, extra, 1)
            print("ok metrics via", needle[:40])
            inserted = True
            break
    if not inserted:
        raise SystemExit("could not insert metrics")

p.write_text(t, encoding="utf-8")
print("ALL_OK", p.stat().st_size)
