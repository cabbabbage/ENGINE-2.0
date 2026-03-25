Anchor point contract (runtime + editor must stay in lockstep)

- Canonical anchors are integer `texture_x`/`texture_y` that point at the center of a source texture pixel; origin is top-left, +X right, +Y down.
- Anchors also store integer `depth_offset` (signed world-pixel units). Runtime resolves the flat texture-relative world point first, then applies `depth_offset` along the normalized `camera -> flat_point` ray:
  - `depth_offset > 0` moves farther from camera.
  - `depth_offset < 0` moves closer to camera.
- Box extrusion uses the same ray convention and creates symmetric endpoints around the flat point (`-extrusion` near, `+extrusion` far).
- Offset and extrusion are only small displacements from the resolved flat world point; they do not replace anchor world projection.
- Runtime conversion (`anchor_pixel_to_uv` in `ENGINE/runtime/assets/asset/anchor_point.hpp`) uses `u = (x + 0.5) / width`, `v = (y + 0.5) / height`, clamped to [0, 1].
- Horizontal flips happen after conversion: if the render path is flipped (e.g., `asset.flipped`), use `u = 1 - u`. `v` is not flipped at runtime; derived animations that flip textures must pre-flip anchor pixels when cloning.
- The editor preview (`_texture_to_canvas` / `_canvas_to_texture` in `scripts/anchor_point_editor.py`) applies the same pixel-center math and mirrors the geometry inversion settings so the overlay matches runtime placement.
- When changing anchor math, update both the runtime helper and the editor helpers together so manifests stay canonical.

## Axis convention (2026-03 refresh)
- X = right/left
- Y = height/up
- Z = depth/forward
- Editor tooling must not treat Z as height; all saved data and previews use Y-up, Z-forward.
