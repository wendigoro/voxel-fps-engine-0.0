# Voxel FPS Engine 0.0

A micro-voxel first-person engine with **cubic unit-grid destruction**, **material-aware ballistics**, and a **voxel painter** for authoring weapons/maps — all sharing a canonical `VOXEL_SIZE = 0.001` grid.

---

## Project Scope

| Layer | Tech | Role |
|-------|------|------|
| **Engine** | C++17 + Vulkan (Win32) | Real-time voxel rasterizer, destruction resolver, 120 Hz frame-paced loop |
| **Authoring** | Python 3.11+ | Projectile/ammo/material definitions, Pymunk effects physics, JSON export |
| **Painter** | Java 21 + Swing | Unit-voxel grid editor for models/sky/character/weapons, bitcrush preview filter |
| **Build/Launch** | PowerShell 5.1 | Unified scripts for export → compile → smoke test → run |

**Core invariant:** Every solid is a 1×1×1 integer voxel cube. World space = `grid * 0.001`. No stretched quads, no continuous collision diverging from the grid.

---

## Unique Systems

### 1. Cubic Unit Voxel Grid (Mandatory Contract)

All layers — rendering, impact, painter, water, effects — operate on the **same integer grid**.

```cpp
// src/main.cpp:39, src/materials.hpp:35, java/painter/src/voxel/painter/grid/VoxelGrid.java:11
static constexpr float VOXEL_SIZE = 0.001f;  // 1000× smaller than legacy 1.0 blocks
```

- **Mesh:** Sharp face emission — 6 unique vertices/face, hard normals (`emitSharpFace`, `main.cpp:677`)
- **Impact/Destruction:** Raycasts and projectiles iterate integer grid cells only (`resolveVoxelHit`, `destruction.hpp:389`)
- **Water:** `WATER_CELL = 2` clumps of unit cubes; visual tide in shader, collision stays on unit cells
- **Painter:** Selections scale by integer pad/crop on-grid (`VoxelGrid.assertCubicUnitInvariant`, `VoxelGrid.java:36`)

### 2. Material-Driven Destruction (C++ ↔ Python Sync)

Materials carry **density, weight, fragility, toughness, damping** — used identically by C++ resolver and Python effects loop.

```cpp
// src/materials.hpp:38-69
inline float breakEnergyThreshold(MaterialId id) {
    const auto& m = materialProps(id);
    return m.toughness * voxelMass(id) / std::max(0.05f, m.fragility);
}
```

```python
# python/projectiles/materials.py:18-42
MATERIALS = {
    "concrete": Material("concrete", density=2.40, weight=1.35, fragility=0.15, toughness=3.50, damping=0.55),
    "dirt":     Material("dirt",     density=1.20, weight=1.10, fragility=0.65, toughness=0.70, damping=0.40),
    ...
}
```

**Effect multipliers** per material × projectile family (`effectMultiplier`, `destruction.hpp:407`; `_effect_mul`, `physics_loop.py:63`):
- `shred` → 2.4× on foliage, 1.3× on wood
- `explosive` → 1.6× on dirt/leaves, 0.75× on concrete
- `kinetic` → baseline

### 3. Projectile & Weapon Assembly (Python → JSON → C++)

Projectiles authored in Python, exported to `data/projectiles.json`, loaded by C++ at runtime.

```python
# python/projectiles/defs.py:31-127
PROJECTILES = [
    ProjectileType(id="slug",        mass=0.06, speed=3.2, radius=0.0025, base_damage=10, penetration=0.40, effect="kinetic"),
    ProjectileType(id="shredder",    mass=0.05, speed=2.8, radius=0.003,  base_damage=7,  penetration=0.25, effect="shred",    splash_radius=0.006),
    ProjectileType(id="impact_charge",mass=0.12, speed=2.0, radius=0.004,  base_damage=18, penetration=0.15, effect="explosive", splash_radius=0.012),
    ProjectileType(id="energy_beam", mass=0.001, speed=50,   radius=0.0015, base_damage=12, penetration=0.70, effect="energy",  hitscan=True),
    # caliber profiles
    ProjectileType(id="light_ball",  ...),
    ProjectileType(id="medium_ball", ...),
    ProjectileType(id="heavy_ball",  ...),
]
```

Weapon JSONs (`data/weapons/*.weapon.json`) define **part-stat mapping**:
- barrel → damage, action → impact, bolt_chamber → recoil, trigger → handling, grip_stock → weight, sight → optic

C++ scales caliber base projectile by weapon stats (`scaleProjectileForWeapon`, `destruction.hpp:258`).

### 4. Effects Physics Loop (Pymunk)

Side-view 2D solver (X/Y) with gravity for **ballistic arcs, debris kick, splash AOE** — outputs **grid-aligned destroy events** the engine consumes.

```python
# python/effects/physics_loop.py:79-248
class EffectsPhysicsLoop:
    def simulate_shot(self, projectile_id, origin, direction, samples, dt=1/120, max_time=2.5):
        # Pymunk body + circle shape, static segment sensors from voxel samples
        # Steps at 120 Hz; on impact:
        #   - computes local energy × effect multiplier
        #   - compares to break threshold (same formula as C++)
        #   - emits DestroyEvent(x,y,z, material, energy, effect)
        #   - spawns debris bodies with mass = voxelMass(material)
        # Returns SimResult(destroys[], impact_pos, flight_time, debris_settled)
```

Run demo: `python -m effects.physics_loop` → prints JSON with destroy list.

### 5. Bitcrush 32× Display Filter (Painter Export Only)

Nearest-neighbor upscale ×32 + 4-bit/channel color quantization — **preview/export only**, never baked into occupancy.

```java
// java/painter/src/voxel/painter/filter/BitcrushUpscale.java:14-16
public static final int DEFAULT_SCALE = 32;
public static final int DEFAULT_BITS_PER_CHANNEL = 4;
```

- Input: painter ortho slice render (RGB grid)
- Output: crushed PNG for retro look + raw `.vox.json` (unit cubes preserved)

### 6. Sky Tile Hemisphere + Moon Light-Source Sprite

Procedural pixel-grid sky dome (28×14 tiles) rebuilt each frame around camera; moon rendered as camera-facing billboard with light direction fed to shaders via UBO.

```cpp
// src/main.cpp:879-951
static void updateMoonSkyTile() {
    // hemisphere tiles (mat=4) + moon quad (mat=3) at g_moonDirWorld bearing
    // UBO: moonDir, moonIntensity, moonColor, moonWorldPos
}
```

Shader uses `mat` vertex attribute to switch: 0=solid, 1=water(tide), 2=bulb, 3=moon, 4=sky.

### 7. 120 Hz Frame Pacing (QPC Busy-Wait + Sleep)

```cpp
// src/main.cpp:834-865
static void paceFrame120() {
    // QueryPerformanceCounter loop; Sleep(remain - 0.7ms) then spin to exact 8.33 ms
    // Tracks min/max/avg frame time and "pace hits" (frames within budget + 0.85 ms)
}
```

---

## Key Files & Entry Points

| Area | File | Purpose |
|------|------|---------|
| Engine main | `src/main.cpp` | Vulkan init, chunk system, warehouse map build, input, render loop, destruction integration |
| Materials (C++) | `src/materials.hpp` | MaterialId enum, props table, `breakEnergyThreshold`, `voxelMass` |
| Destruction | `src/destruction.hpp` | `ProjectileDef`, `resolveVoxelHit`, weapon/ammo parsing, caliber fallbacks |
| Projectiles (Py) | `python/projectiles/defs.py` | `PROJECTILES` table, `projectiles_json()` export |
| Ammo subtypes | `python/projectiles/ammo.py` | `AMMO` table with grain/caliber/effect_tags |
| Materials (Py) | `python/projectiles/materials.py` | `MATERIALS` dict (must match C++) |
| Effects loop | `python/effects/physics_loop.py` | `EffectsPhysicsLoop.simulate_shot()` |
| Export script | `python/export_projectiles.py` | Writes `data/projectiles.json` + `data/materials.json` |
| Painter grid | `java/painter/src/voxel/painter/grid/VoxelGrid.java` | Dense cubic unit grid, material+part per cell |
| Painter filter | `java/painter/src/voxel/painter/filter/BitcrushUpscale.java` | Display-only upscale+quantize |
| Painter smoke | `java/painter/src/voxel/painter/SmokeMain.java` | Headless painter validation |
| Build engine | `scripts/build.ps1` | Runs Python export → compiles C++ (Clang) → `build/engine.exe` |
| Build painter | `scripts/build_painter.ps1` | Compiles Java → runs `SmokeMain` → `build/painter/painter_smoke_ok.txt` |
| Dev launcher | `scripts/launch_dev.ps1` | Menu / `-Action Build|Engine|Painter|SmokeEngine|SmokePainter|SmokeAll|Ui` |
| Root entry | `launch.ps1` | Forwards to `launch_dev.ps1` (legacy `-Build/-Run/-SmokeOnly` supported) |

---

## Quick Start

```powershell
# Interactive dev menu
.\launch.ps1

# Or direct actions
.\launch.ps1 -Action Build        # export Python defs + compile engine
.\launch.ps1 -Action Engine       # run engine (build/engine.exe)
.\launch.ps1 -Action SmokeEngine  # build + headless engine smoke
.\launch.ps1 -Action Painter      # build painter + smoke test
.\launch.ps1 -Action Ui           # build painter + launch Swing UI
.\launch.ps1 -Action SmokeAll     # all smoke tests
```

Working directory for engine is `build/` so `projectiles.json` and shaders resolve next to binary.

---

## Controls (Engine)

| Key | Action |
|-----|--------|
| WASD | Fly (free-float FPS) |
| LMB + drag | Look |
| RMB / F | Fire active projectile |
| 1–4 | Cycle caliber: light / medium / heavy / energy_beam |
| R | Cycle legacy projectile defs (slug, spike, shredder, charge) |
| V | Cycle loaded weapons |
| Mouse wheel | Adjust move speed |
| Esc | Quit |

---

## Extending the Engine

| Add… | Do this |
|------|---------|
| **Map piece** | Write unit voxels via `fillBox`/`placeGirderColumn`/etc. in `buildWarehouseMap()` or chunk API; mark chunk dirty |
| **Projectile/effect** | Add to `python/projectiles/defs.py` → run `python/export_projectiles.py` → rebuild |
| **Material** | Add to both `src/materials.hpp` and `python/projectiles/materials.py`; map `Block → MaterialId` in `blockMaterial()` |
| **Painted asset** | Use Painter UI → save `.vox.json` (unit cubes, `voxel_size=0.001`); optional crushed PNG is preview only |

---

## Rules Reference

See `RULES.md` for the complete non-negotiable contract (cubic unit grid, material sync, bitcrush display-only, launcher paths).