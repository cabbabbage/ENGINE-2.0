# Terrain Validation — Runtime performance

Context. TerrainField and WarpedScreenGrid already implement the caching and frustum boundaries described by the plan, so the remaining work is to instrument and prove the runtime invariants.

1. Cache eviction instrumentation and regression.
   - Add instrumentation around TerrainField::reset_cache_if_needed, sync_runtime_state, and the frame cache to log when caches clear, record the cache size, and confirm resets happen when the frame_id or revision changes.
   - Expose a developer overlay or telemetry counter that reports the cache size before and after each rebuild so runaway memory usage becomes visible.

2. Frustum-limited sampling counts.
   - Record how many grid points call TerrainField::sample_elevation inside WarpedScreenGrid::rebuild_grid to prove we only sample nodes that fall inside the visible frustum.
   - Compare that count to visible_points_.size() and expose a warning if the ratio grows beyond the current cull margin so sampling stays bounded.

3. Projection invalidation after revisions.
   - Simulate a revision bump and assert that GridPoint::needs_projection_update returns true and that the rebuild path reprojections exactly once.
   - Log the number of GridTileRenderer vertices that depend on terrain_revision so we can tell whether projection work spikes after a revision change.

