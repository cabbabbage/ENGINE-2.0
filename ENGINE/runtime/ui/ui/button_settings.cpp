#include "button_settings.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_log.h>

#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct FieldEntry {
    const char* name;
    std::function<std::string(const GlassButtonStyle&)> formatter;
};

const std::vector<FieldEntry> kFieldEntries = {
    {"radius", [](const GlassButtonStyle& style) { return ButtonSettings::format_int(style.radius); }},
    {"refraction_strength", [](const GlassButtonStyle& style) { return ButtonSettings::format_float(style.refraction_strength); }},
    {"rough_scale", [](const GlassButtonStyle& style) { return ButtonSettings::format_float(style.rough_scale); }},
    {"rough_ampl_px", [](const GlassButtonStyle& style) { return ButtonSettings::format_float(style.rough_ampl_px); }},
    {"diffusion_taps", [](const GlassButtonStyle& style) { return ButtonSettings::format_int(style.diffusion_taps); }},
    {"diffusion_radius", [](const GlassButtonStyle& style) { return ButtonSettings::format_float(style.diffusion_radius); }},
    {"chroma_strength", [](const GlassButtonStyle& style) { return ButtonSettings::format_float(style.chroma_strength); }},
    {"mix_normal", [](const GlassButtonStyle& style) { return ButtonSettings::format_float(style.mix_normal); }},
    {"mix_hover", [](const GlassButtonStyle& style) { return ButtonSettings::format_float(style.mix_hover); }},
    {"mix_pressed", [](const GlassButtonStyle& style) { return ButtonSettings::format_float(style.mix_pressed); }},
    {"fresnel_power", [](const GlassButtonStyle& style) { return ButtonSettings::format_float(style.fresnel_power); }},
    {"fresnel_intensity", [](const GlassButtonStyle& style) { return ButtonSettings::format_float(style.fresnel_intensity); }},
    {"overlay_enabled", [](const GlassButtonStyle& style) { return ButtonSettings::format_bool(style.overlay_enabled); }},
    {"overlay_opacity", [](const GlassButtonStyle& style) { return ButtonSettings::format_float(style.overlay_opacity); }},
    {"overlay_bright_to_alpha_gamma", [](const GlassButtonStyle& style) { return ButtonSettings::format_float(style.overlay_bright_to_alpha_gamma); }},
    {"ray_threshold", [](const GlassButtonStyle& style) { return ButtonSettings::format_float(style.ray_threshold); }},
    {"ray_intensity", [](const GlassButtonStyle& style) { return ButtonSettings::format_float(style.ray_intensity); }},
    {"ray_length", [](const GlassButtonStyle& style) { return ButtonSettings::format_float(style.ray_length); }},
    {"ray_steps", [](const GlassButtonStyle& style) { return ButtonSettings::format_int(style.ray_steps); }},
    {"motion_blur_radius", [](const GlassButtonStyle& style) { return ButtonSettings::format_int(style.motion_blur_radius); }},
    {"motion_blur_mix", [](const GlassButtonStyle& style) { return ButtonSettings::format_float(style.motion_blur_mix); }},
    {"text_color", [](const GlassButtonStyle& style) { return ButtonSettings::format_color(style.text_color); }},
    {"text_stroke", [](const GlassButtonStyle& style) { return ButtonSettings::format_color(style.text_stroke); }}
};

GlassButtonStyle build_default_glass_button_style() {
    GlassButtonStyle style;
    style.radius = 0;
    style.refraction_strength = 0.035714f;
    style.rough_scale = 0.241071f;
    style.rough_ampl_px = 8.142857f;
    style.diffusion_taps = 174;
    style.diffusion_radius = 6.285715f;
    style.chroma_strength = 2.185714f;
    style.mix_normal = 0.585714f;
    style.mix_hover = 0.8f;
    style.mix_pressed = 0.378571f;
    style.fresnel_power = 8.071428f;
    style.fresnel_intensity = 1.2f;
    style.overlay_enabled = false;
    style.overlay_opacity = 0.321429f;
    style.overlay_bright_to_alpha_gamma = 0.514286f;
    style.ray_threshold = 0.594f;
    style.ray_intensity = 2.428571f;
    style.ray_length = 0.514286f;
    style.ray_steps = 10;
    style.motion_blur_radius = 48;
    style.motion_blur_mix = 0.76f;
    style.blur_px = 0;
    style.blur_px_hover = 0;
    style.blur_px_pressed = 0;
    style.text_color = SDL_Color{255,252,252,255};
    style.text_stroke = SDL_Color{0,0,0,110};
    style.border_light = SDL_Color{0, 0, 0, 0};
    style.border_dark = SDL_Color{0, 0, 0, 0};
    style.inner_shadow = SDL_Color{0, 0, 0, 0};
    style.outer_shadow = SDL_Color{0, 0, 0, 0};
    style.tint = SDL_Color{0, 0, 0, 0};
    style.tint_hover = SDL_Color{0, 0, 0, 0};
    style.tint_pressed = SDL_Color{0, 0, 0, 0};
    style.noise_opacity = 0.0f;
    style.smudge_opacity = 0.0f;
    style.highlight_color = SDL_Color{255, 255, 255, 255};
    style.highlight_glow_color = SDL_Color{255, 255, 255, 235};
    style.focus_ring_inner = SDL_Color{0, 0, 0, 0};
    style.focus_ring_outer = SDL_Color{0, 0, 0, 0};
    style.disabled_text = SDL_Color{200, 200, 200, 200};
    return style;
}

} // namespace

ButtonSettings& ButtonSettings::instance() {
    static ButtonSettings settings;
    return settings;
}

ButtonSettings::ButtonSettings()
: style_(build_default_style()), defaults_path_(std::filesystem::path(__FILE__)) {}

const GlassButtonStyle& ButtonSettings::style() const {
    return style_;
}

GlassButtonStyle& ButtonSettings::mutable_style() {
    return style_;
}

bool ButtonSettings::save_defaults(std::string* status_message) {
    std::ifstream in(defaults_path_, std::ios::binary);
    if (!in) {
        SDL_Log("ButtonSettings: failed to open defaults file '%s' for reading.", defaults_path_.u8string().c_str());
        if (status_message) *status_message = "Save failed: cannot open defaults file.";
        return false;
    }
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    std::string updated = contents;
    bool success = true;
    for (const FieldEntry& entry : kFieldEntries) {
        const std::string formatted = entry.formatter(style_);
        if (!replace_field(updated, entry.name, formatted)) {
            SDL_Log("ButtonSettings: field '%s' not found in defaults file.", entry.name);
            success = false;
        }
    }
    if (!success) {
        if (status_message) *status_message = "Save failed: defaults file out of date.";
        return false;
    }
    if (updated == contents) {
        if (status_message) *status_message = "Defaults already up to date.";
        SDL_Log("ButtonSettings: no changes detected; skipping write.");
        return true;
    }
    std::ofstream out(defaults_path_, std::ios::binary | std::ios::trunc);
    if (!out) {
        SDL_Log("ButtonSettings: failed to write defaults file '%s'.", defaults_path_.u8string().c_str());
        if (status_message) *status_message = "Save failed: cannot write defaults file.";
        return false;
    }
    out << updated;
    out.close();
    SDL_Log("ButtonSettings: defaults updated successfully.");
    if (status_message) *status_message = "Saved.";
    return true;
}

GlassButtonStyle ButtonSettings::build_default_style() {
    return build_default_glass_button_style();
}

std::string ButtonSettings::format_float(float value) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(6) << value;
    std::string text = trim_trailing_zeros(ss.str());
    if (text.empty() || text == "-0") {
        text = "0";
    }
    return text + "f";
}

std::string ButtonSettings::format_int(int value) {
    return std::to_string(value);
}

std::string ButtonSettings::format_bool(bool value) {
    return value ? "true" : "false";
}

std::string ButtonSettings::format_color(const SDL_Color& color) {
    return "SDL_Color{" + std::to_string(color.r) + "," + std::to_string(color.g) + "," +
           std::to_string(color.b) + "," + std::to_string(color.a) + "}";
}

std::string ButtonSettings::trim_trailing_zeros(std::string value) {
    if (value.find('.') == std::string::npos) {
        return value;
    }
    while (!value.empty() && value.back() == '0') {
        value.pop_back();
    }
    if (!value.empty() && value.back() == '.') {
        value.pop_back();
    }
    return value;
}

bool ButtonSettings::replace_field(std::string& content, const std::string& field_name, const std::string& value_repr) {
    const std::string prefix = "    style." + field_name + " = ";
    size_t pos = content.find(prefix);
    if (pos == std::string::npos) {
        return false;
    }
    pos += prefix.size();
    size_t end = content.find(';', pos);
    if (end == std::string::npos) {
        return false;
    }
    content.replace(pos, end - pos, value_repr);
    return true;
}
