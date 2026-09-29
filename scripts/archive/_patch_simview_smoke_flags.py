with open("src/main.cpp", "r", encoding="utf-8") as f:
    text = f.read()

target = """: 0)
                << "\\n";"""

replacement = """: 0)
                << "\\nsim_visible_ok=" << (g_simViewSmoke.visibleSetOk ? 1 : 0)
                << "\\nanti_cheat_gating_ok=" << (g_simViewSmoke.antiCheatGatingOk ? 1 : 0)
                << "\\nskirt_isolation_ok=" << (g_simViewSmoke.skirtIsolationOk ? 1 : 0)
                << "\\nview_smoothing_ok=" << ((g_simViewSmoke.cornerAoOk && g_simViewSmoke.normalSmoothingOk) ? 1 : 0)
                << "\\nao_corners_ok=" << (g_simViewSmoke.cornerAoOk ? 1 : 0)
                << "\\n";"""

# Normalize \r\n to match
if target.replace("\n", "\r\n") in text:
    text = text.replace(target.replace("\n", "\r\n"), replacement.replace("\n", "\r\n"), 1)
    with open("src/main.cpp", "w", encoding="utf-8", newline="") as f:
        f.write(text)
    print("SUCCESS")
elif target in text:
    text = text.replace(target, replacement, 1)
    with open("src/main.cpp", "w", encoding="utf-8", newline="") as f:
        f.write(text)
    print("SUCCESS")
else:
    print("TARGET NOT FOUND")
