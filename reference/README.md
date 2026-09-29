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
