# Project Rules — Voxel FPS Engine 0.0

## Cubic unit rendering (mandatory)

**All maps, geometry, VFX debris proxies, impact/effect sampling, and painter occupancy MUST use the cubic unit voxel grid.**

This is not optional styling. It is the canonical world representation used by rendering **and** geometric impact calculations.

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

### Required practices

1. **Unit cubes only** — every solid is a full 1×1×1 voxel cube. No stretched quads, billboards-as-terrain, or non-grid scaled planes for map geometry.
2. **Sharp face mesh** — exposed faces emit unique vertices with hard face normals (no smooth shared normals across edges).
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

- Flat “painted” floors/walls that are a single stretched triangle pair without unit voxel occupancy
- Effect radii or hitboxes that destroy continuous AABBs without iterating unit cells
- Changing `VOXEL_SIZE` in one layer (render vs materials vs Python vs painter) without updating all layers
- Painter or filter paths that emit non-cubic occupancy cells
- Inventory / viewmodel voxels written into chunk storage, or composited over map voxels **outside** the rule-12 overlay pass
- Packing or rendering an inventory lattice as continuous, stretched, or non-unit cells instead of integer unit cubes
- Exporting an item whose `size` is the painter canvas instead of the tight box of its solid cells
- Giving a player a base pack, or defaulting malformed backpack data to one, when no pack is equipped

### When adding content

- **Map piece** → write unit voxels into chunk storage; remesh dirty chunks.
- **Projectile / effect** → author in Python, export JSON, resolve hits on the grid in C++.
- **New material** → add to both C++ and Python material tables, then map `Block` → `MaterialId`.
- **Painted asset** → save via painter voxfmt (unit cubes); optional crushed PNG is preview only.
- **Inventory item** → author in painter `Mode.ITEM` and export `*.item.json` into `data/items/`; the footprint is the painted cells' tight box (rule 14).

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
