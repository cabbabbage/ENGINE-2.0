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

class SectionToggleWidget : public Widget {
public:
    SectionToggleWidget(std::string title,
                        std::function<bool()> expanded_getter,
                        std::function<void()> on_toggle)
        : title_(std::move(title)),
          expanded_getter_(std::move(expanded_getter)),
          on_toggle_(std::move(on_toggle)),
          button_(compose_label(), &DMStyles::SecondaryButton(), 1, DMButton::height()) {}

    void set_rect(const SDL_Rect& r) override { button_.set_rect(r); }
    const SDL_Rect& rect() const override { return button_.rect(); }
    int height_for_width(int) const override { return DMButton::height(); }
    bool wants_full_row() const override { return true; }

    bool handle_event(const SDL_Event& e) override {
        bool used = button_.handle_event(e);
        if (used &&
            e.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            e.button.button == SDL_BUTTON_LEFT &&
            on_toggle_) {
            on_toggle_();
            button_.set_text(compose_label());
        }
        return used;
    }

    void render(SDL_Renderer* renderer) const override { button_.render(renderer); }

private:
    std::string compose_label() const {
        const bool expanded = expanded_getter_ ? expanded_getter_() : true;
        return std::string(expanded ? "[-] " : "[+] ") + title_;
    }

private:
    std::string title_;
    std::function<bool()> expanded_getter_;
    std::function<void()> on_toggle_;
    DMButton button_;
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

void CameraUIPanel::set_dirty_callback(std::function<void()> callback) {
    dirty_callback_ = std::move(callback);
}

void CameraUIPanel::open() {
    set_visible(true);
    sync_runtime_lighting_state_with_visibility();
    suppress_apply_once_ = true;
    sync_from_camera();
}

void CameraUIPanel::close() {
    set_visible(false);
    sync_runtime_lighting_state_with_visibility();
}

void CameraUIPanel::toggle() {
    set_visible(!is_visible());
    sync_runtime_lighting_state_with_visibility();
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
    if (currently_visible != previously_visible) {
        sync_runtime_lighting_state_with_visibility();
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
    if (dynamic_renderer_depth_efficiency_depth_slider_) {
        dynamic_renderer_depth_efficiency_depth_slider_->set_range(0.0f, settings.max_cull_depth);
        dynamic_renderer_depth_efficiency_depth_slider_->set_value(
            settings.dynamic_renderer_depth_efficiency_depth);
    }
    if (dynamic_renderer_depth_efficiency_min_density_ratio_slider_) {
        dynamic_renderer_depth_efficiency_min_density_ratio_slider_->set_value(
            settings.dynamic_renderer_depth_efficiency_min_density_ratio);
    }
    if (layer_depth_interval_slider_) layer_depth_interval_slider_->set_value(settings.layer_depth_interval);
    if (layer_depth_curve_slider_) layer_depth_curve_slider_->set_value(settings.layer_depth_curve);
    if (front_layer_light_strength_multiplier_slider_) {
        front_layer_light_strength_multiplier_slider_->set_value(settings.front_layer_light_strength_multiplier);
    }
    if (behind_layer_light_strength_multiplier_slider_) {
        behind_layer_light_strength_multiplier_slider_->set_value(settings.behind_layer_light_strength_multiplier);
    }
    if (blur_px_slider_) blur_px_slider_->set_value(settings.blur_px);
    if (radial_blur_px_slider_) radial_blur_px_slider_->set_value(settings.radial_blur_px);
    if (depth_of_field_checkbox_) {
        depth_of_field_checkbox_->set_value(settings.depth_of_field_enabled);
    }
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
    const float min_visible_default = defaults.min_visible_screen_ratio;
    const float boundary_min_visible_default = assets_
        ? assets_->boundary_min_visible_screen_ratio()
        : 0.015f;

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
    max_cull_depth_slider_->set_tooltip("Maximum depth considered for scene layers and light falloff. Higher values add distant detail but can increase rendering work.");
    max_cull_depth_slider_->set_on_value_changed([this](float) { on_control_value_changed(); });
    dynamic_renderer_depth_efficiency_depth_slider_ = std::make_unique<FloatSliderWidget>(
        "Dynamic Efficiency Depth",
        0.0f,
        std::max(1.0f, defaults.max_cull_depth),
        1.0f,
        defaults.dynamic_renderer_depth_efficiency_depth,
        0);
    dynamic_renderer_depth_efficiency_depth_slider_->set_tooltip(
        "World depth where dynamic boundary thinning and animation freeze begin.");
    dynamic_renderer_depth_efficiency_depth_slider_->set_on_value_changed(
        [this](float) { on_control_value_changed(); });
    dynamic_renderer_depth_efficiency_min_density_ratio_slider_ = std::make_unique<FloatSliderWidget>(
        "Dynamic Efficiency Min Density",
        0.0f,
        1.0f,
        0.01f,
        defaults.dynamic_renderer_depth_efficiency_min_density_ratio,
        2);
    dynamic_renderer_depth_efficiency_min_density_ratio_slider_->set_tooltip(
        "Relative spawn density floor at Max Cull Depth after distance-based thinning.");
    dynamic_renderer_depth_efficiency_min_density_ratio_slider_->set_on_value_changed(
        [this](float) { on_control_value_changed(); });
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
        100.0f,
        0.01f,
        defaults.layer_depth_curve,
        2);
    layer_depth_curve_slider_->set_tooltip("Non-linear bin growth with distance. Higher values create fewer far-depth layers.");
    layer_depth_curve_slider_->set_on_value_changed([this](float) { on_control_value_changed(); });
    front_layer_light_strength_multiplier_slider_ = std::make_unique<FloatSliderWidget>(
        "Front Layer Light Strength",
        0.0f,
        4.0f,
        0.01f,
        defaults.front_layer_light_strength_multiplier,
        2);
    front_layer_light_strength_multiplier_slider_->set_tooltip(
        "Scales lighting energy for layers in front of the camera plane. Higher values punch up nearby lights and may increase additive light passes.");
    front_layer_light_strength_multiplier_slider_->set_on_value_changed([this](float) { on_control_value_changed(); });
    behind_layer_light_strength_multiplier_slider_ = std::make_unique<FloatSliderWidget>(
        "Behind Layer Light Strength",
        0.0f,
        4.0f,
        0.01f,
        defaults.behind_layer_light_strength_multiplier,
        2);
    behind_layer_light_strength_multiplier_slider_->set_tooltip(
        "Scales lighting energy for layers behind the camera plane. Raise to keep deep background lights readable; lower to reduce distant glow and cost.");
    behind_layer_light_strength_multiplier_slider_->set_on_value_changed([this](float) { on_control_value_changed(); });
    blur_px_slider_ = std::make_unique<FloatSliderWidget>("Blur (px)", 0.0f, 128.0f, 0.01f, defaults.blur_px, 3);
    blur_px_slider_->set_tooltip("Per-layer Gaussian-like blur budget. Larger values produce softer focus transitions and increase blur processing cost.");
    blur_px_slider_->set_on_value_changed([this](float) { on_control_value_changed(); });
    radial_blur_px_slider_ = std::make_unique<FloatSliderWidget>(
        "Radial Blur (px)",
        0.0f,
        256.0f,
        0.01f,
        defaults.radial_blur_px,
        3);
    radial_blur_px_slider_->set_tooltip("Additional radial blur from the optical center. Higher values can add cinematic depth but increase post-process work.");
    radial_blur_px_slider_->set_on_value_changed([this](float) { on_control_value_changed(); });
    auto dof_checkbox = std::make_unique<DMCheckbox>("Enable Depth Of Field", defaults.depth_of_field_enabled);
    depth_of_field_checkbox_ = dof_checkbox.get();
    depth_of_field_widget_ = std::make_unique<CallbackCheckboxWidget>(
        std::move(dof_checkbox),
        [this](bool) { on_control_value_changed(); });

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

    movement_section_widget_ = std::make_unique<SectionToggleWidget>(
        "Movement",
        [this]() { return movement_section_expanded_; },
        [this]() {
            movement_section_expanded_ = !movement_section_expanded_;
            rebuild_rows();
        });
    framing_section_widget_ = std::make_unique<SectionToggleWidget>(
        "Framing",
        [this]() { return framing_section_expanded_; },
        [this]() {
            framing_section_expanded_ = !framing_section_expanded_;
            rebuild_rows();
        });
    lighting_section_widget_ = std::make_unique<SectionToggleWidget>(
        "Lighting",
        [this]() { return lighting_section_expanded_; },
        [this]() {
            lighting_section_expanded_ = !lighting_section_expanded_;
            rebuild_rows();
        });
    debug_section_widget_ = std::make_unique<SectionToggleWidget>(
        "Debug",
        [this]() { return debug_section_expanded_; },
        [this]() {
            debug_section_expanded_ = !debug_section_expanded_;
            rebuild_rows();
        });

    rebuild_rows();
}

void CameraUIPanel::sync_runtime_lighting_state_with_visibility() {
    if (!assets_) {
        return;
    }
    assets_->set_camera_settings_panel_active(is_visible());
}

void CameraUIPanel::on_control_value_changed() {
    if (!assets_ || !is_visible()) {
        return;
    }
    apply_settings_if_needed();
}

std::pair<int, int> CameraUIPanel::load_camera_height_bounds() const {
    if (!assets_) return {1, 100000};
    return assets_->camera_height_bounds_px();
}

void CameraUIPanel::save_camera_height_bounds(int min_val, int max_val) const {
    if (!assets_) return;
    assets_->set_camera_height_bounds_px(min_val, max_val);
    assets_->sync_camera_settings_to_map_info_json();
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
    if (movement_section_widget_) rows.push_back({ movement_section_widget_.get() });
    if (movement_section_expanded_) {
        if (camera_height_min_widget_) rows.push_back({ camera_height_min_widget_.get() });
        if (camera_height_max_widget_) rows.push_back({ camera_height_max_widget_.get() });
    }

    if (framing_section_widget_) rows.push_back({ framing_section_widget_.get() });
    if (framing_section_expanded_) {
        if (min_render_size_slider_) rows.push_back({ min_render_size_slider_.get() });
        if (boundary_min_render_size_slider_) rows.push_back({ boundary_min_render_size_slider_.get() });
    }

    if (lighting_section_widget_) rows.push_back({ lighting_section_widget_.get() });
    if (lighting_section_expanded_) {
        if (depth_of_field_widget_) rows.push_back({ depth_of_field_widget_.get() });
        if (blur_px_slider_) rows.push_back({ blur_px_slider_.get() });
        if (radial_blur_px_slider_) rows.push_back({ radial_blur_px_slider_.get() });
        if (front_layer_light_strength_multiplier_slider_) rows.push_back({ front_layer_light_strength_multiplier_slider_.get() });
        if (behind_layer_light_strength_multiplier_slider_) rows.push_back({ behind_layer_light_strength_multiplier_slider_.get() });
    }

    if (debug_section_widget_) rows.push_back({ debug_section_widget_.get() });
    if (debug_section_expanded_) {
        if (max_cull_depth_slider_) rows.push_back({ max_cull_depth_slider_.get() });
        if (dynamic_renderer_depth_efficiency_depth_slider_) {
            rows.push_back({ dynamic_renderer_depth_efficiency_depth_slider_.get() });
        }
        if (dynamic_renderer_depth_efficiency_min_density_ratio_slider_) {
            rows.push_back({ dynamic_renderer_depth_efficiency_min_density_ratio_slider_.get() });
        }
        if (layer_depth_interval_slider_) rows.push_back({ layer_depth_interval_slider_.get() });
        if (layer_depth_curve_slider_) rows.push_back({ layer_depth_curve_slider_.get() });
    }

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
    if (dynamic_renderer_depth_efficiency_depth_slider_) {
        updated.dynamic_renderer_depth_efficiency_depth = std::clamp(
            dynamic_renderer_depth_efficiency_depth_slider_->value(),
            0.0f,
            std::max(1.0f, updated.max_cull_depth));
        dynamic_renderer_depth_efficiency_depth_slider_->set_range(0.0f, std::max(1.0f, updated.max_cull_depth));
    }
    if (dynamic_renderer_depth_efficiency_min_density_ratio_slider_) {
        updated.dynamic_renderer_depth_efficiency_min_density_ratio =
            dynamic_renderer_depth_efficiency_min_density_ratio_slider_->value();
    }
    if (layer_depth_interval_slider_) updated.layer_depth_interval = layer_depth_interval_slider_->value();
    if (layer_depth_curve_slider_) updated.layer_depth_curve = layer_depth_curve_slider_->value();
    if (front_layer_light_strength_multiplier_slider_) {
        updated.front_layer_light_strength_multiplier = front_layer_light_strength_multiplier_slider_->value();
    }
    if (behind_layer_light_strength_multiplier_slider_) {
        updated.behind_layer_light_strength_multiplier = behind_layer_light_strength_multiplier_slider_->value();
    }
    if (blur_px_slider_) updated.blur_px = blur_px_slider_->value();
    if (radial_blur_px_slider_) updated.radial_blur_px = radial_blur_px_slider_->value();
    if (depth_of_field_checkbox_) updated.depth_of_field_enabled = depth_of_field_checkbox_->value();

    auto float_changed = [](float a, float b, float eps = 1e-5f) {
        return std::fabs(a - b) > eps;
    };
    const bool realism_changed =
        float_changed(updated.min_visible_screen_ratio, current.min_visible_screen_ratio) ||
        float_changed(updated.max_cull_depth, current.max_cull_depth) ||
        float_changed(updated.dynamic_renderer_depth_efficiency_depth,
                      current.dynamic_renderer_depth_efficiency_depth) ||
        float_changed(updated.dynamic_renderer_depth_efficiency_min_density_ratio,
                      current.dynamic_renderer_depth_efficiency_min_density_ratio) ||
        float_changed(updated.layer_depth_interval, current.layer_depth_interval) ||
        float_changed(updated.layer_depth_curve, current.layer_depth_curve) ||
        float_changed(updated.front_layer_light_strength_multiplier, current.front_layer_light_strength_multiplier) ||
        float_changed(updated.behind_layer_light_strength_multiplier, current.behind_layer_light_strength_multiplier) ||
        float_changed(updated.blur_px, current.blur_px) ||
        float_changed(updated.radial_blur_px, current.radial_blur_px) ||
        (updated.depth_of_field_enabled != current.depth_of_field_enabled);

    float boundary_value = assets_->boundary_min_visible_screen_ratio();
    if (boundary_min_render_size_slider_) {
        boundary_value = boundary_min_render_size_slider_->value();
    }
    const bool boundary_changed =
        boundary_min_render_size_slider_ &&
        float_changed(boundary_value, assets_->boundary_min_visible_screen_ratio());

    bool changed = false;
    if (realism_changed) {
        cam.set_realism_settings(updated);
        changed = true;
    }
    if (boundary_changed) {
        assets_->set_boundary_min_visible_screen_ratio(boundary_value);
        changed = true;
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
            changed = true;

            // Clamp all room camera heights
            clamp_all_room_camera_heights(clamped_min, clamped_max);
        }
    }

    if (changed && dirty_callback_) {
        dirty_callback_();
    }
}

