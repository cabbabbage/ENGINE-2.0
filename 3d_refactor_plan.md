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

Current baseline (Dec 2025): the engine uses a 3D-aware `world::WorldGrid` with `GridPoint` identity expressed as `(world_x, world_y, world_z, resolution_layer)` (`GridKey` backed by `key_to_id_`). Assets bind through this identity, Screen Grid rebuilds from Map Grid traversal, and legacy 2D identity is kept only for tile chunk bookkeeping. Perspective remains effectively depth-aware with debug toggles.

## Remaining tasks
- Lift the temporary `world_z = 0` restriction in `WarpedScreenGrid::rebuild_grid` so Screen Grid traversal and camera culling can consider multiple heights and layers.
- Migrate tile chunk culling and chunk-based tile renders (`ENGINE/render/grid_tile_renderer.*`, chunk bookkeeping) from power-of-two (`r_chunk`, `vibble::grid::delta`) helpers into Map Grid/Screen Grid bounds or keep them explicitly gated as tile-only paths.
- Extend `LightMap` sampling (`ENGINE/world/chunk.cpp`) to accept arbitrary `world_z`, configurable query radius, and 3D-aware accumulation, then remove the remaining floor-only sampling fallback.
- Remove the dev-only flat camera toggle/logging (`WarpedScreenGrid::flat_camera_debug` and related settings) once the new depth pipeline is stable and well-tested.

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

- `ENGINE/world/grid_point.hpp`: 3D identity fields (`world_x`, `world_y`, `world_z`, `resolution_layer`) with hierarchy links, per frame fields, branch masks, and occupants.
- `ENGINE/world/world_grid.cpp`: `WorldGrid` owns `GridPoint`s keyed by `GridKey` (stored in `key_to_id_`), maps assets via `asset_to_key_`, tracks roots, and drives branch-aware `query_region`/`query_lights` traversals; `GridId` is now only the internal map key while chunk/tile bookkeeping uses legacy `(i,j)` indices.
- `ENGINE/render/warped_screen_grid.cpp`: rebuilds from Map Grid region queries (camera volume, layer/z filters) and populates per frame `warped_points_`, `visible_points_`, and `visible_assets_` without flat `all_assets()` scans; traversal is currently limited to `world_z = 0`.
- `ENGINE/core/AssetsManager.cpp`: `rebuild_active_from_screen_grid` consumes `WarpedScreenGrid::get_visible_points()`, filters `on_screen` nodes, and sorts assets by `distance_to_camera`, `resolution_layer`, `world_z`, with screen-space ties. Active overlays and filters continue to use this new list.
- `ENGINE/render/render.cpp`: `SceneRenderer` drives rendering from `WarpedScreenGrid` data (screen positions, `perspective_scale`, `distance_to_camera`) throughout the composite and tile passes, removing any reliance on `Asset::grid_id`.
- `ENGINE/world/chunk.cpp`: `LightMap` rebuilds query lights via `WorldGrid::query_lights`, samples the returned `LightInstance`s using full 3D `(dx,dy,dz)` distances, and still emits an additive brightness mask for now.
- `ENGINE/utils/map_grid_settings.*` and `ENGINE/utils/grid.*`: Map Grid spacing defaults to powers of 3; legacy `r_chunk`/`vibble::grid::delta` helpers remain tile-only for chunk culling and dev tools.
- `ENGINE/render/composite_asset_renderer.cpp`: consumes the per frame `GridPoint` camera fields so perspective, depth sorting, and parallax now come from Map Grid identity.

### 1.3 GridPoint snapshot (target vs current)

Target:

- World identity (immutable): `resolution_layer`, `world_x`, `world_y`, `world_z`, parent pointer, and six child pointers (`x_child_neg/pos`, `y_child_neg/pos`, `z_child_neg/pos`).
- Per frame camera data: `screen_x`, `screen_y`, `distance_from_camera`, `on_screen`.
- Asset and branch data: `assets_here` (backed by a pointer container), `children_with_assets`, `active_child_mask` with branch bits for the six child directions.

Current reality:

- `GridPoint` carries immutable 3D identity with hierarchy links, per frame camera fields, branch masks, and occupants. `chunk_index`/`chunk*` remain for tile ownership.
- Legacy `GridId` remains as an internal storage key for the container; assets and traversal use `GridKey` and hierarchy pointers.

### 1.4 Resolution vs hierarchy

- `resolution_layer` expresses sampling distance (target: powers of 3) and is separate from the existence of children. Child pointers define the hierarchy.
- A node can have a valid `resolution_layer` but no children yet (lazy creation). The tree grows as assets or queries demand it.
- During migration, power of two spacing (`vibble::grid::delta`) and chunk indices will coexist with `resolution_layer` until all call sites switch to 3^n spacing.

### 1.5 Map Grid snapshot

Target:

- A persistent Map Grid that owns all `GridPoint`s across `(x, y, z, resolution_layer)` with spacing `distance(layer) = 3^(max_layers - layer)`.
- Nodes are indexed by `(world_x, world_y, world_z, resolution_layer)` and can be prebuilt or created lazily.

Current reality:

- `WorldGrid` uses 3^n spacing for GridPoint identity; `ChunkManager` remains 2D indexed by `(i, j)` for tiles only. `MapGridSettings` and related loaders still contain power-of-two fields for tile chunks.
- During migration, tile chunks remain 2D indexed by `(i, j)` and are associated with the relevant `GridPoint` hierarchy on the floor plane. Tiles do not become fully 3D; they are mapped onto the grid.

### 1.6 Screen Grid snapshot

Target:

- Screen Grid is rebuilt each frame from the Map Grid, holds references only, and updates per frame camera fields on the referenced `GridPoint`s.

Current reality:

- `WarpedScreenGrid::rebuild_grid` traverses Map Grid via region query and branch masks, populating per-frame fields and visible lists without legacy flat scans. It currently filters to `world_z = 0` until multi-z rendering is enabled.
- Early versions of Screen Grid focus on `world_z = 0` (floor plane) only. Higher `world_z` usage will come later as camera and lighting become fully 3D aware.

### 1.7 Grid search model

Target:

- Active branch masks drive traversal: skip nodes with no assets and no active children; attach and detach propagate branch activation upward. Exact lookup is `(x, y, z, layer)` via index or child traversal.

Current reality:

- Lookups and traversal are `GridKey`-based through branch-aware queries; legacy `GridId` is confined to internal storage for tiles. Region traversal replaces flat scans for Screen Grid and asset visibility.

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

Pre Phase 5 legacy removal (Phases 1-4): legacy 2D identity bridges have been removed for assets and Screen Grid; remaining legacy elements are tile-only and listed in the cleanup tasks.

---

### Phase 1 - Define 3D GridPoint core

**Goal**  
Introduce the 3D `GridPoint` shape and helpers while keeping current 2D users working. This replaces the current flat layout with a canonical world identity, hierarchy pointers, per frame fields, asset lists, and branch masks.

**Status (Phase 1 - Define 3D GridPoint core)**  
- Immutable `GridKey` identity (`world_x`, `world_y`, `world_z`, `resolution_layer`) is constructed once per node; hierarchy pointers, branch bits, and six child links exist and start `nullptr` under Map Grid ownership.  
- Per-frame camera fields are reset/invalidated every rebuild, and `GridPoint::frame_stamp` prevents stale data; branch/asset helpers (`children_with_assets`, `active_child_mask`, `assets_here`) are wired into constructor and mutators.  
- Resolution helpers (`max_layers`, `distance(layer)`, `grid_spacing_for_layer`) now drive node placement; `grid_resolution_` only selects the default layer while loaders migrate.  
- Identity validity helpers (`debug_identity_and_mask`, `debug_validate_keys_and_masks`) exist; `GridId` is now just the container key and is no longer exposed to gameplay paths.

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

**Status (Phase 2 - Map Grid redesign)**  
- `WorldGrid` owns `GridPoint`s indexed by `GridKey` via `key_to_id_` and tracks roots, branch masks, and 3^n spacing; `GridId` now only exists for `std::unordered_map` storage and chunk compatibility.  
- Region queries, `query_region`, and `grid_points_for_*` already rely on `GridKey` and active branch masks; `find_or_create_grid_point` enforces the canonical identity while instrumentation logs mismatches.  
- Chunk bookkeeping (`ChunkManager`, `grid_index`, `chunk_index`, `MapGridSettings` power-of-two fields) still uses 2D ids; migrating these adapters to the 3D model and/or retiring them is a Phase 10 cleanup task.
- Map grid spacing is now powered by `distance(layer)`/`max_layers`; `MapGridSettings::spacing()` reports a 3^n grid while `grid_resolution_`/`r_chunk` remain tile-only adapters described in the cleanup list.

**Subtasks**

- Establish ownership and indexing  
  - Ensure `WorldGrid` owns `GridPoint`s keyed by `GridKey` and maintains non-owning Screen Grid references; `key_to_id_` keeps lookups fast while `GridId` serves only as the map container key.  
  - Keep `ChunkManager`, `chunk_index`, and `Chunk::tiles` for tile rendering, documenting that they are tile-only and do not define asset identity.  
  - Record how `roots_`, `children_with_assets`, and branch masks describe lifetime so Screen Grid rebuilds can trust the hierarchy.

- Upgrade spatial keys  
  - Keep `key_to_id_` authoritative for queries, remove any fallback to `GridId`, and audit callers (Assets, renderers, loaders) for leftover legacy lookups.  
  - Surface hashing/validation helpers (`debug_validate_keys_and_masks`) to guarantee `GridKey` stability across sessions.  
  - Treat any remaining 2D legacy lookup utilities (e.g., `point_for_id`, `point_id_from_world`) as Phase 10 cleanup candidates unless they serve tile-specific bookkeeping.

- Support 3^n spacing  
  - Use `distance(layer)`/`grid_spacing_for_layer` as the canonical spacing for `GridPoint` placement; keep `grid_resolution_` only to choose the default layer until loaders catch up.  
  - Audit `MapGridSettings`, map loaders, and chunk/residency code for references to `grid_resolution_`, `r_chunk_`, or `vibble::grid::delta`, replacing them with `GridKey` aware settings or capping them as tile-only values.  
  - Document the currently tile-only power-of-two fields so we can remove them in Phase 10 cleanup.

- Child creation strategy  
  - Maintain helpers that lazily allocate children with proper parent links and `GridKey` identity; ensure `ensure_child` logs if parent/child mismatches slip in.  
  - Keep chunk and culling data coherent when children appear by updating `chunk_index` and `children_with_assets` consistently; add tests verifying lazy growth along each axis.  
  - Provide instrumentation or benchmarks that can prebuild subtrees down to `max_layers` for diagnostics.

- Branch tracking hooks  
  - Preserve propagation helpers so `attach_asset_to_hierarchy`/`detach_asset_from_hierarchy` drive branch masks; `prune_empty_points` already cleans up inactive nodes.  
  - Ensure instrumentation counters exist for mask updates and branch bit flips; add debug asserts where masks should always match occupancy.  
  - Track any remaining legacy mask assumptions (for example from chunk-based culling) and log them for Phase 5/10 cleanup.

- Settings and loader compatibility  
  - Update loaders to emit `world_z` and `resolution_layer` defaults, clamp them, and call into the `GridKey`-based creation helpers.  
  - Align `MapGridSettings` with the 3^n model, adding warnings when maps still mix in legacy power-of-two fields.  
  - Keep a short migration checklist that references any loader or script that still writes chunk-focused spacing so we can revisit it in Phase 10.

**Codex guidance**  
Tasks modifying world grid behavior must reference Phase 2 and explain how they move logic toward `(x, y, z, layer)` identity without breaking chunked tile rendering.

---

### Phase 3 - Asset attachment and migration

**Goal**  
Move asset spawn and move logic to attach assets to `GridPoint`s in Map Grid with `world_z` and resolution layers, while keeping current gameplay content stable.

**How it fits**  
Assets must live on `GridPoint`s for grid search, Screen Grid, and renderer traversal to work. This phase connects gameplay content to the new spatial model.

**Status (Phase 3 - Asset attachment and migration)**  
- Assets register, move, and remove through `WorldGrid::attach_asset_to_grid_point`/`detach_asset_from_grid_point`, so `occupants`, branch masks, and per-frame invalidation stay synchronized.  
- Asset bindings use `asset_to_key_`; `Asset::grid_id` is cleared and no longer used outside of rare compatibility code.  
- Spawn and move helpers still default to floor layer (`world_z = 0`, default `resolution_layer`), and dev/UI flows supply the new `MapGridSettings::resolution` to `WorldGrid::set_grid_resolution`.

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
  - Keep asset bindings in sync via `GridKey`/`asset_to_key_`; legacy `GridId` on assets is cleared and no longer used for lookup.  
  - Ensure cache invalidation (`screen_data_valid`) still fires on moves with new parameters.  
  - Add debug utilities to print an asset's bound `GridPoint` identity for dev tools.

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
- Screen Grid rebuild now queries Map Grid via branch-aware region traversal (camera volume, layer/z filters) instead of flat scans, and fills per frame fields each frame.  
- Per frame state is reset with an explicit frame stamp during rebuild; visible points and assets are sourced only from the Screen Grid traversal.  
- Current traversal is intentionally restricted to `world_z = 0` until camera and culling handle multi-z slices; per-frame asset lookup uses Screen Grid’s asset-to-point map (no `GridId` bridge).

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
  - Maintain `grid_point_for_asset()` via a per-frame asset-to-point map; legacy `GridId` lookups are removed.  
  - Add a compatibility flag to fall back to legacy visible list generation for debugging only.

- Retire flat screen grid code  
  - Remove or gate legacy flat rebuild paths once the Screen Grid traversal is stable and debuggable.  
  - Keep existing debug overlay hooks working with the new data structure.  
- Add metrics (counts, time) to detect regressions when switching to the new traversal.

**Codex guidance**  
Tasks touching screen grid or camera to screen mapping must reference Phase 4 and must not introduce new flat 2D only screen grid structures.

---

Ready for Phase 5: core identity, asset attachment, and Screen Grid traversal now rely on 3D `GridKey` + hierarchy only; legacy GridId usage is constrained to tile storage. Multi-z Screen Grid traversal and further search optimizations will build on this 3D-first baseline.

### Phase 5 - Hierarchical search and active branch maintenance

**Goal**  
Make grid operations fast by leveraging the tree structure and active branch masks instead of scanning flat lists.

**How it fits**  
This phase delivers the performance benefit of the hierarchy and underpins render and lighting traversals.

**Key files**  
`ENGINE/world/grid_point.hpp`, `ENGINE/world/world_grid.cpp`, `ENGINE/render/warped_screen_grid.cpp`, any search utilities that currently scan all assets or points.

**Status (Phase 5 - Hierarchical search and active branch maintenance)**  
- Branch-aware exact lookup and region queries now traverse Map Grid using `GridKey`, skipping inactive branches via `active_child_mask`/`children_with_assets`.  
- Screen Grid rebuild consumes the region query instead of flat scans; per-frame metrics record nodes visited and branches skipped.  
- Legacy flat scan helpers remain as compatibility/debug paths only and are listed for Phase 10 cleanup.

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

**Status (Phase 6 - Camera and transform integration)**  
- Camera and Screen Grid projections read `world_z`; per-frame `distance_to_camera`, `on_screen`, and screen positions are derived from 3D identity.  
- Depth-aware sorting uses 3D distance and `world_z`; flat camera is a dev-only toggle with debug logging of depth stats.

**How it fits**  
This connects the vertical dimension to perspective and parallax instead of fake offsets.

**Key files**  
`ENGINE/render/warped_screen_grid.*` (`map_to_screen`, `compute_render_effects`, `project_to_screen`), any camera depth or pitch settings, `ENGINE/render/render.cpp`

**Subtasks**

- Expose current 2D behavior  
  - Replace the old hardcoded depth disable with runtime settings (`depth_enabled`, `flat_camera_debug`, `depth_debug_logging`) and document the default depth-aware behavior.  
  - Add debug toggles that let devs compare legacy flat projections with the new 3D pipeline when needed.  
  - Capture screenshots or metrics for before and after to verify no floor regressions while depth is enabled or temporarily disabled.

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

**Status (Phase 7 - Renderer traversal and LOD)**  
- Active assets now come exclusively from Screen Grid traversal (`grid_visible_points`) built from branch-aware Map Grid queries; render order uses `distance_to_camera` first, then `resolution_layer`, `world_z`, with `screen.y`/`z_index` as tie breakers.  
- Composite renderer consumes `GridPoint` data (perspective scale, distance) per asset; SceneRenderer fetches screen positions from GridPoints instead of recomputing map-to-screen.  
- Tile rendering remains chunk-based and 2D-only for culling/drawing; reuse of Screen Grid bounds for tiles is a noted cleanup item.

**Subtasks**

- Feed renderables from Screen Grid  
  - Ensure `AssetsManager` builds active assets by traversing Screen Grid nodes (branch-aware, `on_screen` only) instead of flat visible lists.  
  - Preserve dev filters and selection lists while swapping the source of truth.  
  - Depth comparator should prioritize `distance_from_camera`, then `resolution_layer`, then `world_z`, using `screen.y`/`z_index` only as tie breakers.

- Recursive traversal for drawing  
  - Add traversal helpers that, for each node: skip if no assets and `active_child_mask == 0`; draw assets when `on_screen`; then recurse into active children.  
  - Keep `grid_point_for_asset` working via Screen Grid's per-frame asset map (no `GridId` dependency).  
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

**Status (Phase 8 - Lighting and 3D usage)**  
- Lights are gathered via branch-aware Map Grid queries (`query_lights`) using `GridKey`/`world_z`; LightMap sampling computes 3D attenuation (`dx/dy/dz`) from `GridPoint` identity.  
- Asset-attached lights default to `world_z` from their owning GridPoint; floor lights keep `world_z = 0`.  
- Lighting still uses a simple additive model (mask/dynamic brightness); advanced shadowing/volumetrics remain future work.

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

**Status (Phase 9 - Dev tools and debug UX)**  
- Dev overlays render Screen Grid nodes (color-coded by `resolution_layer`, highlighted when branches/assets are active) directly from Map Grid traversal; per-frame metrics show nodes visited, branches skipped, depth culls, and z-range from `WarpedScreenGrid`.  
- Picking uses Screen Grid (`pick_nearest_point`) to surface `(world_x, world_y, world_z, resolution_layer)` plus branch masks under the cursor; selection overlays read 3D identity rather than legacy 2D ids.  
- Grid overlay toggle drives the 3D-aware overlay; multi-z slices will follow once camera/culling lift the current `world_z = 0` Screen Grid restriction.

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

**Status (Phase 10 - Cleanup and deprecation)**  
- Power-of-two helpers are confined to tile-only code paths; Map Grid spacing (`MapGridSettings::spacing`) now uses 3^n defaults and `WorldGrid` defaults to 3^n spacing with no legacy id fallbacks.  
- Flat scan helpers on WorldGrid (`all_grid_points`, `grid_points_for_layer/world_z`) have been removed; GridId remains only as the internal map key for storage/chunks.  
- Default pipeline is 3D: assets, lights, camera, and renderer run on GridKey + Screen Grid traversal; depth toggles remain only as debug controls.

**Remaining cleanup (debug/tile only)**  
- Screen Grid currently clamps traversal to `world_z = 0`; lift once camera/culling support full multi-z rendering (`ENGINE/render/warped_screen_grid.cpp`).  
- Tile chunk culling remains power-of-two/tile-only in `ENGINE/render/grid_tile_renderer.*`; keep gated to tiles.  
- LightMap sampling assumes floor height with a fallback radius (`ENGINE/world/chunk.cpp`); extend to arbitrary `world_z` when ready.  
- Debug flat camera toggle (`WarpedScreenGrid::flat_camera_debug`) remains for comparison and should be removed once depth is fully stable.

---

## 4. Summary

- `GridPoint` becomes the single source of spatial truth with 3D identity, hierarchy pointers, per frame camera fields, and branch masks.
- Map Grid owns and indexes all `GridPoint`s; Screen Grid is rebuilt per frame and references only the nodes relevant to the current camera.
- Active branch masks and hierarchical search make queries, movement, and rendering fast while keeping current chunk and tile data working during migration.
- Screen Grid rebuilds now cap visible nodes with 3D distance checks (min/max `world_z`, depth culling) before supplying assets to the renderer; traversal is currently clamped to the floor plane until multi-z rendering is enabled.
- Camera and renderer adopt real `world_z` (depth-enabled by default with debug toggles); lighting uses 3D GridPoint identity/queries for attenuation, with advanced shadowing/volumetrics still deferred.
- Dev tools now surface Screen Grid/Map Grid nodes with 3D identity, branch masks, and per-frame traversal metrics via overlays and picking.
- Backwards compatibility is valued, but the end goal is a single 3D grid based system with no permanent legacy 2D grid logic.

Legacy cleanup tasks (track and remove when 3D identity is fully adopted):
- Screen Grid traversal currently clamps to `world_z = 0` in `ENGINE/render/warped_screen_grid.cpp`; lift once multi-z camera/culling is enabled.
- Tile chunk culling remains power-of-two/tile-only in `ENGINE/render/grid_tile_renderer.*`; keep gated to tiles or replace when tile rendering migrates.
- LightMap sampling assumes floor height with a fallback radius (`ENGINE/world/chunk.cpp`); extend to arbitrary `world_z` when ready.
- Debug flat camera toggle (`WarpedScreenGrid::flat_camera_debug`) remains for comparison; remove once 3D depth is fully stable.

Every Codex task involved in this refactor must reference this plan and state which phase it is implementing or supporting.
