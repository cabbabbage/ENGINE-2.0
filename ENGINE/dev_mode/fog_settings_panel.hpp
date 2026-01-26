#pragma once

#include "DockableCollapsible.hpp"
#include "float_slider_widget.hpp"
#include "render/dynamic_fog_system.hpp"

#include <memory>

class FogSettingsPanel : public DockableCollapsible {
public:
    FogSettingsPanel();
    ~FogSettingsPanel() override = default;

    void build() override;

    void set_grid_spacing_multiplier(float multiplier);
    void set_base_size_scale(float scale);
    void set_vertical_offset(float offset);
    void set_max_random_jitter(float jitter);

private:
    std::unique_ptr<FloatSliderWidget> grid_spacing_slider_;
    std::unique_ptr<FloatSliderWidget> base_scale_slider_;
    std::unique_ptr<FloatSliderWidget> vertical_offset_slider_;
    std::unique_ptr<FloatSliderWidget> max_random_jitter_slider_;
};
