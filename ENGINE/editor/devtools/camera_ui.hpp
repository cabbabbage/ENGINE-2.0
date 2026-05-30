#pragma once

#include <functional>
#include <memory>
#include <vector>
#include "DockableCollapsible.hpp"
#include "rendering/render/warped_screen_grid.hpp"

class Assets;
class Widget;
class Input;
class FloatSliderWidget;
class PitchDialWidget;
class SliderWidget;
class DMSlider;
class DMCheckbox;
class CallbackCheckboxWidget;
class DMDropdown;
class DMButton;
class ButtonWidget;
class DropdownWidget;


class CameraUIPanel : public DockableCollapsible {
public:
    explicit CameraUIPanel(Assets* assets, int x = 80, int y = 80);
    ~CameraUIPanel() override;

    void set_assets(Assets* assets);

    void open();
    void close();
    void toggle();
    bool is_point_inside(int x, int y) const;

    void update(const Input& input, int screen_w, int screen_h);
    bool handle_event(const SDL_Event& e);
    void render(SDL_Renderer* renderer) const;

    void sync_from_camera();
    void sync_debug_controls_from_settings(const WarpedScreenGrid::RealismSettings& settings);
    void set_dirty_callback(std::function<void()> callback);
    bool is_debug_section_expanded() const { return debug_section_expanded_; }



private:

    void build_ui();
    void rebuild_rows();
    void apply_settings_if_needed();
    void sync_runtime_lighting_state_with_visibility();

    void on_control_value_changed();

    std::pair<int, int> load_camera_height_bounds() const;
    void save_camera_height_bounds(int min_val, int max_val) const;
    void clamp_all_room_camera_heights(int min_val, int max_val);

private:
    Assets* assets_ = nullptr;

    bool suppress_apply_once_ = false;
    bool was_visible_ = false;

    std::unique_ptr<Widget> header_spacer_;
    std::unique_ptr<Widget> controls_spacer_;

    std::unique_ptr<FloatSliderWidget> min_render_size_slider_;
    std::unique_ptr<FloatSliderWidget> boundary_min_render_size_slider_;
    std::unique_ptr<FloatSliderWidget> max_cull_depth_slider_;
    std::unique_ptr<FloatSliderWidget> dynamic_renderer_depth_efficiency_depth_slider_;
    std::unique_ptr<FloatSliderWidget> layer_depth_interval_slider_;
    std::unique_ptr<FloatSliderWidget> layer_depth_falloff_slider_;
    std::unique_ptr<FloatSliderWidget> aperture_slider_;
    std::unique_ptr<DMSlider> near_fog_distance_slider_;
    std::unique_ptr<SliderWidget> near_fog_distance_widget_;
    std::unique_ptr<FloatSliderWidget> radial_blur_px_slider_;
    std::unique_ptr<FloatSliderWidget> focus_depth_offset_slider_;
    std::unique_ptr<FloatSliderWidget> focus_falloff_acceleration_slider_;
    std::unique_ptr<FloatSliderWidget> max_near_blur_px_slider_;
    std::unique_ptr<FloatSliderWidget> max_far_blur_px_slider_;
    std::unique_ptr<FloatSliderWidget> near_far_blur_bias_slider_;
    std::unique_ptr<FloatSliderWidget> field_curvature_slider_;
    std::unique_ptr<FloatSliderWidget> edge_softness_slider_;
    std::unique_ptr<FloatSliderWidget> swirl_strength_slider_;
    std::unique_ptr<FloatSliderWidget> swirl_radius_start_slider_;
    std::unique_ptr<FloatSliderWidget> tangential_blur_stretch_slider_;
    std::unique_ptr<FloatSliderWidget> anamorphic_strength_slider_;
    std::unique_ptr<FloatSliderWidget> bokeh_oval_ratio_slider_;
    std::unique_ptr<FloatSliderWidget> bokeh_rotation_slider_;
    std::unique_ptr<FloatSliderWidget> vignette_strength_slider_;
    std::unique_ptr<FloatSliderWidget> vignette_radius_slider_;
    std::unique_ptr<FloatSliderWidget> vignette_softness_slider_;
    std::unique_ptr<FloatSliderWidget> barrel_distortion_slider_;
    std::unique_ptr<FloatSliderWidget> distortion_zoom_compensation_slider_;
    std::unique_ptr<FloatSliderWidget> chromatic_aberration_slider_;
    std::unique_ptr<FloatSliderWidget> chromatic_edge_start_slider_;
    std::unique_ptr<FloatSliderWidget> chromatic_depth_influence_slider_;
    std::unique_ptr<FloatSliderWidget> bloom_strength_slider_;
    std::unique_ptr<FloatSliderWidget> bloom_threshold_slider_;
    std::unique_ptr<FloatSliderWidget> bloom_radius_slider_;
    std::unique_ptr<FloatSliderWidget> halation_strength_slider_;
    std::unique_ptr<FloatSliderWidget> blur_padding_preview_slider_;
    std::unique_ptr<DMDropdown> alpha_debug_dropdown_;
    std::unique_ptr<DropdownWidget> alpha_debug_widget_;
    std::unique_ptr<DMButton> reset_lens_defaults_button_;
    std::unique_ptr<ButtonWidget> reset_lens_defaults_widget_;
    std::vector<std::unique_ptr<Widget>> lens_label_widgets_;
    std::unique_ptr<DMSlider> distance_from_edge_slider_;
    std::unique_ptr<SliderWidget> distance_from_edge_widget_;

    // Global camera height bounds
    std::unique_ptr<DMSlider> camera_height_min_slider_;
    std::unique_ptr<DMSlider> camera_height_max_slider_;
    std::unique_ptr<SliderWidget> camera_height_min_widget_;
    std::unique_ptr<SliderWidget> camera_height_max_widget_;
    std::unique_ptr<CallbackCheckboxWidget> depth_of_field_widget_;
    std::unique_ptr<CallbackCheckboxWidget> alpha_clamp_protection_widget_;
    DMCheckbox* depth_of_field_checkbox_ = nullptr;
    DMCheckbox* alpha_clamp_protection_checkbox_ = nullptr;
    std::unique_ptr<Widget> movement_section_widget_;
    std::unique_ptr<Widget> framing_section_widget_;
    std::unique_ptr<Widget> lens_section_widget_;
    std::unique_ptr<Widget> rendering_section_widget_;
    std::unique_ptr<Widget> debug_section_widget_;
    bool movement_section_expanded_ = false;
    bool framing_section_expanded_ = false;
    bool lens_section_expanded_ = false;
    bool rendering_section_expanded_ = false;
    bool debug_section_expanded_ = false;

    bool applying_settings_ = false;
    int last_screen_w_ = 0;
    int last_screen_h_ = 0;
    std::function<void()> dirty_callback_;

protected:
    std::string_view lock_settings_namespace() const override { return "camera"; }
    std::string_view lock_settings_id() const override { return "controls"; }
    void layout_custom_content(int screen_w, int screen_h) const override;
};
