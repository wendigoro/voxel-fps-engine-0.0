# vox.json schema

Cubic unit occupancy only. `unit` is always 1; `voxel_size` matches engine `0.001`.
Cell extent on Z always equals X and Y (no non-cubic voxels).

```json
{
  "unit": 1,
  "voxel_size": 0.001,
  "mode": "model|sky|character",
  "dims": [sx, sy, sz],
  "seg_u": 28,
  "seg_v": 14,
  "moon_dir": [x, y, z],
  "moon_intensity": 0.95,
  "feet": [x, y, z],
  "voxels": [
    {"x": 0, "y": 0, "z": 0, "mat": "concrete", "rgb": 6710886}
  ]
}
```

Bitcrush 32× is a display/export filter only — it must not rewrite occupancy.
