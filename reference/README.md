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
`git format-patch` output. The first commit, stance/gait/slide/wallrun/dash, is **superseded**
by `src/movement.hpp` (see PR #4 for why it was ported rather than merged). The other five
were **not ported**, and nothing like them is in the current tree:

- `e40bace`: single exe with a fullscreen startup menu (`g_appState`, menu vertex buffer).
- `3d93ccf`: cursor lock and ESC pause (`g_cursorLocked`, `g_paused`, `togglePause`).
  Today ESC quits.
- `cc23298`: a smoothed camera that follows the animated head (`g_cameraSmoothedPos`).
- `f5397e7`: a stamina-driven idle animation and a camera clipping fix.
- `c3b1922`: removes a collision border around water (`isSolidForPhysics`).

Do not apply these as they stand. They write `g_camPos` directly, collide against the view's
`g_chunks`, and poll input inside the update, which are all things `RULES.md` now forbids. Port
the behaviour through `SimInput`, the authoritative grid, and view-side offsets instead.
