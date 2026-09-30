# Historical reference material

Nothing in this directory builds, ships, or is read by the engine or the painter. These
files are kept only so the work is not lost, and so the design decisions behind them stay
recoverable.

## `README_project_overview.txt`

Project overview written during a documentation pass in an abandoned worktree. Kept as-is,
including its original claims; verify anything you intend to rely on.

## `legacy_hitbox_prototype.patch`

A C++/Python prototype of player hitboxes and self-damage: capsule/body hit resolution,
fall damage, and drowning, plus the material and projectile-table changes it needed.

**Superseded.** The current implementation is `src/health.hpp`, which is the authority for
player health, damage, armour zones, fall and drown damage, and death/respawn. Do not apply
this patch. It exists so the original approach can be compared against the implementation
that replaced it.

## `painter_hitbox_prototype.patch`

The painter-side half of the same prototype: `HitboxOverlay` plus the document and UI
wiring needed to author and preview hitboxes.

**Not ported.** Hitboxes are not a painter responsibility in the current design, so no part
of this is in the current tree.

## `movement_prototype_extras.patch`

The six commits from the abandoned movement worktree (`4b71449`..`c3b1922`), as
`git format-patch` output. Do not apply them: they write `g_camPos` directly, collide against
the view's `g_chunks`, and poll input inside the update, which are all things `RULES.md` now
forbids. Where their behaviour was wanted it has been re-ported instead:

- `4b71449` stance, gait, slide, wallrun, dash: **superseded** by `src/movement.hpp` (PR #4).
- `3d93ccf` cursor lock and ESC pause: **re-ported** view-side. Esc pauses the host loop (no
  ticks run), Shift+Esc quits, and a click locks the mouse, with raw input feeding
  `SimInput`. The original never actually locked (`g_cursorLocked` started `true`).
- `cc23298` smoothed head-follow camera: **re-ported as its intent**. The rendered eye
  interpolates between ticks instead of an exponential follow, which added about 66 ms of lag. The
  animated head it followed does not exist in the current tree.
- `c3b1922` water collision border: **superseded**. `isSolidBlock` already excludes water,
  and `move_water_not_solid` gates it.
- `e40bace` fullscreen startup menu: **not ported**.
- `f5397e7` stamina-driven idle animation: **not ported**. It animated a character model that
  the current tree does not render.
