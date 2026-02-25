# Terrain Validation — Deterministic correctness

Context. Implementation in ENGINE/runtime/rendering/render/terrain_runtime_state.cpp, ENGINE/runtime/rendering/render/terrain_field.cpp, and ENGINE/runtime/gameplay/world/grid_point.{hpp,cpp} wires the runtime state, sampling, and per-point caches described in the original plan except for item 7 validation. The remaining work is to prove deterministic heights, flat rooms and trails, and seam closure for every map load.

1. Deterministic regression coverage for the runtime state.
   - Write a fixture that instantiates sanitized terrain settings, calls TerrainRuntimeState::from_settings in ENGINE/runtime/rendering/render/terrain_runtime_state.cpp, and asserts that the session seed and light direction stay the same for a fixed map_id when randomize_session_seed is false.
   - Confirm that tweaking the manifest fields that feed the light base seed changes the derived session seed while the revision counter always advances when the settings mutate.

2. Room and trail flattening plus seam closure.
   - Drive TerrainField::sample_elevation and the region query logic in ENGINE/runtime/rendering/render/terrain_field.cpp with synthetic room bounds to show that points inside a room or trail remain flat and the edge falloff curve smoothly blends to the generated noise outside the bounds.
   - Sample adjacent U/V corners while reusing the grid point hash so cached results stay synchronized across cells and no visible seam appears between tiles.

3. GridPoint caching and slope data.
   - Toggle terrain_state.settings.enabled and terrain_revision around gameplay/world/grid_point.hpp to verify that terrain elevation and slopes reset when terrain is disabled while needs_projection_update flips true after a revision bump.
   - Capture the cached slope and revision metadata after WarpedScreenGrid::rebuild_grid runs to prove the stored values can be replayed across frames without recomputation when the revision stays constant.

