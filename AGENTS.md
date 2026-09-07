# Agent instructions

Read and follow `RULES.md`.

**Non-negotiable:** maps, mesh generation, impact/effects geometry, and painter occupancy use the **cubic unit voxel grid** (`VOXEL_SIZE = 0.001`). Every voxel cell is a cube (Z extent equals X and Y). Do not introduce stretched surface maps or non-grid collision that diverges from unit voxel occupancy.

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
| Painter UI | `.\launch.ps1 -Action Painter` (or `Ui`) |
| Painter smoke | `.\launch.ps1 -Action SmokePainter` |
| All smokes | `.\launch.ps1 -Action SmokeAll` |

## Ownership notes (painter workstream)

| Area | Path |
|------|------|
| Filter / bitcrush | `java/painter/src/voxel/painter/filter/*` |
| Painter smoke entry | `java/painter/src/voxel/painter/SmokeMain.java` |
| Dev launcher | `scripts/launch_dev.ps1`, root `launch.ps1` |
| Painter smoke script | `scripts/smoke_painter.ps1` |
| Grid / format | `java/painter/src/voxel/painter/grid/*` (sibling agent) |
| Swing UI | `java/painter/src/voxel/painter/ui/*` (sibling agent) |
