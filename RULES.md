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

### Forbidden

- Flat “painted” floors/walls that are a single stretched triangle pair without unit voxel occupancy
- Effect radii or hitboxes that destroy continuous AABBs without iterating unit cells
- Changing `VOXEL_SIZE` in one layer (render vs materials vs Python vs painter) without updating all layers
- Painter or filter paths that emit non-cubic occupancy cells

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
