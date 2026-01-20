# Frame Editor Point Selection & Navigation Implementation Plan

## Overview
This plan addresses point selection, visual feedback, frame navigation, and deselection behavior across all frame editor modes to create a consistent, intuitive editing experience.

## Current System Analysis

### Components
1. **Point3DEditor** ([Point3DEditor.cpp](ENGINE/dev_mode/frame_editors/shared/Point3DEditor.cpp))
   - Shared component for point rendering and mouse interaction
   - `handle_mouse_event()` accepts vector of screen positions
   - `render_axis_point()` renders points with axis-based colors (R/G/B)
   - Currently allows clicking ANY point in the passed array

2. **Frame Editors**
   - **MovementFrameEditor** - Movement points per frame (1 point per frame)
   - **SyncChildrenFrameEditor** - Sync child positions (1 point per child per frame)
   - **AsyncChildrenFrameEditor** - Async child timelines (1 point per child)
   - **AttackGeoFrameEditor** - Attack vectors (3 points per vector: start, control, end)
   - **HitGeoFrameEditor** - Hitbox centers (1 point per hitbox type per frame)

### Current Issues

#### 1. Unrestricted Point Selection
- **MovementFrameEditor:238-250** and **HitGeoFrameEditor:239-264**: Pass ALL frames' points to editor
- Clicking any point works, even points from other frames
- This violates the principle that only current frame's points should be selectable

#### 2. Frame Switching on Click
- **MovementFrameEditor:112-127**: Clicking a point switches to that frame
- **HitGeoFrameEditor:64-76**: Clicking a point switches to that frame
- Should use FrameNavigator exclusively for frame changes

#### 3. Inconsistent Visual Feedback
- Points use axis colors (red=X, green=Y, blue=Z)
- No distinction between selectable vs non-selectable points
- No hover feedback

#### 4. Deselection Behavior
- Generally works but needs consistency verification across all modes

## Implementation Requirements

### 1. Point Selection Rules
- **ONLY** points belonging to the current frame/context are selectable
- Points from other frames should be visually present but non-interactive
- Non-selectable points serve as visual reference only

### 2. Visual Feedback System
- **Selectable points**: Orange color (`SDL_Color{255, 165, 0, 255}`)
- **Non-selectable points**: Gray color (`SDL_Color{128, 128, 128, 128}`) with transparency
- **Hovered selectable point**: White outline (2px thick)
- **Selected point**: Larger outer circle with axis color, movement arrows, white border

### 3. Frame Navigation
- **ONLY** FrameNavigator buttons/textbox change frames
- Point clicks NEVER change frames
- Arrow keys navigate between points WITHIN current frame (where applicable)

### 4. Deselection Triggers
- Changing frames via FrameNavigator
- Clicking empty space (off all points)
- Clicking a different selectable point in current frame

## Implementation Steps

### Phase 1: Point3DEditor Core Changes

#### 1.1 Add Selectability Support
**File**: [Point3DEditor.hpp](ENGINE/dev_mode/frame_editors/shared/Point3DEditor.hpp)

Add new parameters and state:
```cpp
// In handle_mouse_event signature
bool handle_mouse_event(const SDL_Event& e,
                       const std::vector<SDL_FPoint>& point_screens,
                       const std::vector<bool>& point_selectable,  // NEW
                       std::function<SDL_FPoint(const SDL_Point&)> screen_to_world);

// New private member for hover tracking
int hovered_point_index_ = -1;
```

#### 1.2 Update Click Detection Logic
**File**: [Point3DEditor.cpp](ENGINE/dev_mode/frame_editors/shared/Point3DEditor.cpp:586-652)

In `handle_mouse_event()`:
```cpp
// At line ~600, add selectability check:
for (std::size_t i = 0; i < point_screens.size(); ++i) {
    if (handle_point_click(mouse_pos, point_screens[i])) {
        // NEW: Check if point is selectable
        const bool is_selectable = (i < point_selectable.size() && point_selectable[i]);

        if (!is_selectable) {
            // Don't select non-selectable points, but stop propagation
            return false;
        }

        clicked_on_point = true;
        const int clicked_index = static_cast<int>(i);
        // ... rest of existing logic
    }
}
```

#### 1.3 Add Hover Detection
**File**: [Point3DEditor.cpp](ENGINE/dev_mode/frame_editors/shared/Point3DEditor.cpp)

Add hover tracking in `handle_mouse_event()`:
```cpp
// Add before click handling:
if (e.type == SDL_MOUSEMOTION) {
    SDL_Point mouse_pos = {e.motion.x, e.motion.y};
    int new_hover = -1;

    for (std::size_t i = 0; i < point_screens.size(); ++i) {
        const bool is_selectable = (i < point_selectable.size() && point_selectable[i]);
        if (is_selectable && handle_point_click(mouse_pos, point_screens[i])) {
            new_hover = static_cast<int>(i);
            break;
        }
    }

    if (new_hover != hovered_point_index_) {
        hovered_point_index_ = new_hover;
        // Could add hover callback here if needed
    }
}
```

#### 1.4 Create New Rendering Methods
**File**: [Point3DEditor.hpp](ENGINE/dev_mode/frame_editors/shared/Point3DEditor.hpp)

Add new rendering signatures:
```cpp
// Render a selectable point (orange)
void render_selectable_point(SDL_Renderer* renderer,
                             SDL_FPoint screen_pos,
                             bool is_selected,
                             bool is_hovered,
                             float radius = 8.0f);

// Render a non-selectable point (gray, transparent)
void render_non_selectable_point(SDL_Renderer* renderer,
                                 SDL_FPoint screen_pos,
                                 float radius = 8.0f);
```

**File**: [Point3DEditor.cpp](ENGINE/dev_mode/frame_editors/shared/Point3DEditor.cpp)

Implement rendering methods:
```cpp
void Point3DEditor::render_selectable_point(SDL_Renderer* renderer,
                                           SDL_FPoint screen_pos,
                                           bool is_selected,
                                           bool is_hovered,
                                           float radius) {
    if (!renderer) return;

    const SDL_Color orange{255, 165, 0, 255};
    const float center_radius = radius * 0.4f;

    // If selected, draw larger outer circle with axis color
    if (is_selected) {
        const float outer_radius = radius * 1.5f;
        SDL_Color axis_color = get_axis_color(selection_ ? selection_->axis : AdjustmentAxis::X);

        // Draw outer circle with axis color
        SDL_SetRenderDrawColor(renderer, axis_color.r, axis_color.g, axis_color.b, 255);
        for (float y = -outer_radius; y <= outer_radius; ++y) {
            for (float x = -outer_radius; x <= outer_radius; ++x) {
                float dist_sq = x * x + y * y;
                if (dist_sq <= outer_radius * outer_radius) {
                    SDL_RenderDrawPointF(renderer, screen_pos.x + x, screen_pos.y + y);
                }
            }
        }

        // White border
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        const float border_inner = outer_radius;
        const float border_outer = outer_radius + 1.5f;
        for (float y = -border_outer; y <= border_outer; ++y) {
            for (float x = -border_outer; x <= border_outer; ++x) {
                float dist_sq = x * x + y * y;
                if (dist_sq >= border_inner * border_inner && dist_sq <= border_outer * border_outer) {
                    SDL_RenderDrawPointF(renderer, screen_pos.x + x, screen_pos.y + y);
                }
            }
        }

        // Movement arrows
        render_movement_arrows(renderer, screen_pos, selection_->axis, 32.0f);
    }
    // If hovered (but not selected), draw white outline
    else if (is_hovered) {
        // Draw orange circle
        SDL_SetRenderDrawColor(renderer, orange.r, orange.g, orange.b, 255);
        for (float y = -radius; y <= radius; ++y) {
            for (float x = -radius; x <= radius; ++x) {
                float dist_sq = x * x + y * y;
                if (dist_sq <= radius * radius) {
                    SDL_RenderDrawPointF(renderer, screen_pos.x + x, screen_pos.y + y);
                }
            }
        }

        // White outline
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        const float outline_inner = radius;
        const float outline_outer = radius + 2.0f;
        for (float y = -outline_outer; y <= outline_outer; ++y) {
            for (float x = -outline_outer; x <= outline_outer; ++x) {
                float dist_sq = x * x + y * y;
                if (dist_sq >= outline_inner * outline_inner && dist_sq <= outline_outer * outline_outer) {
                    SDL_RenderDrawPointF(renderer, screen_pos.x + x, screen_pos.y + y);
                }
            }
        }
    }
    // Otherwise, just draw orange circle
    else {
        SDL_SetRenderDrawColor(renderer, orange.r, orange.g, orange.b, 255);
        for (float y = -radius; y <= radius; ++y) {
            for (float x = -radius; x <= radius; ++x) {
                float dist_sq = x * x + y * y;
                if (dist_sq <= radius * radius) {
                    SDL_RenderDrawPointF(renderer, screen_pos.x + x, screen_pos.y + y);
                }
            }
        }
    }

    // Always draw white center dot
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    for (float y = -center_radius; y <= center_radius; ++y) {
        for (float x = -center_radius; x <= center_radius; ++x) {
            if (x * x + y * y <= center_radius * center_radius) {
                SDL_RenderDrawPointF(renderer, screen_pos.x + x, screen_pos.y + y);
            }
        }
    }
}

void Point3DEditor::render_non_selectable_point(SDL_Renderer* renderer,
                                                SDL_FPoint screen_pos,
                                                float radius) {
    if (!renderer) return;

    const SDL_Color gray{128, 128, 128, 128};

    // Semi-transparent gray circle
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, gray.r, gray.g, gray.b, gray.a);

    for (float y = -radius; y <= radius; ++y) {
        for (float x = -radius; x <= radius; ++x) {
            float dist_sq = x * x + y * y;
            if (dist_sq <= radius * radius) {
                SDL_RenderDrawPointF(renderer, screen_pos.x + x, screen_pos.y + y);
            }
        }
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}
```

### Phase 2: MovementFrameEditor Updates

**File**: [MovementFrameEditor.cpp](ENGINE/dev_mode/frame_editors/MovementFrameEditor.cpp)

#### 2.1 Remove Frame Switching on Point Click
**Location**: Lines 105-128

Change callback to NOT switch frames:
```cpp
point_3d_editor_->set_on_point_selected([this](int index) {
    if (index < 0) {
        // Deselecting - clear selection state
        if (selection_state_) {
            selection_state_->reset();
        }
    } else {
        // Only handle selection if it's the current frame's point
        if (index == selected_index_) {
            if (selection_state_) {
                selection_state_->target = SelectionTarget::MovementPoint;
            }
            if (point_3d_editor_) {
                point_3d_editor_->set_selected_point_index(index);
            }
            refresh_selection_state();
        }
    }
});
```

#### 2.2 Update Point Passing with Selectability
**Location**: Lines 236-250

```cpp
if (!ui_contains_point(mouse_pos)) {
    std::vector<SDL_FPoint> point_screens;
    std::vector<bool> point_selectable;

    for (std::size_t i = 0; i < rel_positions_.size(); ++i) {
        SDL_FPoint screen{};
        if (project_relative_point(i, screen)) {
            point_screens.push_back(screen);
            // Only current frame's point is selectable
            point_selectable.push_back(static_cast<int>(i) == selected_index_);
        }
    }

    consumed = point_3d_editor_->handle_mouse_event(
        e, point_screens, point_selectable,
        [this](const SDL_Point& p) {
            const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
            return cam.screen_to_map(p);
        });
}
```

#### 2.3 Update Rendering
**Location**: Lines 283-290 in `render_world()`

```cpp
for (std::size_t i = 0; i < rel_positions_.size(); ++i) {
    if (!has_screen[i]) continue;
    SDL_FPoint p = screen_points[i];
    const bool is_current_frame = (static_cast<int>(i) == selected_index_);
    const bool is_selected = (is_current_frame &&
                             selection_state_ &&
                             selection_state_->target == SelectionTarget::MovementPoint);
    const bool is_hovered = (static_cast<int>(i) == point_3d_editor_->get_hovered_point_index());

    if (is_current_frame) {
        point_3d_editor_->render_selectable_point(renderer, p, is_selected, is_hovered);
    } else {
        point_3d_editor_->render_non_selectable_point(renderer, p);
    }
}
```

#### 2.4 Ensure Deselection on Frame Change
**Location**: Lines 354-370 in `select_frame()`

Already implemented correctly - verify it clears selection.

### Phase 3: SyncChildrenFrameEditor Updates

**File**: [SyncChildrenFrameEditor.cpp](ENGINE/dev_mode/frame_editors/SyncChildrenFrameEditor.cpp)

#### 3.1 Update Point Selection Callback
**Location**: Lines 40-56

Already correct - doesn't switch frames. Verify it only selects current child.

#### 3.2 Update Point Passing with Selectability
**Location**: Lines 220-241

```cpp
if (SDL_PointInRect(&mouse_pos, &back_rect_) == SDL_FALSE &&
    SDL_PointInRect(&mouse_pos, &ui_rect_) == SDL_FALSE) {
    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    SDL_Point anchor = asset_anchor_world();
    std::vector<SDL_FPoint> point_screens;
    std::vector<bool> point_selectable;

    for (std::size_t idx = 0; idx < child_assets_.size(); ++idx) {
        if (idx >= child_modes_.size() || child_modes_[idx] == AnimationChildMode::Async) {
            continue;
        }
        const ChildWorldPose pose = child_world_pose(static_cast<int>(idx));
        SDL_FPoint world_pos{anchor.x + pose.pos.x, anchor.y + pose.pos.y};
        SDL_FPoint screen = cam.map_to_screen_f(world_pos);
        point_screens.push_back(screen);
        // Only the currently selected child is selectable
        point_selectable.push_back(static_cast<int>(idx) == selected_child_index_);
    }

    if (point_3d_editor_->handle_mouse_event(e, point_screens, point_selectable,
            [this](const SDL_Point& p) {
                const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
                return cam.screen_to_map(p);
            })) {
        return true;
    }
}
```

#### 3.3 Update Rendering
**Location**: Lines 280-295 in `render_world()`

```cpp
SDL_Point anchor = asset_anchor_world();
for (std::size_t idx = 0; idx < child_assets_.size(); ++idx) {
    if (idx >= static_frames_by_child_.size()) continue;
    if (idx >= child_modes_.size()) continue;
    if (child_modes_[idx] == AnimationChildMode::Async) {
        continue;
    }
    const ChildWorldPose pose = child_world_pose(static_cast<int>(idx));
    SDL_FPoint child_pos = pose.pos;
    SDL_FPoint world{anchor.x + child_pos.x, anchor.y + child_pos.y};
    SDL_FPoint screen = cam.map_to_screen_f(world);

    const bool is_current_child = (static_cast<int>(idx) == selected_child_index_);
    const bool is_selected = (is_current_child &&
                             selection_state_ &&
                             selection_state_->target == SelectionTarget::ChildPoint);
    const bool is_hovered = (static_cast<int>(idx) == point_3d_editor_->get_hovered_point_index());

    if (is_current_child) {
        point_3d_editor_->render_selectable_point(renderer, screen, is_selected, is_hovered);
    } else {
        point_3d_editor_->render_non_selectable_point(renderer, screen);
    }
}
```

### Phase 4: AsyncChildrenFrameEditor Updates

**File**: [AsyncChildrenFrameEditor.cpp](ENGINE/dev_mode/frame_editors/AsyncChildrenFrameEditor.cpp)

#### 4.1 Update Point Selection Callback
**Location**: Lines 50-71

Already correct - doesn't switch frames.

#### 4.2 Update Point Passing with Selectability
**Location**: Lines 148-170

```cpp
const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
SDL_Point anchor = asset_anchor_world();
std::vector<SDL_FPoint> point_screens;
std::vector<bool> point_selectable;

for (std::size_t idx = 0; idx < child_assets_.size(); ++idx) {
    if (idx >= child_modes_.size() || child_modes_[idx] != AnimationChildMode::Async) {
        continue;
    }
    const ChildWorldPose pose = child_world_pose(static_cast<int>(idx));
    SDL_FPoint world{anchor.x + pose.pos.x, anchor.y + pose.pos.y};
    SDL_FPoint screen{};
    if (!cam.project_world_point(world, pose.z, screen)) {
        screen = cam.map_to_screen_f(world);
    }
    point_screens.push_back(screen);
    // Only currently selected child is selectable
    point_selectable.push_back(static_cast<int>(idx) == selected_child_index_);
}

if (point_3d_editor_->handle_mouse_event(e, point_screens, point_selectable,
        [this](const SDL_Point& p) {
            const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
            return cam.screen_to_map(p);
        })) {
    return true;
}
```

#### 4.3 Update Rendering
**Location**: Lines 218-236 in `render_world()`

```cpp
const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
SDL_Point anchor = asset_anchor_world();
for (std::size_t idx = 0; idx < child_assets_.size(); ++idx) {
    if (idx >= child_modes_.size()) continue;
    if (child_modes_[idx] != AnimationChildMode::Async) continue;

    const ChildWorldPose pose = child_world_pose(static_cast<int>(idx));
    SDL_FPoint world{anchor.x + pose.pos.x, anchor.y + pose.pos.y};
    SDL_FPoint screen{};
    if (!cam.project_world_point(world, pose.z, screen)) {
        screen = cam.map_to_screen_f(world);
    }

    const bool is_current_child = (static_cast<int>(idx) == selected_child_index_);
    const bool is_selected = (is_current_child &&
                             selection_state_ &&
                             selection_state_->target == SelectionTarget::ChildPoint);
    const bool is_hovered = (static_cast<int>(idx) == point_3d_editor_->get_hovered_point_index());

    if (is_current_child) {
        point_3d_editor_->render_selectable_point(renderer, screen, is_selected, is_hovered);
    } else {
        point_3d_editor_->render_non_selectable_point(renderer, screen);
    }
}
```

### Phase 5: AttackGeoFrameEditor Updates

**File**: [AttackGeoFrameEditor.cpp](ENGINE/dev_mode/frame_editors/AttackGeoFrameEditor.cpp)

#### 5.1 Update Point Selection Callback
**Location**: Lines 76-109

Already correct - doesn't switch frames, only selects within current frame.

#### 5.2 Update Point Passing with Selectability
**Location**: Needs to be added (currently renders points directly)

Find where points are rendered and add selectability array for current frame's attack points only.

#### 5.3 Update Arrow Key Navigation
**Location**: Lines 253-318

Keep this behavior - it's correct to navigate between points within the current frame.

### Phase 6: HitGeoFrameEditor Updates

**File**: [HitGeoFrameEditor.cpp](ENGINE/dev_mode/frame_editors/HitGeoFrameEditor.cpp)

#### 6.1 Remove Frame Switching on Point Click
**Location**: Lines 57-77

```cpp
point_3d_editor_->set_on_point_selected([this](int index) {
    if (index < 0) {
        // Deselecting - clear selection state
        if (selection_state_) {
            selection_state_->reset();
        }
    } else {
        // Only handle selection if it's the current frame's hitbox
        if (index == selected_index_) {
            if (selection_state_) {
                selection_state_->target = SelectionTarget::HitboxCenter;
            }
            if (point_3d_editor_) {
                point_3d_editor_->set_selected_point_index(index);
            }
            refresh_selection_state();
        }
    }
});
```

#### 6.2 Update Point Passing with Selectability
**Location**: Lines 238-270

```cpp
if (!ui_contains_point(mouse_pos)) {
    std::vector<SDL_FPoint> point_screens;
    std::vector<bool> point_selectable;
    const std::string type = current_hitbox_type();
    SDL_Point anchor = asset_anchor_world();
    const float scale = asset_local_scale();
    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();

    for (std::size_t i = 0; i < frames_.size(); ++i) {
        const auto& frame = frames_[i];
        const animation_update::FrameHitGeometry::HitBox* box = nullptr;
        for (const auto& b : frame.hit.boxes) {
            if (b.type == type) {
                box = &b;
                break;
            }
        }
        if (box) {
            SDL_FPoint world{static_cast<float>(anchor.x) + box->center_x * scale,
                            static_cast<float>(anchor.y) - box->center_y * scale};
            point_screens.push_back(cam.map_to_screen_f(world));
            // Only current frame's hitbox is selectable
            point_selectable.push_back(static_cast<int>(i) == selected_index_);
        } else {
            point_screens.push_back(SDL_FPoint{-10000.0f, -10000.0f});
            point_selectable.push_back(false);
        }
    }

    consumed = point_3d_editor_->handle_mouse_event(e, point_screens, point_selectable,
        [this](const SDL_Point& p) {
            return screen_to_world_point(p);
        });
}
```

#### 6.3 Update Rendering
Find rendering code and add selectability distinction.

### Phase 7: Testing & Validation

#### Test Cases

1. **MovementFrameEditor**
   - [ ] Current frame point is orange and selectable
   - [ ] Other frames' points are gray and non-selectable
   - [ ] Clicking other frames' points does nothing
   - [ ] Hovering selectable point shows white outline
   - [ ] Changing frames via navigator deselects point
   - [ ] Clicking empty space deselects point

2. **SyncChildrenFrameEditor**
   - [ ] Current child's point is orange and selectable
   - [ ] Other children's points are gray and non-selectable
   - [ ] Hovering selectable point shows white outline
   - [ ] Tab cycles through children correctly
   - [ ] Changing frames deselects point

3. **AsyncChildrenFrameEditor**
   - [ ] Current child's point is orange and selectable
   - [ ] Other children's points are gray and non-selectable
   - [ ] Comma/period navigation works
   - [ ] Bracket keys adjust start frame
   - [ ] Tab cycles children

4. **AttackGeoFrameEditor**
   - [ ] Current frame's attack points are orange and selectable
   - [ ] Arrow keys navigate between attack points (start/control/end)
   - [ ] Frame navigation via navigator only
   - [ ] Hovering shows white outline

5. **HitGeoFrameEditor**
   - [ ] Current frame's hitbox is orange and selectable
   - [ ] Other frames' hitboxes are gray and non-selectable
   - [ ] Clicking other frames' hitboxes does nothing
   - [ ] Frame navigation via < > buttons only
   - [ ] Hovering shows white outline

## Notes

- The plan maintains backward compatibility where possible
- All existing frame navigation via FrameNavigator remains unchanged
- Point3DEditor API changes require updating all frame editors
- Visual feedback is consistent across all modes
- Non-selectable points provide visual context without being interactive

## Files to Modify

1. [Point3DEditor.hpp](ENGINE/dev_mode/frame_editors/shared/Point3DEditor.hpp)
2. [Point3DEditor.cpp](ENGINE/dev_mode/frame_editors/shared/Point3DEditor.cpp)
3. [MovementFrameEditor.cpp](ENGINE/dev_mode/frame_editors/MovementFrameEditor.cpp)
4. [SyncChildrenFrameEditor.cpp](ENGINE/dev_mode/frame_editors/SyncChildrenFrameEditor.cpp)
5. [AsyncChildrenFrameEditor.cpp](ENGINE/dev_mode/frame_editors/AsyncChildrenFrameEditor.cpp)
6. [AttackGeoFrameEditor.cpp](ENGINE/dev_mode/frame_editors/AttackGeoFrameEditor.cpp)
7. [HitGeoFrameEditor.cpp](ENGINE/dev_mode/frame_editors/HitGeoFrameEditor.cpp)
