#pragma once

#include <memory>
#include "DockableCollapsible.hpp"
#include "rendering/render/warped_screen_grid.hpp"

class Assets;
class Widget;
class Input;
class FloatSliderWidget;
class PitchDialWidget;
class SliderWidget;
class DMSlider;


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



private:

    void build_ui();
    void rebuild_rows();
    void apply_settings_if_needed();

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
    std::unique_ptr<FloatSliderWidget> layer_depth_interval_slider_;
    std::unique_ptr<FloatSliderWidget> layer_depth_curve_slider_;
    std::unique_ptr<FloatSliderWidget> aperture_f_stop_slider_;
    std::unique_ptr<FloatSliderWidget> focal_length_mm_slider_;
    std::unique_ptr<FloatSliderWidget> max_blur_px_slider_;

    // Global camera height bounds
    std::unique_ptr<DMSlider> camera_height_min_slider_;
    std::unique_ptr<DMSlider> camera_height_max_slider_;
    std::unique_ptr<SliderWidget> camera_height_min_widget_;
    std::unique_ptr<SliderWidget> camera_height_max_widget_;

    bool applying_settings_ = false;
    int last_screen_w_ = 0;
    int last_screen_h_ = 0;

protected:
    std::string_view lock_settings_namespace() const override { return "camera"; }
    std::string_view lock_settings_id() const override { return "controls"; }
    void layout_custom_content(int screen_w, int screen_h) const override;
};
