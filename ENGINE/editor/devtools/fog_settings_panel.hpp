#pragma once

#include "DockableCollapsible.hpp"
#include "float_slider_widget.hpp"
#include "rendering/render/dynamic_fog_system.hpp"

#include <memory>
#include <functional>
#include <nlohmann/json.hpp>

class FogSettingsPanel : public DockableCollapsible {
public:
    FogSettingsPanel();
    ~FogSettingsPanel() override = default;

    void build() override;

    void set_map_info(nlohmann::json* map_info, std::function<bool()> on_save);

    void set_grid_spacing_multiplier(float multiplier);
    void set_base_size_scale(float scale);
    void set_vertical_offset(float offset);
    void set_max_random_jitter(float jitter);

private:
    void refresh_from_map();
    void set_max_random_jitter_internal(float jitter, bool persist_dev_setting);
    nlohmann::json& ensure_fog_settings();

    std::unique_ptr<FloatSliderWidget> grid_spacing_slider_;
    std::unique_ptr<FloatSliderWidget> base_scale_slider_;
    std::unique_ptr<FloatSliderWidget> vertical_offset_slider_;
    std::unique_ptr<FloatSliderWidget> max_random_jitter_slider_;

    nlohmann::json* map_info_ = nullptr;
    std::function<bool()> on_save_{};
};
