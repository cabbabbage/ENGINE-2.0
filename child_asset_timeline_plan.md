# Child Asset Timeline Overhaul Plan

## Mission
Replace every legacy child handling path with a flat, timeline-driven child asset model where:
- `child_timelines` entries are the single source of truth for child behavior.
- Child assets join the regular active list once their parent activates and run entirely through the standard update loop.
- Parents never push updates to children after activation; children read their parent’s timeline data during their own `Asset::update()`.

## Guiding Constraints
- **Zero legacy handling.** Delete every read or write path for movement frame child entries, legacy helpers, and ad hoc child placement code. Nothing should remain that reconciles old data, compares frame children with timelines, or tries to normalize both formats.
- **Child timelines only.** Saved manifests keep only `child_timelines` (static + async) with slot identifiers, child references, per-frame visibility/offsets, and async trigger config.
- **Normal asset behavior.** Children are treated as regular active assets. After a parent activates its children, no other code should target them until they run through the shared update list.
- **Explicit async triggers.** Async timelines start only when triggered; once running, they advance each frame through completion, then stop and hide until retriggered.
- **Derive runtime data only.** Per-frame attachment caches must be rebuilt as needed by `Animation::rebuild_frames_from_child_timelines()` and never written to disk or consumed as authoritative sources.

## Phase 0 – Clean Slate
1. **Remove legacy movement frame child usage**
   - Delete every read of movement frame child entries across editor, document rewrite, runtime, and serialization.
   - Remove every serialization path that writes child offsets/visibility into legacy structures.
   - Purge helper functions, structs, or classes whose sole purpose is legacy reconciliation.
2. **Document the new source of truth**
   - Clearly state in comments and dev documentation that `child_timelines` is canonical and that legacy slots are gone.
   - Add lint checks or asserts where practical to ensure legacy paths don't slip back in.

## Phase 1 – Data Modeling & Persistence
1. Ensure animation documents expose `child_timelines` payload in a single canonical shape:
   - Normalize slot keys, type fields, ordering, and field names (visibility + `dx/dy/dz` offsets).
   - Panic if any upstream source still tries to emit legacy child data.
2. Update `FrameChildrenEditor`:
   - Load all UI state exclusively from `child_timelines`.
   - Persist only `child_timelines` when serializing changes; do not touch movement frame entries.
   - Drop UI assumptions/UI that relied on legacy child structures.
3. Update `AnimationDocument::rewrite_child_payloads`:
   - Rewrite child payloads based on `child_timelines` only.
   - Remove any normalization logic that consulted legacy movement frames.

## Phase 2 – Activation & Lifecycle
1. When a parent asset becomes active:
   - Instantiate any missing child assets referenced in `child_timelines`.
   - Add them immediately to the flat active asset list (no recursive calls).
2. Ensure parent activation is the sole point of child registration; after this, children exist independently.
3. Update asset creation flows (manifest ingestion, preview modes, dev-mode loaders) to honor this parent-first activation.

## Phase 3 – Update Ordering
1. Update `AssetsManager::rebuild_non_player_update_buffer_if_needed` and related scheduling:
   - Include children in the update buffer.
   - Order assets so parents always run before their descendants (e.g., via parent depth or topological sort).
2. Remove any `child->update()` invocations issued from parent helpers (e.g., `Asset::sync_child_from_slot`).
3. Keep children in the renderer active set so they render normally after they update.

## Phase 4 – Child Attachment Logic
1. Update `Asset::update()` (or a helper invoked early in update):
   - Step 1: If `parent != nullptr`, sample the parent’s timeline entry (current frame/time) for this slot.
   - Step 2: Determine current visibility (respect the parent’s visibility guard—hidden/inactive parents force children hidden).
   - Step 3: Compute world position: parent’s world transform (including `world_z`) plus `(dx, dy, dz)` from the timeline.
   - Step 4: Apply visibility/transform changes before proceeding with the rest of the normal update logic.
2. Keep the rest of `Asset::update()` identical to other assets once child attachment is resolved.
3. Ensure no other subsystem overrides or duplicates these calculations.

## Phase 5 – Async Child Timelines
1. Ensure async entries store trigger configuration only; no auto-start fields exist.
2. Runtime:
   - Async child timelines stay paused until an explicit `run_child_animation` (or equivalent) trigger fires.
   - Once triggered, they advance frame by frame automatically until completion.
   - After completion, the child remains hidden until the next trigger.
3. Dev-mode editor:
   - UI should disable/ignore auto-start controls for async mode.
   - Persisted data must reflect trigger-only semantics (no `auto_start`).
4. Update validation/serialization to reject conflicting async data.

## Phase 6 – Runtime Derivation
1. Identify systems that previously consumed per-frame child caches.
2. Call `Animation::rebuild_frames_from_child_timelines()` after:
   - Loading or reloading animation payloads.
   - Persisting child edits.
   - Running animation graph updates that invalidate caches.
3. Make derived frame child data read-only caches; never serialize or treat as source-of-truth.

## Phase 7 – Supporting Cleanup
1. Add or update tests verifying:
   - Parents activate children once and then leave them to the normal update loop.
   - Children read timeline data during `Asset::update()` to compute visibility/position.
   - Async children require triggers, play to completion, hide after finishing, and resume only on retrigger.
2. Remove or rewrite documentation referencing old child frames, `render_in_front`, or parent-driven placement.
3. Walk codebase to delete any call sites applying child offsets outside of the child’s update path.

## Verification Checklist
- Manual review and compile: ensure no references to legacy movement frame child data exist anywhere.
- Runtime:
  - Parents update before children.
  - Child assets compute visibility/position during their own update, based purely on parent timeline data.
  - Children render normally once updated.
- Dev mode:
  - `FrameChildrenEditor` loads/persists only `child_timelines`; no fallback logic.
  - Async children start via trigger, animate to completion, then stop/hide until retriggered.
- Data: saved manifests contain only `child_timelines` entries; runtime caches derived from them only.

## Definition of Done
- Only `child_timelines` survive as child data; legacy entries are removed or updated to the new format.
- Parents only activate children once and otherwise ignore them.
- Children read their parent timeline data during their own `Asset::update()` and skip recursive parent-driven updates.
- Async timelines require explicit trigger, play fully, and stop/hide until retriggered.
- Child attachment evaluation runs in one place, with no duplicated logic or legacy helpers remaining.
