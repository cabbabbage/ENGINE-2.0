# 3D GridPoint Refactor Plan

This document defines the target design and implementation plan for migrating the engine to a hierarchical 3D GridPoint system.

All Codex tasks that touch grid, camera, parallax, NDC, renderer, lighting, or dev tools should:

- Reference this file by name: `3d_refactor_plan.md`.
- Confirm that their changes match the intent and constraints described here.
- Mention which phase they are implementing or supporting.

The goal is to replace the current 2D grid and resolution system with a 3D hierarchy that:

- Uses a shared Map Grid and Screen Grid.
- Adds a real `world_z` dimension while keeping most assets on the floor plane (`world_z = 0`).
- Uses resolution layers and spacing based on powers of 3.
- Allows very fast spatial search using active branch flags.
- Keeps perspective, parallax, and NDC logic in the camera modules, not in the grid.

---

## 1. Target architecture overview

### 1.1 Big picture

- The whole world and the current camera view share one spatial structure: a hierarchy of `GridPoint` objects.
- **Map Grid** owns all `GridPoint` instances and defines their fixed world positions `(world_x, world_y, world_z, resolution_layer)`.
- **Screen Grid** is rebuilt every frame and holds references to the subset of `GridPoint`s that matter for the current camera.
- Camera and NDC code handle all perspective, parallax, and warping.  
  Grids only store the final results:
  - `screen_x`, `screen_y`
  - `distance_from_camera`
  - `on_screen`

### 1.2 GridPoint snapshot

`GridPoint` is the core node type.

World identity (never changes after construction):

- `resolution_layer`  tree depth, 0 is coarsest, `max_layers` is finest.
- `world_x`, `world_y`, `world_z`  world coordinates.
- Pointers to parent and up to 6 children:
  - `x_child_neg`, `x_child_pos`
  - `y_child_neg`, `y_child_pos`
  - `z_child_neg`, `z_child_pos`

Per frame camera data (updated every frame by Screen Grid and camera):

- `screen_x`, `screen_y`  final screen position after parallax and NDC.
- `distance_from_camera`  used for depth sorting.
- `on_screen`  true if the node is inside the viewport.

Asset and branch data:

- `assets_here`  assets anchored to this node.
- `children_with_assets`  child nodes that have at least one asset.
- `active_child_mask` (or equivalent)  bitmask indicating which of the 6 child directions lead to an active subtree.

Attach and detach flows maintain:

- `assets_here`
- `children_with_assets`
- `active_child_mask`  
with updates propagated up the parent chain.

### 1.3 Map Grid snapshot

Map Grid is the long lived world index.

- Creates and owns all `GridPoint`s.
- Persists for the entire map or session.
- Does not run any camera or perspective math.

World model:

- Flat 2D world in `(world_x, world_y)` for most assets.
- `world_z` used for lights and any elevated object.
- No zoom or warping in Map Grid  it is a regular lattice of sample points.

Resolution layers and spacing:

- Resolution is the layer depth.
- Spacing shrinks by a factor of 3 per layer.

For `max_layers = 6`:

- `distance(layer) = 3^(max_layers - layer)`

So:

- layer 0: 729 px  
- layer 1: 243 px  
- layer 2: 81 px  
- layer 3: 27 px  
- layer 4: 9 px  
- layer 5: 3 px  
- layer 6: 1 px  

Construction:

- Build layer 0 roots over the map, spaced by `distance(0)`.
- Optionally prebuild child nodes down to `max_layers` or create them lazily.
- Store all nodes in a spatial index keyed by `(world_x, world_y, world_z, resolution_layer)`.

### 1.4 Screen Grid snapshot

Screen Grid is the per frame view of the world.

Role:

- Holds references to the subset of `GridPoint`s near the camera.
- Owns no nodes.
- Updates the per frame camera data on those nodes.

Build each frame:

- Read camera state (center, zoom, warping parameters).
- Compute a world volume that covers the visible area plus margin in X, Y, and Z.
- Ask Map Grid for all `GridPoint`s inside that volume at the layers we care about.
- These nodes become Screen Grid roots.
- For each root:
  - Run camera and NDC math to set `screen_x`, `screen_y`.
  - Compute `distance_from_camera`.
  - Set `on_screen` if inside the viewport.

Screen Grid holds the list of roots for that frame and is rebuilt next frame.

### 1.5 Grid search model

- Each node has at most 6 children and depth is bounded by `max_layers`.
- `active_child_mask` indicates which child branches are non empty.
- Attach and detach operations maintain the active branch mask with upward propagation.
- Exact lookup for asset movement uses:
  - A hash map keyed by `(world_x, world_y, world_z, layer)`, or
  - Coordinate based traversal from the root using child pointers.
- Region queries and rendering traversals:
  - Start from Screen Grid roots.
  - Skip nodes where `assets_here` is empty and `active_child_mask == 0`.
  - Only recurse into children whose branch bits are set.
  - Cost is roughly proportional to the number of nodes that contain assets plus small tree overhead.

---

## 2. How to use this plan in Codex tasks

Every Codex task related to this refactor should:

1. State which phase it targets (for example: `Phase 3  Asset attachment and migration`).
2. Reference this plan by name:
   - Example:  
     `This task must follow the design and constraints in 3d_refactor_plan.md, Phase 3.`
3. Confirm that:
   - New APIs match the data model here.
   - There are no hidden assumptions that break 3D support.
   - Floor assets still behave correctly with `world_z = 0`.

---

## 3. Implementation phases

The phases below describe the high level goal, how the phase fits into the whole refactor, and the detailed subtasks.  
Each individual Codex task should pick a small subset of these subtasks and implement them.

---

### Phase 1  Define 3D GridPoint core

**Goal**  
Introduce the 3D `GridPoint` type that all other systems will depend on. This replaces the current 2D only layout and defines the canonical world identity, per frame fields, asset lists, and branch masks.

**How it fits**  
Every phase depends on `GridPoint` being stable and consistent. Do this first, even if many fields are not yet used.

**Key files**  
Likely `ENGINE/world/grid_point.hpp` and any companion `.cpp` or inline utilities.

**Subtasks**

1. Define world identity and hierarchy fields  
   - Add `world_z` and make `(world_x, world_y, world_z, resolution_layer)` immutable after construction.  
   - Add parent pointer and six child pointers:
     - `x_child_neg`, `x_child_pos`, `y_child_neg`, `y_child_pos`, `z_child_neg`, `z_child_pos`.
   - Provide a constructor or factory that sets identity and parent pointer.

2. Define per frame camera fields  
   - Add `screen_x`, `screen_y`, `distance_from_camera`, and `on_screen`.  
   - Make it clear these are non const and only valid for the current frame.  
   - Initialize them to safe defaults.

3. Define asset and branch tracking fields  
   - Add `assets_here` vector.  
   - Add `children_with_assets` vector.  
   - Add `active_child_mask` as a small integer bitmask with branch constants:
     - `BRANCH_X_NEG`, `BRANCH_X_POS`, `BRANCH_Y_NEG`, `BRANCH_Y_POS`, `BRANCH_Z_NEG`, `BRANCH_Z_POS`.

4. Add resolution and spacing helpers  
   - Define `max_layers` and `distance(layer) = 3^(max_layers  layer)` helpers.  
   - Provide an inline helper `grid_spacing_for_layer(int layer)`.

5. Update all existing uses of `GridPoint`  
   - Map current fields to the new layout or introduce adapters.  
   - For now, keep `world_z` default 0 for all legacy users.

**Codex guidance**  
Any task editing `GridPoint` must reference Phase 1 in `3d_refactor_plan.md` and confirm the type matches this spec.

---

### Phase 2  Map Grid redesign

**Goal**  
Replace the existing chunk based world grid and power of two resolution system with a Map Grid that owns persistent `GridPoint`s across all layers and the full world volume.

**How it fits**  
Map Grid is the source of truth for world space. Screen Grid, renderer traversal, asset attachment, and lighting all depend on it. This phase sets the new spatial index.

**Key files**  
Likely `ENGINE/world/grid.hpp`, `ENGINE/world/grid.cpp`, and any world grid manager classes.

**Subtasks**

1. Introduce Map Grid ownership model  
   - Define a Map Grid class that owns all `GridPoint`s (for example `std::unique_ptr` or object pools).  
   - Map Grid should create layer 0 roots that cover the map using `distance(0)`.

2. Implement spatial index  
   - Implement a hash map or other index keyed by `(world_x, world_y, world_z, resolution_layer)` to find nodes quickly.  
   - Provide `find_or_create_grid_point(world_x, world_y, world_z, layer)` and `find_grid_point` variants.

3. Replace legacy resolution logic  
   - Remove or bypass power of two grid resolution and bit shift logic.  
   - Use the `distance(layer)` helper for spacing and any grid snapping.

4. Child creation strategy  
   - Decide whether to:
     - Prebuild full trees down to `max_layers`, or  
     - Create children lazily when needed.  
   - Implement child creation functions that set parent pointers and identity correctly.

5. Integrate active branch tracking hooks  
   - Reserve a place for active branch propagation, even if not fully implemented yet.  
   - For example: internal helpers that will later call `propagate_branch_active` and `propagate_branch_inactive`.

6. Bridge old systems temporarily  
   - Add compatibility shims so old systems can still get some form of world sampling while Map Grid is under construction.  
   - Document that these shims will be removed in the Cleanup phase.

**Codex guidance**  
Tasks that modify world grid behavior must reference Phase 2 in `3d_refactor_plan.md` and explain how they move logic toward the Map Grid model and away from the old chunk centric model.

---

### Phase 3  Asset attachment and migration

**Goal**  
Move all asset spawn and movement logic to attach assets to `GridPoint`s in Map Grid, with `world_z` and resolution layers used consistently.

**How it fits**  
Assets must live on `GridPoint`s for grid search, Screen Grid, and renderer traversal to work correctly. This phase connects gameplay content to the new spatial model.

**Key files**  
Asset classes, spawn systems, asset loader, and any code that currently maps assets to grid cells or resolution slots.

**Subtasks**

1. Extend asset spawn and move APIs  
   - Update asset spawn and move functions to accept `world_z` (default 0).  
   - Decide how to choose `resolution_layer` based on asset size or collision cell.

2. Implement attach and detach helpers  
   - Implement:
     - `attach_asset_to_grid_point(Asset* asset, GridPoint* node)`  
     - `detach_asset_from_grid_point(Asset* asset, GridPoint* node)`  
   - Handle `assets_here`, `children_with_assets`, and active branch masks in these helpers.

3. Map existing 2D z offset to world_z  
   - Identify existing fields that act as a fake height or vertical offset.  
   - Decide which ones become real `world_z` and which ones stay as purely visual offsets.  
   - Apply a clear convention for floor assets.

4. Data migration and defaults  
   - Ensure existing content that does not specify z still spawns at `world_z = 0`.  
   - Set safe defaults in JSON loaders and map loading code.

**Codex guidance**  
Any task that changes how assets are spawned or moved must reference Phase 3 in `3d_refactor_plan.md` and confirm that all assets attach to `GridPoint`s and that floor assets use `world_z = 0`.

---

### Phase 4  Screen Grid reconstruction

**Goal**  
Build a per frame Screen Grid that references Map Grid nodes in a visible 3D volume and updates per frame camera fields.

**How it fits**  
Screen Grid is the runtime gateway between world space and rendering. The renderer and many searches will start from Screen Grid roots.

**Key files**  
Camera grid modules, warped screen grid code, anything that currently rebuilds 2D screen grids or warp samples.

**Subtasks**

1. Define Screen Grid class  
   - Screen Grid should:
     - Own no `GridPoint`s.  
     - Contain lists of `GridPoint*` roots for the current frame.  
     - Expose traversal helpers.

2. Implement per frame rebuild  
   - Given camera state:
     - Compute world volume that covers the visible area plus margin.  
     - Query Map Grid for nodes in that volume at relevant layers.  
     - Store them as Screen Grid roots.

3. Update per frame camera fields  
   - For each root (and possibly its subtree):
     - Call camera and NDC functions to compute `screen_x`, `screen_y`, and `distance_from_camera`.  
     - Compute `on_screen` based on the viewport rectangles.

4. Remove or adapt old flat screen grid code  
   - Replace `WarpedScreenGrid::rebuild_grid` style flat lists with references to `GridPoint`s.  
   - Maintain any debug overlays using the new structure.

**Codex guidance**  
Tasks that touch screen grid or camera to screen mapping must reference Phase 4 and must not reintroduce new 2D only grid structures. Screen Grid is references only, all ownership stays in Map Grid.

---

### Phase 5  Hierarchical search and active branch maintenance

**Goal**  
Make grid operations fast by leveraging the tree structure and active branch masks instead of scanning flat lists or all nodes.

**How it fits**  
This phase is what makes the hierarchy worth having. It changes asset movement, region queries, and renderer traversal to use branch activity instead of linear search.

**Key files**  
`GridPoint` helpers, Map Grid, search utilities, any code doing grid scans or neighbor searches.

**Subtasks**

1. Define branch bit constants and mask field  
   - Add `BRANCH_X_NEG`, `BRANCH_X_POS`, `BRANCH_Y_NEG`, `BRANCH_Y_POS`, `BRANCH_Z_NEG`, `BRANCH_Z_POS`.  
   - Confirm `active_child_mask` exists and is stored on `GridPoint`.

2. Implement upward propagation helpers  
   - Implement `propagate_branch_active(GridPoint* child)`:
     - On the parent, set the bit for the branch corresponding to `child`.  
     - If the parent was previously completely inactive and is now active, continue upward.  
   - Implement `propagate_branch_inactive(GridPoint* child)`:
     - Clear the bit for that branch.  
     - If the parent has no `assets_here` and all bits are clear, mark it inactive and continue upward.

3. Integrate with attach and detach flows  
   - When attaching to a node that was empty, call `propagate_branch_active` upward.  
   - When detaching so that the node becomes empty and has no active children, call `propagate_branch_inactive` upward.

4. Implement search APIs  
   - Exact lookup:
     - Use either the spatial index or child traversal to get a node for `(x, y, z, layer)` in O(1) or O(depth).  
   - Region queries:
     - Given a region, start at a high level node and recursively:
       - Skip any node that is outside the region.  
       - Skip any node where `assets_here` is empty and `active_child_mask == 0`.  
       - Only recurse into branches whose active bit is set.  
   - Traversal for rendering:
     - See Phase 7, but use the same pattern: skip empty nodes and inactive branches.

**Codex guidance**  
Tasks that add or modify grid search logic must reference Phase 5 and must not add new linear scans over all points unless they are clearly marked as debug only.

---

### Phase 6  Camera and transform integration

**Goal**  
Extend camera and NDC transforms to understand `world_z` and the new grid, while keeping floor assets visually consistent.

**How it fits**  
This phase connects the vertical dimension to real perspective and parallax instead of fake offsets.

**Key files**  
Camera classes, NDC utilities, parallax calculations, any existing depth or z offset code.

**Subtasks**

1. Identify existing 2D camera code paths  
   - Find all places where world to screen mapping is computed for assets or grid.  
   - List any current flags that control depth, perspective, or warp.

2. Extend transforms to include world_z  
   - Modify projection functions to accept `(world_x, world_y, world_z)`.  
   - Decide how `world_z` changes:
     - Screen space Y offset.  
     - `distance_from_camera`.  
     - NDC Z (if used).

3. Reinterpret or rename z offset fields  
   - Any current `z_offset` or height hack should be examined.  
   - Decide which become:
     - Real `world_z` in Map Grid.  
     - Visual offsets only for sprite alignment.

4. Maintain compatibility for floor assets  
   - Ensure that content with `world_z = 0` is rendered equivalently to the old system, or with predictable adjustments.  
   - Add tests or debug overlays to verify this.

**Codex guidance**  
Tasks that change camera transforms must reference Phase 6 and must document how `world_z` now flows into screen positions and depth values.

---

### Phase 7  Renderer traversal and LOD

**Goal**  
Have the renderer use Screen Grid roots and the grid hierarchy to draw assets, instead of relying on flat lists. Use `distance_from_camera` and `resolution_layer` to drive draw order and LOD.

**How it fits**  
This phase changes how content is drawn. It uses the new tree for culling and ordering.

**Key files**  
Renderers such as `SceneRenderer`, `GridTileRenderer`, composite render passes, and any place that currently walks `visible_assets_` or similar lists.

**Subtasks**

1. Define render entry points from Screen Grid  
   - Replace or wrap existing render loops to start from Screen Grid root lists rather than raw vectors of assets.

2. Implement recursive traversal  
   - For each root:
     - If `assets_here` is empty and `active_child_mask == 0`, skip this node.  
     - If `on_screen` is true:
       - Optionally draw grid debug at `screen_x`, `screen_y`.  
       - Draw each asset in `assets_here`, using `screen_x`, `screen_y` and `distance_from_camera`.  
     - Recurse into child pointers only when the corresponding branch bit is set.

3. Integrate LOD and resolution  
   - Use `resolution_layer` to pick LOD variants or control detail.  
   - Decide if very fine layers can be grouped or skipped under heavy load.

4. Update composite and light map passes  
   - Make sure passes that currently rely on 2D depth or tile indices now use:
     - `world_z` for depth.  
     - `distance_from_camera` for sorting.

**Codex guidance**  
Render tasks must reference Phase 7 and verify that they keep the traversal tree aware and that they skip empty branches using `active_child_mask`.

---

### Phase 8  Lighting and 3D usage

**Goal**  
Attach lights using full 3D coordinates and integrate `world_z` into attenuation, shadow behavior, and depth ordering.

**How it fits**  
Lighting is the main early user of real height. This phase validates that the 3D grid is useful beyond floor assets.

**Key files**  
Light asset definitions, light map generation, any runtime light queries or attenuation logic.

**Subtasks**

1. Attach lights to 3D GridPoints  
   - When spawning lights, set `world_x`, `world_y`, `world_z` and attach to the correct `GridPoint`.  
   - Confirm that they participate in branch activation like any other asset.

2. Integrate world_z into light math  
   - Update attenuation to use full 3D distance if needed.  
   - Update shadow or falloff logic to account for height.

3. Update light queries  
   - When sampling lights for a given world position, use Map Grid and branch aware search rather than flat arrays.  
   - Limit queries to relevant volumes, not the entire grid.

**Codex guidance**  
Lighting tasks must reference Phase 8 and must not rely on old 2D only light assumptions. All lights should have a well defined `world_z`.

---

### Phase 9  Dev tools and debug UX

**Goal**  
Update dev tools and debug visualizations to understand the 3D hierarchy and make the new system easy to inspect and tune.

**How it fits**  
Without good tools, the new system will be hard to debug. This phase ensures the hierarchy is visible and interactive in dev mode.

**Key files**  
Dev panels, camera UI, grid overlays, debug rendering layers, dev input handlers.

**Subtasks**

1. 3D grid overlays  
   - Add overlays that can show:
     - Map Grid nodes on the floor plane.  
     - Optional layers or slices at different `world_z`.  
     - Screen Grid nodes and their `on_screen` status.

2. Dev mouse controls  
   - Implement tools for:
     - Selecting a `GridPoint` under the mouse.  
     - Displaying its world identity and branch activity.  
     - Stepping through `world_z` planes.

3. Phase specific toggles  
   - Add toggles that highlight:
     - Active branches.  
     - Nodes with assets.  
     - Nodes used by Screen Grid in the current frame.

**Codex guidance**  
Dev tool tasks must reference Phase 9 and must build on the Map Grid and Screen Grid concepts rather than reintroducing ad hoc 2D debug grids.

---

### Phase 10  Cleanup and deprecation

**Goal**  
Remove obsolete 2D grid and resolution utilities once the new system is fully operational and call sites have been migrated.

**How it fits**  
This phase tidies up after the migration and ensures there is a single, consistent spatial model in the engine.

**Key files**  
Any file that holds old grid utilities, 2D only resolution logic, or compatibility shims.

**Subtasks**

1. Identify deprecated APIs  
   - List:
     - Old grid resolution functions.  
     - Chunk centric world grid operations that duplicate Map Grid.  
     - Old z offset hacks that have been replaced by `world_z`.

2. Remove or gate them  
   - Remove code that is no longer used.  
   - If some sites still depend on old utilities, add clear TODO tags pointing back to this phase and `3d_refactor_plan.md`.

3. Documentation and comments  
   - Update any existing design docs to reference `GridPoint`, Map Grid, and Screen Grid as the canonical model.  
   - Record any remaining technical debt in a clear backlog.

**Codex guidance**  
Cleanup tasks must reference Phase 10 and `3d_refactor_plan.md` and must not remove any code that is still required by earlier phases without providing a migration path.

---

## 4. Summary

- `GridPoint` is the single source of spatial truth for assets and camera transforms.
- Map Grid owns and indexes all `GridPoint`s in the world.
- Screen Grid is rebuilt per frame and references only the nodes relevant to the current camera.
- Active branch masks and hierarchical search make queries and movement fast.
- Camera, renderer, and lighting are refactored to use 3D world positions while keeping floor assets simple.
- Dev tools expose the hierarchy and make it debuggable.

Every Codex task involved in this refactor must reference this plan and state which phase it is implementing or supporting.
