#include <algorithm>

#if defined(FRAME_EDITOR_TEST_PUBLIC_ACCESS)

class DMButton {};
class DMDropdown {};
class DMNumericStepper {};
class DMTextBox {};
class DMCheckbox {};

#include "devtools/frame_editor_session.hpp"
#include "rendering/render/camera_controller.hpp"
#include "rendering/render/warped_screen_grid.hpp"
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

#endif // FRAME_EDITOR_TEST_PUBLIC_ACCESS
