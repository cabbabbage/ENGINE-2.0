Terrain Integration Plan (engine-specific, remaining work)
Context: terrain_settings.hpp, terrain_field.{hpp,cpp}, and manifest parsing in SceneRenderer already exist; keep them as-is.

1) Map-load terrain bootstrap (critical loading step)
- Add a TerrainRuntimeState (under rendering/render/) that holds sanitized TerrainSettings, the finalized session_seed, frozen light vector/anchor, and a terrain_revision.
- Build this state during SceneRenderer construction / map reload where map_manifest is already parsed in render.cpp. Combine TerrainSettings::light.base_seed, map_id, and an optional random flag to pick session_seed; lock the light once per map load.
- Expose TerrainRuntimeState to TerrainField, WarpedScreenGrid, and GridTileRenderer. Bump terrain_revision whenever the map reloads or settings change so downstream caches know to refresh.

2) TerrainField alignment
- Add initialize/reset that accepts TerrainRuntimeState; move seed/light sourcing here instead of per-frame. Keep the existing room/trail region index and flattening logic.
- Key caches by terrain_revision + frame_id; flush when runtime state changes or rooms hash changes. Honor lock_seed_to_world by hashing world coords into the seed when enabled.

3) GridPoint render-only terrain data
- Add terrain_elevation (float), optional slope_x/slope_y or normal, and terrain_revision (u64) to gameplay/world/grid_point.{hpp,cpp}.
- Reset these in constructors/reset_frame_state. Extend needs_projection_update to return true when the stored terrain_revision differs from TerrainRuntimeState.revision so reprojection happens when terrain changes.

4) Sample during WarpedScreenGrid::rebuild_grid
- After collecting visible GridPoints, sample TerrainField using TerrainRuntimeState for those points (skip when terrain_revision already matches).
- Store elevation (and slope/normal) on the GridPoint; room/trail flattening stays handled inside TerrainField.
- Optionally precompute neighbor samples (+/- grid spacing) for cheap normals; do this only for points inside the current frustum to bound cost.

5) Project floor geometry with elevation
- In GridTileRenderer::render, fetch corner heights (from GridPoints when available; otherwise sample TerrainField per corner using grid spacing/default layer).
- Pass corner world_z into cam.project_world_point before warp_floor_screen_y; preserve enforce_trapezoid, batching, and depth ordering.
- Ensure height sampling respects terrain_revision and uses the shared TerrainRuntimeState.

6) Stable shading
- Add terrain_shading.{hpp,cpp} helper to compute brightness from slope/normal and TerrainRuntimeState light; clamp using light_strength/contrast.
- Apply per-vertex color in GridTileRenderer so shading stays fixed across camera motion and map reloads.

7) Validation
- Terrain stays deterministic per map load; rooms/trails remain flat; seams stay closed because corner samples are shared; caches flush on revision change; performance remains bounded to visible points.
