#pragma once

#include "DockableCollapsible.hpp"
#include "float_slider_widget.hpp"

#include <functional>
#include <memory>

#include <nlohmann/json_fwd.hpp>

class DMCheckbox;
class CheckboxWidget;
class DMTextBox;
class TextBoxWidget;
class DMButton;
class ButtonWidget;

class TerrainSettingsPanel : public DockableCollapsible {
public:
    TerrainSettingsPanel();
    ~TerrainSettingsPanel() override = default;

    void build() override;
    void set_map_info(nlohmann::json* map_info, std::function<bool()> on_save);

private:
    void refresh_from_map();
    void sync_widgets_from_state();
    void sync_state_from_widgets();
    void apply_current_values();
    void save_current_values();

    nlohmann::json* map_info_ = nullptr;
    std::function<bool()> on_save_{};

    bool enabled_ = true;
    bool lock_seed_to_world_ = true;

    std::unique_ptr<DMCheckbox> enabled_checkbox_;
    std::unique_ptr<CheckboxWidget> enabled_widget_;

    std::unique_ptr<FloatSliderWidget> max_elevation_slider_;
    std::unique_ptr<FloatSliderWidget> edge_falloff_slider_;
    std::unique_ptr<FloatSliderWidget> smoothness_slider_;
    std::unique_ptr<FloatSliderWidget> noise_variation_slider_;
    std::unique_ptr<FloatSliderWidget> roughness_slider_;
    std::unique_ptr<FloatSliderWidget> blend_strength_slider_;
    std::unique_ptr<FloatSliderWidget> resolution_density_slider_;

    std::unique_ptr<DMTextBox> base_seed_textbox_;
    std::unique_ptr<TextBoxWidget> base_seed_widget_;

    std::unique_ptr<DMCheckbox> lock_seed_checkbox_;
    std::unique_ptr<CheckboxWidget> lock_seed_widget_;

    std::unique_ptr<FloatSliderWidget> direction_x_slider_;
    std::unique_ptr<FloatSliderWidget> direction_y_slider_;
    std::unique_ptr<FloatSliderWidget> position_x_slider_;
    std::unique_ptr<FloatSliderWidget> position_y_slider_;
    std::unique_ptr<FloatSliderWidget> light_strength_slider_;
    std::unique_ptr<FloatSliderWidget> contrast_slider_;

    std::unique_ptr<DMButton> apply_button_;
    std::unique_ptr<ButtonWidget> apply_button_widget_;
    std::unique_ptr<DMButton> save_button_;
    std::unique_ptr<ButtonWidget> save_button_widget_;
};
