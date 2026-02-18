# Anchor Points Overhaul Plan

## Context

Anchor points currently use an "ensure everywhere" pattern — creating, renaming, or deleting an anchor propagates to every frame of every animation for an asset. This makes it impossible to have animation-specific anchors (e.g., "eyes" only on idle animations but not on attack animations). The runtime also lacks a clean binding API; child assets are spawned with hardcoded anchor names buried in controller logic.

This plan makes anchor points **per-animation** by default and introduces a **bind** API for controllers to declaratively attach child assets to named anchors.

## Progress
- [x] Part 1: Editor operations now work per-frame, the tool panel hides anchor fields until a selection exists, and the everywhere helpers were removed (`ENGINE/editor/devtools/frame_editors/AnchorFrameEditor.*`).
- [x] Part 2: Anchor lookups now skip resolution when missing, missing anchors hide dependent assets via the new `anchor_hidden` flag, and the renderer ignores hidden assets (`ENGINE/runtime/assets/asset/Asset.*`, `ENGINE/runtime/rendering/render/render.cpp`).
- [x] Part 3: Added `Asset::bind_child_to_anchor`, routed `spawn_asset_attached` through it, and updated `Vibble_controller` to spawn + bind manually.
- [x] Part 4: Legacy manifest/animation anchor sync cleanups verified and complete; only per-animation payload anchors are loaded, with no asset-level defaults or automatic sync.

---

## Part 1: Editor — Per-Animation Anchor Operations

### 1.1 Remove "ensure everywhere" on create

**File:** `ENGINE/editor/devtools/frame_editors/AnchorFrameEditor.cpp`

`add_anchor()` (line ~775) currently calls `ensure_anchor_exists_everywhere()` which adds the new anchor to every frame of the current animation. Change this so the new anchor is only added to the **currently selected frame**.

- Remove the call to `ensure_anchor_exists_everywhere(name)` inside `add_anchor()`.
- Instead, push the new `FrameAnchorPoint` only into `frames_[selected_frame_].anchors`.
- The user can still propagate via "Apply To Next Frame" or "Apply To Animation" buttons, which already exist in `FrameNavigator`.

### 1.2 Remove "remove everywhere" on delete

**File:** `ENGINE/editor/devtools/frame_editors/AnchorFrameEditor.cpp`

The delete handler (line ~212) calls `remove_anchor_everywhere()` which erases the anchor from every frame. Change this so deletion only removes the anchor from the **currently selected frame**.

- Replace `remove_anchor_everywhere(name)` with an erase on `frames_[selected_frame_].anchors` only.
- The user can propagate deletion via the existing "Apply To Animation" / "Apply To All Animations" buttons.

### 1.3 Remove "rename everywhere" on name change

**File:** `ENGINE/editor/devtools/frame_editors/AnchorFrameEditor.cpp`

`apply_form_to_anchor()` (line ~514) calls `rename_anchor_everywhere()` on name change. Change this so the rename only applies to the anchor in the **currently selected frame**.

- Replace `rename_anchor_everywhere(old_name, new_name)` with a direct rename on `frames_[selected_frame_].anchors[selected_anchor_].name`.
- Duplicate-name validation should only check within the current frame (it already does this).

### 1.4 Keep "Apply To" buttons unchanged

The three existing propagation buttons remain as-is — they are the explicit user action for spreading changes:

- **Apply To Next Frame** — copies current frame's anchors to the next frame.
- **Apply To Animation** — copies current frame's anchors to all frames in this animation.
- **Apply To All Animations** — copies current frame's anchors to all frames of all animations for this asset.

No changes needed in `FrameNavigator` or its callbacks.

### 1.5 Hide anchor form fields until an anchor is selected

**File:** `ENGINE/editor/devtools/frame_editors/AnchorFrameEditor.cpp`

`rebuild_tool_panel_layout()` (line ~807) unconditionally adds the name, px, py, pz, and rotation textbox rows to the tool panel. These fields should only appear when an anchor is actually selected (`selected_anchor_ >= 0`).

- Wrap the name/px/py/pz/rot rows in a `if (selected_anchor_ >= 0)` guard inside `rebuild_tool_panel_layout()`.
- Call `rebuild_tool_panel_layout()` from `select_anchor()` so the panel updates when selection changes (including deselection).
- When no anchor is selected, the tool panel shows only the anchor list and the Add button — a clean, uncluttered state.

### 1.6 Clean up dead helper methods

**File:** `ENGINE/editor/devtools/frame_editors/AnchorFrameEditor.cpp`

After the above changes, the following methods become unused and should be deleted:

- `ensure_anchor_exists_everywhere()` (line ~827)
- `remove_anchor_everywhere()` (line ~840)
- `rename_anchor_everywhere()` (line ~850)

Remove their declarations from the header as well.

---

## Part 2: Runtime — Per-Animation Anchor Lookup with Missing = Hidden

### 2.1 Stop falling back to a default anchor when missing

**File:** `ENGINE/runtime/assets/asset/Asset.cpp`

In `AnchorHandle::update()` (line ~943), when the current frame has no anchor matching `name`, the code currently falls back to a zeroed-out `base_anchor` (position 0,0,0). This means the child still renders at the parent's base.

Change this so that when the anchor is not found in the current frame:

- Set `missing = true` (already done).
- Set `world_px`, `world_z`, `grid` to zero/null.
- **Do not call `resolve_anchor_point()`** — skip resolution entirely.

### 2.2 Hide child asset when anchor is missing

**File:** `ENGINE/runtime/assets/asset/Asset.cpp`

In `apply_anchor_follow_target()` (line ~864), after resolving the parent's anchor, check `resolved->missing`:

```cpp
auto resolved = source->anchor_state(follow.anchor_name);
if (!resolved.has_value() || resolved->missing) {
    set_visible(false);   // hide child — anchor doesn't exist for this animation
    return;
}
set_visible(true);        // anchor exists — show child
```

This requires a `visible` flag on Asset (or equivalent rendering skip). If one already exists, use it. If not, add a `bool anchor_hidden_ = false` flag and check it in the render path.

### 2.3 Verify visibility flag exists or add one

**File:** `ENGINE/runtime/assets/asset/Asset.hpp` / `Asset.cpp`

Check if Asset already has a visibility or render-skip mechanism. If so, use it. If not:

- Add `bool anchor_hidden_ = false;` to Asset.
- Add `bool is_anchor_hidden() const { return anchor_hidden_; }`.
- In the render pipeline (composite_asset_renderer.cpp or render_package building), skip rendering when `anchor_hidden_` is true.

### 2.4 Mark anchors dirty on animation change (already done)

`set_current_animation()` already calls `mark_anchors_dirty()`. This means when the parent switches animations, all anchor handles will re-resolve on the next frame, picking up the new animation's anchor definitions (or marking `missing = true` if absent). No changes needed here.

---

## Part 3: Controller Bind API

### 3.1 Add `bind_to_anchor()` to Asset or AssetController

**File:** `ENGINE/runtime/assets/asset/Asset.hpp` / `Asset.cpp`

Add a method that declaratively binds a child asset to a named anchor on the parent:

```cpp
// On Asset:
void bind_child_to_anchor(Asset* child, const std::string& anchor_name);
```

Implementation:
- Set `child->follow_anchor_ = AnchorFollowTarget{this, anchor_name}`.
- Call `mark_anchors_dirty()` on self to trigger resolution.
- Add child to a tracked list so the parent knows its bound children (if not already tracked).

This replaces the current pattern of manually calling `spawn_asset_attached()` and separately managing the follow target.

### 3.2 Refactor VibbleController to use bind API

**File:** `ENGINE/runtime/animation/controllers/custom_controllers/Vibble_controller.cpp`

Replace the current `spawn_eyes_follower()` implementation:

**Before:**
```cpp
void VibbleController::spawn_eyes_follower() {
    eyes_follower_ = assets->spawn_asset_attached("Vibble_eyes", player_, "eyes");
}
```

**After:**
```cpp
void VibbleController::spawn_eyes_follower() {
    Assets* assets = player_->get_assets();
    if (!assets) return;

    eyes_follower_ = assets->spawn_asset("Vibble_eyes", player_->world_point());
    if (eyes_follower_) {
        player_->bind_child_to_anchor(eyes_follower_, "eyes");
    }
}
```

The bind call sets up the follow target. The runtime (Part 2) handles hiding when "eyes" doesn't exist in the current animation.

### 3.3 Update `spawn_asset_attached()` or deprecate it

**File:** `ENGINE/runtime/core/AssetsManager.cpp`

`spawn_asset_attached()` currently combines spawning + anchor following in one call. Two options:

**Option A (recommended):** Keep `spawn_asset_attached()` but have it internally call `bind_child_to_anchor()`. This avoids breaking other callers while routing through the new system.

**Option B:** Deprecate and replace all callers with separate spawn + bind calls.

Go with Option A — update `spawn_asset_attached()` to delegate to `bind_child_to_anchor()` internally, ensuring all paths go through the same binding logic.

### 3.4 Audit all controllers for hardcoded anchor usage

Search all controllers for calls to `spawn_asset_attached`, `follow_anchor_`, or direct `AnchorFollowTarget` construction. Ensure every one routes through the bind API:

- `Vibble_controller.cpp` — "eyes" anchor (primary target)
- `Bomb_controller.cpp`, `Davey_controller.cpp`, `Carrie_controller.cpp`, etc. — check for any anchor usage
- `default_controller.hpp` — unlikely but verify

---

## Part 4: Remove Legacy Code

### 4.1 Remove dead "everywhere" methods from AnchorFrameEditor

Already covered in 1.5. Delete:
- `ensure_anchor_exists_everywhere()`
- `remove_anchor_everywhere()`
- `rename_anchor_everywhere()`

### 4.2 Remove any asset-wide anchor synchronization logic

Search for any code that synchronizes anchor names across animations (outside of the explicit "Apply To All" button). Remove it.

### 4.3 Ignore legacy asset-level anchor data in manifest.json

**File:** `ENGINE/runtime/assets/asset/animation_loader.cpp` and any manifest parsing code

Currently all anchor data lives inside animation payloads (per-animation, per-frame), which is correct. However, if any legacy code path reads anchor data from the asset level of the manifest (outside of animation payloads), it must be removed or ignored. Specifically:

- Search for any manifest parsing that reads anchor points from the asset root (not inside an animation's payload). If found, delete that code path.
- Ensure the only source of anchor point data is `animation_payload["anchor_points"]` — nothing at the asset level.
- If old manifest entries contain asset-level anchor fields, they should be silently ignored (not loaded, not migrated).

### 4.4 Ensure no default anchor points are created for new animations

**File:** `ENGINE/runtime/assets/asset/animation_loader.cpp`, `ENGINE/editor/devtools/frame_editors/shared/AnchorFrameState.cpp`

Verify that when a new animation is created or an existing animation has no `anchor_points` key, the result is an empty anchor list — not a set of default/placeholder anchors. Currently this is the case (frames resize with empty vectors), but this must remain true:

- No code path should auto-populate anchor points on load or creation.
- Frames without anchor data should produce `AnchorFrame` with an empty `anchors` vector.
- The editor should show an empty anchor list for frames/animations that have no anchors defined.

### 4.5 Verify animation_loader.cpp handles missing anchors gracefully

**File:** `ENGINE/runtime/assets/asset/animation_loader.cpp`

The loader already parses per-frame anchors independently. Verify that:
- Frames with no `anchor_points` key produce empty anchor lists (not an error).
- Different frames can have different anchor names without issue.
- No post-load normalization step forces all frames to have the same anchor set.

---

## Files Modified (Summary)

| File | Changes |
|------|---------|
| `ENGINE/editor/devtools/frame_editors/AnchorFrameEditor.cpp` | Remove "everywhere" calls from add/delete/rename; delete helper methods; hide form fields until anchor selected |
| `ENGINE/editor/devtools/frame_editors/AnchorFrameEditor.hpp` | Remove "everywhere" method declarations |
| `ENGINE/runtime/assets/asset/Asset.hpp` | Add `bind_child_to_anchor()`, add `anchor_hidden_` flag if needed |
| `ENGINE/runtime/assets/asset/Asset.cpp` | Implement bind, skip resolution when missing, hide child when anchor absent |
| `ENGINE/runtime/core/AssetsManager.cpp` | Update `spawn_asset_attached()` to use bind internally |
| `ENGINE/runtime/animation/controllers/custom_controllers/Vibble_controller.cpp` | Use bind API for eyes follower |
| `ENGINE/runtime/assets/asset/animation_loader.cpp` | Verify no default anchors created; ignore any legacy asset-level anchor data |

---
