#include "camera_ui.hpp"
#include "utils/sdl_render_conversions.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <functional>
#include <sstream>

#include "devtools/dev_camera_controls.hpp"
#include "devtools/depth_cue_settings.hpp"
#include "devtools/dm_styles.hpp"
#include "core/AssetsManager.hpp"
#include "devtools/draw_utils.hpp"
#include "devtools/font_cache.hpp"
#include "devtools/float_slider_widget.hpp"
#include "devtools/shared/shared/formatting.hpp"
#include "devtools/widgets.hpp"
#include "gameplay/map_generation/room.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "utils/input.hpp"

class SpacerWidget : public Widget {
public:
    explicit SpacerWidget(int height)
        : height_(std::max(0, height)) {}

    void set_rect(const SDL_Rect& r) override { rect_ = r; }
    const SDL_Rect& rect() const override { return rect_; }
    int height_for_width(int) const override { return height_; }
    bool handle_event(const SDL_Event&) override { return false; }
    void render(SDL_Renderer*) const override {}
    bool wants_full_row() const override { return true; }

private:
    SDL_Rect rect_{0, 0, 0, 0};
    int height_ = 0;
};


CameraUIPanel::CameraUIPanel(Assets* assets, int x, int y)
    : DockableCollapsible("Camera Settings", true, x, y),
      assets_(assets) {
    set_expanded(true);
    set_visible(false);
    set_padding(16);
    set_close_button_enabled(true);
    set_close_button_on_left(false);
    set_floatable(true);
    build_ui();
    apply_settings_if_needed();
    sync_from_camera();
}

CameraUIPanel::~CameraUIPanel() = default;

void CameraUIPanel::set_assets(Assets* assets) {
    assets_ = assets;
    sync_from_camera();
}

void CameraUIPanel::open() {
    set_visible(true);
    suppress_apply_once_ = true;
    sync_from_camera();
}

void CameraUIPanel::close() {
    set_visible(false);
}

void CameraUIPanel::toggle() {
    set_visible(!is_visible());
    if (is_visible()) {
        suppress_apply_once_ = true;
        sync_from_camera();
    }
}

bool CameraUIPanel::is_point_inside(int x, int y) const {
    return DockableCollapsible::is_point_inside(x, y);
}

void CameraUIPanel::update(const Input& input, int screen_w, int screen_h) {
    last_screen_w_ = screen_w;
    last_screen_h_ = screen_h;
    const bool previously_visible = was_visible_;
    DockableCollapsible::update(input, screen_w, screen_h);
    const bool currently_visible = is_visible();
    if (currently_visible && !previously_visible) {

        suppress_apply_once_ = true;
        sync_from_camera();
    }
    was_visible_ = currently_visible;

    if (!currently_visible) return;
    if (!assets_) return;
    if (suppress_apply_once_) {
        suppress_apply_once_ = false;
        return;
    }
    apply_settings_if_needed();
}

bool CameraUIPanel::handle_event(const SDL_Event& e) {
    if (!is_visible()) return false;
    bool used = DockableCollapsible::handle_event(e);
    if (used) {
        apply_settings_if_needed();
    }
    return used;
}

void CameraUIPanel::render(SDL_Renderer* renderer) const {
    if (!renderer) return;
    if (is_visible()) {
        DockableCollapsible::render(renderer);
    }

    DMDropdown::render_active_options(renderer);
}

void CameraUIPanel::layout_custom_content(int screen_w, int screen_h) const {
    set_drag_handle_rect(SDL_Rect{0,0,0,0});
}

void CameraUIPanel::sync_from_camera() {
    if (!assets_) return;
    WarpedScreenGrid& cam = assets_->getView();
    const auto& settings = cam.get_settings();

    if (min_render_size_slider_) min_render_size_slider_->set_value(settings.min_visible_screen_ratio);
    if (max_cull_depth_slider_) max_cull_depth_slider_->set_value(settings.max_cull_depth);
    if (layer_depth_interval_slider_) layer_depth_interval_slider_->set_value(settings.layer_depth_interval);
    if (layer_depth_curve_slider_) layer_depth_curve_slider_->set_value(settings.layer_depth_curve);
    if (aperture_f_stop_slider_) aperture_f_stop_slider_->set_value(settings.aperture_f_stop);
    if (focal_length_mm_slider_) focal_length_mm_slider_->set_value(settings.focal_length_mm);
    if (max_blur_px_slider_) max_blur_px_slider_->set_value(settings.max_blur_px);
    if (boundary_min_render_size_slider_) {
        boundary_min_render_size_slider_->set_value(assets_->boundary_min_visible_screen_ratio());
    }

    // Sync camera height bounds
    const auto [saved_min, saved_max] = load_camera_height_bounds();
    if (camera_height_min_slider_) camera_height_min_slider_->set_value(saved_min);
    if (camera_height_max_slider_) camera_height_max_slider_->set_value(saved_max);

    // Initialize the global camera height bounds used by DevCameraControls
    DevCameraHeightBounds::set(static_cast<double>(saved_min), static_cast<double>(saved_max));
}

void CameraUIPanel::build_ui() {
    set_header_button_style(&DMStyles::AccentButton());
    set_header_highlight_color(DMStyles::AccentButton().bg);
    set_padding(DMSpacing::panel_padding());
    set_row_gap(DMSpacing::item_gap());
    set_col_gap(DMSpacing::item_gap());
    set_floating_content_width(460);

    header_spacer_ = std::make_unique<SpacerWidget>(DMSpacing::header_gap());

    controls_spacer_ = std::make_unique<SpacerWidget>(DMSpacing::small_gap());

    WarpedScreenGrid::RealismSettings defaults;
    if (assets_) {
        defaults = assets_->getView().get_settings();
    }
    const float min_visible_default = devmode::camera_prefs::load_min_visible_screen_ratio(defaults.min_visible_screen_ratio);
    defaults.min_visible_screen_ratio = min_visible_default;
    const float boundary_min_visible_default =
        devmode::camera_prefs::load_boundary_min_visible_screen_ratio(min_visible_default);

    min_render_size_slider_ = std::make_unique<FloatSliderWidget>("Min On-Screen Size", 0.0f, 0.05f, 0.001f, min_visible_default, 3);
    min_render_size_slider_->set_tooltip("Cull sprites once their height drops below this fraction of the screen (0.01 = 1%).");
    min_render_size_slider_->set_on_value_changed([this](float) { on_control_value_changed(); });

    boundary_min_render_size_slider_ = std::make_unique<FloatSliderWidget>(
        "Boundary Min On-Screen Size",
        0.0f,
        0.05f,
        0.001f,
        boundary_min_visible_default,
        3);
    boundary_min_render_size_slider_->set_tooltip(
        "Cull dynamically spawned boundary assets once their height drops below this fraction of the screen (0.01 = 1%).");
    boundary_min_render_size_slider_->set_on_value_changed([this](float) { on_control_value_changed(); });
    max_cull_depth_slider_ = std::make_unique<FloatSliderWidget>("Max Cull Depth", 1.0f, 50000.0f, 1.0f, defaults.max_cull_depth, 0);
    max_cull_depth_slider_->set_on_value_changed([this](float) { on_control_value_changed(); });
    layer_depth_interval_slider_ = std::make_unique<FloatSliderWidget>(
        "Layer Depth Interval",
        1.0f,
        5000.0f,
        1.0f,
        defaults.layer_depth_interval,
        0);
    layer_depth_interval_slider_->set_tooltip("Base world-depth step for near DOF bins. Lower values increase near detail.");
    layer_depth_interval_slider_->set_on_value_changed([this](float) { on_control_value_changed(); });
    layer_depth_curve_slider_ = std::make_unique<FloatSliderWidget>(
        "Layer Depth Curve",
        0.0f,
        8.0f,
        0.01f,
        defaults.layer_depth_curve,
        2);
    layer_depth_curve_slider_->set_tooltip("Non-linear bin growth with distance. Higher values create fewer far-depth layers.");
    layer_depth_curve_slider_->set_on_value_changed([this](float) { on_control_value_changed(); });
    aperture_f_stop_slider_ = std::make_unique<FloatSliderWidget>("Aperture (f-stop)", 0.01f, 64.0f, 0.01f, defaults.aperture_f_stop, 2);
    aperture_f_stop_slider_->set_on_value_changed([this](float) { on_control_value_changed(); });
    focal_length_mm_slider_ = std::make_unique<FloatSliderWidget>("Focal Length (mm)", 0.01f, 500.0f, 0.1f, defaults.focal_length_mm, 1);
    focal_length_mm_slider_->set_on_value_changed([this](float) { on_control_value_changed(); });
    max_blur_px_slider_ = std::make_unique<FloatSliderWidget>("Max Blur (px)", 0.0f, 128.0f, 0.25f, defaults.max_blur_px, 2);
    max_blur_px_slider_->set_on_value_changed([this](float) { on_control_value_changed(); });

    // Global camera height bounds
    const auto [saved_min, saved_max] = load_camera_height_bounds();
    const std::string min_label = "Min Camera Height (px): " + std::to_string(saved_min);
    const std::string max_label = "Max Camera Height (px): " + std::to_string(saved_max);

    camera_height_min_slider_ = std::make_unique<DMSlider>(min_label, 1, 100000, saved_min);
    camera_height_min_slider_->set_on_value_changed([this](int) { on_control_value_changed(); });

    camera_height_max_slider_ = std::make_unique<DMSlider>(max_label, 1, 100000, saved_max);
    camera_height_max_slider_->set_on_value_changed([this](int) { on_control_value_changed(); });

    camera_height_min_widget_ = std::make_unique<SliderWidget>(camera_height_min_slider_.get());
    camera_height_min_widget_->set_tooltip("Global minimum camera height. All room camera heights will be clamped to this value.");
    camera_height_max_widget_ = std::make_unique<SliderWidget>(camera_height_max_slider_.get());
    camera_height_max_widget_->set_tooltip("Global maximum camera height. All room camera heights will be clamped to this value.");

    rebuild_rows();
}

void CameraUIPanel::on_control_value_changed() {
    if (!assets_ || !is_visible()) {
        return;
    }
    apply_settings_if_needed();
}

std::pair<int, int> CameraUIPanel::load_camera_height_bounds() const {
    if (!assets_) return {1, 100000};
    const auto& map_info = assets_->map_info_json();
    int min_val = map_info.value("camera_height_min_px", 1);
    int max_val = map_info.value("camera_height_max_px", 100000);
    if (min_val < 1) min_val = 1;
    if (max_val < min_val) max_val = 100000;
    return {min_val, max_val};
}

void CameraUIPanel::save_camera_height_bounds(int min_val, int max_val) const {
    if (!assets_) return;
    auto& map_info = assets_->map_info_json();
    map_info["camera_height_min_px"] = min_val;
    map_info["camera_height_max_px"] = max_val;

    // Update the global camera height bounds used by DevCameraControls
    DevCameraHeightBounds::set(static_cast<double>(min_val), static_cast<double>(max_val));
}

void CameraUIPanel::clamp_all_room_camera_heights(int min_val, int max_val) {
    if (!assets_) return;
    const auto& rooms = assets_->rooms();
    for (Room* room : rooms) {
        if (!room) continue;
        room->camera_height_px = std::clamp(room->camera_height_px, min_val, max_val);
        // Also update the room's assets_data JSON
        auto& room_data = room->assets_data();
        room_data["camera_height_px"] = room->camera_height_px;
        room->mark_dirty();
    }

    // Also clamp the live camera view if it's outside the new bounds
    WarpedScreenGrid& cam = assets_->getView();
    const int current_height = static_cast<int>(cam.get_scale());
    const int clamped_height = std::clamp(current_height, min_val, max_val);
    if (clamped_height != current_height) {
        cam.set_scale(static_cast<double>(clamped_height));
    }
}



void CameraUIPanel::rebuild_rows() {
    Rows rows;
    if (header_spacer_) rows.push_back({ header_spacer_.get() });
    if (controls_spacer_) rows.push_back({ controls_spacer_.get() });
    if (min_render_size_slider_) rows.push_back({ min_render_size_slider_.get() });
    if (boundary_min_render_size_slider_) rows.push_back({ boundary_min_render_size_slider_.get() });
    if (max_cull_depth_slider_) rows.push_back({ max_cull_depth_slider_.get() });
    if (layer_depth_interval_slider_) rows.push_back({ layer_depth_interval_slider_.get() });
    if (layer_depth_curve_slider_) rows.push_back({ layer_depth_curve_slider_.get() });
    if (aperture_f_stop_slider_) rows.push_back({ aperture_f_stop_slider_.get() });
    if (focal_length_mm_slider_) rows.push_back({ focal_length_mm_slider_.get() });
    if (max_blur_px_slider_) rows.push_back({ max_blur_px_slider_.get() });
    if (enable_aperture_) rows.push_back({ enable_aperture_.get() });

    // Camera height bounds section
    if (camera_height_min_widget_) rows.push_back({ camera_height_min_widget_.get() });
    if (camera_height_max_widget_) rows.push_back({ camera_height_max_widget_.get() });

    set_rows(rows);
}

void CameraUIPanel::apply_settings_if_needed() {
    if (!assets_) return;
    if (applying_settings_) {
        return;
    }
    struct ScopedApplyingGuard {
        bool& flag;
        explicit ScopedApplyingGuard(bool& f) : flag(f) { flag = true; }
        ~ScopedApplyingGuard() { flag = false; }
    } guard(applying_settings_);
    WarpedScreenGrid& cam = assets_->getView();

    const WarpedScreenGrid::RealismSettings current = cam.get_settings();
    WarpedScreenGrid::RealismSettings updated = current;

    if (min_render_size_slider_) updated.min_visible_screen_ratio = min_render_size_slider_->value();
    if (max_cull_depth_slider_) updated.max_cull_depth = max_cull_depth_slider_->value();
    if (layer_depth_interval_slider_) updated.layer_depth_interval = layer_depth_interval_slider_->value();
    if (layer_depth_curve_slider_) updated.layer_depth_curve = layer_depth_curve_slider_->value();
    if (aperture_f_stop_slider_) updated.aperture_f_stop = aperture_f_stop_slider_->value();
    if (focal_length_mm_slider_) updated.focal_length_mm = focal_length_mm_slider_->value();
    if (max_blur_px_slider_) updated.max_blur_px = max_blur_px_slider_->value();

    auto float_changed = [](float a, float b, float eps = 1e-5f) {
        return std::fabs(a - b) > eps;
    };
    const bool realism_changed =
        float_changed(updated.min_visible_screen_ratio, current.min_visible_screen_ratio) ||
        float_changed(updated.max_cull_depth, current.max_cull_depth) ||
        float_changed(updated.layer_depth_interval, current.layer_depth_interval) ||
        float_changed(updated.layer_depth_curve, current.layer_depth_curve) ||
        float_changed(updated.aperture_f_stop, current.aperture_f_stop) ||
        float_changed(updated.focal_length_mm, current.focal_length_mm) ||
        float_changed(updated.max_blur_px, current.max_blur_px);

    float boundary_value = assets_->boundary_min_visible_screen_ratio();
    if (boundary_min_render_size_slider_) {
        boundary_value = boundary_min_render_size_slider_->value();
    }
    const bool boundary_changed =
        boundary_min_render_size_slider_ &&
        float_changed(boundary_value, assets_->boundary_min_visible_screen_ratio());

    if (realism_changed) {
        cam.set_realism_settings(updated);
        devmode::camera_prefs::save_min_visible_screen_ratio(updated.min_visible_screen_ratio);
    }
    if (boundary_changed) {
        assets_->set_boundary_min_visible_screen_ratio(boundary_value);
        devmode::camera_prefs::save_boundary_min_visible_screen_ratio(boundary_value);
    }

    if (realism_changed || boundary_changed) {
        assets_->on_camera_settings_changed();
    }

    // Apply camera height bounds
    if (camera_height_min_slider_ && camera_height_max_slider_) {
        const int new_min = camera_height_min_slider_->value();
        const int new_max = camera_height_max_slider_->value();
        const auto [old_min, old_max] = load_camera_height_bounds();

        if (new_min != old_min || new_max != old_max) {
            // Ensure min <= max
            const int clamped_min = std::min(new_min, new_max);
            const int clamped_max = std::max(new_min, new_max);

            save_camera_height_bounds(clamped_min, clamped_max);

            // Clamp all room camera heights
            clamp_all_room_camera_heights(clamped_min, clamped_max);
        }
    }
}



