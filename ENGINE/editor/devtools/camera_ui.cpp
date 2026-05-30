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


class LabelWidget : public Widget {
public:
    explicit LabelWidget(std::string text) : text_(std::move(text)) {}
    void set_rect(const SDL_Rect& r) override { rect_ = r; }
    const SDL_Rect& rect() const override { return rect_; }
    int height_for_width(int) const override { return 22; }
    bool handle_event(const SDL_Event&) override { return false; }
    void render(SDL_Renderer* renderer) const override {
        if (!renderer) return;
        SDL_SetRenderDrawColor(renderer, 32, 35, 46, 220);
        sdl_render::FillRect(renderer, &rect_);
        SDL_Color color{210, 218, 235, 255};
        auto style = DMStyles::Label();
        style.color = color;
        DrawLabelText(renderer, text_, rect_.x + 8, rect_.y + 3, style);
    }
    bool wants_full_row() const override { return true; }
private:
    std::string text_;
    SDL_Rect rect_{0, 0, 0, 0};
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
        return std::string(expanded ? "▾ " : "▸ ") + title_;
    }

private:
    std::string title_;
    std::function<bool()> expanded_getter_;
    std::function<void()> on_toggle_;
    DMButton button_;
};


CameraUIPanel::CameraUIPanel(Assets* assets, int x, int y)
    : DockableCollapsible("Camera", false, x, y),
      assets_(assets) {
    movement_section_expanded_ = true;
    framing_section_expanded_ = true;
    lens_section_expanded_ = true;
    rendering_section_expanded_ = true;
    debug_section_expanded_ = false;
    set_expanded(true);
    set_visible(false);
    set_padding(16);
    set_close_button_enabled(true);
    set_close_button_on_left(false);
    set_floatable(false);
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
    if (is_visible()) {
        close();
    } else {
        open();
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
    (void)screen_h;
    constexpr int kRightDockMargin = 12;
    const SDL_Rect r = rect();
    const int dock_x = std::max(0, screen_w - r.w - kRightDockMargin);
    const int dock_y = DMSpacing::panel_padding();
    const_cast<CameraUIPanel*>(this)->set_position_from_layout_manager(dock_x, dock_y);
    set_drag_handle_rect(SDL_Rect{0,0,0,0});
}

void CameraUIPanel::sync_from_camera() {
    if (!assets_) return;
    WarpedScreenGrid& cam = assets_->getView();
    const auto& settings = cam.get_settings();

    if (min_render_size_slider_) min_render_size_slider_->set_value(settings.min_visible_screen_ratio);
    sync_debug_controls_from_settings(settings);
    if (aperture_slider_) aperture_slider_->set_value(settings.aperture);
    if (radial_blur_px_slider_) radial_blur_px_slider_->set_value(settings.radial_blur_px);
    if (depth_of_field_checkbox_) {
        depth_of_field_checkbox_->set_value(settings.depth_of_field_enabled);
    }
    if (boundary_min_render_size_slider_) {
        boundary_min_render_size_slider_->set_value(assets_->boundary_min_visible_screen_ratio());
    }
    if (distance_from_edge_slider_) {
        distance_from_edge_slider_->set_value(assets_->live_dynamic_max_spawn_from_room_px());
    }

    // Sync camera height bounds
    const auto [saved_min, saved_max] = load_camera_height_bounds();
    if (camera_height_min_slider_) camera_height_min_slider_->set_value(saved_min);
    if (camera_height_max_slider_) camera_height_max_slider_->set_value(saved_max);

    // Initialize the global camera height bounds used by DevCameraControls
    DevCameraHeightBounds::set(static_cast<double>(saved_min), static_cast<double>(saved_max));
}

void CameraUIPanel::sync_debug_controls_from_settings(const WarpedScreenGrid::RealismSettings& settings) {
    if (max_cull_depth_slider_) max_cull_depth_slider_->set_value(settings.max_cull_depth);
    if (dynamic_renderer_depth_efficiency_depth_slider_) {
        dynamic_renderer_depth_efficiency_depth_slider_->set_range(0.0f, settings.max_cull_depth);
        dynamic_renderer_depth_efficiency_depth_slider_->set_value(
            settings.dynamic_renderer_depth_efficiency_depth);
    }
    if (layer_depth_interval_slider_) layer_depth_interval_slider_->set_value(settings.layer_depth_interval);
    if (layer_depth_falloff_slider_) layer_depth_falloff_slider_->set_value(settings.layer_depth_curve);
    if (near_fog_distance_slider_ && assets_) {
        near_fog_distance_slider_->set_value(assets_->live_dynamic_fog_near_distance_px());
    }
    const auto& lens = settings.lens;
    if (focus_depth_offset_slider_) focus_depth_offset_slider_->set_value(lens.focus_depth_offset);
    if (focus_falloff_acceleration_slider_) focus_falloff_acceleration_slider_->set_value(lens.focus_falloff_acceleration);
    if (max_near_blur_px_slider_) max_near_blur_px_slider_->set_value(lens.max_near_blur_px);
    if (max_far_blur_px_slider_) max_far_blur_px_slider_->set_value(lens.max_far_blur_px);
    if (near_far_blur_bias_slider_) near_far_blur_bias_slider_->set_value(lens.near_far_blur_bias);
    if (field_curvature_slider_) field_curvature_slider_->set_value(lens.field_curvature);
    if (edge_softness_slider_) edge_softness_slider_->set_value(lens.edge_softness);
    if (swirl_strength_slider_) swirl_strength_slider_->set_value(lens.swirl_strength);
    if (swirl_radius_start_slider_) swirl_radius_start_slider_->set_value(lens.swirl_radius_start);
    if (tangential_blur_stretch_slider_) tangential_blur_stretch_slider_->set_value(lens.tangential_blur_stretch);
    if (anamorphic_strength_slider_) anamorphic_strength_slider_->set_value(lens.anamorphic_strength);
    if (bokeh_oval_ratio_slider_) bokeh_oval_ratio_slider_->set_value(lens.bokeh_oval_ratio);
    if (bokeh_rotation_slider_) bokeh_rotation_slider_->set_value(lens.bokeh_rotation);
    if (vignette_strength_slider_) vignette_strength_slider_->set_value(lens.vignette_strength);
    if (vignette_radius_slider_) vignette_radius_slider_->set_value(lens.vignette_radius);
    if (vignette_softness_slider_) vignette_softness_slider_->set_value(lens.vignette_softness);
    if (barrel_distortion_slider_) barrel_distortion_slider_->set_value(lens.barrel_distortion);
    if (distortion_zoom_compensation_slider_) distortion_zoom_compensation_slider_->set_value(lens.distortion_zoom_compensation);
    if (chromatic_aberration_slider_) chromatic_aberration_slider_->set_value(lens.chromatic_aberration);
    if (chromatic_edge_start_slider_) chromatic_edge_start_slider_->set_value(lens.chromatic_edge_start);
    if (chromatic_depth_influence_slider_) chromatic_depth_influence_slider_->set_value(lens.chromatic_depth_influence);
    if (bloom_strength_slider_) bloom_strength_slider_->set_value(lens.bloom_strength);
    if (bloom_threshold_slider_) bloom_threshold_slider_->set_value(lens.bloom_threshold);
    if (bloom_radius_slider_) bloom_radius_slider_->set_value(lens.bloom_radius);
    if (halation_strength_slider_) halation_strength_slider_->set_value(lens.halation_strength);
    if (blur_padding_preview_slider_) blur_padding_preview_slider_->set_value(static_cast<float>(lens.blur_padding_px));
    if (alpha_debug_dropdown_) alpha_debug_dropdown_->set_selected(lens.alpha_debug_mode);
    if (alpha_clamp_protection_checkbox_) alpha_clamp_protection_checkbox_->set_value(lens.alpha_clamp_protection);
}

void CameraUIPanel::build_ui() {
    set_header_button_style(&DMStyles::AccentButton());
    set_header_highlight_color(DMStyles::AccentButton().bg);
    set_padding(DMSpacing::panel_padding());
    set_row_gap(DMSpacing::item_gap());
    set_col_gap(DMSpacing::item_gap());
    set_floating_content_width(420);

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
        "Live Dynamic Min On-Screen Size",
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
        "World depth where dynamic assets switch from full-update to paused+fogged behavior.");
    dynamic_renderer_depth_efficiency_depth_slider_->set_on_value_changed(
        [this](float) { on_control_value_changed(); });
    layer_depth_interval_slider_ = std::make_unique<FloatSliderWidget>(
        "Layer Interval",
        1.0f,
        5000.0f,
        1.0f,
        defaults.layer_depth_interval,
        0);
    layer_depth_interval_slider_->set_tooltip("Base world-depth step for DoF bins. Lower values increase near-depth detail.");
    layer_depth_interval_slider_->set_on_value_changed([this](float) { on_control_value_changed(); });
    layer_depth_falloff_slider_ = std::make_unique<FloatSliderWidget>(
        "Layer Falloff",
        0.0f,
        100.0f,
        0.01f,
        defaults.layer_depth_curve,
        2);
    layer_depth_falloff_slider_->set_tooltip("Non-linear DoF bin growth with distance. Higher values create fewer far-depth layers.");
    layer_depth_falloff_slider_->set_on_value_changed([this](float) { on_control_value_changed(); });
    near_fog_distance_slider_ = std::make_unique<DMSlider>(
        "Near Fog (px)",
        1,
        1000000,
        assets_ ? assets_->live_dynamic_fog_near_distance_px() : 64);
    near_fog_distance_slider_->set_on_value_changed([this](int) { on_control_value_changed(); });
    near_fog_distance_widget_ = std::make_unique<SliderWidget>(near_fog_distance_slider_.get());
    near_fog_distance_widget_->set_tooltip(
        "Dynamic asset fog attenuation starts from this depth (px) in the paused+fogged band.");
    distance_from_edge_slider_ = std::make_unique<DMSlider>(
        "Distance From Edge (px)",
        0,
        20000,
        assets_ ? assets_->live_dynamic_max_spawn_from_room_px() : 128);
    distance_from_edge_slider_->set_on_value_changed([this](int) { on_control_value_changed(); });
    distance_from_edge_widget_ = std::make_unique<SliderWidget>(distance_from_edge_slider_.get());
    distance_from_edge_widget_->set_tooltip("Maximum dynamic spawn distance from room boundary edges.");
    aperture_slider_ = std::make_unique<FloatSliderWidget>("Aperture", 0.1f, 8.0f, 0.01f, defaults.aperture, 2);
    aperture_slider_->set_tooltip("Controls how aggressively DoF blur ramps as layers move away from the focus layer.");
    aperture_slider_->set_on_value_changed([this](float) { on_control_value_changed(); });
    radial_blur_px_slider_ = std::make_unique<FloatSliderWidget>(
        "Radial Blur (px)",
        0.0f,
        256.0f,
        0.01f,
        defaults.radial_blur_px,
        3);
    radial_blur_px_slider_->set_tooltip("Additional radial blur from the optical center. Higher values can add cinematic depth but increase post-process work.");
    radial_blur_px_slider_->set_on_value_changed([this](float) { on_control_value_changed(); });
    auto dof_checkbox = std::make_unique<DMCheckbox>("Enable Depth Effects", defaults.depth_of_field_enabled);
    depth_of_field_checkbox_ = dof_checkbox.get();
    depth_of_field_widget_ = std::make_unique<CallbackCheckboxWidget>(
        std::move(dof_checkbox),
        [this](bool) { on_control_value_changed(); });

    auto make_lens_slider = [this](std::unique_ptr<FloatSliderWidget>& target,
                                   const char* label,
                                   float min_value,
                                   float max_value,
                                   float step,
                                   float value,
                                   int decimals) {
        target = std::make_unique<FloatSliderWidget>(label, min_value, max_value, step, value, decimals);
        target->set_on_value_changed([this](float) { on_control_value_changed(); });
    };
    const auto& lens_defaults = defaults.lens;
    make_lens_slider(focus_depth_offset_slider_, "Focus Depth Offset", -512.0f, 512.0f, 1.0f, lens_defaults.focus_depth_offset, 0);
    make_lens_slider(focus_falloff_acceleration_slider_, "Focus Falloff Accel", 0.5f, 4.0f, 0.01f, lens_defaults.focus_falloff_acceleration, 2);
    make_lens_slider(max_near_blur_px_slider_, "Max Near Blur (px)", 0.0f, 128.0f, 0.5f, lens_defaults.max_near_blur_px, 1);
    make_lens_slider(max_far_blur_px_slider_, "Max Far Blur (px)", 0.0f, 256.0f, 0.5f, lens_defaults.max_far_blur_px, 1);
    make_lens_slider(near_far_blur_bias_slider_, "Near / Far Blur Bias", -1.0f, 1.0f, 0.01f, lens_defaults.near_far_blur_bias, 2);
    make_lens_slider(field_curvature_slider_, "Field Curvature", -4.0f, 4.0f, 0.01f, lens_defaults.field_curvature, 2);
    make_lens_slider(edge_softness_slider_, "Edge Softness", 0.0f, 1.0f, 0.01f, lens_defaults.edge_softness, 2);
    make_lens_slider(swirl_strength_slider_, "Swirl Strength", 0.0f, 2.0f, 0.01f, lens_defaults.swirl_strength, 2);
    make_lens_slider(swirl_radius_start_slider_, "Swirl Radius Start", 0.0f, 1.0f, 0.01f, lens_defaults.swirl_radius_start, 2);
    make_lens_slider(tangential_blur_stretch_slider_, "Tangential Blur Stretch", 0.0f, 4.0f, 0.01f, lens_defaults.tangential_blur_stretch, 2);
    make_lens_slider(anamorphic_strength_slider_, "Anamorphic Strength", 0.0f, 2.0f, 0.01f, lens_defaults.anamorphic_strength, 2);
    make_lens_slider(bokeh_oval_ratio_slider_, "Bokeh Oval Ratio", 1.0f, 4.0f, 0.01f, lens_defaults.bokeh_oval_ratio, 2);
    make_lens_slider(bokeh_rotation_slider_, "Bokeh Rotation", -180.0f, 180.0f, 1.0f, lens_defaults.bokeh_rotation, 0);
    make_lens_slider(vignette_strength_slider_, "Vignette Strength", 0.0f, 1.0f, 0.01f, lens_defaults.vignette_strength, 2);
    make_lens_slider(vignette_radius_slider_, "Vignette Radius", 0.0f, 1.0f, 0.01f, lens_defaults.vignette_radius, 2);
    make_lens_slider(vignette_softness_slider_, "Vignette Softness", 0.001f, 1.0f, 0.001f, lens_defaults.vignette_softness, 3);
    make_lens_slider(barrel_distortion_slider_, "Barrel Distortion", -0.5f, 0.5f, 0.001f, lens_defaults.barrel_distortion, 3);
    make_lens_slider(distortion_zoom_compensation_slider_, "Distortion Zoom Comp", 0.0f, 1.5f, 0.001f, lens_defaults.distortion_zoom_compensation, 3);
    make_lens_slider(chromatic_aberration_slider_, "Chromatic Aberration", 0.0f, 8.0f, 0.05f, lens_defaults.chromatic_aberration, 2);
    make_lens_slider(chromatic_edge_start_slider_, "Chromatic Edge Start", 0.0f, 1.0f, 0.01f, lens_defaults.chromatic_edge_start, 2);
    make_lens_slider(chromatic_depth_influence_slider_, "Chromatic Depth Influence", 0.0f, 1.0f, 0.01f, lens_defaults.chromatic_depth_influence, 2);
    make_lens_slider(bloom_strength_slider_, "Bloom Strength", 0.0f, 1.0f, 0.01f, lens_defaults.bloom_strength, 2);
    make_lens_slider(bloom_threshold_slider_, "Bloom Threshold", 0.0f, 1.0f, 0.01f, lens_defaults.bloom_threshold, 2);
    make_lens_slider(bloom_radius_slider_, "Bloom Radius", 0.0f, 1.0f, 0.01f, lens_defaults.bloom_radius, 2);
    make_lens_slider(halation_strength_slider_, "Halation Strength", 0.0f, 1.0f, 0.01f, lens_defaults.halation_strength, 2);
    make_lens_slider(blur_padding_preview_slider_, "Blur Padding Preview", 0.0f, 256.0f, 1.0f, static_cast<float>(lens_defaults.blur_padding_px), 0);

    alpha_debug_dropdown_ = std::make_unique<DMDropdown>(
        "Alpha / Smear Debug",
        std::vector<std::string>{
            "Off",
            "Source Alpha",
            "Accumulated Blur Alpha",
            "Final Blurred Output",
            "Alpha Clamp Compare",
            "Blur Padding Preview",
            "Focus Debug",
            "Depth-Layer Debug",
            "Blur Amount Debug",
            "Final Lens Contribution"},
        lens_defaults.alpha_debug_mode);
    alpha_debug_dropdown_->set_on_selection_changed([this](int) { on_control_value_changed(); });
    alpha_debug_widget_ = std::make_unique<DropdownWidget>(alpha_debug_dropdown_.get());

    auto clamp_checkbox = std::make_unique<DMCheckbox>("Alpha Clamp Protection", lens_defaults.alpha_clamp_protection);
    alpha_clamp_protection_checkbox_ = clamp_checkbox.get();
    alpha_clamp_protection_widget_ = std::make_unique<CallbackCheckboxWidget>(
        std::move(clamp_checkbox),
        [this](bool) { on_control_value_changed(); });

    reset_lens_defaults_button_ = std::make_unique<DMButton>("Reset Lens Defaults", &DMStyles::SecondaryButton(), 1, DMButton::height());
    reset_lens_defaults_widget_ = std::make_unique<ButtonWidget>(reset_lens_defaults_button_.get(), [this]() {
        WarpedScreenGrid::RealismSettings defaults;
        sync_debug_controls_from_settings(defaults);
        if (aperture_slider_) aperture_slider_->set_value(defaults.lens.aperture);
        if (depth_of_field_checkbox_) depth_of_field_checkbox_->set_value(defaults.depth_of_field_enabled);
        on_control_value_changed();
    });

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
    lens_section_widget_ = std::make_unique<SectionToggleWidget>(
        "Lens",
        [this]() { return lens_section_expanded_; },
        [this]() {
            lens_section_expanded_ = !lens_section_expanded_;
            rebuild_rows();
        });
    rendering_section_widget_ = std::make_unique<SectionToggleWidget>(
        "Rendering",
        [this]() { return rendering_section_expanded_; },
        [this]() {
            rendering_section_expanded_ = !rendering_section_expanded_;
            rebuild_rows();
        });
    debug_section_widget_ = std::make_unique<SectionToggleWidget>(
        "Debug",
        [this]() { return debug_section_expanded_; },
        [this]() {
            debug_section_expanded_ = !debug_section_expanded_;
            rebuild_rows();
        });

    lens_label_widgets_.clear();
    for (const char* label : {
             "Camera",
             "Lens",
             "Anamorphic Character",
             "Image Falloff",
             "Color Fringe",
             "Highlight Response",
             "Alpha / Smear Debug"}) {
        lens_label_widgets_.push_back(std::make_unique<LabelWidget>(label));
    }

    rebuild_rows();
}

void CameraUIPanel::sync_runtime_lighting_state_with_visibility() {
    if (!assets_) {
        return;
    }
    assets_->set_camera_settings_panel_active(is_visible());
}

void CameraUIPanel::on_control_value_changed() {
    if (!assets_ || !is_visible() || suppress_apply_once_) {
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
        if (controls_spacer_) rows.push_back({ controls_spacer_.get() });
    }

    if (framing_section_widget_) rows.push_back({ framing_section_widget_.get() });
    if (framing_section_expanded_) {
        if (min_render_size_slider_) rows.push_back({ min_render_size_slider_.get() });
        if (boundary_min_render_size_slider_) rows.push_back({ boundary_min_render_size_slider_.get() });
        if (controls_spacer_) rows.push_back({ controls_spacer_.get() });
    }

    if (lens_section_widget_) rows.push_back({ lens_section_widget_.get() });
    if (lens_section_expanded_) {
        auto push_label = [&](std::size_t index) {
            if (index < lens_label_widgets_.size()) rows.push_back({ lens_label_widgets_[index].get() });
        };
        push_label(0);
        if (depth_of_field_widget_) rows.push_back({ depth_of_field_widget_.get() });
        if (focus_depth_offset_slider_) rows.push_back({ focus_depth_offset_slider_.get() });
        if (reset_lens_defaults_widget_) rows.push_back({ reset_lens_defaults_widget_.get() });
        push_label(1);
        if (aperture_slider_) rows.push_back({ aperture_slider_.get() });
        if (focus_falloff_acceleration_slider_) rows.push_back({ focus_falloff_acceleration_slider_.get() });
        if (max_near_blur_px_slider_) rows.push_back({ max_near_blur_px_slider_.get() });
        if (max_far_blur_px_slider_) rows.push_back({ max_far_blur_px_slider_.get() });
        if (near_far_blur_bias_slider_) rows.push_back({ near_far_blur_bias_slider_.get() });
        if (field_curvature_slider_) rows.push_back({ field_curvature_slider_.get() });
        if (edge_softness_slider_) rows.push_back({ edge_softness_slider_.get() });
        if (layer_depth_interval_slider_) rows.push_back({ layer_depth_interval_slider_.get() });
        if (layer_depth_falloff_slider_) rows.push_back({ layer_depth_falloff_slider_.get() });
        push_label(2);
        if (swirl_strength_slider_) rows.push_back({ swirl_strength_slider_.get() });
        if (swirl_radius_start_slider_) rows.push_back({ swirl_radius_start_slider_.get() });
        if (tangential_blur_stretch_slider_) rows.push_back({ tangential_blur_stretch_slider_.get() });
        if (anamorphic_strength_slider_) rows.push_back({ anamorphic_strength_slider_.get() });
        if (bokeh_oval_ratio_slider_) rows.push_back({ bokeh_oval_ratio_slider_.get() });
        if (bokeh_rotation_slider_) rows.push_back({ bokeh_rotation_slider_.get() });
        push_label(3);
        if (vignette_strength_slider_) rows.push_back({ vignette_strength_slider_.get() });
        if (vignette_radius_slider_) rows.push_back({ vignette_radius_slider_.get() });
        if (vignette_softness_slider_) rows.push_back({ vignette_softness_slider_.get() });
        if (barrel_distortion_slider_) rows.push_back({ barrel_distortion_slider_.get() });
        if (distortion_zoom_compensation_slider_) rows.push_back({ distortion_zoom_compensation_slider_.get() });
        push_label(4);
        if (chromatic_aberration_slider_) rows.push_back({ chromatic_aberration_slider_.get() });
        if (chromatic_edge_start_slider_) rows.push_back({ chromatic_edge_start_slider_.get() });
        if (chromatic_depth_influence_slider_) rows.push_back({ chromatic_depth_influence_slider_.get() });
        push_label(5);
        if (bloom_strength_slider_) rows.push_back({ bloom_strength_slider_.get() });
        if (bloom_threshold_slider_) rows.push_back({ bloom_threshold_slider_.get() });
        if (bloom_radius_slider_) rows.push_back({ bloom_radius_slider_.get() });
        if (halation_strength_slider_) rows.push_back({ halation_strength_slider_.get() });
        push_label(6);
        if (alpha_debug_widget_) rows.push_back({ alpha_debug_widget_.get() });
        if (alpha_clamp_protection_widget_) rows.push_back({ alpha_clamp_protection_widget_.get() });
        if (blur_padding_preview_slider_) rows.push_back({ blur_padding_preview_slider_.get() });
        if (controls_spacer_) rows.push_back({ controls_spacer_.get() });
    }

    if (rendering_section_widget_) rows.push_back({ rendering_section_widget_.get() });
    if (rendering_section_expanded_) {
        if (distance_from_edge_widget_) rows.push_back({ distance_from_edge_widget_.get() });
        if (near_fog_distance_widget_) rows.push_back({ near_fog_distance_widget_.get() });
        if (controls_spacer_) rows.push_back({ controls_spacer_.get() });
    }

    if (debug_section_widget_) rows.push_back({ debug_section_widget_.get() });
    if (debug_section_expanded_) {
        if (max_cull_depth_slider_) rows.push_back({ max_cull_depth_slider_.get() });
        if (dynamic_renderer_depth_efficiency_depth_slider_) {
            rows.push_back({ dynamic_renderer_depth_efficiency_depth_slider_.get() });
        }
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
    if (layer_depth_interval_slider_) updated.layer_depth_interval = layer_depth_interval_slider_->value();
    if (layer_depth_falloff_slider_) updated.layer_depth_curve = layer_depth_falloff_slider_->value();
    if (depth_of_field_checkbox_) updated.depth_of_field_enabled = depth_of_field_checkbox_->value();
    updated.lens.enabled = updated.depth_of_field_enabled;
    if (aperture_slider_) updated.lens.aperture = aperture_slider_->value();
    if (focus_depth_offset_slider_) updated.lens.focus_depth_offset = focus_depth_offset_slider_->value();
    if (focus_falloff_acceleration_slider_) updated.lens.focus_falloff_acceleration = focus_falloff_acceleration_slider_->value();
    if (max_near_blur_px_slider_) updated.lens.max_near_blur_px = max_near_blur_px_slider_->value();
    if (max_far_blur_px_slider_) updated.lens.max_far_blur_px = max_far_blur_px_slider_->value();
    if (near_far_blur_bias_slider_) updated.lens.near_far_blur_bias = near_far_blur_bias_slider_->value();
    if (field_curvature_slider_) updated.lens.field_curvature = field_curvature_slider_->value();
    if (edge_softness_slider_) updated.lens.edge_softness = edge_softness_slider_->value();
    if (swirl_strength_slider_) updated.lens.swirl_strength = swirl_strength_slider_->value();
    if (swirl_radius_start_slider_) updated.lens.swirl_radius_start = swirl_radius_start_slider_->value();
    if (tangential_blur_stretch_slider_) updated.lens.tangential_blur_stretch = tangential_blur_stretch_slider_->value();
    if (anamorphic_strength_slider_) updated.lens.anamorphic_strength = anamorphic_strength_slider_->value();
    if (bokeh_oval_ratio_slider_) updated.lens.bokeh_oval_ratio = bokeh_oval_ratio_slider_->value();
    if (bokeh_rotation_slider_) updated.lens.bokeh_rotation = bokeh_rotation_slider_->value();
    if (vignette_strength_slider_) updated.lens.vignette_strength = vignette_strength_slider_->value();
    if (vignette_radius_slider_) updated.lens.vignette_radius = vignette_radius_slider_->value();
    if (vignette_softness_slider_) updated.lens.vignette_softness = vignette_softness_slider_->value();
    if (barrel_distortion_slider_) updated.lens.barrel_distortion = barrel_distortion_slider_->value();
    if (distortion_zoom_compensation_slider_) updated.lens.distortion_zoom_compensation = distortion_zoom_compensation_slider_->value();
    if (chromatic_aberration_slider_) updated.lens.chromatic_aberration = chromatic_aberration_slider_->value();
    if (chromatic_edge_start_slider_) updated.lens.chromatic_edge_start = chromatic_edge_start_slider_->value();
    if (chromatic_depth_influence_slider_) updated.lens.chromatic_depth_influence = chromatic_depth_influence_slider_->value();
    if (bloom_strength_slider_) updated.lens.bloom_strength = bloom_strength_slider_->value();
    if (bloom_threshold_slider_) updated.lens.bloom_threshold = bloom_threshold_slider_->value();
    if (bloom_radius_slider_) updated.lens.bloom_radius = bloom_radius_slider_->value();
    if (halation_strength_slider_) updated.lens.halation_strength = halation_strength_slider_->value();
    if (alpha_debug_dropdown_) updated.lens.alpha_debug_mode = alpha_debug_dropdown_->selected();
    if (blur_padding_preview_slider_) {
        updated.lens.blur_padding_px = static_cast<int>(std::lround(blur_padding_preview_slider_->value()));
    }
    if (alpha_clamp_protection_checkbox_) updated.lens.alpha_clamp_protection = alpha_clamp_protection_checkbox_->value();

    auto float_changed = [](float a, float b, float eps = 1e-5f) {
        return std::fabs(a - b) > eps;
    };
    auto lens_changed = [&](const auto& a, const auto& b) {
        return (a.enabled != b.enabled) ||
            float_changed(a.focus_depth_offset, b.focus_depth_offset) ||
            float_changed(a.aperture, b.aperture) ||
            float_changed(a.focus_falloff_acceleration, b.focus_falloff_acceleration) ||
            float_changed(a.max_near_blur_px, b.max_near_blur_px) ||
            float_changed(a.max_far_blur_px, b.max_far_blur_px) ||
            float_changed(a.near_far_blur_bias, b.near_far_blur_bias) ||
            float_changed(a.field_curvature, b.field_curvature) ||
            float_changed(a.edge_softness, b.edge_softness) ||
            float_changed(a.swirl_strength, b.swirl_strength) ||
            float_changed(a.swirl_radius_start, b.swirl_radius_start) ||
            float_changed(a.tangential_blur_stretch, b.tangential_blur_stretch) ||
            float_changed(a.anamorphic_strength, b.anamorphic_strength) ||
            float_changed(a.bokeh_oval_ratio, b.bokeh_oval_ratio) ||
            float_changed(a.bokeh_rotation, b.bokeh_rotation) ||
            float_changed(a.vignette_strength, b.vignette_strength) ||
            float_changed(a.vignette_radius, b.vignette_radius) ||
            float_changed(a.vignette_softness, b.vignette_softness) ||
            float_changed(a.barrel_distortion, b.barrel_distortion) ||
            float_changed(a.distortion_zoom_compensation, b.distortion_zoom_compensation) ||
            float_changed(a.chromatic_aberration, b.chromatic_aberration) ||
            float_changed(a.chromatic_edge_start, b.chromatic_edge_start) ||
            float_changed(a.chromatic_depth_influence, b.chromatic_depth_influence) ||
            float_changed(a.bloom_strength, b.bloom_strength) ||
            float_changed(a.bloom_threshold, b.bloom_threshold) ||
            float_changed(a.bloom_radius, b.bloom_radius) ||
            float_changed(a.halation_strength, b.halation_strength) ||
            (a.alpha_debug_mode != b.alpha_debug_mode) ||
            (a.alpha_clamp_protection != b.alpha_clamp_protection) ||
            (a.blur_padding_px != b.blur_padding_px) ||
            (a.sample_count != b.sample_count) ||
            float_changed(a.downsample_scale, b.downsample_scale) ||
            (a.quality_preset != b.quality_preset);
    };
    const bool realism_changed =
        float_changed(updated.min_visible_screen_ratio, current.min_visible_screen_ratio) ||
        float_changed(updated.max_cull_depth, current.max_cull_depth) ||
        float_changed(updated.dynamic_renderer_depth_efficiency_depth,
                      current.dynamic_renderer_depth_efficiency_depth) ||
        float_changed(updated.layer_depth_interval, current.layer_depth_interval) ||
        float_changed(updated.layer_depth_curve, current.layer_depth_curve) ||
        lens_changed(updated.lens, current.lens) ||
        (updated.depth_of_field_enabled != current.depth_of_field_enabled);

    float boundary_value = assets_->boundary_min_visible_screen_ratio();
    if (boundary_min_render_size_slider_) {
        boundary_value = boundary_min_render_size_slider_->value();
    }
    const bool boundary_changed =
        boundary_min_render_size_slider_ &&
        float_changed(boundary_value, assets_->boundary_min_visible_screen_ratio());
    const int near_fog_distance = near_fog_distance_slider_
        ? near_fog_distance_slider_->value()
        : assets_->live_dynamic_fog_near_distance_px();
    const bool near_fog_changed = near_fog_distance_slider_ &&
        near_fog_distance != assets_->live_dynamic_fog_near_distance_px();
    const int distance_from_edge = distance_from_edge_slider_
        ? distance_from_edge_slider_->value()
        : assets_->live_dynamic_max_spawn_from_room_px();
    const bool distance_from_edge_changed = distance_from_edge_slider_ &&
        distance_from_edge != assets_->live_dynamic_max_spawn_from_room_px();

    bool changed = false;
    if (realism_changed) {
        cam.set_realism_settings(updated);
        changed = true;
    }
    if (boundary_changed) {
        assets_->set_boundary_min_visible_screen_ratio(boundary_value);
        changed = true;
    }
    if (near_fog_changed) {
        assets_->set_live_dynamic_fog_near_distance_px(near_fog_distance);
        changed = true;
    }
    if (distance_from_edge_changed) {
        assets_->set_live_dynamic_max_spawn_from_room_px(distance_from_edge);
        assets_->notify_dynamic_spawn_distance_changed();
        changed = true;
    }

    if (realism_changed || boundary_changed || near_fog_changed || distance_from_edge_changed) {
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
