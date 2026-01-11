# Frame Editor Session Nuclear Cleanup Plan

## Overview
Complete elimination of XZ plane complexity and render_in_front legacy system. **BREAKING CHANGES** - no backward compatibility. Replace Z adjustment with Shift+scroll mechanism.

## BREAKING CHANGES - NO COMPATIBILITY

### **1. Complete XZ Plane Annihilation**

#### **frame_editor_session.hpp**
- **DELETE** `enum class EditPlane { XY, XZ };` - remove entire enum
- **DELETE** `EditPlane edit_plane_;` member
- **DELETE** `EditPlane edit_plane() const;` function
- **DELETE** all plane-related functions:
  - `void toggle_edit_plane();`
  - `bool is_xy_plane() const;`
  - `const char* plane_axis_label() const;`
  - `const char* plane_button_label() const;`
  - `float movement_plane_delta(...)`
  - `float& movement_plane_delta(...)`
  - `float child_plane_delta(...)`
  - `float& child_plane_delta(...)`
  - `float hitbox_plane_center(...)`
  - `float& hitbox_plane_center(...)`
  - `float attack_plane_start(...)`
  - `float& attack_plane_start(...)`
  - `float attack_plane_control(...)`
  - `float& attack_plane_control(...)`
  - `float attack_plane_end(...)`
  - `float& attack_plane_end(...)`
- **DELETE** `btn_plane_toggle_;` UI element

#### **frame_editor_session.cpp**
- **DELETE** all XZ plane functions:
  - `handle_xz_scroll()`
  - `fit_camera_y_distance_for_xz()`
- **MODIFY** `render_plane_grid()` - remove XZ case, only XY logic
- **MODIFY** `project_texture_bounds()` - remove XZ projection
- **MODIFY** `apply_camera_defaults()` - remove plane parameter, always XY
- **MODIFY** `update_plane_labels()` - remove entirely
- **MODIFY** `begin()` - remove `edit_plane_ = EditPlane::XZ;`
- **MODIFY** `handle_event()` - remove plane toggle handling
- **MODIFY** `render()` - remove XZ grid plane logic
- **MODIFY** `ensure_widgets()` - remove plane toggle button creation

### **2. render_in_front Total Extinction**

#### **ChildFrame struct** (frame_editor_session.hpp)
- **DELETE** `bool render_in_front = true;`

#### **All parsing/serialization files**
- **MODIFY** `child_frame_from_timeline_sample()` - remove render_in_front reading
- **MODIFY** `child_frame_to_json()` - remove render_in_front writing
- **MODIFY** `parse_movement_frames_json()` - remove render_in_front array element
- **MODIFY** animation_loader.cpp - remove render_in_front parsing
- **MODIFY** asset_info.cpp - remove render_in_front handling

#### **UI Elements**
- **DELETE** `cb_child_render_front_;` checkbox
- **DELETE** `last_child_front_value_;` cache
- **MODIFY** `ensure_widgets()` - remove render_in_front checkbox creation
- **MODIFY** `rebuild_layout()` - remove render_in_front from ChildrenToolboxMetrics
- **MODIFY** `update()` - remove render_in_front UI updates
- **MODIFY** `handle_event()` - remove render_in_front event handling

#### **Rendering Pipeline**
- **MODIFY** `composite_asset_renderer.cpp` - remove all render_in_front checks
- **MODIFY** child preview rendering - remove render_in_front logic
- **MODIFY** `build_child_frame_descriptor()` - remove render_in_front

### **3. New Z Adjustment Implementation**

#### **Input Handling** (frame_editor_session.cpp)
- **ADD** to `update()` function:
```cpp
if (input.getScrollY() != 0 && (input.getModifiers() & KMOD_SHIFT)) {
    float delta = input.getScrollY() * 8.0f; // Adjust multiplier as needed
    if (is_children_mode(mode_) && selected_child_index_ >= 0) {
        auto* child = current_child_frame();
        if (child) {
            child->dz += delta;
            child->has_data = true;
            persist_changes();
        }
    } else {
        // Adjust current movement frame Z
        current_movement_frame().dz += delta;
        rebuild_rel_positions();
        persist_changes();
    }
}
```

#### **UI Updates**
- **MODIFY** labels to show Z instead of Y/Z plane switching
- **ADD** Z value display in child toolbox
- **ADD** Z value display in movement toolbox

### **4. Data Format Breaking Changes**

#### **JSON Structure Changes**
- **BREAKING**: Remove render_in_front from all child frame arrays/objects
- **BREAKING**: Child arrays now: `[child_index, dx, dy, dz, degree, visible]` (6 elements instead of 7)
- **BREAKING**: Child objects remove "render_in_front" field
- **BREAKING**: Timeline samples remove "render_in_front" field

#### **Animation Loading**
- **MODIFY** all loaders to expect new format
- **DELETE** render_in_front from AnimationChildFrameData
- **DELETE** render_in_front from asset rendering structs

### **5. Child Rendering Simplification**

#### **Single Rendering Guarantee**
- **MODIFY** `render()` - ensure children render exactly once
- **REMOVE** any conditional rendering based on render_in_front
- **MODIFY** child world position calculation:
```cpp
SDL_FPoint child_world = {
    parent_pos.x + dx * scale,
    parent_pos.y + dy * scale + dz // Z now affects Y position in 2D view
};
```

### **6. Massive Code Deletion**

#### **Dead Code Removal**
- **DELETE** all plane-related conditional logic
- **DELETE** XZ camera handling code
- **DELETE** plane-dependent metrics and calculations
- **DELETE** render_in_front state management
- **DELETE** plane toggle UI layout code

#### **Simplification Targets**
- `MovementToolboxMetrics` - remove plane-specific fields
- `ChildrenToolboxMetrics` - remove render_in_front fields
- `build_directory_panel_metrics()` - remove plane button
- All plane-related label updating functions

### **7. Test and Manifest Updates**

#### **Test Files**
- **UPDATE** all test files expecting render_in_front to expect new format
- **DELETE** render_in_front test assertions
- **ADD** tests for new Z adjustment method

#### **Manifest Files**
- **BREAKING**: Update all manifest.json files to new child format
- **BREAKING**: Remove render_in_front from all child entries
- **BREAKING**: Update animation JSON structure

### **8. Implementation Sequence**

1. **Immediate Breaking Changes:**
   - Delete XZ plane enum and all XZ functions
   - Delete render_in_front from structs and UI
   - Update data parsing to new format

2. **New Functionality:**
   - Implement Shift+scroll Z adjustment
   - Update child positioning logic

3. **Cleanup:**
   - Remove dead code and conditionals
   - Simplify rendering pipeline
   - Update all tests and manifests

4. **Final Simplification:**
   - Remove plane-related UI elements
   - Streamline toolbox layouts
   - Ensure single child rendering

This plan completely eliminates the XZ plane complexity and render_in_front legacy, replacing Z adjustment with a simple Shift+scroll mechanism. All old data formats are broken and must be updated.
