# Engine-Wide Axis Transition Finalization Plan

This plan enumerates the remaining cleanup required to fully lock the engine to canonical world-space semantics:

- `X`: right
- `Y`: height (vertical)
- `Z`: depth (forward/back)

It is intentionally exhaustive and uses concrete code touchpoints from the current tree.

## 1) Runtime core coordinate API lock-down

1. Replace ambiguous 2D world helper APIs that still imply `X/Y` planar coordinates.
   - Example: `Asset::world_point()` currently returns `{world_x(), world_y()}` even though planar world navigation logic often operates on `X/Z` depth space.
   - Task: split into explicit helpers (`world_xz_point`, `world_xy_point`) or remove ambiguous helper entirely.
2. Standardize constructor/method parameter names so any `world_y` parameter is always vertical height, and depth parameters are always `world_z`.
3. Remove any remaining comments that mention migration/legacy interpretation.

## 2) Runtime bootstrap and camera/view initialization

1. Update startup positional naming and propagation so no depth variable uses `*_y` names.
   - Example: `start_py` is sourced from `player_ptr->world_y()`.
   - Task: rename bootstrap variables and propagate the matching semantic argument names through camera/assets initialization.
2. Confirm that initial camera/world center placement uses canonical `(x, y-height, z-depth)` all the way through constructor boundaries.

## 3) Spawn, map generation, spatial checks, and occupancy

1. Normalize planar/depth naming in spawn helpers.
   - Example: spawn code uses `SDL_Point{world_x(), world_z()}` for plane logic while neighboring variables still use `min_y/max_y` naming from room-area APIs.
   - Task: replace axis-ambiguous local names with explicit `depth` names where data is depth.
2. Audit all `world_to_index` and occupancy calls to ensure there is no hidden axis reinterpretation in adapters.
3. Ensure spawn debug logs and inspector output label `height` and `depth` correctly.

## 4) Room editor, map editor, save/load and manifest flow

1. Finalize room placement serialization naming and conversion boundaries.
   - Example: `RoomEditor::update_exact_json` derives `dz` from `asset.world_y() - center.y`.
   - Task: migrate room-center and area APIs to explicit depth-bearing types so `dz` is sourced from `world_z()` and height remains on `world_y()`.
2. Remove axis-ambiguous local variables in percent/perimeter serialization helpers (`dy`, `percent_y`) when representing depth.
3. Verify map mode, room mode, and save coordinator all pass raw canonical values with no remap layers.
4. Confirm manifest map blobs and room payloads only contain canonical axis keys and labels.

## 5) Rendering pipeline, sorting, projection and overlays

1. Audit depth-sort and projection inputs so all depth terms are fed by `world_z` and all vertical elevation by `world_y`.
2. Rename overlay/editor intermediate variables where `y` currently means depth due to screen-space historical carryover.
3. Confirm inspector/tooltips/gizmos display axis labels matching canonical meaning.

## 6) Dev Mode tools and inspectors

1. Standardize axis labels in panels, inspectors, and editor widgets:
   - Input labels should explicitly say `Height (Y)` and `Depth (Z)` where both are exposed.
2. Update gizmo/preview coordinate readouts to avoid mixed axis language.
3. Ensure child asset binding, anchors, and helper snapping read/write canonical axes directly.

## 7) Serialization/deserialization and schema hardening

1. Ensure all coordinate-bearing payloads explicitly encode canonical fields with no synonym fallback path.
2. Align comments/docs with runtime behavior so docs do not describe swapped semantics.
3. Add one authoritative schema note for world coordinates that all serializers reference.

## 8) Terminology and naming cleanup (code + docs)

1. Replace legacy or ambiguous variable names (`py`, `dy` when depth, `world_point` for ambiguous 2D projection) with explicit axis names.
2. Remove stale “migration in progress” language now that canonical semantics are the only supported behavior.
3. Keep only one source-of-truth axis document and link all subsystem docs to it.

## 9) Validation of direct end-to-end flow (load → edit → runtime → render → save)

1. Manually verify representative scenarios:
   - map load + room load
   - room edit and spawn-group edit
   - runtime movement/spawn/collision
   - render sort and depth-cue behavior
   - save and reload equivalence
2. Confirm no path performs implicit Y/Z translation.

## 10) Repository policy alignment

1. Keep engine behavior strict and native to canonical format only.
2. Do not reintroduce compatibility swaps, legacy adapters, or marker-based gatekeeping logic.

## Concrete references from current code audit

- `ENGINE/runtime/assets/asset/Asset.hpp`
- `ENGINE/runtime/app/main.cpp`
- `ENGINE/runtime/gameplay/spawn/map_wide_asset_spawner.cpp`
- `ENGINE/runtime/gameplay/spawn/check.cpp`
- `ENGINE/editor/devtools/room_editor.cpp`

These files contain current high-signal axis naming/semantic hotspots and should be prioritized first in the remaining sweep.
