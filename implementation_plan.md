# Implementation Plan

Modify the axis point UI tool to display 3 textboxes at the bottom of the screen for editing the selected point's dx, dy, dz values, replacing the per-mode toolbox textboxes.

The axis point tool currently renders visual indicators for selected points and handles axis cycling. This change adds a global bottom UI that appears whenever a point is selected, regardless of the current frame editor mode, and removes the individual textboxes from each frame editor's toolbox.

[Types]

No new types are required. The existing SelectionState struct already contains the world_pos (SDL_FPoint with x, y) and world_z (float) fields that will be edited through the bottom textboxes.

[Files]

New files to be created:
- ENGINE/dev_mode/frame_editors/shared/PointValueEditor.hpp: Header file for the bottom UI component that manages the dx, dy, dz textboxes and their interaction with SelectionState.
- ENGINE/dev_mode/frame_editors/shared/PointValueEditor.cpp: Implementation file for rendering and handling events for the bottom textboxes.

Existing files to be modified:
- ENGINE/dev_mode/frame_editor_session.hpp: Add PointValueEditor member and include its header.
- ENGINE/dev_mode/frame_editor_session.cpp: Initialize PointValueEditor, add render_overlays call for it, and handle events for the bottom UI.
- ENGINE/dev_mode/frame_editors/MovementFrameEditor.hpp: Remove tb_dx_, tb_dy_, tb_dz_ members and related code.
- ENGINE/dev_mode/frame_editors/MovementFrameEditor.cpp: Remove textbox creation, rendering, event handling, and sync/apply functions for dx, dy, dz.
- ENGINE/dev_mode/frame_editors/SyncChildrenFrameEditor.hpp: Remove tb_dx_, tb_dy_, tb_dz_ members.
- ENGINE/dev_mode/frame_editors/SyncChildrenFrameEditor.cpp: Remove textbox creation, rendering, event handling, and related functions.
- ENGINE/dev_mode/frame_editors/AttackGeoFrameEditor.hpp: Remove tb_start_x_, tb_start_y_, tb_start_z_, tb_control_x_, tb_control_y_, tb_control_z_, tb_end_x_, tb_end_y_, tb_end_z_ members and related last_* strings.
- ENGINE/dev_mode/frame_editors/AttackGeoFrameEditor.cpp: Remove textbox creation, rendering, event handling, and sync/apply functions for attack vector positions.

Configuration files: No changes required.

[Functions]

New functions:
- PointValueEditor::render(SDL_Renderer*): Renders the 3 textboxes at the bottom of the screen when a point is selected.
- PointValueEditor::handle_event(const SDL_Event&): Handles input events for the textboxes.
- PointValueEditor::update(): Syncs textbox values with current SelectionState world_pos and world_z.
- PointValueEditor::set_visible(bool): Shows/hides the UI based on selection state.

Modified functions:
- FrameEditorSession::render(): Add call to PointValueEditor::render() after editor render_overlays.
- FrameEditorSession::handle_event(): Add call to PointValueEditor::handle_event() if not consumed by editor.
- FrameEditorSession::update(): Add call to PointValueEditor::update().

Removed functions:
- MovementFrameEditor::sync_text_fields(): Remove sync logic for dx, dy, dz textboxes.
- MovementFrameEditor::apply_text_field_changes(): Remove apply logic for dx, dy, dz textboxes.
- SyncChildrenFrameEditor::apply_text_box_changes(): Remove apply logic for position textboxes.
- SyncChildrenFrameEditor::update_text_boxes_from_current_frame(): Remove sync logic.
- AttackGeoFrameEditor::apply_text_fields(): Remove apply logic for attack vector textboxes.

[Classes]

New classes:
- PointValueEditor: Manages the bottom UI textboxes for editing selected point values. Contains DMTextBox instances for dx, dy, dz, handles layout at screen bottom, and updates SelectionState when values change.

Modified classes:
- FrameEditorSession: Add PointValueEditor member, initialize it in begin(), call its methods in render/update/handle_event.
- MovementFrameEditor: Remove DMTextBox members for dx, dy, dz, remove related mutable strings and sync/apply methods.
- SyncChildrenFrameEditor: Remove DMTextBox members for dx, dy, dz, rotation, remove related methods.
- AttackGeoFrameEditor: Remove multiple DMTextBox members for attack vector coordinates, remove related mutable strings and methods.

No classes removed.

[Dependencies]

No new packages required. Uses existing DMTextBox from dev_mode/widgets.hpp and SelectionState from shared headers.

[Testing]

Add unit tests for PointValueEditor:
- Test rendering when point selected vs not selected.
- Test value syncing with SelectionState changes.
- Test textbox editing updates SelectionState.
- Test event handling for textbox focus/editing.

Modify existing frame editor tests to remove expectations for toolbox textboxes and verify bottom UI appears instead.

Integration testing: Verify that selecting points in any mode shows bottom textboxes, editing values updates the point position, and toolbox textboxes are absent.

[Implementation Order]

1. Create PointValueEditor.hpp and .cpp files.
2. Modify FrameEditorSession to include and use PointValueEditor.
3. Remove textboxes from MovementFrameEditor.
4. Remove textboxes from SyncChildrenFrameEditor.
5. Remove textboxes from AttackGeoFrameEditor.
6. Update any remaining frame editors that have position textboxes.
7. Add tests for PointValueEditor.
8. Update existing tests to reflect UI changes.
9. Manual testing across all frame editor modes.
