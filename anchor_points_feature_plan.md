# Displaced Anchor Points Plan

## Outcome
- Per-frame anchor offsets (percent-of-height + rotation) defined in JSON and loaded into `AnimationFrame` objects.
- Runtime API for controllers to request named anchors, update them explicitly, and read world grid + rotation without implicit recomputation.
- Deterministic world resolution helper that turns percent offsets into grid points using the asset’s current anchor, height, scale, and grid resolution.
- Dev-mode frame editor that mirrors the movement editor UX to create/edit/delete anchor paths per frame, persisting immediately to manifest JSON.

## Data Model & Schema
- New struct `DisplacedAssetAnchorPoint { std::string name; float px, py, pz; float rotation_deg; }` stored on every `AnimationFrame` as `std::vector<DisplacedAssetAnchorPoint> anchor_points`.
- Semantics: `px/py/pz` are percentages of the asset’s current post-scale pixel height applied along world X/Y/Z relative to the frame anchor (bottom-middle of the asset). Rotation is degrees around Z, right-hand, 0 = facing camera forward; sign chosen to match existing attack/hit geometry conventions (clockwise positive unless codebase uses counter-clockwise—finalize during implementation).
- JSON schema per animation payload: add `anchor_points` array sized to frame count. Each entry is an array of anchors for that frame:
  ```json
  "anchor_points": [
    [ { "name": "muzzle", "px": 0.00, "py": -0.05, "pz": 0.65, "rotation": -15.0 } ],
    [ { "name": "muzzle", "px": 0.02, "py": -0.04, "pz": 0.65, "rotation": -10.0 },
      { "name": "offhand", "px": -0.08, "py": 0.02, "pz": 0.55, "rotation": 45.0 } ],
    ...
  ]
  ```
  Missing `anchor_points` means “no anchors” for that frame; missing `rotation` defaults to `0`.
- Validation rules: names must be non-empty/unique per frame; `px/py/pz` finite; `pz` clamped [0,1] when saving; `anchor_points` array auto-resized when frame count changes.
- Derived animations/cloning: `animation_cloner.cpp` mirrors anchor data. Horizontal flip negates `px` and rotation (and optionally `py` if vertical flip). Reverse source reorders anchor arrays to align with reversed frames.

## Runtime Resolution & APIs
- Helper `resolve_anchor_point(...)` (world map/grid helper or an extension of `FramePointResolver`) takes the owning `Asset` (or grid point + runtime height) and a `DisplacedAssetAnchorPoint`, returning `{ SDL_Point world_px; world::GridPoint* grid_point; float rotation_deg; }`.
  - Base origin: `animation_update::detail::bottom_middle_for(asset, asset.world_point())`.
  - Height source: “final” pixel height after scale/variant/perspective (derive from `Asset::height()` + `last_scale_usage_` or expose a dedicated `runtime_height_px()` that uses the active variant/scale pipeline).
  - Convert percent -> world offset: `offset_x = px * height_px`, `offset_y = py * height_px`, `offset_z = pz * height_px`.
  - World grid lookup: use asset grid point’s resolution/layer; prefer `WorldGrid::find_grid_point_strict` fallback to `find_or_create` to avoid nullptr; keep both world pixels and grid pointer.
- Runtime anchor handle owned by `Asset`: `struct AnchorHandle { std::string name; world::GridPoint* grid; SDL_Point world_px; float rotation; bool dirty; int last_frame_index; std::string last_anim; void update(); };`
  - `Asset::get_anchor_point(const std::string&)` creates or retrieves a handle keyed by name (map + stable vector for iteration).
  - `AnchorHandle::update()` fetches the current `AnimationFrame`, finds the matching `DisplacedAssetAnchorPoint` (fast lookup table built per frame on load), calls `resolve_anchor_point`, stores grid/px/rotation, clears `dirty`.
  - `Asset` marks all handles dirty when animation/frame changes, when scale/height changes, or when world position changes (hook into `AnimationRuntime::advance`, `Asset::move_to_world_position`, `Asset::on_scale_factor_changed`, and `set_current_animation`).
  - Fallback behavior: if the frame lacks the requested anchor, default to base anchor (px/py/pz = 0, rotation = 0) and flag “missing” so controllers can branch.
  - Optional convenience: `std::optional<ResolvedAnchor> Asset::anchor_state(const std::string&)` that calls `update()` if dirty and returns immutable data for controllers/renderers.

## Serialization / Loader Pipeline
- `animation_loader.cpp`: parse `anchor_points` alongside `movement`, `hit_geometry`, `attack_geometry`. Ensure array size matches frames; fill missing frames with empty vectors; ignore invalid entries gracefully. Store parsed anchors on every `AnimationFrame` in every movement path (so primary `frames` vector and `movement_paths_` remain consistent).
- `animation_frame.hpp`/`animation.hpp`: expose `anchor_points` accessors, optionally a per-frame name->index map built after load for O(1) lookups.
- `animation_cloner.cpp`: copy anchors; apply flips/reverse; ensure derived animations inherit anchors unless explicitly overridden.
- `AssetInfo` / manifest mutation (`AssetInfo::upsert_animation`, `animation_editor::AnimationDocument`): preserve `anchor_points` when updating payloads; don’t strip unknown fields.
- Manifest cleaner/sanitizers (`clean_manifest.py`, etc.) updated to keep or normalize `anchor_points`.

## Dev-Mode Editor (Anchor Path Editor)
- New editor mode (e.g., `AnchorFrameEditor`) under `ENGINE/editor/devtools/frame_editors/` added to `FrameEditorLaunchMode`, `FrameEditorSession`, and the animation editor UI so it can be launched like Movement/Hit/Attack.
- Data state: introduce `AnchorFrame`/`FrameAnchorPoint { name, px, py, pz, rotation }` plus parse/build helpers (`parse_anchor_frames_from_payload`, `build_payload_with_anchors`) that merge with existing payload fields so no data loss. Resizes `anchor_points` array to frame count; preserves existing movement/hit/attack data untouched.
- UI/UX (mirrors Movement editor):
  - Frame navigator + “apply to all frames” for quick propagation.
  - Anchor list panel (create/rename/delete, enforce unique names) and per-anchor color coding.
  - World overlay: render path for selected anchor across frames, draw current frame point + rotation arrow/gizmo; optional toggle to show all anchor paths ghosted.
  - Editing: use `Point3DEditor` in percent mode (Z percent) with parent height set from the asset; map between percent and world via `resolve_anchor_point` so dragging snaps to grid while writing back percent values; rotation textbox/slider with live preview.
  - Live persistence: `ManifestTransaction` immediate save; update preview provider and invalidate cached frame builds so the viewport reflects edits instantly.
  - Dev-only controls: show/hide anchor visuals, “reset frame anchor to zero,” copy/paste anchor frame values.

## Runtime Integration for Controllers
- Target API: `auto& anchor = asset->get_anchor_point("muzzle"); anchor.update(); auto gp = anchor.grid; auto rot = anchor.rotation;`
- Provide helper to bind to controller lifetime (handles store pointer to owning asset, so they remain valid until asset destruction).
- Optionally expose `Asset::anchor_points()` for iteration (e.g., to draw debug overlays).
- Ensure `FramePointResolver` (or new helper) reusable in runtime and editor to avoid drift between edit-time and play-time math.

## Edge Cases & Compatibility
- Assets without `anchor_points` remain unchanged; handles simply return base anchor.
- Frame count changes (cloning, atlas rebuilds, movement edits) auto-resize anchor arrays to avoid index mismatches.
- Flipped/derived animations adjust offsets/rotations consistently; document the sign rules in code comments.
- Multi-path animations: ensure anchors attach to every path instance so `path_index` changes don’t desync anchor lookup.

## Testing & Validation
- Unit tests: JSON parse/build round-trips for `anchor_points`; resolve helper converts percent→world correctly with varying heights/resolutions; flip/reverse transformations keep anchors aligned with frames.
- Integration tests: animation runtime updates handles when frames advance; controller mock reads anchor positions; anchors stay stable under scale changes.
- Dev-mode manual checks: create anchors, drag in viewport, verify JSON updates and runtime preview; reload manifest to ensure data persists.
- Add a debug overlay (dev toggle) to render resolved anchors for the currently selected asset at runtime to visually verify alignment.

## Implementation Order
1) Define runtime structs (`DisplacedAssetAnchorPoint`, `ResolvedAnchor`/handle) and add `anchor_points` storage to `AnimationFrame`.  
2) Implement `resolve_anchor_point` helper (shared math between runtime/editor) and an accessor for runtime height on `Asset`.  
3) Extend `animation_loader.cpp` and `animation_cloner.cpp` to parse/copy/transform anchors; ensure `frames` and `movement_paths_` carry anchor data.  
4) Add `Asset` anchor handle map, dirtying hooks, and `get_anchor_point`/`anchor_state` APIs; wire dirty flags on animation/frame/scale/position changes.  
5) Update derived/manifest plumbing (`AssetInfo`, `AnimationDocument`, cleaner scripts) to preserve new `anchor_points` field.  
6) Build editor-side anchor frame state parser/builder (JSON merge-safe) and utilities to convert percent↔world using the shared resolver.  
7) Implement `AnchorFrameEditor` UI (navigator, anchor list, path overlay, rotation control, snap) and hook it into `FrameEditorSession`/menus.  
8) Add live preview invalidation hooks so edits refresh rendered frames; ensure immediate-save path writes `anchor_points`.  
9) Add tests (unit + small integration) and a dev debug overlay to visualize resolved anchors; fix any math discrepancies.  
10) Perform manual QA in dev mode (create anchors, flip/scale assets, switch paths) and document the controller usage pattern.
