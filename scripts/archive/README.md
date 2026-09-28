# Archived one-shot codegen scripts

These scripts have **already been applied** to the tree. They are kept for history only.

Each one mutates `src/main.cpp` (or the shaders, or the launcher scripts) by exact
string match and then exits. Because they match on literal text, they break
immediately if the file is restructured — `scripts/build.ps1` and
`scripts/demo.ps1` never invoke them, and nothing in the build references them.

| Script | What it produced |
|--------|------------------|
| `_patch_culling_120.py` | per-chunk `firstVertex` draw ranges |
| `_patch_culling_120_full.py` | rewrote `struct Chunk` + `meshAllChunks` |
| `_patch_engine_features.py` | water/current, character units, `mat` vertex channel |
| `_patch_night_lighting.py` | `FrameUBO` moon/bulb fields |
| `_patch_bench_timing.py` | frame-time telemetry |
| `_write_shaders.py` | earliest `shaders/voxel.*` (superseded) |
| `_write_night_shaders.py` | current `shaders/voxel.vert` + `voxel.frag`, incl. analytic shadow code |
| `_write_launchers.py` | `scripts/demo.ps1` + `scripts/run.ps1` |

## Do not re-run

Running any of these will fail (loudly, via `raise SystemExit("missing: ...")`)
or, worse, partially re-apply against text that has since moved. The current
shaders, launcher scripts, and engine source are the authoritative versions —
edit them directly.

If the view/sim split needs a genuine shader or source generator, write a real
one under `tools/` that reads structured input and emits whole files. That is a
different thing from what these scripts were.
