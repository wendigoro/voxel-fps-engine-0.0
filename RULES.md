# Project Rules — Voxel FPS Engine 0.0

## Cubic unit rendering (mandatory)

**All maps, geometry, VFX debris proxies, impact/effect sampling, and painter occupancy MUST use the cubic unit voxel grid.**

This is not optional styling. It is the canonical world representation used by rendering **and** geometric impact calculations.

## Authority and the view/sim split (mandatory)

The engine is being split into an **authoritative simulation process** (physics, damage, destruction, effects, map loading) and one or more **view processes** (window, input, meshing, painting). This section states which grid each is allowed to trust.

### Single source of grid truth

- The **unit-cubic grid is the sole authority** for occupancy, collision, impact, destruction, damage accumulation, and visibility. Nothing else may be consulted to answer "is this cell solid".
- The view process holds **no world grid, no material table, and no rules**. It receives only what the simulation chooses to send it.
- **A view may derive any smoothed, merged, or painted surface from the cells it was sent, for display only.** Smoothed vertex normals, merged coplanar quads, procedural/triplanar material detail, and screen-space smoothing passes are all permitted in the view. They are a *rendering* transform over grid data, not a replacement for it.
- **No derived surface may ever feed back.** A smoothed or painted surface must never be read by collision, impact, destruction, damage, map authoring, or visibility. If a visual technique would require that, it is forbidden — smooth the display, not the grid.
- The simulation must never read rendering state (camera matrices, view/projection, per-chunk mesh data, shader-class ids) to decide simulation outcomes. Derived state flows one way: sim computes, view renders.

### Visibility filtering (anti-cheat)

- The simulation computes each client's **currently-visible set** and sends **only** that. This is an anti-cheat boundary, not an optimization: a client that holds data it was never shown can be exploited by anything that reads its memory.
- The visible set is **per client** and **transient**. There is no remembered-but-hidden rendering: a client receives geometry that is visible now, and nothing else.
- A client that falls behind must be **dropped and re-synced**, never allowed to stall or block the simulation.
- Every stream is filtered — geometry, damage values, and other players' transforms. If client *i* cannot see a change, the change does not cross the wire to client *i*.
- Any visual filtering (depth of field, blur, smoothing) must be masked by the visible set. A filter kernel may not sample a cell the client was not sent, and heavily-filtered regions must be eroded by the kernel radius so blur cannot bleed the shape of hidden geometry.

### Menus, HUD and other view overlays

- A menu, HUD, reticle, damage indicator, debug panel, or score readout is **view state, never world state**. It is drawn by the view process from data the simulation chose to send; the simulation never reads it and it never occupies a voxel.
- A menu is **not** a world render and must not fight one. Draw order is world first, then overlay, and the overlay must not depend on world pixels surviving depth so that a menu can never be z-fought, clipped, or hidden by geometry.
- An overlay pass uses its own **scissor/viewport rectangle** and its own pipeline state (depth test and depth write off, blending on, and no world vertex buffer or world descriptor bound). Binding world draw state for a UI pass invites state leakage back into the world pass and is not allowed.
- An overlay must not write into world-owned GPU resources — no vertex/index buffer of a world chunk, no world uniform buffer, and no damage or paint buffer.
- A full-screen effect that shades the *world* (e.g. `applyFireOverlay` in `shaders/voxel.frag`) is a **world pass**, not a menu. Keep the two categories distinct: a world post effect runs inside the world pipeline over already-shaded geometry, whereas a menu is separate geometry in a separate pass. Do not implement a menu by extending the world fragment shader's output.
- If an overlay ever needs information the client was not sent (another player's inventory, a cell behind a wall), that is a **contract violation**, not a rendering problem. Route it through the visibility filter like any other stream.

### Constants

| Symbol | Value | Meaning |
|--------|------:|---------|
| `VOXEL_SIZE` / `kVoxelSize` | `0.001` | World-space edge length of one unit cube (1000× smaller than original 1.0 blocks) |
| Grid cell | `1×1×1` integer | One solid block occupies exactly one grid cell |

Integer grid coordinates `(ix, iy, iz)` map to world space as:

```text
world = (ix * VOXEL_SIZE, iy * VOXEL_SIZE, iz * VOXEL_SIZE)
```

Every voxel cell is a **cube**: Z extent always equals X and Y unit size. No stretched non-grid geometry for occupancy.

This is a constraint on **occupancy and collision**, not on how the result is drawn. See "Authority and the view/sim split" above: a view may smooth, merge, or paint over the surfaces it is sent, provided the unit-cubic grid underneath is unchanged and nothing derived is read back by the simulation.

### Required practices

1. **Unit cubes only** — every solid is a full 1×1×1 voxel cube. No stretched quads, billboards-as-terrain, or non-grid scaled planes for **map geometry, occupancy, or collision**. A view may merge coplanar cells into larger quads for display; the cells underneath must all exist and be unit cubes.
2. **Sharp face mesh (authority) / smoothed faces permitted (view)** — occupancy and collision are defined by unit cubes with hard faces. A view may emit smooth shared normals, merged coplanar quads, and procedurally shaded surfaces for display only, provided the simulation never reads them.
3. **Impact / destruction** — raycasts, projectiles, splash, and break tests operate on **integer grid indices**, then convert with `VOXEL_SIZE`. Never invent continuous collision volumes that disagree with the voxel occupancy grid.
4. **Materials** — per-voxel material IDs (dirt, concrete, wood, sheet_metal, girder, …) drive density/weight/fragility. C++ (`src/materials.hpp`) and Python (`python/projectiles/materials.py`) must stay in sync.
5. **Effects (Python)** — Pymunk (or any effects loop) may simulate continuous motion under gravity, but **destruction outputs must snap to unit grid cells** and reference the same material table.
6. **Maps** — new maps are built by placing unit voxels (fill boxes, beams, 1-voxel walls, crates). Do not author map collision as separate meshes.
7. **Water** — water occupancy is still unit cubes on the grid. "Larger" water is multiple adjacent unit cells (`WATER_CELL`). Visual tide may paint between vertices in shaders; collision/current sampling stays on unit cells.
8. **Performance look** — internal downscale (`RENDER_SCALE`) + bitcrush/quantization are allowed post styles; they must not change the unit occupancy grid used for impact.
9. **Painter** — model / sky / character tools edit the **same integer unit grid**. "Stretch" is integer scale of a selection box (pad/crop on-grid), never continuous non-unit geometry. Saved assets keep `unit=1`, `voxel_size=0.001`.
10. **Bitcrush 32× filter** — nearest-neighbor upscale ×32 + color quantize is a **display/export layer only** (`voxel.painter.filter.BitcrushUpscale`). It must not rewrite occupancy to non-unit cells. Export may write raw `.vox.json` plus a crushed preview PNG.
11. **Debris / degradation (8×8×8)** — bullet impacts may spawn visual fragment chips from an **8×8×8 sub-lattice inside each unit voxel** (`src/debris.hpp`). Chips use trajectory/ricochet via a 3×3 matrix calculator. This is **display/effects only** and must never rewrite occupancy away from unit cubes.

### Forbidden

- Flat “painted” floors/walls that are a single stretched triangle pair **without** unit voxel occupancy behind them
- Effect radii or hitboxes that destroy continuous AABBs without iterating unit cells
- Changing `VOXEL_SIZE` in one layer (render vs materials vs Python vs painter) without updating all layers
- Painter or filter paths that emit non-cubic occupancy cells
- Reading a smoothed, merged, painted, or otherwise derived surface from collision, impact, destruction, damage, map authoring, or visibility
- Reading rendering state (camera matrices, per-chunk mesh data, shader-class ids) to decide a simulation outcome
- Sending a client any cell, damage value, or entity it is not currently able to see
- Applying a visual filter so it samples a cell the client was not sent

### Determinism (mandatory)

Splitting simulation systems across machines is a planned goal, so the simulation must stay replicable.

- The simulation advances on a **fixed timestep with a tick counter**. Wall-clock `dt` may size how many ticks to run, but must never be fed into simulation state.
- All randomness is **explicitly seeded**. No `rand()`, `srand`, `random_device`, or time-seeded generators.
- `QueryPerformanceCounter` / `GetTickCount` are **telemetry only** and must never feed a value that reaches world state.
- Keep the tick shaped as repeated single-step calls (`for (n = 0; n < steps; ++n) simulateOnce(TICK_DT);`) so replay, rollback, and migration stay possible.
- Determinism is far cheaper to preserve now than to retrofit. Do not introduce a source of nondeterminism "just for this one case".

### When adding content

- **Map piece** → write unit voxels into chunk storage; remesh dirty chunks.
- **Projectile / effect** → author in Python, export JSON, resolve hits on the grid in C++.
- **New material** → add to both C++ and Python material tables, then map `Block` → `MaterialId`.
- **Painted asset** → save via painter voxfmt (unit cubes); optional crushed PNG is preview only.

## Launcher contract

Use repo scripts so paths stay consistent:

| Script | Purpose |
|--------|---------|
| `launch.ps1` | Root entry → `scripts/launch_dev.ps1` (also legacy `-Build`/`-Run`/`-SmokeOnly`) |
| `scripts/launch_dev.ps1` | Dev menu / `-Action Build\|Painter\|Engine\|SmokeEngine\|SmokePainter\|SmokeAll\|Ui\|Help` |
| `scripts/build.ps1` | Export Python defs + compile engine |
| `scripts/build_painter.ps1` | Compile Java painter + run `voxel.painter.SmokeMain` |
| `scripts/smoke_painter.ps1` | Painter smoke wrapper → `build_painter.ps1` |
| `scripts/run_painter_ui.ps1` | Build painter then launch `voxel.painter.ui.PainterApp` |
| `scripts/run.ps1` | Launch interactive engine |
| `scripts/demo.ps1` | Full demo: export, build, smoke test, optional interactive |

Working directory for the engine executable is always `build/` so `projectiles.json` and shaders resolve next to the binary.

Painter classes compile to `build/painter/`; painter smoke OK file is `build/painter/painter_smoke_ok.txt`. Optional crushed preview PNGs may also be written under `build/painter/` (display-only; never occupancy).
