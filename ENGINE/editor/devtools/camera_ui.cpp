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

#include "core/AssetsManager.hpp"
#include "devtools/depth_cue_settings.hpp"
#include "devtools/dm_styles.hpp"
#include "devtools/draw_utils.hpp"
#include "devtools/font_cache.hpp"
#include "devtools/float_slider_widget.hpp"
#include "devtools/shared/shared/formatting.hpp"
#include "devtools/widgets.hpp"
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
    if (boundary_min_render_size_slider_) {
        boundary_min_render_size_slider_->set_value(assets_->boundary_min_visible_screen_ratio());
    }
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

    rebuild_rows();
}

void CameraUIPanel::on_control_value_changed() {
    if (!assets_ || !is_visible()) {
        return;
    }
    apply_settings_if_needed();
}



void CameraUIPanel::rebuild_rows() {
    Rows rows;
    if (header_spacer_) rows.push_back({ header_spacer_.get() });
    if (controls_spacer_) rows.push_back({ controls_spacer_.get() });
    if (min_render_size_slider_) rows.push_back({ min_render_size_slider_.get() });
    if (boundary_min_render_size_slider_) rows.push_back({ boundary_min_render_size_slider_.get() });

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

    auto float_changed = [](float a, float b, float eps = 1e-5f) {
        return std::fabs(a - b) > eps;
    };
    const bool realism_changed =
        float_changed(updated.min_visible_screen_ratio, current.min_visible_screen_ratio);
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
}




