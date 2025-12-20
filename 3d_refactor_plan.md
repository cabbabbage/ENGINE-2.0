# 3D GridPoint Refactor Plan

This document defines the target design and implementation plan for migrating the engine from the current 2D, chunked grid to a hierarchical 3D GridPoint system.

All Codex tasks that touch grid, camera, parallax, NDC, renderer, lighting, or dev tools should:

- Reference this file by name: `3d_refactor_plan.md`.
- Confirm that their changes match the intent and constraints described here.
- Mention which phase they are implementing or supporting.

Backwards compatibility policy:

- While we are migrating, try to keep existing assets, maps, and game logic behaving as close as possible to what they do now.
- However, we do not want to permanently maintain two grid systems.
- The final state should have everything on the new 3D GridPoint system, with no legacy 2D grid logic or duplicate path for core functionality.
- If a legacy system can be kept as a thin adapter on top of the 3D grid without adding complexity, fine. If it fights the new design or forces duplicate logic, we delete it and refactor.

Current baseline (Dec 2025): the engine uses `world::WorldGrid` (power of two spacing) with `world::GridPoint` nodes stored in an `unordered_map` keyed by 2D `(i, j)` plus `ChunkManager` for tiles. `WarpedScreenGrid` rebuilds flat lists of `GridPoint*` and assets each frame and sets per frame screen data directly on those nodes. There is no `world_z`, no hierarchy, and perspective is effectively disabled.

The goal is to replace the current 2D grid and resolution system with a 3D hierarchy that:

- Uses a shared Map Grid and Screen Grid.
- Adds a real `world_z` dimension while keeping most assets on the floor plane (`world_z = 0`).
- Uses resolution layers and spacing based on powers of 3.
- Allows very fast spatial search using active branch flags.
- Keeps perspective, parallax, and NDC logic in the camera modules, not in the grid.
- Ends with a single 3D grid model and no permanent legacy 2D grid paths.

---

## 1. Target architecture overview

### 1.1 Big picture

- The whole world and the current camera view share one spatial structure: a hierarchy of `GridPoint` objects.
- Map Grid owns all `GridPoint` instances and defines their fixed world positions `(world_x, world_y, world_z, resolution_layer)`.
- Screen Grid is rebuilt every frame and holds references to the subset of `GridPoint`s that matter for the current camera.
- Camera and NDC code handle all perspective, parallax, and warping. Grids only store the final results: `screen_x`, `screen_y`, `distance_from_camera`, `on_screen`.

### 1.2 Current engine snapshot

- `ENGINE/world/grid_point.hpp`: flat 2D struct with `id`, `world` (`SDL_Point`), `grid_index`, `chunk_index`, `chunk*`, per frame camera fields (`screen`, `parallax_dx`, `vertical_scale`, `horizon_fade_alpha`, `perspective_scale`, `distance_to_camera`, `tilt_radians`, `on_screen`, `screen_data_valid/frame`), and `std::vector<Asset*> occupants` or an equivalent pointer container. No `world_z`, no hierarchy, no resolution layer.
- `ENGINE/world/world_grid.cpp`: `WorldGrid` stores `GridPoint` in `points_` keyed by `(i, j)` (`GridId`), maps assets to points (`asset_to_point_`) and chunks (`residency_`), and uses `ChunkManager` with power of two spacing from `vibble::grid::delta`. Chunk tiles live in `Chunk::tiles` and are consumed by renderers.
- `ENGINE/render/warped_screen_grid.cpp`: builds `warped_points_`, `visible_points_`, and `visible_assets_` each frame by iterating `world_grid.all_assets()`. Perspective code paths exist but are largely disabled (`kForceDepthPerspectiveDisabled = true`). Uses current per frame fields on `GridPoint` directly.
- `ENGINE/core/AssetsManager.cpp`: `rebuild_active_from_screen_grid` reads `WarpedScreenGrid::grid_visible_points()` and sorts assets by `GridPoint::screen.y`. Dev controls consume the same lists. Asset lookup uses `Asset::grid_id` -> `WorldGrid::point_for_asset`.
- `ENGINE/render/render.cpp`: `SceneRenderer::render` calls `WarpedScreenGrid::rebuild_grid`, renders tiles from `ChunkManager`, and renders `active_assets` without grid traversal. `LightMap` is a stub that only mirrors active chunks.
- `ENGINE/utils/map_grid_settings.*` and `ENGINE/utils/grid.*`: all spacing uses power of two deltas. `MapGridSettings` stores `resolution`, `r_chunk`, and jitter for the current grid.
- `ENGINE/render/composite_asset_renderer.cpp`: consumes per frame projection data (`perspective_scale`, `distance_from_camera`), but today those fields are filled only with 2D data and no `world_z`.

### 1.3 GridPoint snapshot (target vs current)

Target:

- World identity (immutable): `resolution_layer`, `world_x`, `world_y`, `world_z`, parent pointer, and six child pointers (`x_child_neg/pos`, `y_child_neg/pos`, `z_child_neg/pos`).
- Per frame camera data: `screen_x`, `screen_y`, `distance_from_camera`, `on_screen`.
- Asset and branch data: `assets_here` (backed by a pointer container), `children_with_assets`, `active_child_mask` with branch bits for the six child directions.

Current reality:

- `GridPoint` has `id`, 2D positions, chunk indices, per frame camera fields, and `occupants` but no hierarchy, no `world_z`, and no branch mask. `screen_data_valid` is used as a per frame cache flag.
- `chunk_index`, `grid_index`, and `chunk*` are used by `WorldGrid`, `ChunkManager`, `WarpedScreenGrid`, and `GridTileRenderer` and must stay valid during the migration.
- For now, `GridPoint` identity is effectively defined both by the legacy `(grid_index, chunk_index)` and by any new `(world_x, world_y, world_z, resolution_layer)` fields we add. They must remain consistent until Cleanup.

### 1.4 Resolution vs hierarchy

- `resolution_layer` expresses sampling distance (target: powers of 3) and is separate from the existence of children. Child pointers define the hierarchy.
- A node can have a valid `resolution_layer` but no children yet (lazy creation). The tree grows as assets or queries demand it.
- During migration, power of two spacing (`vibble::grid::delta`) and chunk indices will coexist with `resolution_layer` until all call sites switch to 3^n spacing.

### 1.5 Map Grid snapshot

Target:

- A persistent Map Grid that owns all `GridPoint`s across `(x, y, z, resolution_layer)` with spacing `distance(layer) = 3^(max_layers - layer)`.
- Nodes are indexed by `(world_x, world_y, world_z, resolution_layer)` and can be prebuilt or created lazily.

Current reality:

- `WorldGrid` uses power of two spacing (`vibble::grid::delta`) and `ChunkManager` to build chunks keyed by `(i, j)`; `GridPoint` identity is `(grid_index.x, grid_index.y)` only. `MapGridSettings` and related loaders assume power of two spacing. Chunks carry tiles for the renderer.
- During migration, tile chunks remain 2D indexed by `(i, j)` and are associated with the relevant `GridPoint` hierarchy on the floor plane. Tiles do not become fully 3D; they are mapped onto the grid.

### 1.6 Screen Grid snapshot

Target:

- Screen Grid is rebuilt each frame from the Map Grid, holds references only, and updates per frame camera fields on the referenced `GridPoint`s.

Current reality:

- `WarpedScreenGrid::rebuild_grid` iterates all assets, projects their points to screen, fills `visible_points_` and `visible_assets_`, and tracks `active_chunks_`. There is no separate Screen Grid type or hierarchy; everything is a flat list keyed by `GridPoint::id` and vector indices.
- Early versions of Screen Grid will likely focus on `world_z = 0` (floor plane) only. Higher `world_z` usage will come later as camera and lighting become fully 3D aware.

### 1.7 Grid search model

Target:

- Active branch masks drive traversal: skip nodes with no assets and no active children; attach and detach propagate branch activation upward. Exact lookup is `(x, y, z, layer)` via index or child traversal.

Current reality:

- Lookups are by `(i, j)` `GridId` or the `asset_to_point_` map. Region queries and render loops scan all assets or visible lists. No branch masks exist yet.

---

## 2. How to use this plan in Codex tasks

Every Codex task related to this refactor should:

1. State which phase it targets (for example: `Phase 3 - Asset attachment and migration`).
2. Reference this plan by name: `3d_refactor_plan.md` and the relevant phase.
3. Confirm that:
   - New APIs match the data model here.
   - There are no hidden assumptions that break 3D support.
   - Floor assets still behave correctly with `world_z = 0`.
   - No new permanent dependency is added to legacy 2D grid logic.

---

## 3. Implementation phases

The phases below describe the high level goal, how the phase fits into the refactor, and the detailed subtasks. Each individual Codex task should pick a small subset of these subtasks and implement them.

---

### Phase 1 - Define 3D GridPoint core

**Goal**  
Introduce the 3D `GridPoint` shape and helpers while keeping current 2D users working. This replaces the current flat layout with a canonical world identity, hierarchy pointers, per frame fields, asset lists, and branch masks.

**Status (Dec 2025)**  
- Identity fields (world_x, world_y, world_z, resolution_layer) and constructor are in place and wired through WorldGrid creation; legacy ids remain intact.  
- Hierarchy pointers, direction helpers, and per-frame reset/frame stamping are implemented.  
- Asset/branch tracking fields, branch bits, and `assets_here` alias are present; per-frame cache reset is invoked during Screen Grid rebuilds.  
- Resolution layer defaults mirror current grid resolution; spacing helpers and distance fallback to legacy power-of-two spacing are available for migration.
- Child allocation helpers exist; attach/detach propagation will be finalized in later phases as hierarchy and branch masks become authoritative.  
- 3D GridKey mapping coexists with legacy GridId; lookups warn on legacy fallback.  
- Identity/mask validation helpers exist; legacy identity usage is tracked in cleanup tasks.

**Status (Phase 3 - Asset attachment and migration)**  
- Asset spawn/move paths attach via 3D GridPoints (GridKey) with `world_z` and `resolution_layer` parameters (default `world_z = 0`, default layer).  
- Asset bindings maintain both legacy `GridId` and 3D `GridKey`, preferring 3D for lookups; legacy fallbacks are logged.  
- Attach/detach flows invalidate per-frame data and update branch activity; hierarchy links are required for accurate masks.  
- Remaining legacy caches (e.g., 2D grid residency) still exist and must be upgraded in cleanup phases.

**How it fits**  
Every phase depends on `GridPoint` being stable and consistent. Do this first, even if many fields are not yet used by callers.

**Key files**  
`ENGINE/world/grid_point.hpp`, `ENGINE/world/world_grid.cpp`, `ENGINE/render/warped_screen_grid.cpp`, `ENGINE/core/AssetsManager.cpp`, `ENGINE/render/composite_asset_renderer.cpp`

**Subtasks**

- Add 3D identity fields  
  - Add `world_z` and `resolution_layer` with immutable semantics once set.  
  - Keep existing `id`, `grid_index`, `chunk_index`, and `chunk*` as legacy fields until Cleanup.  
  - Provide a constructor or factory that sets `(world_x, world_y, world_z, resolution_layer, parent)` and const accessors for identity.  
  - Note clearly that identity is defined both by legacy and new fields during migration, and they must match.  
  - Mark any mutators for legacy fields as deprecated where possible.

- Add hierarchy pointers  
  - Add `parent` plus six child pointers (`x_child_neg/pos`, `y_child_neg/pos`, `z_child_neg/pos`).  
  - Initialize them to `nullptr` and document that Map Grid owns all nodes; Screen Grid only references them.  
  - Add `set_child(direction, GridPoint*)` and `child(direction)` helpers to centralize direction to pointer mapping.

- Clarify per frame camera fields  
  - Keep `screen`, `parallax_dx`, `vertical_scale`, `horizon_fade_alpha`, `perspective_scale`, `distance_to_camera`, `tilt_radians`, `on_screen`.  
  - Document that these are per frame only and must be reset or invalidated when the camera changes.  
  - Add an explicit `frame_stamp` update helper and a reset path used by Screen Grid rebuilds.

- Add asset and branch tracking fields  
  - Keep `occupants` as the canonical asset pointer list for now and alias it as `assets_here` through helper functions.  
  - Add `children_with_assets` and `active_child_mask` with branch bit constants for `X_NEG`, `X_POS`, `Y_NEG`, `Y_POS`, `Z_NEG`, `Z_POS`.  
  - Add inline helpers to set, clear, and test bits and a helper `has_assets_or_active_children()`.

- Add resolution helpers  
  - Define `max_layers` and `distance(layer) = 3^(max_layers - layer)` helpers alongside legacy `vibble::grid::delta`.  
  - Provide `grid_spacing_for_layer(int layer)` that can fall back to power of two spacing until Map Grid is migrated.  
  - Add a conversion helper from current `grid_resolution_` to a default `resolution_layer` for legacy nodes.

- Wire new fields into creation  
  - Update `WorldGrid::ensure_point` and any `GridPoint` factories to set new identity fields and defaults.  
  - Keep `world_z = 0` and derive `resolution_layer` from current `grid_resolution_` until later phases set real values.  
  - Add a debug or test only constructor that can fabricate parent or child links without chunks.

**Codex guidance**  
Any task editing `GridPoint` must reference Phase 1 and confirm that both the new 3D fields and the legacy chunk and grid fields stay valid for existing callers.

---

### Phase 2 - Map Grid redesign

**Goal**  
Refactor `WorldGrid` into a Map Grid that owns persistent `GridPoint`s across all layers and `world_z`, while preserving chunk and tile data for the current renderer.

**How it fits**  
Map Grid is the source of truth for world space. Screen Grid, renderer traversal, asset attachment, and lighting will depend on it.

**Key files**  
`ENGINE/world/world_grid.hpp`, `ENGINE/world/world_grid.cpp`, `ENGINE/world/chunk_manager.*`, `ENGINE/world/chunk.*`, `ENGINE/utils/map_grid_settings.*`, `ENGINE/utils/grid.*`

**Subtasks**

- Establish ownership and indexing  
  - Introduce a Map Grid container (extend `WorldGrid` or wrap it) that owns all `GridPoint`s with `(x, y, z, layer)` identity.  
  - Keep chunk bookkeeping (`ChunkManager`, `chunk_index`, `Chunk::tiles`) intact for renderers while nodes move to the new identity.  
  - Document lifetime: nodes owned by Map Grid; Screen Grid holds references only.

- Upgrade spatial keys  
  - Extend `GridId` (or add a new key type) to encode `(world_x, world_y, world_z, resolution_layer)` while keeping a compatibility path for current `(i, j)` ids.  
  - Add `find_or_create_grid_point` and `find_grid_point` APIs that use the new key and map back to `Asset::grid_id` until Cleanup.  
  - Add hashing helpers and tests to ensure stable keys across sessions.

- Support 3^n spacing alongside legacy spacing  
  - Add `distance(layer)` helpers and store `max_layers` and origin on Map Grid.  
  - Keep `grid_resolution_`, `r_chunk_`, and `vibble::grid::delta` working until all callers switch. Document the conversion path.  
  - Add translators between `(i, j, grid_resolution_)` and `(world_x, world_y, resolution_layer)` for transition code.  
  - Make it explicit that tile chunks remain 2D indexed by `(i, j)` and are simply associated with the floor plane `GridPoint` hierarchy during migration.

- Child creation strategy  
  - Add helpers to allocate children lazily with correct parent links and identity.  
  - Ensure chunk and culling data stay coherent when children are created. `chunk_index` and `Chunk*` must remain valid for floor plane assets.  
  - Add a prebuild option for tests or benchmarks to populate full trees down to `max_layers`.

- Branch tracking hooks  
  - Add stub calls for branch activation and inactivation that will be filled in during Phase 5.  
  - Ensure `prune_empty_points` and `rebuild_chunks` can coexist with hierarchical nodes.  
  - Add instrumentation counters to verify branch propagation calls during attach and detach.

- Settings and loader compatibility  
  - Update `MapGridSettings` read and write helpers to accept base 3 spacing while still honoring existing power of two fields in map JSON.  
  - Add validation that warns when a map mixes power of two spacing with a requested `max_layers`.  
  - Note clearly which settings are temporary compatibility shims that will be removed in Phase 10.

**Codex guidance**  
Tasks modifying world grid behavior must reference Phase 2 and explain how they move logic toward `(x, y, z, layer)` identity without breaking chunked tile rendering.

---

### Phase 3 - Asset attachment and migration

**Goal**  
Move asset spawn and move logic to attach assets to `GridPoint`s in Map Grid with `world_z` and resolution layers, while keeping current gameplay content stable.

**How it fits**  
Assets must live on `GridPoint`s for grid search, Screen Grid, and renderer traversal to work. This phase connects gameplay content to the new spatial model.

**Key files**  
`ENGINE/core/AssetsManager.*`, `ENGINE/world/world_grid.*`, `ENGINE/asset/Asset.hpp`, asset spawn and move helpers, any JSON or map loaders that currently set `grid_resolution` or z offsets.

**Subtasks**

- Extend spawn and move APIs  
  - Add optional `world_z` (default 0) and `resolution_layer` parameters to asset spawn, move, and teleport code paths in `AssetsManager` and `WorldGrid`.  
  - Keep existing call sites compiling by defaulting to floor plane and current `grid_resolution_`.  
  - Update any cached grid residency (for example `Asset::grid_residency_cache`) to carry z and layer info.

- Attach and detach helpers  
  - Implement `attach_asset_to_grid_point(Asset* asset, GridPoint* node)` and `detach_asset_from_grid_point(Asset* asset, GridPoint* node)` to update `occupants`, `children_with_assets`, branch masks, and `Asset::grid_id`.  
  - Replace direct `occupants.push_back` and `erase` in `WorldGrid::register_asset`, `move_asset`, and `remove_asset` with these helpers.  
  - Ensure `screen_data_valid` is invalidated on attach, move, and detach.

- Align z related fields  
  - Map existing `Asset::z_offset`, `Asset::z_index`, and any fake height fields to either real `world_z` or render only offsets.  
  - Document the convention and update loader defaults so floor assets keep `world_z = 0`.  
  - Add clamps and asserts to catch negative `world_z` or out of range `resolution_layer`.

- Grid id and caches  
  - Keep `asset_to_point_` and `Asset::grid_id` in sync with the new identity while `GridId` is upgraded.  
  - Ensure cache invalidation (`screen_data_valid`) still fires on moves with new parameters.  
  - Add debug utilities to print an asset’s bound `GridPoint` identity for dev tools.

- Data migration safeguards  
  - Update JSON and map loaders to set `world_z` and `resolution_layer` defaults and clamp to valid ranges.  
  - Add debug assertions to catch assets that are attached without a `GridPoint`.  
  - Add a migration checklist for any spawning systems outside `AssetsManager` (for example scripted spawners).

**Codex guidance**  
Tasks that change how assets are spawned or moved must reference Phase 3 and confirm that all assets attach through the new helpers with `world_z = 0` as the floor default.

---

### Phase 4 - Screen Grid reconstruction

**Goal**  
Build a per frame Screen Grid that references Map Grid nodes in the visible 3D volume and updates per frame camera fields, replacing the flat lists in `WarpedScreenGrid`.

**Status (Phase 4 - Screen Grid reconstruction)**  
- Screen Grid rebuild now traverses Map Grid nodes (`all_grid_points`) instead of the flat `all_assets` list and fills per frame fields each frame.  
- Per frame state is reset with an explicit frame stamp during rebuild; visible points and assets are sourced from the traversal.  
- Current traversal is intentionally restricted to `world_z = 0` until camera and culling handle multi-z slices; legacy `GridId` lookup (`id_to_index_`) remains as a compatibility bridge.

**How it fits**  
Screen Grid is the runtime gateway between world space and rendering. Renderer and searches will start from Screen Grid roots.

**Key files**  
`ENGINE/render/warped_screen_grid.hpp`, `ENGINE/render/warped_screen_grid.cpp`, `ENGINE/core/AssetsManager.cpp` (active asset rebuild), `ENGINE/render/render.cpp`

**Subtasks**

- Introduce a Screen Grid container  
  - Define a struct or class to hold the per frame root list (`GridPoint*`) and traversal helpers.  
  - Keep `WarpedScreenGrid` API (`grid_visible_points()`, `grid_point_for_asset()`) but back it with the new container.  
  - Add a frame local allocator or buffer to avoid reallocations each frame.

- Rebuild per frame from Map Grid  
  - Replace the `world_grid.all_assets()` loop with a query against Map Grid for nodes within the camera volume (use current cull rect logic as a fallback).  
  - Store the queried nodes as Screen Grid roots per layer or plane. Preserve `active_chunks_` for tile rendering.  
  - Make the world volume computation explicit: camera center, zoom or scale, margin, and optional z slice bounds.  
  - For early integration, restrict Screen Grid selection to `world_z = 0` and document this in the code and in dev controls.  
  - Add a debug path to log how many nodes per layer and per z slice are pulled each frame.

- Update per frame camera fields  
  - Use `project_to_screen` or a new helper to set `screen`, `distance_from_camera`, `on_screen`, `parallax_dx`, and related fields on each referenced `GridPoint`.  
  - Invalidate or reset per frame fields when camera or zoom changes, not only on asset movement.  
  - Ensure per frame stamps are written so stale nodes are not reused across frames.

- Preserve current outputs for callers  
  - Keep `visible_points_` and `visible_assets_` compatible for `AssetsManager::rebuild_active_from_screen_grid`, but source them from the Screen Grid traversal rather than ad hoc vectors.  
  - Maintain `id_to_index_` lookups until renderers stop using `Asset::grid_id` directly.  
  - Add a compatibility flag to fall back to legacy visible list generation for debugging only.

- Retire flat screen grid code  
  - Remove or gate legacy flat rebuild paths once the Screen Grid traversal is stable and debuggable.  
  - Keep existing debug overlay hooks working with the new data structure.  
  - Add metrics (counts, time) to detect regressions when switching to the new traversal.

**Codex guidance**  
Tasks touching screen grid or camera to screen mapping must reference Phase 4 and must not introduce new flat 2D only screen grid structures.

---

### Phase 5 - Hierarchical search and active branch maintenance

**Goal**  
Make grid operations fast by leveraging the tree structure and active branch masks instead of scanning flat lists.

**How it fits**  
This phase delivers the performance benefit of the hierarchy and underpins render and lighting traversals.

**Key files**  
`ENGINE/world/grid_point.hpp`, `ENGINE/world/world_grid.cpp`, `ENGINE/render/warped_screen_grid.cpp`, any search utilities that currently scan all assets or points.

**Subtasks**

- Define branch bits and mask storage  
  - Add `BRANCH_X_NEG`, `BRANCH_X_POS`, `BRANCH_Y_NEG`, `BRANCH_Y_POS`, `BRANCH_Z_NEG`, `BRANCH_Z_POS` constants and ensure `active_child_mask` lives on `GridPoint`.  
  - Add helpers to translate a child pointer to a bit and vice versa.

- Implement propagation helpers  
  - Add `propagate_branch_active(GridPoint* child)` and `propagate_branch_inactive(GridPoint* child)` that walk parents and update masks.  
  - Ensure they respect `occupants` and `children_with_assets` before clearing a branch.  
  - Add early exit checks so propagation stops when a parent already had the bit set or cleared.

- Hook into attach, detach, and move  
  - Call the propagation helpers inside the Phase 3 attach and detach functions and in `WorldGrid::prune_empty_points`.  
  - Add tests or debug asserts so mask and asset invariants stay in sync.  
  - Ensure moves that change parent links update both old and new branches.

- Add branch aware queries  
  - Implement exact lookup by `(x, y, z, layer)` and region queries that skip inactive branches.  
  - Use these queries in `WarpedScreenGrid::rebuild_grid` and in any gameplay searches to replace full map scans.  
  - Provide iterators or generators that yield nodes in depth first or breadth first order, skipping inactive branches.

- Keep compatibility paths gated  
  - Allow a temporary fallback to flat scans (clearly flagged) so early integration can ship without breaking current behavior.  
  - Add telemetry or logging when a fallback path is used and plan to remove these paths in Phase 10.

**Codex guidance**  
Tasks that add or modify grid search logic must reference Phase 5 and must not add new linear scans unless explicitly marked debug only.

---

### Phase 6 - Camera and transform integration

**Goal**  
Extend camera and NDC transforms to understand `world_z` and the new grid while keeping floor assets visually consistent.

**How it fits**  
This connects the vertical dimension to perspective and parallax instead of fake offsets.

**Key files**  
`ENGINE/render/warped_screen_grid.*` (`map_to_screen`, `compute_render_effects`, `project_to_screen`), any camera depth or pitch settings, `ENGINE/render/render.cpp`

**Subtasks**

- Expose current 2D behavior  
  - Make `kForceDepthPerspectiveDisabled` and related flags runtime settings and document current flat behavior.  
  - Add debug toggles to compare legacy 2D projection versus new 3D projection.  
  - Capture screenshots or metrics for before and after to verify no floor regressions.

- Accept `world_z` in projections  
  - Update projection and render effect helpers to take `(world_x, world_y, world_z)` and populate `GridPoint::distance_to_camera` accordingly.  
  - Ensure `on_screen` tests include vertical volume checks when `world_z` is non zero.  
  - Thread `world_z` through `map_to_screen`, `compute_render_effects`, and `project_to_screen` without breaking current callers.

- Reinterpret z offsets  
  - Decide how `Asset::z_offset`, `Asset::z_index`, and `GridPoint::tilt_radians` or `perspective_scale` map to real height versus visual tweaks.  
  - Keep floor assets (`world_z = 0`) rendering identically unless you intentionally change that behavior.  
  - Add a migration flag that can switch between old z offset behavior and new `world_z` aware behavior for testing.

- Validate with overlays  
  - Add debug overlays or assertions showing `world_z`, projected screen Y, and `distance_from_camera` for a sample set of assets.  
  - Include a per frame report of min and max `world_z` seen and how many points were culled by z volume.

**Codex guidance**  
Tasks that change camera transforms must reference Phase 6 and document how `world_z` flows into screen positions and depth values without regressing floor assets.

---

### Phase 7 - Renderer traversal and LOD

**Goal**  
Have the renderer use Screen Grid roots and the hierarchy to draw assets, using `distance_from_camera` and `resolution_layer` for draw order and LOD.

**How it fits**  
This changes how content is drawn and ties render cost to the hierarchical traversal instead of flat lists.

**Key files**  
`ENGINE/render/render.cpp` (SceneRenderer), `ENGINE/core/AssetsManager.cpp` (active asset rebuild), `ENGINE/render/composite_asset_renderer.*`, `ENGINE/render/warped_screen_grid.*`, `ENGINE/render/grid_tile_renderer` paths that rely on chunks.

**Subtasks**

- Feed renderables from Screen Grid  
  - Replace `Assets::rebuild_active_from_screen_grid` sorting logic with traversal of Screen Grid roots that skips empty or inactive nodes.  
  - Preserve dev filters and selection lists while swapping the source of truth.  
  - Add a comparator that prefers `distance_from_camera` and `resolution_layer` before falling back to `z_index` and `screen.y`.

- Recursive traversal for drawing  
  - Add traversal helpers that, for each node: skip if no assets and `active_child_mask == 0`; draw assets when `on_screen`; then recurse into active children.  
  - Keep `id_to_index_` and `grid_point_for_asset` working during the transition.  
  - Provide layer aware traversal so coarse layers can be skipped or aggregated for far distances.

- Draw order and LOD  
  - Use `resolution_layer` and `distance_from_camera` to order assets, tie breaking with `z_index` as needed.  
  - Surface `GridPoint` data to `CompositeAssetRenderer::update` so perspective and scale can respect the grid node.  
  - Add optional LOD hooks: for example drop fine layers when over budget, or swap variants based on `resolution_layer`.

- Integrate tile and composite passes  
  - Ensure `GridTileRenderer` and composite or light passes pull camera data from the Screen Grid where appropriate while still supporting chunk tiles during migration.  
  - Add a path for tile rendering to reuse Screen Grid bounds instead of recomputing from chunks.

**Codex guidance**  
Render tasks must reference Phase 7 and verify they traverse via Screen Grid, skip inactive branches, and use grid provided depth and LOD data. Do not add new render paths that ignore the grid hierarchy.

---

### Phase 8 - Lighting and 3D usage

**Goal**  
Attach lights using full 3D coordinates and integrate `world_z` into attenuation, shadow behavior, and depth ordering.

**How it fits**  
Lighting is the first practical user of real height and validates the usefulness of the 3D grid.

**Key files**  
Light definitions on assets (`AssetInfo::light_sources`), `ENGINE/render/composite_asset_renderer.cpp` (light emission), `ENGINE/world/chunk.cpp` (`LightMap`), any runtime light queries.

**Subtasks**

- Attach lights to GridPoints  
  - When spawning lights or light emitting assets, set `world_z` and attach to the correct `GridPoint` using Phase 3 helpers so branch masks stay accurate.  
  - For animated or moving lights, ensure moves trigger branch propagation and cache invalidation.

- Integrate `world_z` into light math  
  - Update attenuation and shadow calculations to use 3D distance; store any per light height metadata needed.  
  - Keep floor only lights (`world_z = 0`) compatible with current visuals during rollout.  
  - Add clamps and fallbacks so lights without explicit `world_z` default to 0 with a logged warning.

- Branch aware light queries  
  - Add Map Grid queries for lights that respect region bounds and active branches instead of scanning all assets.  
  - Make `LightMap` rebuild and use these queries instead of the current stubs.  
  - Expose debug stats (lights queried, lights skipped by branch masks) to validate performance.

**Codex guidance**  
Lighting tasks must reference Phase 8 and must not rely on 2D only assumptions. All lights should carry a defined `world_z`.

---

### Phase 9 - Dev tools and debug UX

**Goal**  
Update dev tools and debug visualizations to understand the 3D hierarchy and make the new system easy to inspect and tune.

**How it fits**  
Without good tools, the new system will be hard to debug. Dev controls and overlays must show 3D state, not just 2D assets.

**Key files**  
`ENGINE/dev_mode/dev_controls.*`, `ENGINE/core/AssetsManager.cpp` (dev_controls integration), `ENGINE/render/warped_screen_grid.*` overlays, any debug drawing in `SceneRenderer`.

**Subtasks**

- 3D grid overlays  
  - Add overlays that can render Map Grid nodes on the floor plane and optional `world_z` slices, plus Screen Grid nodes with `on_screen` state.  
  - Add color coding for `resolution_layer` and indicators for active vs inactive branches.

- Dev input and selection  
  - Add picking helpers that map mouse screen position to the nearest `GridPoint` (respecting `world_z` slices) and display identity and branch activity.  
  - Keep existing selection and highlight flows working for 2D assets during the transition.  
  - Add a gizmo to scrub `world_z` slices and highlight nodes at that height.

- Phase toggles and instrumentation  
  - Add toggles to highlight active branches, nodes with assets, and nodes referenced by the current Screen Grid frame.  
  - Expose debug info for `world_z`, `resolution_layer`, and branch masks in dev panels.  
  - Add a per frame counter overlay (nodes pulled, nodes rendered, branches skipped).

**Codex guidance**  
Dev tool tasks must reference Phase 9 and must build on Map Grid and Screen Grid concepts instead of reintroducing ad hoc 2D debug grids.

---

### Phase 10 - Cleanup and deprecation

**Goal**  
Remove obsolete 2D grid and power of two utilities once the new system is operational and call sites have been migrated. The goal is to end with a single 3D grid model and no legacy duplicates.

**How it fits**  
This ensures a single, consistent spatial model in the engine and stops technical debt from growing.

**Key files**  
`ENGINE/utils/grid.*`, `ENGINE/utils/map_grid_settings.*`, `ENGINE/world/world_grid.*`, `ENGINE/render/warped_screen_grid.*`, any compatibility shims added in earlier phases.

**Subtasks**

- Identify deprecated APIs  
  - List power of two grid helpers, chunk centric utilities that duplicate Map Grid behavior, and flat Screen Grid rebuild paths.  
  - Include hardcoded disables such as `kForceDepthPerspectiveDisabled` and any temporary compatibility flags.  
  - Enumerate remaining uses of `grid_resolution_`, `r_chunk_`, and `vibble::grid::delta` in gameplay and rendering paths.

- Remove or gate them  
  - Delete unused code or guard remaining legacy paths with clear TODOs pointing to this phase and `3d_refactor_plan.md`.  
  - Keep tile and chunk data only if still required by renderers, and note the migration plan.  
  - If a piece of legacy 2D grid logic cannot be kept as a thin adapter on top of the 3D grid, remove it and refactor the caller to use the new system directly. No permanent dual grid systems.

- Documentation and comments  
  - Update design docs and inline comments to reference `GridPoint`, Map Grid, and Screen Grid as the canonical model.  
  - Record any remaining technical debt with concrete owners and files.  
  - Add a short done checklist confirming: no power of two only paths, no flat screen lists, `world_z` present on all assets and lights, and no core feature relies on a legacy 2D grid.

**Codex guidance**  
Cleanup tasks must reference Phase 10 and must not keep long term dependencies on legacy 2D grid paths. If a legacy helper survives, it should be a trivial adapter on top of the 3D grid, not a second system.

---

## 4. Summary

- `GridPoint` becomes the single source of spatial truth with 3D identity, hierarchy pointers, per frame camera fields, and branch masks.
- Map Grid owns and indexes all `GridPoint`s; Screen Grid is rebuilt per frame and references only the nodes relevant to the current camera.
- Active branch masks and hierarchical search make queries, movement, and rendering fast while keeping current chunk and tile data working during migration.
- Camera, renderer, and lighting adopt real `world_z` while keeping floor assets simple and backward compatible during rollout.
- Dev tools must expose the hierarchy so the migration stays debuggable.
- Backwards compatibility is valued, but the end goal is a single 3D grid based system with no permanent legacy 2D grid logic.

Legacy cleanup tasks (track and remove when 3D identity is fully adopted):
- Remove reliance on legacy `grid_index`/`chunk_index`/`GridId` once all callers use `GridKey` (Phase 10).
- Remove temporary logging/compatibility paths that collapse 3D identity to legacy ids.
- Finalize branch mask propagation and detach any legacy mask assumptions tied to 2D traversal.
- Remove debug validation utilities added for migration once Phase 10 cleanup is underway.
- Upgrade any remaining 2D-only caches (e.g., grid residency caches) to store `world_z` and `resolution_layer`, then remove the legacy forms.
- Lift the temporary Screen Grid floor-only restriction once camera/culling support multi-z traversal; update `WarpedScreenGrid` to include higher `world_z` layers.
- Replace `id_to_index_` GridId compatibility lookups with `GridKey`-backed indexing once renderers stop using `Asset::grid_id`; remove the legacy map in `WarpedScreenGrid`.

Every Codex task involved in this refactor must reference this plan and state which phase it is implementing or supporting.
