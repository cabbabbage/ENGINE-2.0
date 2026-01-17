# Task Catalog

This file consolidates every task that used to live under `Tasks/`. Each section restates the objective, the verified scope of the work, and the subset of CSV rows that still describe applicable actions in the current ENGINE tree. Tasks that had no structured change rows are documented purely via their narrative instructions. Once this catalog is accepted, the old `Tasks/` directory can be removed entirely.

## Add Default Movement Animation Helper to Animation Editor

**Goal:** Add a "Create Defaults" workflow that lets the animation editor spin up the five canonical left-facing movement animations, mirrors them to the right, and routes the generated animations through the existing build/upload path without disturbing manual creation.

**Scope:** New HPP/CPP files live under `ENGINE/dev_mode/asset_sections/animation_editor_window`, and the work must integrate with the existing manifest/upload helpers exposed by `AnimationEditorWindow`.

### Verified change plan
**Additions**
- `ENGINE/dev_mode/asset_sections/animation_editor_window/DefaultMovementDefaultsPanel.hpp`: declare the floating panel with direction name inputs, folder/sequence pickers, and a displacement control so users can define the five base directions. (Status: pending)
- `ENGINE/dev_mode/asset_sections/animation_editor_window/DefaultMovementDefaultsPanel.cpp`: build the UI, expose the Basic Defaults action that computes per-frame displacement, generates mirrored animations and vectors, and pushes results through the existing animation_update/manifest integration helpers. (Status: pending)

**Modifications**
- `ENGINE/dev_mode/asset_sections/animation_editor_window/AnimationEditorWindow.hpp`: add a `Create Defaults` button to the header layout and the necessary panel state fields. (Status: pending)
- `ENGINE/dev_mode/asset_sections/animation_editor_window/AnimationEditorWindow.cpp`: instantiate and manage the new panel, hook the button handler to show/hide it, and feed its output into the upload/build flow. (Status: pending)

## Core Interaction Model Implementation

**Goal:** Implement the Shift-gated interaction mode in the frame editor so that camera pan/zoom remain the default, and holding Shift suspends the camera to let the user select a point, cycle axes, and adjust values using scroll wheel with grid snapping while receiving visual feedback.

**Scope:** All work touches `ENGINE/dev_mode/frame_editor_session.{hpp,cpp}` and the existing input/camera helpers that currently own plane toggles.

### Verified change plan
**Additions**
- Introduce a `PointSelectionState` structure and dedicated helpers (`select_point`, `cycle_axis`, `adjust_selected_axis`, `render_selection_feedback`) in `frame_editor_session.hpp` with matching implementations in `frame_editor_session.cpp`. (Status: pending)

**Deletions**
- Remove the legacy plane toggle button, XZ scroll handling, and plane grid rendering logic across `frame_editor_session.hpp/.cpp` because they conflict with the new Shift-based selection mode. (Status: pending)

**Modifications**
- Extend `handle_event`, `update`, and `render` in both the header and implementation to detect Shift, manage the point selection state, cycle axes, snap scroll deltas, and emit the axis-colored feedback. (Status: pending)

## Data Format Updates Implementation

**Goal:** Transition every child array, serializer, and manifest/test artifact to the new six-element format `[child_index, dx, dy, dz, degree, visible]` while simultaneously removing every `render_in_front` reference so that parsers and JSON samples share the same schema.

**Scope:** Update child frame definitions, animation loaders, asset info parsers, serialization helpers, manifest generators, and test expectations under `ENGINE/`.

**Action items**
- Update the `ChildFrame` struct and all loaders/serialization paths to emit and consume the new six-element arrays.
- Remove `render_in_front` from frame data, animation samples, manifests, and asset parsers so that it no longer affects rendering semantics.
- Refresh manifest generators and tests to assert against the new child format and omit the obsolete field.

## Fix Non-Tillable Asset Positioning to Match Tiled Assets

**Goal:** Anchor non-tillable assets to the same bottom-center point as tiled sprites by recalculating their destination rectangles while preserving smoothing and child rendering order.

**Scope:** Changes are confined to `ENGINE/render/composite_asset_renderer.cpp`.

**Approach**
- Detect when `!slot.info->tillable`, compute `dest_rect.x = smoothed_translation_x() - final_w/2` and `dest_rect.y = smoothed_translation_y() - final_h`, and pass the old rectangle into `add_render_object` so vertical smoothing and child rendering receive the previous geometry.

## Frame Editor Session Persistence Fix

**Goal:** Prevent frame editor changes from being truncated during persistence by ensuring `number_of_frames` is written and by removing redundant manifest store updates.

**Verified change plan**
- Removed the redundant manifest store lambda block around lines 360-365 in `ENGINE/dev_mode/frame_editor_session.cpp`; `AnimationDocument::save_to_file()` now exclusively owns persistence. (Status: completed)
- The payload now sets `payload["number_of_frames"] = frames_.size();` before serialization inside `persist_changes()` around lines 5278-5279, so `coerce_payload()` no longer defaults to a single frame. (Status: completed)

## Fix Lighting Flicker

**Note:** The `Tasks/fix_lighting_flicker` directory contains no `instructions.md` or CSV logs, so no requirements were captured. Follow-up context is required before this task can be migrated into actionable backlog entries.

## Fix Tile Assets Flashing and Visual Artifacts

**Goal:** Stabilize tile rendering by enforcing deterministic world-to-screen positions, depth ordering, and texture lifetime so tiles stop flashing under any camera motion.

**Approach**
- Audit `ENGINE/render/render.cpp`, `ENGINE/render/warped_screen_grid.cpp`, and `ENGINE/render/composite_asset_renderer.cpp` for non-deterministic transformations.
- Instrument `GridTileRenderer::render` and the render object queue to log `world_z_offset`, texture pointers, and variant selection so we can correlate flashes to rendering decisions.
- Clamp projection math, reorder the render queue as needed, and keep `SDL_RenderGeometry` vertices constant so textures are not destroyed/recreated each frame.
- Verify the artifacts disappear under every camera condition by replaying deterministic frame sequences.

## Improve Runtime Cache Efficiency

**Goal:** Reduce redundant conversions and metadata recomputation by adding lightweight caching, metrics, and eviction guards across core asset caches.

**Approach**
- Add cache hit/miss instrumentation inside `AssetsManager::ensure_object`, `world::WorldGrid` chunk loaders, and texture/AssetInfo loaders, but keep the logs toggleable so normal runs stay clean.
- Lazily initialize expensive AssetInfo metadata once per manifest load and reuse it wherever possible.
- Introduce eviction or compression guards for the largest caches so memory usage stays bounded while correctness is preserved.

## Input & Camera Integration Implementation

**Goal:** Tie the Shift-gated adjustment mode into the camera and input subsystems so Shift suspends pan/zoom, scroll changes axis values, and normal pan behavior resumes immediately after Shift releases.

**Scope:** `ENGINE/dev_mode/dev_controls.{hpp,cpp}`, `ENGINE/dev_mode/camera_ui.{hpp,cpp}`, and `ENGINE/dev_mode/frame_editor_session.cpp`.

### Verified change plan
**Additions**
- `ENGINE/dev_mode/dev_controls.hpp/.cpp`: add `is_shift_pressed()` helpers plus `suspend_camera_for_point_adjustment()`/`resume_camera_controls()` to gate pan/zoom when the user is modifying a point. (Status: pending)

**Deletions**
- Remove the plane-specific camera handling path in `dev_controls.cpp` so Shift+scroll no longer triggers separate plane logic. (Status: pending)
- Remove `handle_xz_scroll` from `frame_editor_session.cpp` to let the new camera helpers take control. (Status: pending)

**Modifications**
- `dev_controls.cpp::handle_sdl_event`: add Shift detection to avoid passing pan/zoom events while adjusting a point, and route the events through the new suspend/resume helpers. (Status: pending)
- `dev_controls.cpp::update`: integrate point selection state so the camera stays suspended as long as a point is active, then kicks back in. (Status: pending)
- `camera_ui.{hpp,cpp}::handle_event`: recognize Shift and transition the camera UI between adjustment and pan states without flicker. (Status: pending)

## Mode-Specific Behaviors Implementation

**Goal:** Implement per-mode point selection/adjustment semantics so Movement, StaticChildren, AsyncChildren, AttackGeometry, and HitGeometry each get intuitive Shift+click, axis cycling, scrolling, and smoothing behavior.

**Approach**
- Add mode-specific helpers in `ENGINE/dev_mode/frame_editor_session.cpp` such as `select_movement_point`, `select_child_point`, `select_attack_point`, `select_hitbox_point`, and the corresponding axis-adjustment routines. (Status: pending)
- Ensure `select_point`, `adjust_selected_axis`, and `render_selection_feedback` interpret the current mode when deciding which point to target and what visual feedback to show. (Status: pending)
- Apply smoothing consistently as soon as Shift releases or a different point is chosen.

## Optimize AnimationRuntime DRY Violation

**Goal:** Eliminate the duplicated `compute_attachment_scale` lambda inside `AnimationRuntime` by promoting the logic to a single private member function so both `advance_child_frames()` and `apply_child_frame_data()` call it instead of repeating the calculation.

### Verified change plan
**Additions**
- `ENGINE/animation_update/animation_runtime.hpp`: add a private `float compute_attachment_scale() const` declaration immediately after the class declaration to centralize the child-scaling math. (Status: pending)

**Modifications**
- `ENGINE/animation_update/animation_runtime.cpp::advance_child_frames`: replace the in-place `compute_attachment_scale` lambda with a call to the new member function. (Status: pending)
- `ENGINE/animation_update/animation_runtime.cpp::apply_child_frame_data`: likewise replace its local lambda with the shared helper. (Status: pending)

## Optimize Map Generation Performance

**Goal:** Profile and optimize the map generation pipeline by instrumenting room layout, asset spawning, and trail generation, caching repeated data, and introducing careful parallelism without compromising determinism.

**Approach**
- Add chrono-based timing logs across `ENGINE/map_generation/` (e.g., `generate_rooms.cpp`, `trails.cpp`, `map_layers_geometry.cpp`, `room.cpp`) to spot bottlenecks.
- Cache repeated JSON parsing in `AssetSpawner/AssetSpawnPlanner`, reuse area lookups, and memoize spawn queue calculations.
- Evaluate job-queue parallelism for room asset placement and geometry generation while protecting RNG and grid state.
- Surface performance metrics, ensure deterministic output, and verify caching tweaks do not change the generated maps.

## Realism Effects Cleanup Instructions

**Goal:** Remove the optional realism toggles so the engine always delivers the natural realism effects that previously required various UI controls and state tracking.

**Verified change plan**
**Modifications**
- `ENGINE/render/warped_screen_grid.cpp`: drop the `realism_enabled` check when building floor depth parameters so the pipeline applies depth effects regardless of a toggle. (Status: pending)
- `ENGINE/core/AssetsManager.cpp`: simplify `render_quality()` to always return 100%, remove the old settings reference, and drop the clamping logic. (Status: pending)

**Removals (61 entries across 13 files)**
- `ENGINE/dev_mode/camera_ui.{hpp,cpp}`: remove the hero banner, realism and depth-effect widgets, callbacks (`set_on_realism_enabled_changed`, `set_on_depth_effects_enabled_changed`), settings caching, and all toggle-driven UI rows. (Status: pending)
- `ENGINE/dev_mode/dev_controls.{hpp,cpp}`: delete the forced realism disable flags, depth-effect callbacks, parallax probe overlay, and the Realism effect change wiring. (Status: pending)
- `ENGINE/dev_mode/asset_info_ui.{hpp,cpp}`: drop the cached realism/parallax state, camera toggles, and the `cam.set_realism_enabled`/`cam.set_parallax_enabled` calls. (Status: pending)
- `ENGINE/dev_mode/frame_editor_session.{hpp,cpp}`: remove the previous realism state tracking fields and the methods that forced camera realism toggles. (Status: pending)
- `ENGINE/dev_mode/room_editor.{hpp,cpp}`: delete the cached realism/parallax flags and the change detection logic that toggled camera realism. (Status: pending)
- `ENGINE/dev_mode/foreground_background_effect_panel.cpp`: strip the commented-out realism settings scaffolding. (Status: pending)
- `ENGINE/render/warped_screen_grid.{hpp,cpp}`: erase the `realism_enabled`/`parallax_enabled` getters, setters, flags, and any references in `RealismSettings`. (Status: pending)

## Remove Boundary/Impassable System Implementation

**Goal:** Eliminate the old boundary/impassable system by switching to collision_area queries for pathfinding, deleting the manifest/UI bits, and cleaning up asset spawning hooks.

**Scope:** `ENGINE/main.cpp`, `ENGINE/core/AssetsManager.cpp`, `ENGINE/core/asset_loader.*`, `manifest.json`, `ENGINE/dev_mode/dev_controls.cpp`, `ENGINE/room_editor.cpp`, `ENGINE/map_generation/generate_rooms.cpp`, `ENGINE/AssetSpawner`, `ENGINE/Asset.hpp`, `ENGINE/spawn/spawn_context.hpp`, and the animation update/pathfinding helpers.

### Verified change plan
**Additions**
- `ENGINE/animation_update/{animation_runtime.cpp,path_sanitizer.cpp,get_best_path.cpp}`: add `collision_area` queries so the old impassable logic is replaced everywhere the runtime previously asked about map boundaries. (Status: pending)

**Removals**
- `ENGINE/main.cpp`, `ENGINE/core/AssetsManager.cpp`, `ENGINE/core/asset_loader.*`, and `manifest.json`: remove every `map_boundary_data` reference. (Status: pending)
- `ENGINE/dev_mode/dev_controls.cpp` and `ENGINE/room_editor.cpp`: delete the boundary button, modal hooks, and panel creation code. (Status: pending)
- `ENGINE/map_generation/generate_rooms.cpp`, `ENGINE/AssetSpawner`: stop calling `spawn_boundary_from_json` and remove boundary-specific generators. (Status: pending)
- `ENGINE/Asset.hpp`: delete `get_impassable_neighbors`. (Status: pending)
- `ENGINE/spawn/spawn_context.hpp`: drop impassable filters for spawn logic. (Status: pending)

**Modifications**
- `ENGINE/dev_mode/dev_controls.cpp` and `ENGINE/room_editor.cpp`: remove the UI hooks that opened the boundary panels and the button handlers. (Status: pending)
- `ENGINE/map_generation/generate_rooms.cpp`: clean the `regenerate` command so it no longer spawns boundaries. (Status: pending)

## Rendering & Math Changes Implementation

**Goal:** Remove the plane-based math entirely by deleting plane enums/UI, simplifying camera defaults, and ensuring child positions are computed with full 3D math before projection so children render once and do not rely on `render_in_front` tweaks.

**Approach**
- Delete `render_plane_grid()`, all plane projection helpers, and plane labels across the codebase.
- Calculate each child position as `parent_world + scaled(dx, dy, dz)` in 3D and then project to 2D with consistent axis scaling.
- Drop every `render_in_front` shortcut (e.g., in `composite_asset_renderer.cpp` and preview code) and rely on stable 3D math.

## Selection State & Visualization Implementation

**Goal:** Track the selected point, current axis, and adjustment state while overlaying axis colors, ghost previews, and HUD value readouts so that the user sees exactly what will move.

### Verified change plan
**Additions**
- `ENGINE/dev_mode/frame_editor_session.hpp`: declare `render_point_highlight`, `render_axis_indicators`, `render_value_display`, and `render_directional_arrows` to provide the new visual helpers. (Status: pending)
- `ENGINE/dev_mode/frame_editor_session.cpp`: implement every rendered cue so highlights, axis indicators, value readouts, and directional arrows appear in the frame editor while scrolling. (Status: pending)

**Modifications**
- `ENGINE/dev_mode/frame_editor_session.cpp::render`: hook the new helpers into the existing rendering pass so the selected point is highlighted and the HUD shows axis/value context. (Status: pending)

## Smoothing Triggers Implementation

**Goal:** Run smoothing when a selection is cleared (Shift released, clicking away, or selecting a different point) for every mode that supports interpolation so that the interaction feels smooth across movement, child, and attack timelines.

**Approach**
- Identify which modes actually need smoothing and document where it can be a no-op.
- Factor smoothing into a shared helper that executes after every deselect action.
- Keep the smoothing fast enough to avoid blocking the UI while the preview updates.

## State & Rendering Pipeline Implementation

**Goal:** Implement a reusable selection state machine that captures the selected point, axis, and adjustment state, integrates with the rendering pipeline, and dispatches smoothing whenever the state transitions to idle.

**Approach**
- Drive the state machine from input events, track axis scroll deltas snapped to the configured grid, and feed the state into the main render loop.
- Ensure rendering pays attention to the machine, yielding directional arrows and colored indicators whenever adjustments are in flight.
- Stitch smoothing into the machine so deselecting triggers the appropriate cleanup.

## Testing & Manifest Updates Implementation

**Goal:** Update manifests and tests so they agree with the six-element child format, the absence of `render_in_front`, and the new interaction model.

**Approach**
- Refresh unit/integration tests to expect `[child_index, dx, dy, dz, degree, visible]` for children and to drop any `render_in_front` assertions.
- Add tests that cover Shift-scoped point selection, axis cycling defaults to X, scroll-based adjustments with grid snapping, and smoothing on Shift release or losing selection.
- Update manifest generation and sample animation JSON to match the new schema and to reference the sanitized controller keys when applicable.

## UI & Widget Cleanup Implementation

**Goal:** Remove plane toggle widgets and the `render_in_front` checkbox/caches so the new Shift-based controls replace the old grid-specific UI.

**Approach**
- Drop `btn_plane_toggle_`, plane toggle creation inside `ensure_widgets()`, and the related metrics/labels that exposed plane state.
- Remove `cb_child_render_front_`, `last_child_front_value_`, and their usage in `ensure_widgets()`, `rebuild_layout()`, `update()`, and `handle_event()`.
- Ensure the toolboxes no longer report plane/render-in-front metrics.

## Update Animation Editor Custom Controller Creation

**Goal:** Normalize controller keys by sanitizing asset names so new controllers are consistently named, yet the factory can still fall back to legacy `_controller` keys.

**Approach**
- Make `AnimationEditorWindow::generate_controller_key`/`add_controller` and `info_ptr->custom_controller_key` rely on the sanitized asset name.
- Update `AnimationEditorWindow` to name generated headers/source files to match the new keys and include the newer animation_update helpers (e.g., `auto_move.h`).
- Refactor `ControllerFactory::create_for_asset`/`create_by_key` to map asset names to controllers, while keeping compatibility with the pre-existing `_controller` keys curated in `manifest.json`.

## Code Cleanup Assessment - No Legacy Code Found

**Summary:** A repo-wide search for keywords like "deprecated", "legacy", "fallback", conditional compilation, and TODO/FIXME comments found no legacy or deprecated code paths that require removal. No ENGINE/ files need changes as part of this task. (Status: informational)
