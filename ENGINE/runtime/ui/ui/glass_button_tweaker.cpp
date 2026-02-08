#include "glass_button_tweaker.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/ttf_render_utils.hpp"

#include "button.hpp"
#include "styles.hpp"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using FieldKind = GlassButtonTweaker::FieldKind;
using FieldDefinition = GlassButtonTweaker::FieldDefinition;

namespace {

constexpr int kPanelPadding = 12;
constexpr int kLineHeight   = 28;
constexpr int kLegendHeight = 44;
constexpr int kButtonHeight = 36;
constexpr int kButtonSpacing = 8;
constexpr int kStatusTimeoutMs = 1500;

constexpr int kLabelColW = 260;
constexpr int kValueColW = 120;
constexpr int kSliderW   = 140;
constexpr int kSliderH   = 14;

constexpr char kLegendText[] =
    "Mouse: drag sliders, click value to type  |  Up/Down select  Left/Right adjust (Shift big)\n"
    "Enter toggles bool or commits text  |  Tab cycles color channel  |  Esc closes or cancels text";

static inline float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
static inline int clampi(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

void render_text(SDL_Renderer* renderer, TTF_Font* font, const std::string& text, int x, int y, SDL_Color color) {
    if (!renderer || !font || text.empty()) return;
    SDL_Surface* surface = ttf_util::RenderTextBlended(font, text.c_str(), color);
    if (!surface) return;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture) {
        SDL_Rect dst{ x, y, surface->w, surface->h };
        sdl_render::Texture(renderer, texture, nullptr, &dst);
        SDL_DestroyTexture(texture);
    }
    SDL_DestroySurface(surface);
}

std::string format_decimal(float value) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(3) << value;
    std::string text = ss.str();
    while (!text.empty() && text.back() == '0') text.pop_back();
    if (!text.empty() && text.back() == '.') text.pop_back();
    if (text.empty()) text = "0";
    return text;
}

static inline bool point_in_rect(const SDL_Point& p, const SDL_Rect& r) {
    return (p.x >= r.x && p.x < (r.x + r.w) && p.y >= r.y && p.y < (r.y + r.h));
}

struct Range {
    float minv = 0.0f;
    float maxv = 1.0f;
};

Range range_for_field_label(const std::string& label, FieldKind kind) {
    if (kind == FieldKind::Boolean) return {0.0f, 1.0f};
    if (kind == FieldKind::Color)   return {0.0f, 255.0f};

    // Reasonable clamps so sliders are usable.
    if (label == "Radius") return {0.0f, 60.0f};
    if (label == "Refraction Strength") return {0.0f, 0.25f};
    if (label == "Rough Scale") return {0.0f, 0.25f};
    if (label == "Rough Ampl Px") return {0.0f, 20.0f};
    if (label == "Diffusion Taps") return {0.0f, 200.0f};
    if (label == "Diffusion Radius") return {0.0f, 20.0f};
    if (label == "Chroma Strength") return {0.0f, 3.0f};
    if (label == "Mix Normal") return {0.0f, 1.0f};
    if (label == "Mix Hover") return {0.0f, 1.0f};
    if (label == "Mix Pressed") return {0.0f, 1.0f};
    if (label == "Fresnel Power") return {0.0f, 10.0f};
    if (label == "Fresnel Intensity") return {0.0f, 3.0f};
    if (label == "Overlay Opacity") return {0.0f, 1.0f};
    if (label == "Overlay Bright Gamma") return {0.0f, 6.0f};
    if (label == "Ray Threshold") return {0.0f, 0.99f};
    if (label == "Ray Intensity") return {0.0f, 10.0f};
    if (label == "Ray Length") return {0.0f, 2.0f};
    if (label == "Ray Steps") return {0.0f, 32.0f};
    if (label == "Motion Blur Radius") return {0.0f, 64.0f};
    if (label == "Motion Blur Mix") return {0.0f, 0.95f};

    // Fallback ranges.
    if (kind == FieldKind::Integer) return {0.0f, 200.0f};
    return {0.0f, 2.0f};
}

float slider_t_from_mouse_x(int mx, const SDL_Rect& slider) {
    if (slider.w <= 1) return 0.0f;
    float t = (static_cast<float>(mx - slider.x) / static_cast<float>(slider.w));
    return clampf(t, 0.0f, 1.0f);
}

int channel_from_local_x(int mx, const SDL_Rect& valueRect) {
    // For colors, click roughly in 4 equal columns to pick R,G,B,A.
    int rel = mx - valueRect.x;
    int w = std::max(1, valueRect.w);
    int idx = (rel * 4) / w;
    return clampi(idx, 0, 3);
}

struct RowHit {
    SDL_Rect rowRect{};
    SDL_Rect sliderRect{};
    SDL_Rect valueRect{};
};

struct TweakerState {
    bool dragging = false;
    int drag_index = -1;
    bool dragging_color = false;
    int drag_color_channel = 0;

    bool editing_text = false;
    int edit_index = -1;
    std::string edit_buffer;

    int mouse_over_index = -1;

    uint64_t last_click_ticks = 0;
    int last_click_index = -1;
};

std::unordered_map<const GlassButtonTweaker*, TweakerState>& state_map() {
    static std::unordered_map<const GlassButtonTweaker*, TweakerState> s;
    return s;
}

void ensure_text_input(bool on) {
    if (on) {
        if (!SDL_TextInputActive(SDL_GetKeyboardFocus())) SDL_StartTextInput(SDL_GetKeyboardFocus());
    } else {
        if (SDL_TextInputActive(SDL_GetKeyboardFocus())) SDL_StopTextInput(SDL_GetKeyboardFocus());
    }
}

} // namespace

GlassButtonTweaker::GlassButtonTweaker() {
    fields_.reserve(24);
    fields_.push_back({"Radius", FieldKind::Integer, &GlassButtonStyle::radius, 1.0f, 5.0f});
    fields_.push_back({"Refraction Strength", FieldKind::Float, &GlassButtonStyle::refraction_strength, 0.002f, 0.01f});
    fields_.push_back({"Rough Scale", FieldKind::Float, &GlassButtonStyle::rough_scale, 0.002f, 0.01f});
    fields_.push_back({"Rough Ampl Px", FieldKind::Float, &GlassButtonStyle::rough_ampl_px, 0.1f, 0.5f});
    fields_.push_back({"Diffusion Taps", FieldKind::Integer, &GlassButtonStyle::diffusion_taps, 5.0f, 10.0f});
    fields_.push_back({"Diffusion Radius", FieldKind::Float, &GlassButtonStyle::diffusion_radius, 0.1f, 0.5f});
    fields_.push_back({"Chroma Strength", FieldKind::Float, &GlassButtonStyle::chroma_strength, 0.05f, 0.2f});
    fields_.push_back({"Mix Normal", FieldKind::Float, &GlassButtonStyle::mix_normal, 0.02f, 0.1f});
    fields_.push_back({"Mix Hover", FieldKind::Float, &GlassButtonStyle::mix_hover, 0.02f, 0.1f});
    fields_.push_back({"Mix Pressed", FieldKind::Float, &GlassButtonStyle::mix_pressed, 0.02f, 0.1f});
    fields_.push_back({"Fresnel Power", FieldKind::Float, &GlassButtonStyle::fresnel_power, 0.1f, 0.5f});
    fields_.push_back({"Fresnel Intensity", FieldKind::Float, &GlassButtonStyle::fresnel_intensity, 0.05f, 0.2f});
    fields_.push_back({"Overlay Enabled", FieldKind::Boolean, &GlassButtonStyle::overlay_enabled, 0.0f, 0.0f});
    fields_.push_back({"Overlay Opacity", FieldKind::Float, &GlassButtonStyle::overlay_opacity, 0.05f, 0.2f});
    fields_.push_back({"Overlay Bright Gamma", FieldKind::Float, &GlassButtonStyle::overlay_bright_to_alpha_gamma, 0.05f, 0.2f});
    fields_.push_back({"Ray Threshold", FieldKind::Float, &GlassButtonStyle::ray_threshold, 0.01f, 0.05f});
    fields_.push_back({"Ray Intensity", FieldKind::Float, &GlassButtonStyle::ray_intensity, 0.05f, 0.2f});
    fields_.push_back({"Ray Length", FieldKind::Float, &GlassButtonStyle::ray_length, 0.05f, 0.2f});
    fields_.push_back({"Ray Steps", FieldKind::Integer, &GlassButtonStyle::ray_steps, 1.0f, 2.0f});
    fields_.push_back({"Motion Blur Radius", FieldKind::Integer, &GlassButtonStyle::motion_blur_radius, 1.0f, 2.0f});
    fields_.push_back({"Motion Blur Mix", FieldKind::Float, &GlassButtonStyle::motion_blur_mix, 0.02f, 0.1f});
    fields_.push_back({"Text Color", FieldKind::Color, &GlassButtonStyle::text_color, 1.0f, 5.0f});
    fields_.push_back({"Text Stroke", FieldKind::Color, &GlassButtonStyle::text_stroke, 1.0f, 5.0f});

    // Ensure state exists.
    state_map()[this] = TweakerState{};
}

GlassButtonTweaker::~GlassButtonTweaker() {
    // Stop text input if this instance was using it.
    auto it = state_map().find(this);
    if (it != state_map().end()) {
        if (it->second.editing_text) ensure_text_input(false);
        state_map().erase(it);
    }
}

void GlassButtonTweaker::toggle() {
    active_ = !active_;
    if (active_) {
        color_channel_ = 0;
    } else {
        auto& st = state_map()[this];
        st.dragging = false;
        st.editing_text = false;
        ensure_text_input(false);
    }
}

void GlassButtonTweaker::open() {
    active_ = true;
    color_channel_ = 0;
}

void GlassButtonTweaker::close() {
    active_ = false;
    auto& st = state_map()[this];
    st.dragging = false;
    st.editing_text = false;
    ensure_text_input(false);
}

bool GlassButtonTweaker::is_active() const {
    return active_;
}

void GlassButtonTweaker::update_layout(int screen_w, int screen_h) {
    int width = std::min(720, std::max(420, screen_w - 64));
    panel_rect_.w = width;
    panel_rect_.x = 32;
    panel_rect_.y = 32;

    int visible_rows = static_cast<int>(fields_.size());
    int content_height = visible_rows * kLineHeight;
    int buttons_section = kButtonHeight + kPanelPadding + 8;

    panel_rect_.h = kPanelPadding * 2 + content_height + kLegendHeight + buttons_section;
    if (panel_rect_.h > screen_h - 32) panel_rect_.h = screen_h - 32;

    int button_y = panel_rect_.y + panel_rect_.h - kPanelPadding - kButtonHeight;
    int button_width = (panel_rect_.w - kPanelPadding * 2 - kButtonSpacing * 2) / 3;

    save_button_rect_   = { panel_rect_.x + kPanelPadding, button_y, button_width, kButtonHeight };
    random_button_rect_ = { save_button_rect_.x + button_width + kButtonSpacing, button_y, button_width, kButtonHeight };
    close_button_rect_  = { random_button_rect_.x + button_width + kButtonSpacing, button_y, button_width, kButtonHeight };
}

void GlassButtonTweaker::update_status(const std::string& text) {
    status_text_ = text;
    status_expire_ticks_ = SDL_GetTicks64() + kStatusTimeoutMs;
}

bool GlassButtonTweaker::is_point_inside(const SDL_Point& point, const SDL_Rect& rect) const {
    return point_in_rect(point, rect);
}

std::string GlassButtonTweaker::format_field_value(const GlassButtonStyle& style, const FieldDefinition& field) const {
    switch (field.kind) {
        case FieldKind::Integer:
            return std::to_string(style.*std::get<int GlassButtonStyle::*>(field.member));
        case FieldKind::Float:
            return format_decimal(style.*std::get<float GlassButtonStyle::*>(field.member));
        case FieldKind::Boolean:
            return (style.*std::get<bool GlassButtonStyle::*>(field.member)) ? "true" : "false";
        case FieldKind::Color: {
            const SDL_Color& color = style.*std::get<SDL_Color GlassButtonStyle::*>(field.member);
            std::ostringstream oss;
            oss << static_cast<int>(color.r) << "," << static_cast<int>(color.g) << ","
                << static_cast<int>(color.b) << "," << static_cast<int>(color.a);
            return oss.str();
        }
    }
    return {};
}

void GlassButtonTweaker::cycle_color_channel() {
    color_channel_ = (color_channel_ + 1) % 4;
}

void GlassButtonTweaker::toggle_current_bool() {
    if (fields_.empty()) return;
    FieldDefinition& field = fields_[selected_index_];
    if (field.kind != FieldKind::Boolean) return;
    auto& style = ButtonSettings::instance().mutable_style();
    bool& value = style.*std::get<bool GlassButtonStyle::*>(field.member);
    value = !value;
}

void GlassButtonTweaker::adjust_current_field(bool increase, bool fast) {
    if (fields_.empty()) return;
    auto& style = ButtonSettings::instance().mutable_style();
    FieldDefinition& field = fields_[selected_index_];
    const float step = fast ? field.large_step : field.step;

    const Range r = range_for_field_label(field.label, field.kind);

    switch (field.kind) {
        case FieldKind::Integer: {
            int delta = static_cast<int>(std::round(step));
            if (delta == 0) delta = 1;
            if (!increase) delta = -delta;
            int& value = style.*std::get<int GlassButtonStyle::*>(field.member);
            value = clampi(value + delta, static_cast<int>(r.minv), static_cast<int>(r.maxv));
            break;
        }
        case FieldKind::Float: {
            float delta = step;
            if (!increase) delta = -delta;
            float& value = style.*std::get<float GlassButtonStyle::*>(field.member);
            value = clampf(value + delta, r.minv, r.maxv);
            break;
        }
        case FieldKind::Color: {
            int delta = static_cast<int>(std::round(step));
            if (delta == 0) delta = 1;
            if (!increase) delta = -delta;
            SDL_Color& color = style.*std::get<SDL_Color GlassButtonStyle::*>(field.member);
            Uint8* channel = &color.r;
            if (color_channel_ == 1) channel = &color.g;
            else if (color_channel_ == 2) channel = &color.b;
            else if (color_channel_ == 3) channel = &color.a;
            int updated = clampi(static_cast<int>(*channel) + delta, 0, 255);
            *channel = static_cast<Uint8>(updated);
            break;
        }
        case FieldKind::Boolean:
        default:
            break;
    }
}

static RowHit compute_row_hit(const SDL_Rect& panel, int row_y) {
    RowHit hit{};
    hit.rowRect = { panel.x + 4, row_y, panel.w - 8, kLineHeight };

    // Layout: label left, slider middle, value right.
    int x0 = panel.x + kPanelPadding;
    int label_x = x0;
    int slider_x = panel.x + panel.w - kPanelPadding - kValueColW - kSliderW - 10;
    int value_x  = panel.x + panel.w - kPanelPadding - kValueColW;

    hit.sliderRect = { slider_x, row_y + (kLineHeight - kSliderH) / 2, kSliderW, kSliderH };
    hit.valueRect  = { value_x, row_y + 2, kValueColW, kLineHeight - 4 };

    (void)label_x;
    return hit;
}

static void set_value_from_slider(GlassButtonStyle& style, FieldDefinition& field, int mouse_x, const SDL_Rect& slider, int& color_channel) {
    const Range r = range_for_field_label(field.label, field.kind);
    const float t = slider_t_from_mouse_x(mouse_x, slider);
    const float v = r.minv + (r.maxv - r.minv) * t;

    if (field.kind == FieldKind::Integer) {
        int& value = style.*std::get<int GlassButtonStyle::*>(field.member);
        value = clampi(static_cast<int>(std::round(v)), static_cast<int>(r.minv), static_cast<int>(r.maxv));
    } else if (field.kind == FieldKind::Float) {
        float& value = style.*std::get<float GlassButtonStyle::*>(field.member);
        value = clampf(v, r.minv, r.maxv);
    } else if (field.kind == FieldKind::Color) {
        SDL_Color& c = style.*std::get<SDL_Color GlassButtonStyle::*>(field.member);
        Uint8* ch = &c.r;
        if (color_channel == 1) ch = &c.g;
        else if (color_channel == 2) ch = &c.b;
        else if (color_channel == 3) ch = &c.a;
        int iv = clampi(static_cast<int>(std::round(v)), 0, 255);
        *ch = static_cast<Uint8>(iv);
    }
}

static bool commit_text_value(GlassButtonStyle& style, FieldDefinition& field, const std::string& text, int& color_channel) {
    if (field.kind == FieldKind::Integer) {
        try {
            int v = std::stoi(text);
            const Range r = range_for_field_label(field.label, field.kind);
            v = clampi(v, static_cast<int>(r.minv), static_cast<int>(r.maxv));
            int& dst = style.*std::get<int GlassButtonStyle::*>(field.member);
            dst = v;
            return true;
        } catch (...) {
            return false;
        }
    }
    if (field.kind == FieldKind::Float) {
        try {
            float v = std::stof(text);
            const Range r = range_for_field_label(field.label, field.kind);
            v = clampf(v, r.minv, r.maxv);
            float& dst = style.*std::get<float GlassButtonStyle::*>(field.member);
            dst = v;
            return true;
        } catch (...) {
            return false;
        }
    }
    if (field.kind == FieldKind::Color) {
        // Accept "r,g,b,a" or a single number to current channel.
        SDL_Color& c = style.*std::get<SDL_Color GlassButtonStyle::*>(field.member);

        int r = -1, g = -1, b = -1, a = -1;
        if (std::sscanf(text.c_str(), "%d,%d,%d,%d", &r, &g, &b, &a) == 4) {
            c.r = static_cast<Uint8>(clampi(r, 0, 255));
            c.g = static_cast<Uint8>(clampi(g, 0, 255));
            c.b = static_cast<Uint8>(clampi(b, 0, 255));
            c.a = static_cast<Uint8>(clampi(a, 0, 255));
            return true;
        }

        try {
            int v = std::stoi(text);
            v = clampi(v, 0, 255);
            Uint8* ch = &c.r;
            if (color_channel == 1) ch = &c.g;
            else if (color_channel == 2) ch = &c.b;
            else if (color_channel == 3) ch = &c.a;
            *ch = static_cast<Uint8>(v);
            return true;
        } catch (...) {
            return false;
        }
    }
    return false;
}

bool GlassButtonTweaker::handle_event(const SDL_Event& e, int screen_w, int screen_h) {
    if (!active_) return false;
    update_layout(screen_w, screen_h);

    auto& st = state_map()[this];

    // Keep track of which row mouse is over for nicer UX.
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        st.mouse_over_index = -1;
        SDL_Point p{ e.motion.x, e.motion.y };
        if (point_in_rect(p, panel_rect_)) {
            for (int i = 0; i < static_cast<int>(fields_.size()); ++i) {
                int y = panel_rect_.y + kPanelPadding + i * kLineHeight;
                RowHit hit = compute_row_hit(panel_rect_, y);
                if (point_in_rect(p, hit.rowRect)) {
                    st.mouse_over_index = i;
                    break;
                }
            }
        }

        if (st.dragging && st.drag_index >= 0 && st.drag_index < static_cast<int>(fields_.size())) {
            auto& style = ButtonSettings::instance().mutable_style();
            FieldDefinition& field = fields_[st.drag_index];
            int y = panel_rect_.y + kPanelPadding + st.drag_index * kLineHeight;
            RowHit hit = compute_row_hit(panel_rect_, y);
            if (field.kind == FieldKind::Color) {
                set_value_from_slider(style, field, e.motion.x, hit.sliderRect, st.drag_color_channel);
            } else {
                set_value_from_slider(style, field, e.motion.x, hit.sliderRect, color_channel_);
            }
            return true;
        }
    }

    if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
        if (st.dragging) {
            st.dragging = false;
            st.drag_index = -1;
            return true;
        }
    }

    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point p{ e.button.x, e.button.y };

        if (!point_in_rect(p, panel_rect_)) return false;

        // Buttons first.
        if (point_in_rect(p, save_button_rect_)) {
            std::string message;
            if (ButtonSettings::instance().save_defaults(&message)) {
                update_status(message.empty() ? "Saved" : message);
            } else {
                update_status(message.empty() ? "Save failed" : message);
            }
            return true;
        }
        if (point_in_rect(p, random_button_rect_)) {
            Button::refresh_glass_overlay();
            update_status("Overlay randomized");
            return true;
        }
        if (point_in_rect(p, close_button_rect_)) {
            close();
            return true;
        }

        // Row interactions.
        for (int i = 0; i < static_cast<int>(fields_.size()); ++i) {
            int y = panel_rect_.y + kPanelPadding + i * kLineHeight;
            RowHit hit = compute_row_hit(panel_rect_, y);
            if (!point_in_rect(p, hit.rowRect)) continue;

            selected_index_ = i;
            FieldDefinition& field = fields_[selected_index_];

            // Clicking the value box starts text editing (for numeric/color), or toggles bool.
            if (point_in_rect(p, hit.valueRect)) {
                // Double click toggles bool quickly.
                uint64_t now = SDL_GetTicks64();
                bool is_double = (st.last_click_index == i) && (now - st.last_click_ticks < 350);
                st.last_click_ticks = now;
                st.last_click_index = i;

                if (field.kind == FieldKind::Boolean) {
                    toggle_current_bool();
                    return true;
                }

                if (field.kind == FieldKind::Color) {
                    // Choose channel based on click position inside value box.
                    color_channel_ = channel_from_local_x(p.x, hit.valueRect);
                    st.drag_color_channel = color_channel_;
                } else {
                    color_channel_ = 0;
                }

                if (is_double && field.kind == FieldKind::Color) {
                    // Double click on color row cycles channel.
                    cycle_color_channel();
                }

                // Start text editing.
                st.editing_text = true;
                st.edit_index = i;
                st.edit_buffer = format_field_value(ButtonSettings::instance().style(), field);
                ensure_text_input(true);
                return true;
            }

            // Slider drag.
            if (field.kind != FieldKind::Boolean && point_in_rect(p, hit.sliderRect)) {
                st.dragging = true;
                st.drag_index = i;
                st.dragging_color = (field.kind == FieldKind::Color);
                if (field.kind == FieldKind::Color) {
                    // If clicking slider on color row, edit whichever channel is currently active.
                    st.drag_color_channel = color_channel_;
                }
                auto& style = ButtonSettings::instance().mutable_style();
                if (field.kind == FieldKind::Color) {
                    set_value_from_slider(style, field, p.x, hit.sliderRect, st.drag_color_channel);
                } else {
                    set_value_from_slider(style, field, p.x, hit.sliderRect, color_channel_);
                }
                return true;
            }

            // Click anywhere else on row selects it.
            return true;
        }

        return true;
    }

    // Text input mode.
    if (st.editing_text) {
        if (e.type == SDL_EVENT_TEXT_INPUT) {
            if (st.edit_buffer.size() < 64) st.edit_buffer += e.text.text;
            return true;
        }
        if (e.type == SDL_EVENT_KEY_DOWN) {
            if (e.key.key == SDLK_BACKSPACE) {
                if (!st.edit_buffer.empty()) st.edit_buffer.pop_back();
                return true;
            }
            if (e.key.key == SDLK_ESCAPE) {
                st.editing_text = false;
                st.edit_index = -1;
                st.edit_buffer.clear();
                ensure_text_input(false);
                update_status("Edit canceled");
                return true;
            }
            if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) {
                if (st.edit_index >= 0 && st.edit_index < static_cast<int>(fields_.size())) {
                    auto& style = ButtonSettings::instance().mutable_style();
                    FieldDefinition& field = fields_[st.edit_index];
                    bool ok = commit_text_value(style, field, st.edit_buffer, color_channel_);
                    update_status(ok ? "Applied" : "Invalid value");
                }
                st.editing_text = false;
                st.edit_index = -1;
                st.edit_buffer.clear();
                ensure_text_input(false);
                return true;
            }
        }
        return true;
    }

    // Keyboard navigation and nudging.
    if (e.type == SDL_EVENT_KEY_DOWN) {
        const bool fast = (e.key.mod & SDL_KMOD_SHIFT) != 0;
        switch (e.key.key) {
            case SDLK_UP:
                selected_index_ = (selected_index_ + static_cast<int>(fields_.size()) - 1) % static_cast<int>(fields_.size());
                if (fields_[selected_index_].kind != FieldKind::Color) color_channel_ = 0;
                return true;
            case SDLK_DOWN:
                selected_index_ = (selected_index_ + 1) % static_cast<int>(fields_.size());
                if (fields_[selected_index_].kind != FieldKind::Color) color_channel_ = 0;
                return true;
            case SDLK_LEFT:
                adjust_current_field(false, fast);
                return true;
            case SDLK_RIGHT:
                adjust_current_field(true, fast);
                return true;
            case SDLK_TAB:
                if (fields_[selected_index_].kind == FieldKind::Color) cycle_color_channel();
                return true;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                if (fields_[selected_index_].kind == FieldKind::Boolean) toggle_current_bool();
                return true;
            case SDLK_ESCAPE:
                close();
                return true;
            default:
                // Let other keys pass through.
                return false;
        }
    }

    return false;
}

void GlassButtonTweaker::render(SDL_Renderer* renderer, int screen_w, int screen_h) {
    if (!active_ || !renderer) return;

    update_layout(screen_w, screen_h);
    auto& st = state_map()[this];

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 12, 12, 20, 220);
    sdl_render::FillRect(renderer, &panel_rect_);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 70);
    sdl_render::Rect(renderer, &panel_rect_);

    TTF_Font* font = Styles::LabelSmallSecondary().open_font();
    if (!font) return;

    const GlassButtonStyle& style = ButtonSettings::instance().style();

    // Rows
    for (int idx = 0; idx < static_cast<int>(fields_.size()); ++idx) {
        int y = panel_rect_.y + kPanelPadding + idx * kLineHeight;
        RowHit hit = compute_row_hit(panel_rect_, y);

        const bool selected = (idx == selected_index_);
        const bool hovered = (idx == st.mouse_over_index);

        if (selected) {
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 38);
            sdl_render::FillRect(renderer, &hit.rowRect);
        } else if (hovered) {
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 18);
            sdl_render::FillRect(renderer, &hit.rowRect);
        }

        std::string label = fields_[idx].label;
        if (fields_[idx].kind == FieldKind::Color) {
            static const char* chn[] = {"R","G","B","A"};
            label += " [" + std::string(chn[color_channel_]) + "]";
        }
        render_text(renderer, font, label, panel_rect_.x + kPanelPadding, y + 5, SDL_Color{220, 220, 220, 255});

        // Slider
        FieldDefinition& field = fields_[idx];
        if (field.kind != FieldKind::Boolean) {
            SDL_SetRenderDrawColor(renderer, 40, 40, 40, 220);
            sdl_render::FillRect(renderer, &hit.sliderRect);
            SDL_SetRenderDrawColor(renderer, 200, 200, 200, 70);
            sdl_render::Rect(renderer, &hit.sliderRect);

            const Range r = range_for_field_label(field.label, field.kind);
            float t = 0.0f;

            if (field.kind == FieldKind::Integer) {
                int v = style.*std::get<int GlassButtonStyle::*>(field.member);
                t = (r.maxv > r.minv) ? (static_cast<float>(v) - r.minv) / (r.maxv - r.minv) : 0.0f;
            } else if (field.kind == FieldKind::Float) {
                float v = style.*std::get<float GlassButtonStyle::*>(field.member);
                t = (r.maxv > r.minv) ? (v - r.minv) / (r.maxv - r.minv) : 0.0f;
            } else if (field.kind == FieldKind::Color) {
                const SDL_Color& c = style.*std::get<SDL_Color GlassButtonStyle::*>(field.member);
                int v = 0;
                if (color_channel_ == 0) v = c.r;
                else if (color_channel_ == 1) v = c.g;
                else if (color_channel_ == 2) v = c.b;
                else v = c.a;
                t = static_cast<float>(v) / 255.0f;
            }

            t = clampf(t, 0.0f, 1.0f);
            SDL_Rect knob = hit.sliderRect;
            knob.w = 10;
            knob.x = hit.sliderRect.x + static_cast<int>(std::round(t * (hit.sliderRect.w - knob.w)));

            SDL_SetRenderDrawColor(renderer, 230, 230, 230, 160);
            sdl_render::FillRect(renderer, &knob);
        }

        // Value box
        SDL_SetRenderDrawColor(renderer, 28, 28, 28, 220);
        sdl_render::FillRect(renderer, &hit.valueRect);
        SDL_SetRenderDrawColor(renderer, 200, 200, 200, 70);
        sdl_render::Rect(renderer, &hit.valueRect);

        std::string valueText = format_field_value(style, field);
        if (st.editing_text && st.edit_index == idx) {
            valueText = st.edit_buffer;
            if ((SDL_GetTicks64() / 350) % 2 == 0) valueText += "|";
        }

        // Boolean shows as checkbox-ish.
        if (field.kind == FieldKind::Boolean) {
            bool v = style.*std::get<bool GlassButtonStyle::*>(field.member);
            valueText = v ? "true" : "false";
        }

        int tw = 0, th = 0;
        ttf_util::GetStringSize(font, valueText, &tw, &th);
        int vx = hit.valueRect.x + hit.valueRect.w - 6 - tw;
        int vy = hit.valueRect.y + (hit.valueRect.h - th) / 2;
        render_text(renderer, font, valueText, vx, vy, SDL_Color{255, 255, 255, 255});
    }

    // Legend
    int legend_y = panel_rect_.y + kPanelPadding + static_cast<int>(fields_.size()) * kLineHeight;
    render_text(renderer, font, kLegendText, panel_rect_.x + kPanelPadding, legend_y, SDL_Color{180, 180, 180, 255});

    // Status
    if (!status_text_.empty()) {
        if (SDL_GetTicks64() >= status_expire_ticks_) {
            status_text_.clear();
        } else {
            int tw = 0, th = 0;
            ttf_util::GetStringSize(font, status_text_, &tw, &th);
            int tx = panel_rect_.x + panel_rect_.w - kPanelPadding - tw;
            int ty = random_button_rect_.y - 24;
            render_text(renderer, font, status_text_, tx, ty, SDL_Color{180, 255, 180, 255});
        }
    }

    // Buttons
    auto draw_button = [&](const SDL_Rect& rect, const std::string& text) {
        SDL_SetRenderDrawColor(renderer, 60, 60, 60, 220);
        sdl_render::FillRect(renderer, &rect);
        SDL_SetRenderDrawColor(renderer, 200, 200, 200, 120);
        sdl_render::Rect(renderer, &rect);

        int tw = 0, th = 0;
        ttf_util::GetStringSize(font, text, &tw, &th);
        int tx = rect.x + (rect.w - tw) / 2;
        int ty = rect.y + (rect.h - th) / 2;
        render_text(renderer, font, text, tx, ty, SDL_Color{230, 230, 230, 255});
    };

    draw_button(save_button_rect_, "Save");
    draw_button(random_button_rect_, "Randomize Overlay");
    draw_button(close_button_rect_, "Close");

    TTF_CloseFont(font);
}



