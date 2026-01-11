# Frame Editor Session Nuclear Cleanup Plan

## Overview
**BREAKING CHANGES** - Complete elimination of XZ plane complexity and render_in_front legacy system. Replace grid-based point clicking with single-axis scroll adjustment for all 3D frame data. The frame editor visualizes 3D coordinates in a 2D interface but operates exclusively with 3D transformations.

## Core System: Single-Axis Point Adjustment

### How Point Selection Works
- **Select Points**: Left-click any adjustable point (movement frames, child positions, attack vectors, hitboxes) to select it
- **Cycle Axes**: Left-click the selected point again to cycle through adjustable axes: X → Y → Z → X
- **Adjust Values**: Use mouse scroll wheel to increase/decrease the currently selected axis value
- **Apply Changes**: Click elsewhere or select another point to apply smoothing effects and deselect

### Grid Resolution & Snapping
All adjustments use the same grid resolution as existing dx/dy spacing for consistent snapping behavior across all point types and axes.

### Point Types by Editor Mode
- **Movement Mode**: Single frame position point with X/Y/Z axes
- **StaticChildren Mode**: Individual child asset positions relative to parent with X/Y/Z axes
- **AsyncChildren Mode**: Timeline-aware child positions with X/Y/Z axes
- **AttackGeometry Mode**: Multi-point attack vectors (start/control/end) each with X/Y/Z axes
- **HitGeometry Mode**: Hitbox center positions with X/Y/Z axes

## Camera & Input Controls

### When No Point Selected (Normal Operation)
- **Left-click + drag**: Pan camera using standard dev mode pan_height controls
- **Scroll wheel**: Zoom in/out using standard dev mode controls
- **Right-click**: Context menus and mode-specific actions

### When Point Selected (Adjustment Mode)
- **Left-click**: Reserved for point selection and axis cycling
- **Scroll wheel**: Adjusts selected axis value (disables zoom)
- **Camera movement**: Use keyboard shortcuts if needed during adjustment

## Visual Feedback & 3D Indicators

### Point Selection Feedback
- Selected points show bright colored outlines
- Current axis (X/Y/Z) and value are displayed prominently
- Color-coded cycling: Red for X-axis, Green for Y-axis, Blue for Z-axis

### 3D Direction Visualization
- Directional arrows show movement direction for current axis:
  - **X-axis (Red)**: Horizontal left/right movement indicators
  - **Y-axis (Green)**: Vertical up/down movement indicators
  - **Z-axis (Blue)**: Depth forward/back movement indicators
- Ghosted previews show point position during scroll adjustments
- Real-time value updates as adjustments are made

## Data Format Changes (BREAKING)

### JSON Structure Updates
- **Child arrays**: Now `[child_index, dx, dy, dz, degree, visible]` (6 elements, was 7)
- **Remove fields**: `render_in_front` from all child objects and timeline samples
- **Animation data**: Update all loaders to expect new 6-element child format

### Coordinate System
All frame data operates in pure 3D space. Child positions calculated as: `child_3d_pos = parent_3d_pos + scaled(dx, dy, dz)`, then projected to 2D for display.

## Implementation Changes (BREAKING)

### Removed Systems
- **XZ plane editing**: Delete all plane-related enums, functions, and UI elements
- **render_in_front**: Remove from all structs, rendering logic, and data formats
- **Grid clicking**: Replace old point grid manipulation system entirely

### New Systems
- **Point selection state**: Track selected point, current axis, and point type
- **Scroll adjustment**: Apply grid-resolution changes to selected axis
- **3D visualization**: Proper 3D position calculation with 2D projection
- **Smoothing effects**: Apply when points are deselected, maintaining animation continuity

### Rendering Updates
- **Child positioning**: Use proper 3D calculations with consistent scaling on all axes
- **Grid display**: Rely on standard dev mode grid overlay (no special XY grid)
- **Single rendering**: Ensure children render exactly once without conditional logic

## Implementation Sequence

1. **Breaking Changes First**:
   - Delete XZ plane system and render_in_front entirely
   - Update all data parsers to new JSON format
   - Remove plane-specific UI and rendering code

2. **Core Adjustment System**:
   - Implement point selection and axis cycling
   - Add scroll-based value adjustment with grid snapping
   - Create 3D directional visual feedback

3. **Integration & Cleanup**:
   - Update camera controls for adjustment mode
   - Remove old grid clicking code
   - Update all tests and manifest files

4. **Polish & Verification**:
   - Ensure smooth interaction between point adjustment and camera controls
   - Verify 3D position calculations work correctly
   - Test smoothing effects maintain animation quality
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
- **MODIFY** `render_plane_grid()` - completely remove, use standard dev mode grid overlay instead
- **MODIFY** `project_texture_bounds()` - remove XZ projection
- **MODIFY** `apply_camera_defaults()` - remove plane parameter, use standard camera setup
- **MODIFY** `update_plane_labels()` - remove entirely
- **MODIFY** `begin()` - remove `edit_plane_ = EditPlane::XZ;`
- **MODIFY** `handle_event()` - remove plane toggle handling
- **MODIFY** `render()` - remove XZ grid plane logic, rely on standard dev mode grid overlay
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
- **MODIFY** child world position calculation to use proper 3D coordinates: Calculate the child's 3D world position by adding the scaled child offset (dx, dy, dz) to the parent's 3D world position, then project this 3D position to 2D screen coordinates for display in the frame editor interface. All three axes (X, Y, Z) must be scaled consistently when computing the child's position relative to the parent.

### **6. Massive Code Deletion**

#### **Dead Code Removal**
- **DELETE** all plane-related conditional logic
- **DELETE** XZ camera handling code
- **DELETE** plane-dependent metrics and calculations
- **DELETE** render_in_front state management
- **DELETE** plane toggle UI layout code
- **DELETE** old point grid clicking system code
- **DELETE** all grid-based point adjustment functions

#### **Simplification Targets**
- `MovementToolboxMetrics` - remove plane-specific fields
- `ChildrenToolboxMetrics` - remove render_in_front fields
- `build_directory_panel_metrics()` - remove plane button
- All plane-related label updating functions
- Old point manipulation UI elements

### **7. Test and Manifest Updates**

#### **Test Files**
- **UPDATE** all test files expecting render_in_front to expect new format
- **DELETE** render_in_front test assertions
- **ADD** tests for new point selection and axis cycling
- **ADD** tests for scroll adjustment with grid resolution
- **ADD** tests for smoothing effects on unselection

#### **Manifest Files**
- **BREAKING**: Update all manifest.json files to new child format (if needed)
- **BREAKING**: Remove render_in_front from all child entries (if needed)
- **BREAKING**: Update animation JSON structure (if needed)

### **8. Implementation Sequence**

1. **Immediate Breaking Changes:**
   - Delete XZ plane enum and all XZ functions
   - Delete render_in_front from structs and UI
   - Update data parsing to new format

2. **New Point Adjustment System:**
   - Implement point selection state and axis cycling
   - Add scroll-based adjustment with grid resolution
   - Implement visual feedback and UI indicators
   - Add smoothing effects application on unselection

3. **Cleanup:**
   - Remove dead code and conditionals
   - Simplify rendering pipeline
   - Update all tests and manifests
   - Remove old grid clicking system

4. **Final Simplification:**
   - Remove plane-related UI elements
   - Streamline toolbox layouts
   - Ensure single child rendering
   - Verify point adjustment controls work smoothly

This plan completely eliminates the XZ plane complexity and render_in_front legacy, replacing all point manipulation with an intuitive single-axis scroll adjustment system that cycles through X/Y/Z axes per point. All old data formats are broken and must be updated.
