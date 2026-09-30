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

### Player body, and the camera-offset contract (mandatory)

The player's body position and velocity are **simulation state**. The camera is a **view output**. These are separate, and the separation is not negotiable.

- **The simulation owns the body.** A player capsule's position, velocity, stance, gait, and collision all live in the simulation next to the player's other authoritative state (health, inventory). Nothing in the view may write them.
- **Movement may not move the camera to move the player.** Writing a world position into the camera transform and letting the next frame's collision resolve from it is forbidden: it makes the camera the authority, makes the player's position a function of view state, and makes two clients with different render settings disagree about where the player is. Movement code integrates the body and then the camera *follows* it.
- **The simulation emits a camera offset, the view applies it.** Anything that should make the eye feel different from the body's authoritative position — stance eye height, wallrun roll, slide lean, landing dip, view bob — is computed in the simulation and sent as an **offset** (and, where a direction is needed, the axis it is relative to). The view composes `eye = bodyEye + offset`. The offset is a *presentation* quantity: it is applied after the body has been simulated and it is never fed back into collision, health, or visibility.
- **An offset may not become load-bearing.** If dropping the offset would change where the player lands, what they hit, or what they can see, the offset is wrong — move the body instead. Eye height above a prone body is a real body height, not an offset, and is simulated as such.
- **Movement input arrives as intent, never as a device poll.** The simulation reads a `SimInput` struct, not `g_keys`, not `GetAsyncKeyState`, and not a `static bool prev` latch held across frames. A press/release edge is expressed as an edge in the intent struct. Polling the device inside simulation makes the outcome depend on how many frames were sampled and on whether a tap was caught, which is exactly the nondeterminism the fixed-step rule forbids.
- **A held input is level, not an edge.** A key that stays down sets a level field each frame (`sprint`, `crouch`); a key that must fire once per press sets an edge field consumed and cleared by the simulation (`dash`, `stanceCycle`). Never infer a toggle from a level changing, and never infer an edge from a latch you kept yourself.
- **The headless movement harness drives scripted intent, not a fake device.** Smoke tests produce a `SimInput` sequence through the same path the real input path uses, so the harness exercises the seam rather than bypassing it. A test that pokes `g_keys` directly is testing a build that cannot happen in production.

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
12. **Inventory / item voxels (overlay)** — items are authored and stored as **1×1×1 unit cubes** on the same integer grid, with a `unit=1` / `voxel_size=0.001` footprint (`src/inventory.hpp`). Inventory volume is a **separate occupancy layer**: it is **never** written into chunk storage (`Block`) and must not affect player collision, raycast impact, destruction, or water sampling.
    - **Exception to "no painting over map voxels"** — when the inventory is open, its unit cubes are composited **over** the map in a dedicated overlay render pass with a freshly cleared depth buffer. The lattice self-occludes correctly within that cleared depth buffer but ignores world depth, so the inventory always reads in front of the world. This is the **only** sanctioned way to draw non-map voxels over map voxels.
    - This is **display-layer only**, same status as the debris sub-lattice (rule 11). Lattice packing, rotation, and rendering all operate on integer unit cells; 90° lattice rotations are legal, continuous rescaling and stretched cells are not.
    - **Material 7 is reserved for the inventory lattice. Material 8 is reserved for world pickups.** `voxel.frag` tests materials in descending order, so the mat-7 branch must come **before** the mat-6 muzzle-flash branch or lattice cubes render as muzzle glow. `voxel.vert` must skip the screen-space fisheye for mat 7 (a camera-parented lattice wants a clean projection) and must restrict its depth bias to mat 6 only. Mat 8 is **not** overlay geometry: it stays on the world pass with world lighting and normal fisheye, so `voxel.frag` remaps mat 8 to the world-lit mat-0 path *before* the mat-7 test.
    - **The overlay pass needs its own pipeline *and* its own framebuffers.** Reusing `g_framebuffers` looks fine — compatibility is usually described in terms of attachment formats — but `VUID-VkRenderPassBeginInfo-renderPass-00904` also compares subpass **dependency chains**, and the overlay legitimately wants a different barrier. Color `initialLayout` must be `PRESENT_SRC_KHR` (`VUID-00900`), depth stays `UNDEFINED` so contents are discarded.
    - **The display basis must stay orthonormal.** Build it as composed true rotations (yaw about view-up, then pitch about the yawed right). Tilting a single axis, e.g. `up*cos + fwd*sin`, shears the basis and turns unit cells into rhomboids — the exact thing rule 12 forbids. `overlay_unit_cubes` in `smoke_ok.txt` guards this.
    - Run `VOXEL_VALIDATE=1` (opt-in `VK_LAYER_KHRONOS_validation`, see `createInstance`) when touching the overlay pass. Pass/framebuffer/layout rules are not reliably reasoned about correctly; the smoke asserts `overlay_frames_drawn > 0` so the pass is actually submitted.
    - **Backpack storage is owned by the pack item.** Equipping a `backpack` swaps the lattice to the item's `pack_size`; unequipping or lifting the pack out of its marker sets the volume to **none** (0 cells) — it must never silently fall back to the 3×3×4 base pack, and it must never re-grant the base pack to a player who is not wearing one. `setVolume` clears packed items **and** `held`, so revoke storage *before* putting an item into the hand: doing it after destroys the item just lifted. `smoke_ok.txt` guards this with `backpack_volume_ok` and `backpack_unequip_lift_ok`.
13. **Inventory controls** — `TAB` toggles the overlay (and releases the mouse; opening drops a held item back into the pack). With the overlay up the cursor, not the look direction, drives selection: **LMB** lifts a packed item into the hand or places the held item, **RMB** stows, **R** rotates the held item 90° about view-up. With the overlay closed, **G** takes the world pickup under the crosshair; it auto-places into the first free lattice position, and when the pack is full the item is *carried in hand* rather than dropped (the pickup itself stays in the world). Inventory actions never fire while the mouse is captured for looking.
14. **Painter item mode → `*.item.json`** — painter `Mode.ITEM` (`voxel.painter.grid.Items`) authors the **same unit grid** as every other mode. The document grid is a *work area*, not the item: the exported `size` is the **tight bounding box of the solid cells** and `cells` lists them rebased to that origin, so an item never pays storage for empty canvas around it. There is no scale control, so the footprint can never diverge from unit cubes (rule 12).
    - Classes: `weapon_primary`, `weapon_small`, `ammo_pouch`, `armor`, `backpack`, `misc`. `armor` requires an `armor_zone` (`head`/`chest`/`arms`/`legs`); `backpack` requires a positive `pack_size`. Malformed backpack data grants **no** storage rather than defaulting to the base pack.
    - Magazine sizes and pouch round counts stay **metadata only** until the firing/reload path consumes them. Bitcrush never reaches item occupancy.
    - Smoke artifacts belong in `data/voxfmt/`, never `data/items/` — the engine loads *every* file in `data/items`, so a test item would silently become real game content.

### Forbidden

- Flat “painted” floors/walls that are a single stretched triangle pair **without** unit voxel occupancy behind them
- Effect radii or hitboxes that destroy continuous AABBs without iterating unit cells
- Changing `VOXEL_SIZE` in one layer (render vs materials vs Python vs painter) without updating all layers
- Painter or filter paths that emit non-cubic occupancy cells
- Inventory / viewmodel voxels written into chunk storage, or composited over map voxels **outside** the rule-12 overlay pass
- Packing or rendering an inventory lattice as continuous, stretched, or non-unit cells instead of integer unit cubes
- Exporting an item whose `size` is the painter canvas instead of the tight box of its solid cells
- Giving a player a base pack, or defaulting malformed backpack data to one, when no pack is equipped
- Reading a smoothed, merged, painted, or otherwise derived surface from collision, impact, destruction, damage, map authoring, or visibility
- Reading rendering state (camera matrices, per-chunk mesh data, shader-class ids) to decide a simulation outcome
- Sending a client any cell, damage value, or entity it is not currently able to see
- Applying a visual filter so it samples a cell the client was not sent
- Moving a player by writing the camera transform instead of the body, or resolving player collision against the camera's position
- Reading `g_keys`, `GetAsyncKeyState`, or a cross-frame `static` latch inside simulation code to derive intent
- Treating a held input as an edge, or inferring a toggle from whether a level field changed since last frame
- Applying a camera offset to collision, health, or visibility, so that dropping the offset would change where the player lands or what they can see
- Driving the headless movement harness by writing `g_keys` rather than through the same intent path the real input uses

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
- **Inventory item** → author in painter `Mode.ITEM` and export `*.item.json` into `data/items/`; the footprint is the painted cells' tight box (rule 14).
- **Map / map entities** → author in painter `Mode.MAP`. The grid is ordinary unit-cubic voxels and entity coordinates are integer cell coordinates on that same grid, so they rescale with `VOXEL_SIZE` alone. Entity ids are assigned per document in authoring order, never from a clock or RNG, so identical authoring order always yields identical ids. Schema: `data/voxfmt/schema.md`. Authoring and interchange only until an engine consumer exists.

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
