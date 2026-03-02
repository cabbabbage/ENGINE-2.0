#include "terrain_settings_panel.hpp"

#include "devtools/dm_styles.hpp"
#include "devtools/widgets.hpp"
#include "rendering/render/terrain_settings.hpp"
#include "rendering/render/terrain_settings_manifest.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <nlohmann/json.hpp>

namespace {

std::uint32_t parse_seed(const std::string& text, std::uint32_t fallback) {
    if (text.empty()) {
        return fallback;
    }
    const char* begin = text.c_str();
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(begin, &end, 10);
    if (begin == end || (end && *end != '\0') || errno == ERANGE) {
        return fallback;
    }
    const std::uint64_t raw = static_cast<std::uint64_t>(parsed);
    return static_cast<std::uint32_t>(raw & 0xffffffffu);
}

}

TerrainSettingsPanel::TerrainSettingsPanel()
    : DockableCollapsible("Terrain Settings", /*floatable=*/true, 460, 110) {
    set_close_button_enabled(true);
    set_scroll_enabled(true);
    set_visible_height(520);
}

void TerrainSettingsPanel::build() {
    enabled_checkbox_ = std::make_unique<DMCheckbox>("Enable Terrain", true);
    enabled_widget_ = std::make_unique<CheckboxWidget>(enabled_checkbox_.get());

    auto build_slider = [](const std::string& label, float min, float max, float step, float value, int decimals) {
        return std::make_unique<FloatSliderWidget>(label, min, max, step, value, decimals);
    };

    max_elevation_slider_ = build_slider("Max Elevation", 0.0f, 4000.0f, 10.0f, 520.0f, 0);
    edge_falloff_slider_ = build_slider("Edge Falloff Distance", 0.0f, 3000.0f, 10.0f, 360.0f, 0);
    smoothness_slider_ = build_slider("Smoothness", 0.0f, 2.0f, 0.01f, 0.9f, 2);
    noise_variation_slider_ = build_slider("Noise Variation", 0.0f, 2.0f, 0.01f, 1.1f, 2);
    roughness_slider_ = build_slider("Roughness", 0.05f, 1.5f, 0.01f, 0.78f, 2);
    blend_strength_slider_ = build_slider("Blend Strength", 0.0f, 1.0f, 0.01f, 0.6f, 2);
    resolution_density_slider_ = build_slider("Resolution Density", 0.1f, 8.0f, 0.01f, 0.85f, 2);

    base_seed_textbox_ = std::make_unique<DMTextBox>("Light Base Seed", "0");
    base_seed_widget_ = std::make_unique<TextBoxWidget>(base_seed_textbox_.get(), true);

    lock_seed_checkbox_ = std::make_unique<DMCheckbox>("Lock Seed To World", true);
    lock_seed_widget_ = std::make_unique<CheckboxWidget>(lock_seed_checkbox_.get());

    direction_x_slider_ = build_slider("Light Direction X", -1.0f, 1.0f, 0.01f, -0.52f, 2);
    direction_y_slider_ = build_slider("Light Direction Y", -1.0f, 1.0f, 0.01f, -0.85f, 2);
    position_x_slider_ = build_slider("Light Position X", -10000.0f, 10000.0f, 10.0f, 0.0f, 0);
    position_y_slider_ = build_slider("Light Position Y", -10000.0f, 10000.0f, 10.0f, -1200.0f, 0);
    light_strength_slider_ = build_slider("Light Strength", 0.0f, 4.0f, 0.01f, 1.2f, 2);
    contrast_slider_ = build_slider("Light Contrast", 0.25f, 4.0f, 0.01f, 1.3f, 2);

    apply_button_ = std::make_unique<DMButton>("Apply", &DMStyles::AccentButton(), 0, DMButton::height());
    apply_button_widget_ = std::make_unique<ButtonWidget>(apply_button_.get(), [this]() { this->apply_current_values(); });

    save_button_ = std::make_unique<DMButton>("Save", &DMStyles::HeaderButton(), 0, DMButton::height());
    save_button_widget_ = std::make_unique<ButtonWidget>(save_button_.get(), [this]() { this->save_current_values(); });

    Rows rows;
    rows.push_back({enabled_widget_.get()});
    rows.push_back({max_elevation_slider_.get()});
    rows.push_back({edge_falloff_slider_.get()});
    rows.push_back({smoothness_slider_.get()});
    rows.push_back({noise_variation_slider_.get()});
    rows.push_back({roughness_slider_.get()});
    rows.push_back({blend_strength_slider_.get()});
    rows.push_back({resolution_density_slider_.get()});
    rows.push_back({base_seed_widget_.get()});
    rows.push_back({lock_seed_widget_.get()});
    rows.push_back({direction_x_slider_.get(), direction_y_slider_.get()});
    rows.push_back({position_x_slider_.get(), position_y_slider_.get()});
    rows.push_back({light_strength_slider_.get(), contrast_slider_.get()});
    rows.push_back({apply_button_widget_.get(), save_button_widget_.get()});
    set_rows(rows);

    refresh_from_map();
}

void TerrainSettingsPanel::set_map_info(nlohmann::json* map_info,
                                        std::function<void()> on_apply,
                                        std::function<bool()> on_save) {
    map_info_ = map_info;
    on_apply_ = std::move(on_apply);
    on_save_ = std::move(on_save);
    refresh_from_map();
}

void TerrainSettingsPanel::refresh_from_map() {
    TerrainSettings settings = map_info_ ? terrain_manifest::read_settings(map_info_)
                                         : TerrainSettings::sanitized(TerrainSettings::readonly());
    enabled_ = settings.enabled;
    lock_seed_to_world_ = settings.light.lock_seed_to_world;

    if (max_elevation_slider_) max_elevation_slider_->set_value(settings.max_elevation_world);
    if (edge_falloff_slider_) edge_falloff_slider_->set_value(settings.edge_falloff_distance_world);
    if (smoothness_slider_) smoothness_slider_->set_value(settings.smoothness);
    if (noise_variation_slider_) noise_variation_slider_->set_value(settings.noise_variation);
    if (roughness_slider_) roughness_slider_->set_value(settings.roughness);
    if (blend_strength_slider_) blend_strength_slider_->set_value(settings.blend_strength);
    if (resolution_density_slider_) resolution_density_slider_->set_value(settings.resolution_density_scale);
    if (base_seed_textbox_) base_seed_textbox_->set_value(std::to_string(settings.light.base_seed));
    if (direction_x_slider_) direction_x_slider_->set_value(settings.light.direction_world.x);
    if (direction_y_slider_) direction_y_slider_->set_value(settings.light.direction_world.y);
    if (position_x_slider_) position_x_slider_->set_value(settings.light.position_world.x);
    if (position_y_slider_) position_y_slider_->set_value(settings.light.position_world.y);
    if (light_strength_slider_) light_strength_slider_->set_value(settings.light.light_strength);
    if (contrast_slider_) contrast_slider_->set_value(settings.light.contrast);

    sync_widgets_from_state();
}

void TerrainSettingsPanel::sync_widgets_from_state() {
    if (enabled_checkbox_) {
        enabled_checkbox_->set_value(enabled_);
    }
    if (lock_seed_checkbox_) {
        lock_seed_checkbox_->set_value(lock_seed_to_world_);
    }
}

void TerrainSettingsPanel::sync_state_from_widgets() {
    TerrainSettings current = TerrainSettings::sanitized(TerrainSettings::readonly());
    if (enabled_checkbox_) {
        enabled_ = enabled_checkbox_->value();
    }
    if (lock_seed_checkbox_) {
        lock_seed_to_world_ = lock_seed_checkbox_->value();
    }

    current.enabled = enabled_;
    current.max_elevation_world = max_elevation_slider_ ? max_elevation_slider_->value() : current.max_elevation_world;
    current.edge_falloff_distance_world = edge_falloff_slider_ ? edge_falloff_slider_->value() : current.edge_falloff_distance_world;
    current.smoothness = smoothness_slider_ ? smoothness_slider_->value() : current.smoothness;
    current.noise_variation = noise_variation_slider_ ? noise_variation_slider_->value() : current.noise_variation;
    current.roughness = roughness_slider_ ? roughness_slider_->value() : current.roughness;
    current.blend_strength = blend_strength_slider_ ? blend_strength_slider_->value() : current.blend_strength;
    current.resolution_density_scale = resolution_density_slider_ ? resolution_density_slider_->value() : current.resolution_density_scale;
    current.light.base_seed = parse_seed(base_seed_textbox_ ? base_seed_textbox_->value() : std::string{}, current.light.base_seed);
    current.light.lock_seed_to_world = lock_seed_to_world_;
    current.light.direction_world.x = direction_x_slider_ ? direction_x_slider_->value() : current.light.direction_world.x;
    current.light.direction_world.y = direction_y_slider_ ? direction_y_slider_->value() : current.light.direction_world.y;
    current.light.position_world.x = position_x_slider_ ? position_x_slider_->value() : current.light.position_world.x;
    current.light.position_world.y = position_y_slider_ ? position_y_slider_->value() : current.light.position_world.y;
    current.light.light_strength = light_strength_slider_ ? light_strength_slider_->value() : current.light.light_strength;
    current.light.contrast = contrast_slider_ ? contrast_slider_->value() : current.light.contrast;

    current = TerrainSettings::sanitized(current);

    TerrainSettings::apply(current);
    if (base_seed_textbox_) {
        base_seed_textbox_->set_value(std::to_string(current.light.base_seed));
    }
    if (enabled_checkbox_) {
        enabled_checkbox_->set_value(current.enabled);
    }
    if (lock_seed_checkbox_) {
        lock_seed_checkbox_->set_value(current.light.lock_seed_to_world);
    }
}

void TerrainSettingsPanel::apply_current_values() {
    sync_state_from_widgets();
    if (on_apply_) {
        on_apply_();
    }
}

void TerrainSettingsPanel::save_current_values() {
    apply_current_values();
    if (!map_info_) {
        return;
    }

    terrain_manifest::write_settings(*map_info_, TerrainSettings::readonly());
    if (on_save_) {
        on_save_();
    }
}
