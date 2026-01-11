#include <algorithm>

#if defined(FRAME_EDITOR_TEST_PUBLIC_ACCESS)

class DMButton {};
class DMDropdown {};
class DMNumericStepper {};
class DMTextBox {};
class DMCheckbox {};

#include "dev_mode/frame_editor_session.hpp"
#include "render/camera_controller.hpp"
#include "render/warped_screen_grid.hpp"
#include "utils/area.hpp"

FrameEditorSession::FrameEditorSession() = default;
FrameEditorSession::~FrameEditorSession() = default;

CameraController::CameraController(double fallback_height_px) {
    fallback_height_px_ = fallback_height_px;
    state_.params.height_px = fallback_height_px;
}

WarpedScreenGrid::WarpedScreenGrid(int screen_width, int screen_height, const Area& starting_view)
    : screen_width_(screen_width),
      screen_height_(screen_height),
      aspect_(screen_height != 0 ? static_cast<double>(screen_width) / screen_height : 1.0),
      base_view_(starting_view),
      current_view_(starting_view) {}

WarpedScreenGrid::~WarpedScreenGrid() = default;

Area::Area(const std::string& name, int resolution)
    : area_name_(name), resolution_(resolution) {}

Area::Area(const std::string& name, const std::vector<Point>& pts, int resolution)
    : points(pts), area_name_(name), resolution_(resolution) {
    if (!points.empty()) {
        min_x_ = max_x_ = points[0].x;
        min_y_ = max_y_ = points[0].y;
    }
}

Area::Area(const std::string& name, SDL_Point center, int w, int h, const std::string& geometry,
           int edge_smoothness, int map_width, int map_height, int resolution)
    : area_name_(name),
      area_type_(geometry),
      center_x(center.x),
      center_y(center.y),
      resolution_(resolution) {
    (void)edge_smoothness;
    (void)map_width;
    (void)map_height;

    points = {
        center,
        SDL_Point{center.x + w, center.y},
        SDL_Point{center.x + w, center.y + h},
        SDL_Point{center.x, center.y + h}
    };
    min_x_ = center.x;
    min_y_ = center.y;
    max_x_ = center.x + w;
    max_y_ = center.y + h;
    bounds_valid_ = true;
}

void FrameEditorSession::reset_adjustment_selection() {
    adjustment_selection_.reset();
}

void FrameEditorSession::exit_adjustment_mode(bool apply_smoothing) {
    (void)apply_smoothing;
    adjustment_mode_active_ = false;
    adjustment_dirty_ = false;
    adjustment_selection_.reset();
}

void FrameEditorSession::select_adjustment_target(AdjustmentTarget target, int child_index,
                                                  SDL_FPoint world_pos, SDL_Point screen_pos) {
    adjustment_selection_.target = target;
    adjustment_selection_.child_index = child_index;
    adjustment_selection_.world_pos = world_pos;
    adjustment_selection_.screen_pos = screen_pos;
    adjustment_selection_.axis = AdjustmentAxis::X;
    adjustment_mode_active_ = (target != AdjustmentTarget::None);
    if (!adjustment_mode_active_) {
        adjustment_dirty_ = false;
    }
}

void FrameEditorSession::cycle_adjustment_axis() {
    adjustment_selection_.axis = static_cast<AdjustmentAxis>((static_cast<int>(adjustment_selection_.axis) + 1) % 3);
}

void FrameEditorSession::adjust_selection_axis(int scroll_delta, const WarpedScreenGrid& cam) {
    (void)cam;
    if (!adjustment_mode_active_ || frames_.empty()) {
        return;
    }

    const int max_frame_index = static_cast<int>(frames_.size()) - 1;
    const int frame_index = std::clamp(selected_index_, 0, max_frame_index);
    MovementFrame& frame = frames_[frame_index];
    auto apply_delta = [&](float& value) {
        value += static_cast<float>(scroll_delta);
    };

    switch (adjustment_selection_.target) {
        case AdjustmentTarget::MovementPoint:
            switch (adjustment_selection_.axis) {
                case AdjustmentAxis::X: apply_delta(frame.dx); break;
                case AdjustmentAxis::Y: apply_delta(frame.dy); break;
                case AdjustmentAxis::Z: apply_delta(frame.dz); break;
            }
            break;
        case AdjustmentTarget::ChildPoint: {
            const int child_index = (adjustment_selection_.child_index >= 0)
                ? adjustment_selection_.child_index
                : selected_child_index_;
            if (child_index >= 0 && child_index < static_cast<int>(frame.children.size())) {
                auto& child = frame.children[child_index];
                switch (adjustment_selection_.axis) {
                    case AdjustmentAxis::X: apply_delta(child.dx); break;
                    case AdjustmentAxis::Y: apply_delta(child.dy); break;
                    case AdjustmentAxis::Z: apply_delta(child.dz); break;
                }
            }
            break;
        }
        default:
            break;
    }

    adjustment_dirty_ = true;
}

#endif // FRAME_EDITOR_TEST_PUBLIC_ACCESS
