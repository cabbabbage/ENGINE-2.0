# Axis Conventions

This section documents the canonical 3D axis ordering for the runtime, rendering, and Dev Mode tooling. Every new type, helper, comment, or test dealing with world coordinates must align with this scheme so the migration from the old Z-up mentality stays consistent.

## Canonical axes

- **X (right):** increases toward the right-hand side of a room or viewport. It is used for horizontal offsets and left/right movement.
- **Y (up / height):** represents vertical elevation. Room geometry, camera eye height, and any “up/down” offsets live on this axis.
- **Z (forward / depth):** points into and out of the screen. It drives depth sorting, perspective scaling, and world grid spacing.

The axis:: namespace in `runtime/core/axis_convention.hpp` provides `axis::WorldPos`, helpers like `is_height`, and a compile-time guard (`axis::kUsingLegacyAxisOrdering`) that stay `false` while migrations continue.

## Non‑negotiable conventions

1. **Use the canonical struct whenever you represent a world point.** Favor `axis::WorldPos` over ad-hoc structs or tuples. Any accessor that returns a coordinate should map `X->right`, `Y->height`, `Z->depth`.
2. **Grid helpers continue to expect world_z as depth.** The `world::GridPoint` API stores `world_z()` as the depth field even when helpers project it onto the screen; keep `world_z` lining up with the forward/back axis and treat planar offsets with `world_pos.x`/`world_pos.y`.
3. **Avoid escaping axis metadata into comments or identifiers.** Descriptions that tie the Z axis to vertical height will be flagged.
   Treating `world_z` as a vertical distance or elevation also trips the scanner. If you need to explain how a projection works, describe the actual axis role (e.g., “the separate `world_z` argument carries vertical offsets while `world_pos.y` tracks planar depth”).
4. **Update legacy helpers while removing temporary aliases.** Builders like `render_pipeline::BuildScalingProfiles` now live nowhere in the code, and compatibility aliases such as `ZDisplayMode` or `world_grid::kBranch*` are removed to keep the API surface focused on the final coordinate system.

## Guarding the codebase

- The repository now includes `scripts/check_legacy_markers.py`, and the `CMakeLists.txt` file wires it into the new `legacy_axis_marker_check` target. Running `cmake --build . --target legacy_axis_marker_check` (or building `engine`/`engine_tests`) executes the script and fails if any banned patterns remain.
- When adding new tooling, note whether the axis names should appear in prose. Prefer describing spelled-out roles like “vertical offset” or “depth value” rather than reusing the legacy phrasing.

Keep this note in sync with `engine/runtime/core/axis_convention.hpp`, `gameplay/world/grid_point.hpp`, and the rendering helpers (`rendering/render/warped_screen_grid.hpp`, `world_grid`, etc.). It is the authoritative reference for everyone touching coordinates during the migration.
