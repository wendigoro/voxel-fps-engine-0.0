# Agent instructions

Read and follow `RULES.md`.

**Non-negotiable:** maps, mesh generation, impact/effects geometry, and painter occupancy use the **cubic unit voxel grid** (`VOXEL_SIZE = 0.001`). Every voxel cell is a cube (Z extent equals X and Y). Do not introduce stretched surface maps or non-grid collision that diverges from unit voxel occupancy.

**View/sim split (in progress).** The engine is being separated into an authoritative **simulation** process (physics, damage, destruction, effects, map loading) and one or more **view** processes (window, input, meshing, painting). Two rules carry the weight:

- The unit-cubic grid stays the sole authority for occupancy, collision, impact, destruction, damage, and visibility. A view may smooth, merge, or paint over the surfaces it is sent, but **no derived surface may ever be read back by the simulation**.
- The simulation computes each client's **currently-visible set** and sends only that. A view holds no world grid, no material table, and no rules, so it cannot be exploited for information it was never shown. Do not weaken this into a rendering-only cull.

**Determinism is mandatory.** Fixed timestep with a tick counter, explicitly seeded randomness only, `QueryPerformanceCounter` for telemetry only. Splitting simulation systems across machines is a planned goal and retrofitting determinism is far more expensive than preserving it.

**Bitcrush 32×** (`java/painter/src/voxel/painter/filter/BitcrushUpscale.java`) is display/export only — never bake crushed pixels back into occupancy.

When changing scale, materials, or launch flow, update:

- `src/main.cpp` / `src/materials.hpp` / `src/destruction.hpp`
- `python/projectiles/*` and `python/effects/*`
- `scripts/*.ps1` and root `launch.ps1`
- `java/painter/**` when painter tools, filter, or smoke change
- `RULES.md` if the contract itself changes

## Launcher quick reference

| Action | Command |
|--------|---------|
| Interactive menu | `.\launch.ps1` or `.\scripts\launch_dev.ps1` |
| Engine build | `.\launch.ps1 -Action Build` |
| Engine run | `.\launch.ps1 -Action Engine` |
| Engine smoke | `.\launch.ps1 -Action SmokeEngine` |
| Painter build/smoke | `.\launch.ps1 -Action Painter` or `SmokePainter` |
| Painter UI | `.\launch.ps1 -Action Ui` (`scripts/run_painter_ui.ps1`) |
| All smokes | `.\launch.ps1 -Action SmokeAll` |

`SmokePainter` / `Painter` run `scripts/build_painter.ps1` (or `smoke_painter.ps1` wrapper). Expect `build/painter/painter_smoke_ok.txt`.

## Ownership notes (painter workstream)

| Area | Path |
|------|------|
| Filter / bitcrush | `java/painter/src/voxel/painter/filter/*` (`BitcrushUpscale`, `PngExport`) |
| Painter smoke entry | `java/painter/src/voxel/painter/SmokeMain.java` |
| Dev launcher | `scripts/launch_dev.ps1`, root `launch.ps1` |
| Painter build/smoke | `scripts/build_painter.ps1`, `scripts/smoke_painter.ps1` |
| Painter UI launch | `scripts/run_painter_ui.ps1` |
| Grid / format | `java/painter/src/voxel/painter/grid/*` |
| Swing UI | `java/painter/src/voxel/painter/ui/*` |

## Ownership notes (view/render workstream)

| Area | Path |
|------|------|
| World pass (meshing, chunk VB, draw) | `src/main.cpp` (`meshChunk`, `uploadMesh`, `recordCommandBuffer`) |
| World full-screen effects | `shaders/voxel.frag` (`applyFireOverlay`) — **world pass, not a menu** |
| Fisheye projection curve | `src/fisheye.hpp` (canonical), mirrored in `shaders/voxel.vert` |
| Sim/view boundary + overlay rules | `RULES.md` ("Authority and the view/sim split", "Menus, HUD and other view overlays") |

Any new menu/HUD/debug overlay is a **separate view pass** — own scissor/viewport, depth test and depth write off, blending on, no world vertex buffer or world UBO bound. Do not add menus by extending the world fragment shader; see `RULES.md`.
