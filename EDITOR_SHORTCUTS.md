# ENGINE Editor Keyboard Shortcuts Guide

## Overview

This document provides a comprehensive reference for all keyboard shortcuts, key combinations, and mouse+key interactions available in the ENGINE editor's development mode (`ENGINE/editor/devtools/`).

**Total Shortcuts: 65+**
**Subsystems: 12**
**Last Updated: 2026-02-09**

---

## Quick Reference

| Shortcut | Action | Context |
|----------|--------|---------|
| **Escape** | Close/Cancel | Universal (all modals, dialogs, panels) |
| **Ctrl+C** | Copy or Toggle Camera | Room Editor (copy), Global (toggle camera panel) |
| **Ctrl+V** | Paste | Room Editor only |
| **Ctrl+L** | Toggle Asset Library | Room Editor only |
| **Ctrl+A** | Save Camera Defaults | Room Editor only |
| **Delete** | Delete Selected Assets | Room Editor only |
| **F8** | Toggle Layers Panel | Map Editor only |
| **Shift+Click** | Select + Open Map Assets Panel | Room Editor |
| **Alt+Shift+Click** | Delete Spawn Group | Room Editor |
| **Ctrl+Scroll** | Adjust Zoom | Camera controls |
| **Scroll** | Adjust Camera Height | Camera controls |
| **Ctrl+Drag** | Pan Camera or Tilt Camera | Camera controls |
| **Tab / Shift+Tab** | Navigate Focus/Next Child | Global (UI navigation) |
| **Return/Enter** | Commit/Activate | Text editing, dropdowns, dialogs |
| **Arrow Keys** | Navigate/Adjust | Frame editors, text boxes, dropdowns |

---

## Detailed Shortcuts by Category

### 1. Global Editor Shortcuts

#### Ctrl+C (Toggle Camera Panel)
- **File:** [dev_controls.cpp:1102-1108](ENGINE/editor/devtools/dev_controls.cpp)
- **Action:** Toggle camera settings panel visibility
- **Modifiers:** LCTRL or RCTRL + C
- **Context:** Only when Room Editor is NOT active
- **Notes:** Useful for quick access to camera controls

#### F8 (Toggle Layers Panel)
- **File:** [dev_controls.cpp:1116-1118](ENGINE/editor/devtools/dev_controls.cpp)
- **Action:** Toggle visibility of layers panel
- **Context:** Map Editor mode only
- **Scancode:** SDL_SCANCODE_F8

---

### 2. Room Editor Operations (11 shortcuts)

#### Ctrl+C (Copy Selected Spawn Group)
- **File:** [room_editor.cpp:3680-1681](ENGINE/editor/devtools/room_editor.cpp)
- **Action:** Copy currently selected spawn group to clipboard
- **Function:** `copy_selected_spawn_group()`
- **Context:** Room Editor only
- **Notes:** Allows pasting elsewhere in the scene

#### Ctrl+V (Paste Spawn Group)
- **File:** [room_editor.cpp:3683-1684](ENGINE/editor/devtools/room_editor.cpp)
- **Action:** Paste previously copied spawn group
- **Function:** `paste_spawn_group_from_clipboard()`
- **Context:** Room Editor only
- **Notes:** Creates a new instance of the copied group

#### Ctrl+L (Toggle Asset Library)
- **File:** [room_editor.cpp:3687-1692](ENGINE/editor/devtools/room_editor.cpp)
- **Action:** Show/hide the asset library panel
- **Function:** `toggle_asset_library()`
- **Context:** Room Editor only
- **Warnings:** Ignored if library is locked (shows warning message)

#### Ctrl+A (Save Camera Defaults)
- **File:** [room_editor.cpp:3695-3730](ENGINE/editor/devtools/room_editor.cpp)
- **Action:** Save current camera position, tilt, and zoom as room defaults
- **Details:** Validates and clamps values before saving to room config
- **Context:** Room Editor only
- **Saved Values:**
  - Camera height
  - Camera tilt angle
  - Zoom percentage

#### Delete (Delete Selected Assets)
- **File:** [room_editor.cpp:3950](ENGINE/editor/devtools/room_editor.cpp)
- **Action:** Delete all selected spawn groups
- **Function:** `handle_delete_shortcut()`
- **Context:** Room Editor only
- **Behavior:** Deletes all assets with matching spawn IDs

#### Shift+Space (Cycle Selection Filter)
- **File:** [room_editor.cpp:2491-2493](ENGINE/editor/devtools/room_editor.cpp)
- **Action:** Cycle through available selection filters
- **Function:** `cycle_selection_filter()`
- **Context:** While Shift is held, pressing Space cycles to next filter
- **Details:** Resets filter on next Shift press release

#### Shift (Hold - Selection Mode)
- **File:** [room_editor.cpp:2481-2500](ENGINE/editor/devtools/room_editor.cpp)
- **Action:** Enable asset selection and dragging mode
- **Effect:** Blocks camera panning, enables asset interaction
- **Context:** Room Editor viewport
- **Behavior:** Released state resets filter for next Shift press

#### Shift+Click (Select + Show Map Assets Panel)
- **File:** [room_editor.cpp:3366, 3400, 3407, 5022](ENGINE/editor/devtools/room_editor.cpp)
- **Action:** Select asset and request map assets panel to open
- **Context:** Clicking on assets in Room Editor viewport
- **Details:** Sets `map_assets_panel_requested_by_shift_click_` flag
- **Result:** Selected asset info displayed in map assets panel

#### Alt+Shift+Click (Delete Spawn Group)
- **File:** [room_editor.cpp:3347-3354](ENGINE/editor/devtools/room_editor.cpp)
- **Action:** Immediately delete the spawn group under cursor
- **Function:** `delete_spawn_group_internal()`
- **Modifiers:** BOTH Alt and Shift must be held
- **Context:** Room Editor viewport
- **Warning:** Immediate deletion, no confirmation

#### Ctrl+Drag (Drag with Ctrl Modifier)
- **File:** [room_editor.cpp:2625-2626](ENGINE/editor/devtools/room_editor.cpp)
- **Action:** Begin drag session with ctrl modifier flag set
- **Function:** `begin_drag_session()` with ctrl=true
- **Context:** Left mouse button drag while Ctrl held
- **Effect:** May affect how assets are moved (behavior varies)

---

### 3. Camera Controls (4 shortcuts)

#### Mouse Scroll (Adjust Camera Height)
- **File:** [dev_camera_controls.cpp:31-60](ENGINE/editor/devtools/dev_camera_controls.cpp)
- **Action:** Increase/decrease camera height (zoom in/out)
- **Function:** Exponential scaling based on `height_scale_factor_`
- **Direction:**
  - Scroll up = move camera higher (zoom out)
  - Scroll down = move camera lower (zoom in)
- **Details:** Default scroll behavior in camera controls

#### Ctrl+Scroll (Adjust Zoom Percentage)
- **File:** [dev_camera_controls.cpp:32-40](ENGINE/editor/devtools/dev_camera_controls.cpp)
- **Action:** Adjust zoom percentage incrementally
- **Step:** `kZoomStepPercent` per scroll tick
- **Range:** Min 1%, Max 500%
- **Modifiers:** CTRL must be held during scroll
- **Direction:**
  - Scroll up = increase zoom percentage
  - Scroll down = decrease zoom percentage

#### Left Drag (Pan Camera)
- **File:** [dev_camera_controls.cpp:71-99, 115-127](ENGINE/editor/devtools/dev_camera_controls.cpp)
- **Action:** Pan camera across the scene (horizontal/vertical movement)
- **Context:** When not blocked by UI
- **Behavior:** Maps mouse movement directly to camera position
- **Condition:** Only works when camera controls are active

#### Ctrl+Left Drag (Adjust Camera Tilt)
- **File:** [dev_camera_controls.cpp:72-86, 115-127](ENGINE/editor/devtools/dev_camera_controls.cpp)
- **Action:** Rotate camera pitch (tilt angle)
- **Rate:** `kTiltDegreesPerPixel` (typically 0.2° per pixel)
- **Range:** Clamped to WarpedScreenGrid min/max pitch values
- **Direction:**
  - Drag up = tilt camera forward
  - Drag down = tilt camera backward
- **Modifiers:** CTRL must be held during drag

---

### 4. Map Editor Shortcuts (1 shortcut)

#### Shift (Hold - Label Selection Mode)
- **File:** [map_editor.cpp:115-134](ENGINE/editor/devtools/map_editor.cpp)
- **Action:** Enable room label selection mode (prevents room area selection)
- **Context:** While hovering over room areas/labels
- **Effect:** Allows clicking room labels instead of room interior areas
- **Blocking:** Blocks camera panning when hovering over room

---

### 5. Animation Editor Shortcuts (5 shortcuts)

#### Escape (Close Animation Editor)
- **File:** [AnimationEditorWindow.cpp:980, 1015](ENGINE/editor/devtools/asset_editor/animation_editor_window/AnimationEditorWindow.cpp)
- **Action:** Close the animation editor window
- **Context:** When animation editor window is in focus
- **Behavior:** Discards any unsaved changes (confirm dialog may appear)

#### Tab (Navigate Focus)
- **File:** [AnimationInspectorPanel.cpp:577-593](ENGINE/editor/devtools/asset_editor/animation_editor_window/AnimationInspectorPanel.cpp)
- **Action:** Move focus to next element in focus order
- **Direction:** Forward through `focus_order()` array
- **Context:** Animation inspector panel active

#### Shift+Tab (Navigate Focus - Reverse)
- **File:** [AnimationInspectorPanel.cpp:580](ENGINE/editor/devtools/asset_editor/animation_editor_window/AnimationInspectorPanel.cpp)
- **Action:** Move focus to previous element in focus order
- **Direction:** Backward through focus targets
- **Modifiers:** Shift+Tab (detected via Shift modifier check)

#### Return / Enter / Space (Activate)
- **File:** [AnimationInspectorPanel.cpp:599-600](ENGINE/editor/devtools/asset_editor/animation_editor_window/AnimationInspectorPanel.cpp)
- **Action:** Activate currently focused element
- **Keys:** SDLK_RETURN, SDLK_KP_ENTER, SDLK_SPACE
- **Context:** When a focus target is selected
- **Behavior:** Depends on focused element type

#### Escape (Close Context Menu)
- **File:** [AnimationListContextMenu.cpp:133](ENGINE/editor/devtools/asset_editor/animation_editor_window/AnimationListContextMenu.cpp)
- **Action:** Close animation list context menu
- **Context:** Context menu open

---

### 6. Frame Editor Shortcuts (10 shortcuts)

#### Left Arrow (Previous Frame)
- **File:** [FrameNavigator.cpp:171-176](ENGINE/editor/devtools/frame_editors/shared/FrameNavigator.cpp)
- **Action:** Navigate to previous frame in sequence
- **Condition:** Only if `current_frame_ > 0`
- **Behavior:** Wraps to first frame if at beginning (depends on implementation)
- **Context:** Frame editor active

#### Right Arrow (Next Frame)
- **File:** [FrameNavigator.cpp:177-182](ENGINE/editor/devtools/frame_editors/shared/FrameNavigator.cpp)
- **Action:** Navigate to next frame in sequence
- **Condition:** Only if not at last frame
- **Context:** Frame editor active

#### Left Arrow (Previous Attack Point)
- **File:** [AttackGeoFrameEditor.cpp:311-342](ENGINE/editor/devtools/frame_editors/AttackGeoFrameEditor.cpp)
- **Action:** Navigate to previous attack geometry handle
- **Context:** Attack geometry frame editor active
- **Details:** Cycles through Start/Control/End handles of attack vectors
- **Behavior:** Moves to previous point in sequence

#### Right Arrow (Next Attack Point)
- **File:** [AttackGeoFrameEditor.cpp:343-374](ENGINE/editor/devtools/frame_editors/AttackGeoFrameEditor.cpp)
- **Action:** Navigate to next attack geometry handle
- **Context:** Attack geometry frame editor active
- **Details:** Cycles through attack vector handles

#### Escape (Close Movement Editor)
- **File:** [MovementFrameEditor.cpp:266-268](ENGINE/editor/devtools/frame_editors/MovementFrameEditor.cpp)
- **Action:** Close the movement frame editor
- **Effect:** Sets `wants_close_ = true`
- **Context:** Movement frame editor open

#### Tab (Next Child Asset)
- **File:** [SyncChildrenFrameEditor.cpp:489-495](ENGINE/editor/devtools/frame_editors/SyncChildrenFrameEditor.cpp)
- **Action:** Cycle to next child asset in list
- **Behavior:** Wraps around to first child after last
- **Updates:** `selected_child_index_` and selection state
- **Context:** Synchronized children frame editor

#### Up Arrow (Increase Point Position)
- **File:** [Point3DEditor.cpp:207-232](ENGINE/editor/devtools/frame_editors/shared/Point3DEditor.cpp)
- **Action:** Increase selected 3D point position along current axis
- **Step:** `grid_step_world_` (default 1.0f if not set)
- **Axes:** X, Y, or Z depending on selected axis
- **Context:** 3D point editor active

#### Down Arrow (Decrease Point Position)
- **File:** [Point3DEditor.cpp:207-232](ENGINE/editor/devtools/frame_editors/shared/Point3DEditor.cpp)
- **Action:** Decrease selected 3D point position along current axis
- **Step:** Same as Up Arrow
- **Context:** 3D point editor active

---

### 7. Text Input Widgets (7 shortcuts)

#### Backspace (Delete Character Before Caret)
- **File:** [widgets.cpp:516-521](ENGINE/editor/devtools/widgets.cpp)
- **Action:** Delete character immediately before text cursor
- **Context:** DMTextBox in editing mode
- **Behavior:** Moves caret back one position and deletes character

#### Delete (Delete Character at Caret)
- **File:** [widgets.cpp:525-529](ENGINE/editor/devtools/widgets.cpp)
- **Action:** Delete character at current caret position
- **Context:** DMTextBox in editing mode
- **Behavior:** Removes character without moving caret

#### Return / Enter (Commit Text)
- **File:** [widgets.cpp:523-524](ENGINE/editor/devtools/widgets.cpp)
- **Action:** Stop editing and commit text value
- **Keys:** SDLK_RETURN or SDLK_KP_ENTER
- **Context:** DMTextBox in editing mode
- **Effect:** Text box exits editing mode

#### Left Arrow (Move Caret Left)
- **File:** [widgets.cpp:530-531](ENGINE/editor/devtools/widgets.cpp)
- **Action:** Move text cursor one character to the left
- **Context:** DMTextBox in editing mode
- **Boundary:** Stops at beginning of text

#### Right Arrow (Move Caret Right)
- **File:** [widgets.cpp:532-533](ENGINE/editor/devtools/widgets.cpp)
- **Action:** Move text cursor one character to the right
- **Context:** DMTextBox in editing mode
- **Boundary:** Stops at end of text

#### Home (Jump to Text Start)
- **File:** [widgets.cpp:534-535](ENGINE/editor/devtools/widgets.cpp)
- **Action:** Move caret to beginning of text
- **Context:** DMTextBox in editing mode
- **Effect:** Immediate jump, no intermediate steps

#### End (Jump to Text End)
- **File:** [widgets.cpp:536-537](ENGINE/editor/devtools/widgets.cpp)
- **Action:** Move caret to end of text
- **Context:** DMTextBox in editing mode
- **Effect:** Immediate jump, no intermediate steps

---

### 8. Widget Navigation - Sliders (2 shortcuts)

#### Left Arrow / A (Decrease Slider Value)
- **File:** [widgets.cpp:1321-1324](ENGINE/editor/devtools/widgets.cpp)
- **Action:** Decrease slider value by 1
- **Keys:** SDL_SCANCODE_LEFT or SDL_SCANCODE_A
- **Context:** DMSlider in focused state
- **Step:** Fixed increment of 1 unit

#### Right Arrow / D (Increase Slider Value)
- **File:** [widgets.cpp:1326-1329](ENGINE/editor/devtools/widgets.cpp)
- **Action:** Increase slider value by 1
- **Keys:** SDL_SCANCODE_RIGHT or SDL_SCANCODE_D
- **Context:** DMSlider in focused state
- **Step:** Fixed increment of 1 unit

#### Return / Enter (Commit Slider)
- **File:** [widgets.cpp:1331-1336](ENGINE/editor/devtools/widgets.cpp)
- **Action:** Commit pending slider value and unfocus
- **Keys:** SDLK_RETURN or SDLK_KP_ENTER
- **Context:** DMSlider focused
- **Effect:** Slider exits edit mode

---

### 9. Widget Navigation - Dropdowns (3 shortcuts)

#### Up Arrow / W (Previous Option)
- **File:** [widgets.cpp:2346-2358](ENGINE/editor/devtools/widgets.cpp)
- **Action:** Move to previous dropdown option
- **Keys:** SDL_SCANCODE_UP or SDL_SCANCODE_W
- **Context:** Dropdown in focused state
- **Behavior:** Wraps to last option when at first option
- **Visual:** Highlights previous option

#### Down Arrow / S (Next Option)
- **File:** [widgets.cpp:2360-2372](ENGINE/editor/devtools/widgets.cpp)
- **Action:** Move to next dropdown option
- **Keys:** SDL_SCANCODE_DOWN or SDL_SCANCODE_S
- **Context:** Dropdown in focused state
- **Behavior:** Wraps to first option when at last option
- **Visual:** Highlights next option

#### Return / Enter (Commit Selection)
- **File:** [widgets.cpp:2374-2381](ENGINE/editor/devtools/widgets.cpp)
- **Action:** Commit selected dropdown option and unfocus
- **Keys:** SDLK_RETURN or SDLK_KP_ENTER
- **Context:** Dropdown focused
- **Effect:** Dropdown closes, selection applied

---

### 10. Modal Dialog Shortcuts (10+ shortcuts)

#### Return / Y / Space (Confirm Delete)
- **Files:**
  - [asset_library_ui.cpp:2051-2052](ENGINE/editor/devtools/asset_library_ui.cpp)
  - [asset_info_ui.cpp:2009](ENGINE/editor/devtools/asset_info_ui.cpp)
- **Action:** Confirm delete action in modal dialog
- **Keys:** SDLK_RETURN, SDLK_Y, SDLK_SPACE
- **Context:** Delete confirmation dialog displayed
- **Effect:** Deletion proceeds immediately

#### Escape / N (Cancel Delete)
- **Files:**
  - [asset_library_ui.cpp:2055-2056](ENGINE/editor/devtools/asset_library_ui.cpp)
  - [asset_info_ui.cpp:2010](ENGINE/editor/devtools/asset_info_ui.cpp)
- **Action:** Cancel delete action
- **Keys:** SDLK_ESCAPE, SDLK_N
- **Context:** Delete confirmation dialog displayed
- **Effect:** Dialog closes, deletion cancelled

#### Escape (Close Various Modals)
- **Files:**
  - [asset_info_ui.cpp:647, 682, 751](ENGINE/editor/devtools/asset_info_ui.cpp)
  - [room_configurator.cpp:611](ENGINE/editor/devtools/config/room_config/room_configurator.cpp)
  - [map_mode_ui.cpp:908](ENGINE/editor/devtools/map_mode_ui.cpp)
  - [SpawnGroupConfig.cpp:1925](ENGINE/editor/devtools/spawn_groups/SpawnGroupConfig.cpp)
  - [CandidateEditorPieGraphWidget.cpp:84](ENGINE/editor/devtools/spawn_groups/widgets/CandidateEditorPieGraphWidget.cpp)
  - [DockableCollapsible.cpp:774, 817](ENGINE/editor/devtools/DockableCollapsible.cpp)
- **Action:** Close various editor panels and modals
- **Context:** Different panels (asset info, room config, map mode, spawn config, etc.)
- **Notes:** Escape works universally across most dialogs

#### Return (Commit ID Edit)
- **File:** [asset_info_ui.cpp:725](ENGINE/editor/devtools/asset_info_ui.cpp)
- **Action:** Commit asset ID change
- **Context:** Editing asset ID field
- **Effect:** Text validated and applied

#### Escape (Cancel ID Edit)
- **File:** [asset_info_ui.cpp:731](ENGINE/editor/devtools/asset_info_ui.cpp)
- **Action:** Cancel asset ID edit and revert changes
- **Context:** ID editor active
- **Effect:** Original ID restored

#### Backspace (ID Editor)
- **File:** [asset_info_ui.cpp:735](ENGINE/editor/devtools/asset_info_ui.cpp)
- **Action:** Delete character in ID editor
- **Context:** Editing asset ID
- **Standard:** Standard text editing backspace behavior

#### Return / Enter (Commit Tag Edit)
- **File:** [tag_editor_widget.cpp:314](ENGINE/editor/devtools/config/room_config/tag_editor_widget.cpp)
- **Action:** Commit tag search/edit
- **Keys:** SDLK_RETURN or SDLK_KP_ENTER
- **Context:** Tag search box in editing mode
- **Effect:** Tag applied/filter updated

#### Escape (Close Floatable Panel)
- **File:** [DockableCollapsible.cpp:774, 817](ENGINE/editor/devtools/DockableCollapsible.cpp)
- **Action:** Close and undock floatable panel
- **Context:** When panel is floatable (floatable_=true)
- **Condition:** Only works if `floatable_` flag is set
- **Effect:** Panel hidden and detached from dock

---

### 11. Camera Settings Widget (3 shortcuts)

#### Left Drag (Camera Tilt or Pan)
- **File:** [room_editor.cpp:2381-2385](ENGINE/editor/devtools/room_editor.cpp)
- **Action:**
  - Default: Adjust camera tilt
  - With Shift: Pan camera
- **Context:** Camera settings widget when lock is active
- **Modes:** Mode depends on Shift modifier state

#### Shift+Left Drag (Camera Pan)
- **File:** [room_editor.cpp:2384](ENGINE/editor/devtools/room_editor.cpp)
- **Action:** Pan camera using mouse drag
- **Context:** Camera settings widget active
- **Effect:** Moves camera position in scene

#### Right Drag (Camera Pan)
- **File:** [room_editor.cpp:2387-2390](ENGINE/editor/devtools/room_editor.cpp)
- **Action:** Pan camera (alternative to Shift+Left Drag)
- **Context:** Camera settings widget active
- **Effect:** Same as Shift+Left Drag

---

## Summary Statistics

| Metric | Value |
|--------|-------|
| Total Shortcuts | 65+ |
| Files Analyzed | 30+ |
| Key Categories | 11 |
| Most Used Modifier | Shift (20+ uses) |
| Second Most Used | Ctrl (18+ uses) |
| Most Universal Key | Escape (15+ contexts) |
| Mouse+Key Combos | 7+ |

---

## Key Patterns & Conventions

### 1. **Escape = Universal Close/Cancel**
Escape is used across nearly all modal dialogs and panels to close or cancel current operation.

### 2. **Ctrl = Advanced/Clipboard Operations**
- Ctrl+C: Copy (clipboard operations)
- Ctrl+V: Paste
- Ctrl+L: Library toggle
- Ctrl+A: Save/Apply
- Ctrl+Scroll: Fine-grain zoom

### 3. **Shift = Selection & Modification**
- Shift+Click: Extended selection
- Shift+Drag: Alternative interaction mode
- Shift+Space: Cycle filters
- Shift+Tab: Reverse navigation

### 4. **Arrow Keys = Navigation & Adjustment**
- Up/Down: Dropdown navigation, value adjustment
- Left/Right: Frame navigation, text caret movement
- Tab: Focus navigation and asset cycling

### 5. **Return/Enter = Commit/Confirm**
Standard confirmation key across all input widgets and dialogs.

### 6. **Alt = Destructive Operations**
- Alt+Shift+Click: Delete (requires confirmation combination)

---

## Implementation Reference Guide

### Keyboard Input Patterns Used

#### SDL_Scancode Checking
```cpp
// Direct scancode checks
SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN
SDL_SCANCODE_A, SDL_SCANCODE_D, SDL_SCANCODE_W, SDL_SCANCODE_S
SDL_SCANCODE_F8
```

#### SDL_Keycode Checking
```cpp
// SDL key constants
SDLK_RETURN, SDLK_KP_ENTER, SDLK_SPACE
SDLK_ESCAPE, SDLK_Y, SDLK_N
```

#### Modifier Detection
```cpp
// Modifier key checking
KMOD_CTRL (LCTRL | RCTRL)
KMOD_SHIFT (LSHIFT | RSHIFT)
KMOD_ALT (LALT | RALT)
KMOD_GUI (LGUI | RGUI)
```

#### ImGui Integration
```cpp
// ImGui keyboard/mouse queries
ImGui::IsKeyPressed(key)
ImGui::IsKeyDown(key)
ImGui::IsMouseClicked(button)
ImGui::IsMouseDragging(button)
ImGui::GetIO().KeyCtrl, KeyShift, KeyAlt, KeySuper
```

---

## Notes for Developers

1. **Context Matters**: Most shortcuts are context-specific. Always check which subsystem/panel is active before assuming a shortcut works.

2. **Modifier Combinations**: When combining modifiers, both left and right variants typically work (LCTRL and RCTRL, LSHIFT and RSHIFT, etc.).

3. **UI Blocking**: Many shortcuts are blocked when UI elements are being hovered or focused to prevent conflicts.

4. **Conditional Shortcuts**: Some shortcuts only function under specific conditions (e.g., "only if not at last frame").

5. **Mouse+Key Combos**: Mouse interactions combined with modifiers follow consistent patterns:
   - Shift+Click usually extends selection
   - Alt+Click usually triggers destructive operations
   - Ctrl+Drag usually triggers pan/tilt operations

6. **Widget Focus**: Keyboard navigation within widgets requires the widget to be focused (Tab to focus, then use arrow keys/Return).

---

## Adding New Shortcuts

When adding new keyboard shortcuts to the editor:

1. **Document the shortcut** in this file with full context
2. **Use consistent modifiers** (follow patterns established above)
3. **Avoid conflicts** with existing shortcuts
4. **Test context blocking** to ensure UI doesn't interfere
5. **Update the Quick Reference table** at the top of this document

---

## File Structure Reference

```
ENGINE/editor/devtools/
├── dev_controls.cpp              # Global editor shortcuts
├── room_editor.cpp               # Room editing, selection, camera
├── dev_camera_controls.cpp       # Camera manipulation
├── map_editor.cpp                # Map mode shortcuts
├── widgets.cpp                   # Text box, slider, dropdown shortcuts
├── asset_editor/
│   └── animation_editor_window/
│       ├── AnimationEditorWindow.cpp      # Animation editor
│       ├── AnimationInspectorPanel.cpp    # Animation inspector
│       └── AnimationListContextMenu.cpp   # Context menus
├── frame_editors/
│   ├── shared/
│   │   ├── FrameNavigator.cpp             # Frame navigation
│   │   └── Point3DEditor.cpp              # 3D point editing
│   ├── AttackGeoFrameEditor.cpp           # Attack geometry
│   ├── MovementFrameEditor.cpp            # Movement editing
│   └── SyncChildrenFrameEditor.cpp        # Synchronized children
└── [other dialog files]          # Various modal dialogs
```

---

**Document Last Updated:** 2026-02-09
**Total References:** 65+ keyboard inputs across 30+ files
