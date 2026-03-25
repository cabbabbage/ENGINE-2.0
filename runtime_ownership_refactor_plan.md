# runtime_ownership_refactor_plan.md

## Instructions

Verify the progress of each task for this plan.  
If a task is complete then update this file.  
If it is not complete complete it then update this file.  
This plan will be complete when we have verified that all tasks have been completed.

---

## Overview

This plan represents a full consolidation and correction of the engine's runtime ownership model, data flow, and system boundaries.

The goal is not incremental cleanup. The goal is to:

- Establish a single, correct ownership model across all runtime systems
- Remove duplicated data representations and parallel systems
- Clearly separate world state, render state, and runtime behavior
- Eliminate legacy and fallback logic
- Make the engine predictable, debuggable, and extensible

This is a structural refactor. Behavior should remain equivalent, but implementation will change aggressively.

---

## Task 1: Establish a Single Source of Truth for Ownership (Rooms + Assets)

**Description:**  
Fix all ownership inconsistencies across the runtime. Rooms and assets must have a single, unambiguous owner at all times. No system should “kind of own” something.

**Core Rules:**
- Rooms are owned only by `RuntimeWorldContext`
- Assets are owned only through `WorldGrid`
- No raw pointer ownership recovery
- No duplicate ownership paths

**Includes:**
- Remove:
  - `AssetLoader::all_rooms_`
  - `AssetLoader::rooms_`
  - Any `Assets::rooms_` storage
- Ensure all systems access rooms through `RuntimeWorldContext` only
- Ensure `RuntimeWorldContext`:
  - Owns `std::vector<std::unique_ptr<Room>>`
  - Exposes a stable `std::vector<Room*>` view
  - Tracks topology generation for invalidation
- Refactor bootstrap:
  - Loader populates `RuntimeWorldContext`
  - Loader is not required for runtime ownership after bootstrap
- Enforce strict ownership in `WorldGrid`:
  - `attach_asset_to_grid_point` must take `std::unique_ptr<Asset>`
  - Remove any fallback paths that reconstruct ownership from raw pointers
- Refactor `move_asset`:
  - If ownership is missing → log and return failure
  - Do NOT mutate any state on failure
- Audit all callsites:
  - Ensure no system assumes ownership without going through `WorldGrid`
  - Ensure no system writes to invalid or partially-owned state

**Validation:**
- Every asset is owned exactly once
- No raw pointer can become an owner
- Room lifetime is valid even if loader is destroyed

**Likelihood of Completion:** 85%

---

## Task 2: Fully Separate World State from Rendering and Region Logic (GridPoint Refactor)

**Description:**  
Fix `GridPoint` so it represents only world structure. Rendering data and region logic must not live inside it.

Right now it is overloaded and mixing:
- Ownership
- Spatial structure
- Projection data
- Region classification

This must be split.

**Includes:**

### GridPoint Decomposition
- Split into:
  - **Core node (world state):**
    - Occupants
    - Coordinates
    - Hierarchy/links
    - Activity flags
  - **Projection cache subobject:**
    - Screen position
    - Perspective scale
    - Distance to camera
    - Visibility flags
- Remove all projection fields from main GridPoint
- Update all callsites:
  - Rendering must use projection cache only
  - No direct field access

### Region Model Cleanup
- Remove:
  - `region_kind`
  - `region_owner`
  - Any region metadata stored in GridPoint
- Make region classification exist ONLY in `DynamicBoundarySystem`
- Remove any writeback from boundary system into GridPoint
- Ensure all region queries go through the boundary system

### Invalidation and Lifecycle
- Ensure projection cache is:
  - Explicitly invalidated
  - Recomputed per frame or camera update
- Ensure region cache invalidates correctly on:
  - Room topology changes
  - Generation increments

**Validation:**
- GridPoint contains no rendering or region logic
- Rendering works entirely off projection cache
- Region classification is consistent across systems

**Likelihood of Completion:** 70%

---

## Task 3: Unify Animation Runtime Ownership and Remove All Duplicate Representations

**Description:**  
Fix animation so there is only one real runtime representation of frames.

Currently there are multiple overlapping concepts:
- movement_paths_
- frame pointer views (`.frames`)
- cache/bundle representations

This must be unified.

**Includes:**

### Ownership Model
- `movement_paths_` becomes the ONLY frame owner
- Remove:
  - Public `Animation::frames`
  - Any pointer-view representation
- Add and enforce:
  - `has_frames()`
  - `frame_count()`
  - `primary_frames()`
  - `primary_frame_at()`

### Callsite Migration
- Replace ALL `.frames` usage across:
  - Runtime systems
  - Editor systems
  - Tests
  - Code generation (animation editor)
- Ensure no stale references remain

### Cache Separation
- Treat cache system as:
  - Load-time input ONLY
- Ensure:
  - No runtime logic depends on cache structures
  - CacheManager / bundle system does not leak into runtime ownership

### Binding and Sync
- Ensure correct binding of:
  - Textures
  - Frame metadata
- Validate:
  - Loader
  - Cloner
  - Cache adoption paths

**Validation:**
- Only one frame ownership path exists
- No `.frames` references remain
- Runtime works without knowledge of cache structures

**Likelihood of Completion:** 75%

---

## Task 4: Eliminate Redundant Systems, Indexes, and Legacy Code

**Description:**  
Remove duplicated systems and enforce a single source of truth for all runtime data.

The engine currently maintains multiple parallel representations of the same data.

This creates sync bugs and unnecessary complexity.

**Includes:**

### Index Consolidation
- Audit and unify:
  - `WorldGrid` indexing
  - `Assets` active lists
  - `WarpedScreenGrid` mappings
- Remove:
  - Duplicate maps
  - Redundant caches
- Ensure:
  - All systems derive from one canonical index

### Dead System Removal
- Remove:
  - `AssetList` / neighbor system
  - `TextureLoadQueue` (if not used)
- Remove any unused helper systems

### Legacy and Fallback Logic
- Remove:
  - Compatibility layers
  - Fallback behavior
  - Temporary refactor scaffolding
- No “just in case” logic

### Data Flow Simplification
- Ensure:
  - Systems do not duplicate work
  - Data is not recomputed in multiple places unnecessarily

**Validation:**
- No duplicate runtime indexes exist
- No dead systems remain
- Data flows through one clear path

**Likelihood of Completion:** 60%

---

## Task 5: Decompose Core Systems and Establish a Clear Runtime Pipeline

**Description:**  
Fix the “god object” problem and define a clean runtime pipeline.

Right now, `Assets` and `Asset` do too much and mix responsibilities.

We need clear system boundaries.

**Includes:**

### Asset Decomposition
- Split `Asset` into logical components:
  - RenderState
  - AnchorState
  - CombatState
  - Movement/RuntimeState
- Reduce cross-cutting logic

### Assets Manager Cleanup
- Reduce responsibilities of `Assets`
- Remove mixed ownership, rendering, and update logic

### Pipeline Definition
Introduce explicit stages:
- `WorldUpdateStage`
- `VisibilityStage`
- `RuntimeEffectsStage`
- `RenderStage`

Each stage:
- Has a clear responsibility
- Does not overlap with others
- Consumes and produces well-defined data

### API Simplification
- Reduce large, ambiguous interfaces
- Ensure each system has a narrow responsibility

**Validation:**
- No system acts as a “god object”
- Runtime flow is predictable and staged
- Responsibilities are clearly separated

**Likelihood of Completion:** 50%

---

## Task 6: Final Validation, Testing, and Graph Alignment

**Description:**  
Ensure the system is correct, stable, and accurately represented.

This step verifies everything above and removes any remaining inconsistencies.

**Includes:**

### Graph Alignment
- Update `runtime_ownership.drawio.xml`:
  - Add `RuntimeWorldContext`
  - Remove old room ownership
  - Remove animation pointer-view
  - Reflect GridPoint decomposition
  - Reflect animation ownership
  - Fix incorrect edges

### Ownership Validation
- Verify:
  - No dangling references
  - No duplicate ownership
  - Clean lifecycle:
    - Bootstrap → runtime → shutdown

### Testing
- Add tests for:
  - Ownership invariants
  - Asset transfer correctness
  - Index consistency
  - Region consistency
  - Animation integrity

### Runtime Validation
- Run engine and verify:
  - Rendering correctness
  - Asset placement
  - Region behavior
  - Animation playback
- Ensure no performance regressions

### Final Cleanup
- Remove:
  - Temporary code
  - Refactor scaffolding
  - TODOs and debug hacks

**Validation:**
- Diagram matches code exactly
- Engine runs correctly with no hidden issues
- No leftover refactor artifacts

**Likelihood of Completion:** 80%

---

## Notes

This plan is a compressed version of the full detailed plan fileciteturn2file0, grouped by system-level concerns instead of individual tasks.

**Biggest Risks:**
- Incomplete GridPoint separation
- Missed `.frames` usage in editor or generated code
- Hidden duplicate indexing systems

**Biggest Wins:**
- One clear ownership model
- Predictable runtime behavior
- Major reduction in complexity and bugs
- Strong foundation for future engine work
