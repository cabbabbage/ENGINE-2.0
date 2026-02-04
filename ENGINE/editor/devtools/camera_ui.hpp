#pragma once

#include <functional>
#include <memory>
#include "DockableCollapsible.hpp"
#include "rendering/render/warped_screen_grid.hpp"

class Assets;
class DMCheckbox;
class DMButton;
class Widget;
class ButtonWidget;
class CheckboxWidget;
class DMDropdown;
class DropdownWidget;
class Input;
class FloatSliderWidget;
class SectionToggleWidget;
class DiscreteSliderWidget;
class PitchDialWidget;


class CameraUIPanel : public DockableCollapsible {
public:
    explicit CameraUIPanel(Assets* assets, int x = 80, int y = 80);
    ~CameraUIPanel() override;

    void set_assets(Assets* assets);
    void set_image_effects_panel_callback(std::function<void()> cb);

    void open();
    void close();
    void toggle();
    bool is_point_inside(int x, int y) const;
    bool is_blur_section_visible() const { return is_visible() && depthcue_section_expanded_; }
    bool is_depth_section_visible() const { return is_visible() && depth_section_expanded_; }

    void update(const Input& input, int screen_w, int screen_h);
    bool handle_event(const SDL_Event& e);
    void render(SDL_Renderer* renderer) const;

    void sync_from_camera();



private:

    void build_ui();
    void rebuild_rows();
    void apply_settings_if_needed();

    void on_control_value_changed();

private:
    Assets* assets_ = nullptr;

    bool suppress_apply_once_ = false;
    bool was_visible_ = false;

    std::unique_ptr<Widget> header_spacer_;
    std::unique_ptr<Widget> controls_spacer_;
    std::unique_ptr<DMCheckbox> depthcue_checkbox_;
    std::unique_ptr<CheckboxWidget> depthcue_widget_;
    std::unique_ptr<SectionToggleWidget> visibility_section_header_;
    std::unique_ptr<SectionToggleWidget> depth_section_header_;
    std::unique_ptr<SectionToggleWidget> depthcue_section_header_;

    std::unique_ptr<FloatSliderWidget> min_render_size_slider_;
    std::unique_ptr<FloatSliderWidget> cull_margin_slider_;
    std::unique_ptr<FloatSliderWidget> meters_slider_;

    std::unique_ptr<DMButton> image_effect_button_;
    std::unique_ptr<ButtonWidget> image_effect_widget_;

    std::unique_ptr<DiscreteSliderWidget> render_quality_slider_;
    bool visibility_section_expanded_ = true;
    bool depth_section_expanded_ = true;
    bool depthcue_section_expanded_ = false;
    bool applying_settings_ = false;


    std::function<void()> open_image_effects_cb_;
    int last_screen_w_ = 0;
    int last_screen_h_ = 0;

protected:
    std::string_view lock_settings_namespace() const override { return "camera"; }
    std::string_view lock_settings_id() const override { return "controls"; }
    void layout_custom_content(int screen_w, int screen_h) const override;
};
