# Animation Clip Format Specification (`.anim.json`)

## Overview

Animation clips in Voxel FPS Engine 0.0 define skeletal bone tracks for both the **humanoid character sub-lattice** (`char_model::BoneId`) and **weapon model parts** (`weapon_view`).
Clips are purely view-side presentation data (per `RULES.md` "Render layers: Models layer"). Simulation state never reads animation transforms.

## Schema

```json
{
  "format_version": 1,
  "id": "clip_identifier",
  "name": "Human-Readable Name",
  "duration_sec": 1.5,
  "loop": true,
  "frame_rate": 30.0,
  "tracks": [
    {
      "bone": "bone_name",
      "frames": [
        {
          "time": 0.0,
          "pos": [0.0, 0.0, 0.0],
          "rot": [0.0, 0.0, 0.0, 1.0]
        }
      ]
    }
  ]
}
```

### Fields

* **`format_version` (integer):** Version of this file schema (currently 1).
* **`id` (string):** Unique clip identifier (e.g. `rifle_idle`, `rifle_fire`, `rifle_reload`, `locomotion_walk`).
* **`name` (string):** Descriptive name for tooling and editor display.
* **`duration_sec` (float):** Total length of the animation cycle in seconds.
* **`loop` (boolean):** `true` if this animation is designed to loop continuously (e.g. idle, walk), `false` for one-shot actions (e.g. fire, reload).
* **`frame_rate` (float, optional):** Reference sample frame rate (default 30.0).
* **`tracks` (array):** List of bone tracks.

### Track Specification

* **`bone` (string):** Target bone identifier:
  * **Humanoid Body:** `root`, `pelvis`, `spine`, `chest`, `neck`, `head`, `l_shoulder`, `l_upper_arm`, `l_forearm`, `l_hand`, `r_shoulder`, `r_upper_arm`, `r_forearm`, `r_hand`, `l_thigh`, `l_calf`, `l_foot`, `r_thigh`, `r_calf`, `r_foot`.
  * **Weapon Rig:** `weapon_root`, `weapon_bolt`, `weapon_mag`.
* **`frames` (array):** Ordered list of keyframes sorted by ascending `time`:
  * **`time` (float):** Time offset in seconds from $0.0$ to `duration_sec`.
  * **`pos` ([float, float, float]):** Relative local translation $[x, y, z]$ in meters ($1\text{ mm} = 0.001$).
  * **`rot` ([float, float, float, float]):** Normalized rotation quaternion $[q_x, q_y, q_z, q_w]$. Identity rotation is $[0.0, 0.0, 0.0, 1.0]$.
