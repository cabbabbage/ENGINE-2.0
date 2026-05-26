#include "widgets.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "draw_utils.hpp"
#include "font_cache.hpp"
#include "dm_icons.hpp"
#include <SDL3/SDL_log.h>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iterator>
#include <sstream>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <limits>
#include "utils/sdl_mouse_utils.hpp"
#include "utils/ttf_render_utils.hpp"

namespace {
constexpr int kBoxTopPadding = 8;
constexpr int kBoxBottomPadding = 8;
constexpr int kLabelControlGap = 8;
constexpr int kTextboxHorizontalPadding = 8;
constexpr int kSliderControlHeight = 44;
constexpr int kSliderValueWidth = 60;
constexpr int kDropdownControlHeight = 32;
constexpr int kButtonHorizontalPadding = 28;
constexpr int kCheckboxLabelGap = 8;
constexpr int kSliderValueHorizontalPadding = 8;
constexpr int kSliderTrackThickness = 10;
constexpr int kSliderKnobWidth = 14;
constexpr int kSliderKnobHeight = 18;
constexpr int kSliderKnobVerticalInset = (kSliderKnobHeight - kSliderTrackThickness) / 2;
constexpr int kControlOutlineThickness = 1;
constexpr int kFocusRingThickness = 2;
constexpr int kKnobOutlineThickness = 1;
constexpr int kNumericStepperHeight = 32;
constexpr int kNumericStepperButtonWidth = 32;
constexpr int kNumericStepperValueMinWidth = 56;
constexpr int kTooltipIconSize = 16;
constexpr int kTooltipIconPadding = 6;
constexpr int kTooltipHoverDelayMs = 1000;
constexpr int kTooltipBoxPadding = 6;
constexpr int kTooltipBoxMargin = 6;
constexpr int kTooltipCornerRadius = 6;

struct DropdownCandidate {
    int delta;
    float scale;
    float alpha;
};

constexpr DropdownCandidate kDropdownCandidates[] = {
    { -2, 0.82f, 0.35f },
    { -1, 0.9f, 0.65f },
    { 0, 1.0f, 1.0f },
    { 1, 0.9f, 0.65f },
    { 2, 0.82f, 0.35f },
};

SDL_Color ApplyAlpha(SDL_Color col, float alpha) {
    const int scaled = static_cast<int>(std::round(col.a * alpha));
    col.a = static_cast<Uint8>(std::clamp(scaled, 0, 255));
    return col;
}

struct SliderFormatStats {
    int format_calls = 0;
    int allocations = 0;
    int last_logged_calls = 0;
    int last_logged_allocations = 0;

    void log_if_needed() {
        constexpr int kLogInterval = 120;
        if (format_calls - last_logged_calls < kLogInterval) {
            return;
        }
        SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, "[DMSlider] format stats: calls=%d allocations=%d (delta=%d)", format_calls, allocations, allocations - last_logged_allocations);
        last_logged_calls = format_calls;
        last_logged_allocations = allocations;
    }
};

SliderFormatStats& slider_format_stats() {
    static SliderFormatStats stats;
    return stats;
}

int slider_value_height() {
    const DMSliderStyle& st = DMStyles::Slider();
    return std::max(DMTextBox::height(), st.value.font_size + DMSpacing::small_gap());
}

int range_value_width(int total_width) {
    int candidate = total_width / 4;
    candidate = std::max(candidate, 64);
    candidate = std::min(candidate, std::max(64, total_width / 2));
    return candidate;
}

std::unordered_set<const void*> g_slider_scroll_captures;

void set_slider_scroll_capture(const void* owner, bool capture) {
    if (capture) {
        g_slider_scroll_captures.insert(owner);
    } else {
        g_slider_scroll_captures.erase(owner);
    }
}

bool slider_scroll_captured() {
    return !g_slider_scroll_captures.empty();
}

DMLabelStyle tooltip_icon_label_style() {
    const DMLabelStyle& base = DMStyles::Label();
    return DMLabelStyle{ base.font_path, 14, base.color };
}

DMLabelStyle tooltip_text_label_style() {
    const DMLabelStyle& base = DMStyles::Label();
    return DMLabelStyle{ base.font_path, 14, base.color };
}

SDL_Color tooltip_icon_background(bool hovered) {
    return hovered ? DMStyles::ButtonHoverFill() : DMStyles::ButtonBaseFill();
}

SDL_Color tooltip_icon_border() {
    return DMStyles::Border();
}

SDL_Color tooltip_box_background() {
    return DMStyles::PanelHeader();
}

SDL_Color tooltip_box_border() {
    return DMStyles::Border();
}
}

bool DMWidgetsSliderScrollCaptured() {
    return slider_scroll_captured();
}

void DMWidgetsSetSliderScrollCapture(const void* owner, bool capture) {
    set_slider_scroll_capture(owner, capture);
}

void DMWidgetsClearSliderScrollCaptures() {
    g_slider_scroll_captures.clear();
}

SDL_Rect DMWidgetTooltipIconRect(const SDL_Rect& bounds) {
    SDL_Rect icon{
        bounds.x + std::max(0, bounds.w - kTooltipIconSize - kTooltipIconPadding), bounds.y + kTooltipIconPadding, kTooltipIconSize, kTooltipIconSize };
    const int min_x = bounds.x + kTooltipIconPadding;
    const int min_y = bounds.y + kTooltipIconPadding;
    if (icon.x < min_x) {
        icon.x = min_x;
    }
    if (icon.y < min_y) {
        icon.y = min_y;
    }
    const int max_x = bounds.x + bounds.w;
    const int max_y = bounds.y + bounds.h;
    icon.w = std::max(0, std::min(kTooltipIconSize, max_x - icon.x));
    icon.h = std::max(0, std::min(kTooltipIconSize, max_y - icon.y));
    return icon;
}

bool DMWidgetTooltipEnabled(const DMWidgetTooltipState& state) {
    return state.enabled && !state.text.empty();
}

void DMWidgetTooltipResetHover(DMWidgetTooltipState& state) {
    state.icon_hovered = false;
    state.hover_start_ms = 0;
}

bool DMWidgetTooltipHandleEvent(const SDL_Event& e, const SDL_Rect& bounds, DMWidgetTooltipState& state) {
    if (!DMWidgetTooltipEnabled(state)) {
        return false;
    }
    SDL_Rect icon_rect = DMWidgetTooltipIconRect(bounds);
    if (icon_rect.w <= 0 || icon_rect.h <= 0) {
        return false;
    }

    auto point_in_icon = [&](int x, int y) {
        SDL_Point p{x, y};
        return SDL_PointInRect(&p, &icon_rect);
};

    switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION: {
        const bool inside = point_in_icon(e.motion.x, e.motion.y);
        if (inside) {
            if (!state.icon_hovered) {
                state.icon_hovered = true;
                state.hover_start_ms = SDL_GetTicks();
            }
        } else if (state.icon_hovered) {
            DMWidgetTooltipResetHover(state);
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (point_in_icon(e.button.x, e.button.y)) {
            return true;
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (state.icon_hovered) {
            return true;
        }
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        DMWidgetTooltipResetHover(state);
        break;
    default:
        break;
    }
    return false;
}

bool DMWidgetTooltipShouldDisplay(const DMWidgetTooltipState& state, Uint32 now_ticks) {
    if (!DMWidgetTooltipEnabled(state) || !state.icon_hovered || state.hover_start_ms == 0) {
        return false;
    }
    const Uint32 elapsed = now_ticks - state.hover_start_ms;
    return elapsed >= static_cast<Uint32>(kTooltipHoverDelayMs);
}

void DMWidgetTooltipRender(SDL_Renderer* renderer, const SDL_Rect& bounds, const DMWidgetTooltipState& state) {
    if (!DMWidgetTooltipEnabled(state)) {
        return;
    }

    SDL_Rect icon_rect = DMWidgetTooltipIconRect(bounds);
    if (icon_rect.w <= 0 || icon_rect.h <= 0) {
        return;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    const SDL_Color bg = tooltip_icon_background(state.icon_hovered);
    SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, bg.a);
    sdl_render::FillRect(renderer, &icon_rect);

    const SDL_Color border = tooltip_icon_border();
    SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, border.a);
    sdl_render::Rect(renderer, &icon_rect);

    const auto icon_style = tooltip_icon_label_style();
    const std::string icon_text{DMIcons::Info()};
    SDL_Point glyph = DMFontCache::instance().measure_text(icon_style, icon_text);
    const int text_x = icon_rect.x + std::max(0, (icon_rect.w - glyph.x) / 2);
    const int text_y = icon_rect.y + std::max(0, (icon_rect.h - glyph.y) / 2);
    DMFontCache::instance().draw_text(renderer, icon_style, icon_text, text_x, text_y);

    if (!DMWidgetTooltipShouldDisplay(state, SDL_GetTicks())) {
        return;
    }

    const auto text_style = tooltip_text_label_style();
    SDL_Point text_size = DMFontCache::instance().measure_text(text_style, state.text);
    const int box_w = text_size.x + kTooltipBoxPadding * 2;
    const int box_h = text_size.y + kTooltipBoxPadding * 2;
    const int bounds_right = bounds.x + bounds.w;
    const int bounds_bottom = bounds.y + bounds.h;

    int box_x = icon_rect.x + icon_rect.w - box_w;
    box_x = std::clamp(box_x, bounds.x + kTooltipIconPadding, bounds_right - box_w);
    int box_y = icon_rect.y + icon_rect.h + kTooltipBoxMargin;
    if (box_y + box_h > bounds_bottom) {
        box_y = icon_rect.y - kTooltipBoxMargin - box_h;
    }
    if (box_y < bounds.y) {
        box_y = bounds.y;
        if (box_y + box_h > bounds_bottom) {
            box_y = bounds_bottom - box_h;
        }
    }

    SDL_Rect tooltip_rect{box_x, box_y, box_w, box_h};
    dm_draw::DrawBeveledRect(renderer, tooltip_rect, kTooltipCornerRadius, 1, tooltip_box_background(), DMStyles::HighlightColor(), DMStyles::ShadowColor(), false, DMStyles::HighlightIntensity() * 0.5f, DMStyles::ShadowIntensity() * 0.5f);
    dm_draw::DrawRoundedOutline(renderer, tooltip_rect, kTooltipCornerRadius, 1, tooltip_box_border());

    const int text_draw_x = tooltip_rect.x + kTooltipBoxPadding;
    const int text_draw_y = tooltip_rect.y + kTooltipBoxPadding;
    DMFontCache::instance().draw_text(renderer, text_style, state.text, text_draw_x, text_draw_y);
}

void Widget::set_tooltip_text(std::string text) {
    tooltip_state_.text = std::move(text);
    if (tooltip_state_.text.empty()) {
        tooltip_state_.enabled = false;
        DMWidgetTooltipResetHover(tooltip_state_);
    }
}

void Widget::set_tooltip_enabled(bool enabled) {
    tooltip_state_.enabled = enabled && !tooltip_state_.text.empty();
    if (!tooltip_state_.enabled) {
        DMWidgetTooltipResetHover(tooltip_state_);
    }
}

DMButton::DMButton(const std::string& text, const DMButtonStyle* style, int w, int h)
    : rect_{0,0,w,h}, text_(text), style_(style) {
    update_preferred_width();
    rect_.w = std::max(rect_.w, preferred_width_);
}

void DMButton::set_rect(const SDL_Rect& r) {
    rect_ = r;
    rect_.w = std::max(rect_.w, preferred_width_);
}

void DMButton::set_text(const std::string& t) {
    text_ = t;
    update_preferred_width();
    rect_.w = std::max(rect_.w, preferred_width_);
}

void DMButton::set_style(const DMButtonStyle* style) {
    if (style_ == style) {
        return;
    }
    style_ = style;
    update_preferred_width();
    rect_.w = std::max(rect_.w, preferred_width_);
}

void DMButton::set_tooltip_state(DMWidgetTooltipState* state) {
    tooltip_state_ = state;
    if (tooltip_state_) {
        DMWidgetTooltipResetHover(*tooltip_state_);
    }
}

void DMButton::update_preferred_width() {
    if (!style_) {
        preferred_width_ = rect_.w;
        return;
    }
    SDL_Point size = DMFontCache::instance().measure_text(style_->label, text_);
    preferred_width_ = std::max(size.x + kButtonHorizontalPadding, kButtonHorizontalPadding);
}

void DMButton::cancel_interaction() {
    pressed_ = false;
    hovered_ = false;
}

bool DMButton::handle_event(const SDL_Event& e) {
    if (tooltip_state_ && DMWidgetTooltipHandleEvent(e, rect_, *tooltip_state_)) {
        return true;
    }
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        SDL_Point p = sdl_mouse_util::MotionPoint(e.motion);
        hovered_ = SDL_PointInRect(&p, &rect_);
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
        if (SDL_PointInRect(&p, &rect_)) { pressed_ = true; return true; }
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
        bool inside = SDL_PointInRect(&p, &rect_);
        bool was = pressed_;
        pressed_ = false;
        return inside && was;
    }
    return false;
}

void DMButton::draw_label(SDL_Renderer* r, SDL_Color col) const {
    if (!style_) return;
    DMLabelStyle label_style{ style_->label.font_path, style_->label.font_size, col };
    SDL_Point size = DMFontCache::instance().measure_text(label_style, text_);
    int draw_x = rect_.x + (rect_.w - size.x) / 2;
    int draw_y = rect_.y + (rect_.h - size.y) / 2;
    DMFontCache::instance().draw_text(r, label_style, text_, draw_x, draw_y);
}

void DMButton::render(SDL_Renderer* r) const {
    if (!style_) return;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_Rect button_rect = rect_;
    const int corner_radius = DMStyles::CornerRadius();

    const SDL_Color base = pressed_ ? style_->press_bg : (hovered_ ? style_->hover_bg : style_->bg);
    const float top_bias = hovered_ ? 0.06f : 0.04f;
    const float bottom_bias = pressed_ ? 0.16f : 0.10f;
    const SDL_Color top_color = dm_draw::LightenColor(base, top_bias);
    const SDL_Color bottom_color = dm_draw::DarkenColor(base, bottom_bias);

    SDL_Rect shadow_rect = button_rect;
    shadow_rect.y += 1;
    SDL_Color shadow_color = DMStyles::ShadowColor();
    shadow_color.a = static_cast<Uint8>(std::clamp<int>(static_cast<int>(shadow_color.a * 0.22f), 0, 255));
    dm_draw::DrawRoundedSolidRect(r, shadow_rect, corner_radius, shadow_color);

    if (hovered_) {
        SDL_Rect glow_rect = button_rect;
        glow_rect.x -= 1;
        glow_rect.y -= 1;
        glow_rect.w += 2;
        glow_rect.h += 2;
        SDL_Color glow = DMStyles::HighlightColor();
        glow.a = static_cast<Uint8>(std::clamp<int>(static_cast<int>(glow.a * (pressed_ ? 0.12f : 0.22f)), 0, 255));
        dm_draw::DrawRoundedSolidRect(r, glow_rect, corner_radius + 2, glow);
    }

    dm_draw::DrawRoundedGradientRect(r, button_rect, corner_radius, top_color, bottom_color);

    SDL_Color border = style_->border;
    if (hovered_ || pressed_) {
        border = DMStyles::ButtonFocusOutline();
    }
    dm_draw::DrawRoundedOutline(r, button_rect, corner_radius, kControlOutlineThickness, border);

    draw_label(r, style_->text);
    if (tooltip_state_) {
        DMWidgetTooltipRender(r, rect_, *tooltip_state_);
    }
}

DMTextBox::DMTextBox(const std::string& label, const std::string& value)
    : label_(label), default_label_(label), text_(value), caret_pos_(value.size()) {}

void DMTextBox::set_rect(const SDL_Rect& r) {
    rect_ = r;
    update_geometry(false);
}

void DMTextBox::set_value(const std::string& v) {
    bool value_changed = (text_ != v);
    text_ = v;
    caret_pos_ = std::min(caret_pos_, text_.size());
    if (value_changed) {
        update_geometry(true);
    }
}

void DMTextBox::set_tooltip_state(DMWidgetTooltipState* state) {
    tooltip_state_ = state;
    if (tooltip_state_) {
        DMWidgetTooltipResetHover(*tooltip_state_);
    }
}

void DMTextBox::start_editing() {
    if (editing_) {
        return;
    }
    editing_ = true;
    caret_pos_ = text_.size();
    SDL_StartTextInput(SDL_GetKeyboardFocus());
}

void DMTextBox::stop_editing() {
    if (!editing_) {
        return;
    }
    editing_ = false;
    SDL_StopTextInput(SDL_GetKeyboardFocus());
}

int DMTextBox::height_for_width(int w) const {
    return preferred_height(w);
}

void DMTextBox::set_on_height_changed(std::function<void()> cb) {
    on_height_changed_ = std::move(cb);
}

void DMTextBox::set_label_text(const std::string& label) {
    if (label_ == label) {
        return;
    }
    label_ = label;
    update_geometry(true);
}

void DMTextBox::reset_label_text() {
    set_label_text(default_label_);
}

void DMTextBox::set_label_color_override(const SDL_Color& color) {
    label_color_override_ = color;
}

void DMTextBox::clear_label_color_override() {
    label_color_override_.reset();
}

bool DMTextBox::handle_event(const SDL_Event& e) {
    if (tooltip_state_ && DMWidgetTooltipHandleEvent(e, rect_, *tooltip_state_)) {
        return true;
    }
    bool changed = false;
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        SDL_Point p{ static_cast<int>(std::lround(e.motion.x)), static_cast<int>(std::lround(e.motion.y)) };
        hovered_ = SDL_PointInRect(&p, &box_rect_);
        // Consume mouse motion when inside the textbox to block propagation
        if (hovered_) {
            return true;
        }
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point p{ static_cast<int>(std::lround(e.button.x)), static_cast<int>(std::lround(e.button.y)) };
        bool inside = SDL_PointInRect(&p, &box_rect_);
        if (inside) {
            if (!editing_) {
                editing_ = true;
                SDL_StartTextInput(SDL_GetKeyboardFocus());
            }
            caret_pos_ = caret_pos_from_point(p.x, p.y);
            // Consume click inside textbox to block propagation
            return true;
        } else if (editing_) {
            editing_ = false;
            SDL_StopTextInput(SDL_GetKeyboardFocus());
        }
    } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        // Consume scroll wheel events when hovering over the textbox
        // to prevent them from propagating to underlying widgets
        SDL_Point mouse{0, 0};
        sdl_mouse_util::GetMouseState(&mouse.x, &mouse.y);
        if (SDL_PointInRect(&mouse, &box_rect_) || SDL_PointInRect(&mouse, &rect_)) {
            return true;
        }
    } else if (editing_ && e.type == SDL_EVENT_TEXT_INPUT) {
        text_.insert(caret_pos_, e.text.text);
        caret_pos_ += std::strlen(e.text.text);
        changed = true;
    } else if (editing_ && e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.key == SDLK_BACKSPACE) {
            if (caret_pos_ > 0 && !text_.empty()) {
                size_t erase_pos = caret_pos_ - 1;
                text_.erase(erase_pos, 1);
                caret_pos_ = erase_pos;
                changed = true;
            }
        } else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) {
            editing_ = false; SDL_StopTextInput(SDL_GetKeyboardFocus());
        } else if (e.key.key == SDLK_DELETE) {
            if (caret_pos_ < text_.size()) {
                text_.erase(caret_pos_, 1);
                changed = true;
            }
        } else if (e.key.key == SDLK_LEFT) {
            if (caret_pos_ > 0) --caret_pos_;
        } else if (e.key.key == SDLK_RIGHT) {
            if (caret_pos_ < text_.size()) ++caret_pos_;
        } else if (e.key.key == SDLK_HOME) {
            caret_pos_ = 0;
        } else if (e.key.key == SDLK_END) {
            caret_pos_ = text_.size();
        } else if (e.key.key == SDLK_ESCAPE) {
            editing_ = false; SDL_StopTextInput(SDL_GetKeyboardFocus());
        }
    }
    if (changed) {
        update_geometry(true);
    }
    return changed;
}

size_t DMTextBox::caret_pos_from_point(int mouse_x, int mouse_y) const {
    const DMLabelStyle label_style = DMStyles::Label();
    TTF_Font* font = DMFontCache::instance().get_font(label_style.font_path, label_style.font_size);
    if (!font || text_.empty()) {
        return text_.size();
    }

    const int content_w = std::max(1, box_rect_.w - 16);
    const std::vector<std::string> lines = wrap_lines(font, text_, content_w);
    if (lines.empty()) {
        return 0;
    }

    int line_height = 0;
    if (!TTF_GetStringSize(font, "A", 1, nullptr, &line_height) || line_height <= 0) {
        line_height = 18;
    }
    const int local_y = std::max(0, mouse_y - (box_rect_.y + 6));
    const size_t line_index = std::min(lines.size() - 1, static_cast<size_t>(local_y / std::max(1, line_height)));

    size_t char_offset = 0;
    for (size_t i = 0; i < line_index; ++i) {
        char_offset += lines[i].size();
    }

    const std::string& target_line = lines[line_index];
    const int local_x = mouse_x - (box_rect_.x + 8);
    if (local_x <= 0) {
        return char_offset;
    }

    for (size_t i = 0; i <= target_line.size(); ++i) {
        const std::string prefix = target_line.substr(0, i);
        int prefix_w = 0;
        if (!prefix.empty()) {
            TTF_GetStringSize(font, prefix.c_str(), static_cast<int>(prefix.size()), &prefix_w, nullptr);
        }
        if (prefix_w >= local_x) {
            return char_offset + i;
        }
    }
    return char_offset + target_line.size();
}

void DMTextBox::draw_text(SDL_Renderer* r, const std::string& s, int x, int y, int max_width, const DMLabelStyle& ls) const {
    TTF_Font* f = DMFontCache::instance().get_font(ls.font_path, ls.font_size);
    if (!f) return;
    const int content_w = std::max(1, max_width);
    auto lines = wrap_lines(f, s, content_w);
    int line_y = y;
    const int gap = DMSpacing::small_gap();
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto& line = lines[i];
        SDL_Surface* surf = ttf_util::RenderTextBlended(f, line.c_str(), ls.color);
        if (surf) {
            SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
            if (tex) {
                SDL_Rect dst{ x, line_y, surf->w, surf->h };
                sdl_render::Texture(r, tex, nullptr, &dst);
                SDL_DestroyTexture(tex);
            }
            line_y += surf->h;
            if (i + 1 < lines.size()) line_y += gap;
            SDL_DestroySurface(surf);
        }
    }
}

void DMTextBox::render(SDL_Renderer* r) const {
    const DMTextBoxStyle& st = DMStyles::TextBox();
    if (!label_.empty() && label_height_ > 0) {
        DMLabelStyle lbl = DMStyles::Label();
        if (label_color_override_) {
            lbl.color = *label_color_override_;
        }
        draw_text(r, label_, label_rect_.x, label_rect_.y, label_rect_.w, lbl);
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    const SDL_Color fill = (hovered_ || editing_) ? DMStyles::TextboxHoverFill() : DMStyles::TextboxBaseFill();
    dm_draw::DrawBeveledRect( r, box_rect_, DMStyles::CornerRadius(), DMStyles::BevelDepth(), fill, DMStyles::HighlightColor(), DMStyles::ShadowColor(), false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());

    SDL_Color border = st.border;
    if (hovered_ && !editing_) {
        border = DMStyles::TextboxHoverOutline();
    }
    if (editing_) {
        const SDL_Color focus = DMStyles::TextboxFocusOutline();
        dm_draw::DrawRoundedFocusRing( r, box_rect_, DMStyles::CornerRadius(), kFocusRingThickness, focus);
        border = DMStyles::TextboxActiveOutline();
    }
    dm_draw::DrawRoundedOutline( r, box_rect_, DMStyles::CornerRadius(), kControlOutlineThickness, border);
    DMLabelStyle valStyle{ st.label.font_path, st.label.font_size, st.text };
    draw_text(r, text_, box_rect_.x + kTextboxHorizontalPadding, box_rect_.y + kTextboxHorizontalPadding, std::max(1, box_rect_.w - 2 * kTextboxHorizontalPadding), valStyle);
    if (editing_) {
        TTF_Font* f = DMFontCache::instance().get_font(valStyle.font_path, valStyle.font_size);
        if (f) {
            int max_width = std::max(1, box_rect_.w - 2 * kTextboxHorizontalPadding);
            size_t caret_index = std::min(caret_pos_, text_.size());
            std::string prefix = text_.substr(0, caret_index);
            auto lines = wrap_lines(f, prefix, max_width);
            int caret_x = box_rect_.x + kTextboxHorizontalPadding;
            int caret_y = box_rect_.y + kTextboxHorizontalPadding;
            int caret_height = TTF_GetFontHeight(f);
            const int gap = DMSpacing::small_gap();
            if (!lines.empty()) {
            for (size_t i = 0; i < lines.size(); ++i) {
                const std::string& line = lines[i];
                int w = 0, h = 0;
                if (!line.empty()) {
                    ttf_util::GetStringSize(f, line, &w, &h);
                } else {
                    w = 0; h = TTF_GetFontHeight(f);
                }
                if (i + 1 < lines.size()) {
                    caret_y += h + gap;
                    } else {
                        caret_x += w;
                        caret_height = (h > 0) ? h : TTF_GetFontHeight(f);
                    }
                }
            }
            const SDL_Color caret = DMStyles::TextCaretColor();
            SDL_SetRenderDrawColor(r, caret.r, caret.g, caret.b, caret.a);
            SDL_RenderLine(r, caret_x, caret_y, caret_x, caret_y + caret_height);
        }
    }
    if (tooltip_state_) {
        DMWidgetTooltipRender(r, rect_, *tooltip_state_);
    }
}

std::vector<std::string> DMTextBox::wrap_lines(TTF_Font* f, const std::string& s, int max_width) const {
    std::vector<std::string> out;
    if (!f) return out;
    size_t start = 0;
    auto push_wrapped = [&](const std::string& para) {
        if (para.empty()) { out.emplace_back(""); return; }
        size_t pos = 0;
        while (pos < para.size()) {
            size_t best_break = pos;
            size_t last_space = std::string::npos;
            bool consumed_all = false;
            for (size_t i = pos; i <= para.size(); ++i) {
                std::string trial = para.substr(pos, i - pos);
                int w=0,h=0; ttf_util::GetStringSize(f, trial, &w, &h);
                if (w <= max_width) {
                    best_break = i;
                    if (i < para.size() && std::isspace((unsigned char)para[i])) last_space = i;
                    if (i == para.size()) { consumed_all = true; break; }
                } else break;
            }
            size_t brk = best_break;
            if (!consumed_all && brk > pos && last_space != std::string::npos && last_space >= pos) {
                brk = (brk > last_space) ? std::min(para.size(), last_space + 1) : last_space;
            }
            if (brk == pos) brk = std::min(para.size(), pos + 1);
            std::string ln = para.substr(pos, brk - pos);
            out.push_back(ln);
            pos = brk;
        }
};
    while (true) {
        size_t nl = s.find('\n', start);
        if (nl == std::string::npos) { push_wrapped(s.substr(start)); break; }
        push_wrapped(s.substr(start, nl - start));
        start = nl + 1;
    }
    if (out.empty()) out.emplace_back("");
    return out;
}

int DMTextBox::preferred_height(int width) const {
    int label_h = compute_label_height(width);
    int box_h   = compute_box_height(width);
    return kBoxTopPadding + label_h + (label_h > 0 ? kLabelControlGap : 0) + box_h + kBoxBottomPadding;
}

int DMTextBox::compute_label_height(int width) const {
    if (label_.empty()) return 0;
    DMLabelStyle lbl = DMStyles::Label();
    TTF_Font* f = DMFontCache::instance().get_font(lbl.font_path, lbl.font_size);
    if (!f) return lbl.font_size;
    auto lines = wrap_lines(f, label_, std::max(1, width));
    int total = 0;
    const int gap = DMSpacing::small_gap();
    for (size_t i = 0; i < lines.size(); ++i) {
        int w = 0, h = 0;
        ttf_util::GetStringSize(f, lines[i], &w, &h);
        total += h;
        if (i + 1 < lines.size()) total += gap;
    }
    return total;
}

int DMTextBox::compute_text_height(TTF_Font* f, int width) const {
    if (!f) {
        return 0;
    }
    auto lines = wrap_lines(f, text_, std::max(1, width));
    if (lines.empty()) {
        return TTF_GetFontHeight(f);
    }
    const int gap = DMSpacing::small_gap();
    int total = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        int line_height = 0;
        if (!line.empty()) {
            int w = 0;
            if (!ttf_util::GetStringSize(f, line, &w, &line_height)) {
                line_height = 0;
            }
        }
        if (line_height <= 0) {
            line_height = TTF_GetFontHeight(f);
        }
        total += line_height;
        if (i + 1 < lines.size()) {
            total += gap;
        }
    }
    return total;
}

int DMTextBox::compute_box_height(int width) const {
    const DMTextBoxStyle& st = DMStyles::TextBox();
    TTF_Font* f = DMFontCache::instance().get_font(st.label.font_path, st.label.font_size);
    int content_width = std::max(1, width - 2 * kTextboxHorizontalPadding);
    int text_height = compute_text_height(f, content_width);
    if (text_height <= 0) {
        text_height = f ? TTF_GetFontHeight(f) : st.label.font_size;
    }
    int padded_height = text_height + 2 * kTextboxHorizontalPadding;
    return std::max(DMTextBox::height(), padded_height);
}

bool DMTextBox::update_geometry(bool notify_change) {
    int previous_height = rect_.h;
    rect_.w = std::max(0, rect_.w);
    label_height_ = compute_label_height(rect_.w);
    int y = rect_.y + kBoxTopPadding;
    label_rect_ = SDL_Rect{ rect_.x, y, rect_.w, label_height_ };
    int control_y = y + label_height_ + (label_height_ > 0 ? kLabelControlGap : 0);
    int control_h = compute_box_height(rect_.w);
    box_rect_ = SDL_Rect{ rect_.x, control_y, rect_.w, control_h };
    rect_.h = (box_rect_.y - rect_.y) + box_rect_.h + kBoxBottomPadding;
    bool height_changed = (rect_.h != previous_height);
    if (notify_change && height_changed && on_height_changed_) {
        on_height_changed_();
    }
    return height_changed;
}

SDL_Rect DMTextBox::box_rect() const {
    return box_rect_;
}

SDL_Rect DMTextBox::label_rect() const {
    return label_rect_;
}

DMCheckbox::DMCheckbox(const std::string& label, bool value)
    : label_(label), value_(value) {}

void DMCheckbox::set_rect(const SDL_Rect& r) { rect_ = r; }

void DMCheckbox::set_label(const std::string& label) {
    label_ = label;
}

void DMCheckbox::set_tooltip_state(DMWidgetTooltipState* state) {
    tooltip_state_ = state;
    if (tooltip_state_) {
        DMWidgetTooltipResetHover(*tooltip_state_);
    }
}

int DMCheckbox::preferred_width() const {
    const DMCheckboxStyle& st = DMStyles::Checkbox();
    SDL_Point label_size = DMFontCache::instance().measure_text(st.label, label_);
    int box_size = rect_.h > 0 ? rect_.h : height();
    int gap = label_size.x > 0 ? kCheckboxLabelGap : 0;
    return box_size + gap + label_size.x;
}

bool DMCheckbox::handle_event(const SDL_Event& e) {
    if (tooltip_state_ && DMWidgetTooltipHandleEvent(e, rect_, *tooltip_state_)) {
        return true;
    }
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        SDL_Point p = sdl_mouse_util::MotionPoint(e.motion);
        hovered_ = SDL_PointInRect(&p, &rect_);
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
        if (SDL_PointInRect(&p, &rect_)) { value_ = !value_; return true; }
    }
    return false;
}

void DMCheckbox::draw_label(SDL_Renderer* r) const {
    const DMCheckboxStyle& st = DMStyles::Checkbox();
    SDL_Point size = DMFontCache::instance().measure_text(st.label, label_);
    int draw_x = rect_.x + rect_.h + kCheckboxLabelGap;
    int draw_y = rect_.y + (rect_.h - size.y) / 2;
    DMFontCache::instance().draw_text(r, st.label, label_, draw_x, draw_y);
}

void DMCheckbox::render(SDL_Renderer* r) const {
    const DMCheckboxStyle& st = DMStyles::Checkbox();
    SDL_Rect box{ rect_.x, rect_.y, rect_.h, rect_.h };
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    const SDL_Color fill = hovered_ ? DMStyles::CheckboxHoverFill() : DMStyles::CheckboxBaseFill();
    dm_draw::DrawBeveledRect( r, box, DMStyles::CornerRadius(), DMStyles::BevelDepth(), fill, DMStyles::HighlightColor(), DMStyles::ShadowColor(), false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());

    SDL_Color border = DMStyles::CheckboxOutlineColor();
    if (hovered_) {
        border = DMStyles::CheckboxHoverOutline();
    }
    if (value_) {
        border = DMStyles::CheckboxActiveOutline();
    }
    dm_draw::DrawRoundedOutline( r, box, DMStyles::CornerRadius(), kControlOutlineThickness, border);
    if (value_) {
        SDL_Color check = DMStyles::CheckboxCheckColor();
        SDL_Rect inner{ box.x + 4, box.y + 4, box.w - 8, box.h - 8 };
        dm_draw::DrawBeveledRect( r, inner, std::min(DMStyles::CornerRadius(), 3), std::max(0, DMStyles::BevelDepth() - 1), check, DMStyles::HighlightColor(), DMStyles::ShadowColor(), false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
    }
    draw_label(r);
    if (tooltip_state_) {
        DMWidgetTooltipRender(r, rect_, *tooltip_state_);
    }
}

DMNumericStepper::DMNumericStepper(const std::string& label, int min_value, int max_value, int value)
    : label_(label) {
    set_range(min_value, max_value);
    set_value(value);
}

DMNumericStepper::~DMNumericStepper() {
    focused_ = false;
    set_slider_scroll_capture(this, false);
}

int DMNumericStepper::clamp_value(int v) const {
    if (min_value_ > max_value_) {
        return v;
    }
    return std::max(min_value_, std::min(max_value_, v));
}

void DMNumericStepper::set_range(int min_value, int max_value) {
    if (min_value > max_value) {
        std::swap(min_value, max_value);
    }
    min_value_ = min_value;
    max_value_ = max_value;
    value_ = clamp_value(value_);
}

void DMNumericStepper::set_step(int step) {
    if (step <= 0) {
        step = 1;
    }
    step_ = step;
}

void DMNumericStepper::set_value(int v) {
    value_ = clamp_value(v);
}

void DMNumericStepper::set_rect(const SDL_Rect& r) {
    rect_ = r;
    rect_.h = std::max(rect_.h, height());
    update_layout();
}

void DMNumericStepper::set_tooltip_state(DMWidgetTooltipState* state) {
    tooltip_state_ = state;
    if (tooltip_state_) {
        DMWidgetTooltipResetHover(*tooltip_state_);
    }
}

void DMNumericStepper::update_layout() {
    label_height_ = compute_label_height(rect_.w);

    const int control_width_min = kNumericStepperButtonWidth * 2 + kNumericStepperValueMinWidth;
    int control_w = std::min(rect_.w, control_width_min);
    if (rect_.w < control_width_min) {
        control_w = std::max(0, rect_.w);
    }

    int label_gap = DMSpacing::small_gap();
    int label_w = std::max(0, rect_.w - control_w - label_gap);
    if (label_w <= 0) {
        label_gap = 0;
        label_w = std::max(0, rect_.w - control_w);
    }

    int label_y = rect_.y + (rect_.h - label_height_) / 2;
    label_rect_ = SDL_Rect{ rect_.x, label_y, label_w, label_height_ };

    int control_x = rect_.x + rect_.w - control_w;
    int control_y = rect_.y + (rect_.h - kNumericStepperHeight) / 2;
    control_rect_ = SDL_Rect{ control_x, control_y, control_w, kNumericStepperHeight };

    int button_space = std::max(0, control_rect_.w - kNumericStepperValueMinWidth);
    int button_w = button_space / 2;
    button_w = std::clamp(button_w, 0, kNumericStepperButtonWidth);
    if (button_w <= 0 && control_rect_.w > 0) {
        button_w = std::max(0, control_rect_.w / 4);
    }
    int value_w = std::max(0, control_rect_.w - button_w * 2);
    if (value_w <= 0 && control_rect_.w > 0) {
        value_w = std::max(0, control_rect_.w / 2);
        button_w = (control_rect_.w - value_w) / 2;
    }

    dec_rect_ = SDL_Rect{ control_rect_.x, control_rect_.y, button_w, control_rect_.h };
    value_rect_ = SDL_Rect{ dec_rect_.x + dec_rect_.w, control_rect_.y, value_w, control_rect_.h };
    inc_rect_ = SDL_Rect{ value_rect_.x + value_rect_.w, control_rect_.y, button_w, control_rect_.h };
}

void DMNumericStepper::update_hover(SDL_Point p) {
    hovered_dec_ = SDL_PointInRect(&p, &dec_rect_);
    hovered_inc_ = SDL_PointInRect(&p, &inc_rect_);
    hovered_value_ = SDL_PointInRect(&p, &value_rect_);
}

bool DMNumericStepper::apply_delta(int delta_steps) {
    if (delta_steps == 0) {
        return false;
    }
    const long long raw = static_cast<long long>(value_) + static_cast<long long>(delta_steps) * static_cast<long long>(step_);
    int proposed;
    if (raw > static_cast<long long>(std::numeric_limits<int>::max())) {
        proposed = std::numeric_limits<int>::max();
    } else if (raw < static_cast<long long>(std::numeric_limits<int>::min())) {
        proposed = std::numeric_limits<int>::min();
    } else {
        proposed = static_cast<int>(raw);
    }
    proposed = clamp_value(proposed);
    if (proposed == value_) {
        return false;
    }
    commit_value(proposed);
    return true;
}

void DMNumericStepper::commit_value(int new_value) {
    int clamped = clamp_value(new_value);
    if (clamped == value_) {
        value_ = clamped;
        return;
    }
    value_ = clamped;
    if (on_change_) {
        on_change_(value_);
    }
}

bool DMNumericStepper::handle_event(const SDL_Event& e) {
    if (tooltip_state_ && DMWidgetTooltipHandleEvent(e, rect_, *tooltip_state_)) {
        return true;
    }
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        SDL_Point p = sdl_mouse_util::MotionPoint(e.motion);
        update_hover(p);
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
        const bool inside = SDL_PointInRect(&p, &rect_);
        if (focused_ != inside) {
            focused_ = inside;
            set_slider_scroll_capture(this, focused_);
        }
        if (!inside) {
            pressed_dec_ = false;
            pressed_inc_ = false;
            return false;
        }
        update_hover(p);
        if (hovered_dec_) {
            pressed_dec_ = true;
            return true;
        }
        if (hovered_inc_) {
            pressed_inc_ = true;
            return true;
        }
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
        if (!SDL_PointInRect(&p, &rect_) && focused_) {
            focused_ = false;
            set_slider_scroll_capture(this, false);
        }
        update_hover(p);
        bool used = false;
        bool had_dec = pressed_dec_;
        bool had_inc = pressed_inc_;
        if (pressed_dec_) {
            bool inside = SDL_PointInRect(&p, &dec_rect_);
            pressed_dec_ = false;
            if (inside) {
                used = apply_delta(-1) || used;
            }
        }
        if (pressed_inc_) {
            bool inside = SDL_PointInRect(&p, &inc_rect_);
            pressed_inc_ = false;
            if (inside) {
                used = apply_delta(1) || used;
            }
        }
        return used || had_dec || had_inc;
    } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        if (!focused_) {
            return false;
        }
        SDL_Point mouse{0, 0};
        sdl_mouse_util::GetMouseState(&mouse.x, &mouse.y);
        if (!SDL_PointInRect(&mouse, &rect_)) {
            return false;
        }
        int delta = e.wheel.integer_y;
        if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            delta = -delta;
        }
        if (delta == 0) {
            delta = static_cast<int>(std::round(e.wheel.y));
        }
        if (delta == 0) {
            return false;
        }
        return apply_delta(delta);
    }
    return false;
}

int DMNumericStepper::compute_label_height(int width) const {
    if (label_.empty()) {
        return 0;
    }
    (void)width;
    const DMSliderStyle& st = DMStyles::Slider();
    SDL_Point size = DMFontCache::instance().measure_text(st.label, label_);
    return size.y;
}

int DMNumericStepper::preferred_height(int width) const {
    (void)width;
    return height();
}

int DMNumericStepper::height() {
    return kNumericStepperHeight;
}

void DMNumericStepper::render(SDL_Renderer* r) const {
    const DMSliderStyle& slider_style = DMStyles::Slider();
    if (!label_.empty() && label_rect_.w > 0 && label_rect_.h > 0) {
        DMFontCache::instance().draw_text(r, slider_style.label, label_, label_rect_.x, label_rect_.y);
    }

    auto draw_button = [&](const SDL_Rect& rect, bool hovered, bool pressed, std::string_view symbol) {
        if (rect.w <= 0 || rect.h <= 0) {
            return;
        }
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_Color fill = DMStyles::ButtonBaseFill();
        if (pressed) {
            fill = DMStyles::ButtonPressedFill();
        } else if (hovered) {
            fill = DMStyles::ButtonHoverFill();
        }
        const SDL_Color& highlight = DMStyles::HighlightColor();
        const SDL_Color& shadow = DMStyles::ShadowColor();
        const int radius = std::min(DMStyles::CornerRadius(), std::min(rect.w, rect.h) / 2);
        const int bevel = std::min(DMStyles::BevelDepth(), std::max(0, std::min(rect.w, rect.h) / 2));
        dm_draw::DrawBeveledRect( r, rect, radius, bevel, fill, highlight, shadow, false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
        dm_draw::DrawRoundedOutline( r, rect, radius, kControlOutlineThickness, DMStyles::Border());
        if (!symbol.empty()) {
            std::string text(symbol);
            SDL_Point size = DMFontCache::instance().measure_text(slider_style.label, text);
            int text_x = rect.x + (rect.w - size.x) / 2;
            int text_y = rect.y + (rect.h - size.y) / 2;
            DMFontCache::instance().draw_text(r, slider_style.label, text, text_x, text_y);
        }
};

    draw_button(dec_rect_, hovered_dec_, pressed_dec_, "-");
    draw_button(inc_rect_, hovered_inc_, pressed_inc_, "+");

    if (value_rect_.w > 0 && value_rect_.h > 0) {
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_Color fill = hovered_value_ ? DMStyles::TextboxHoverFill() : DMStyles::TextboxBaseFill();
        const SDL_Color& highlight = DMStyles::HighlightColor();
        const SDL_Color& shadow = DMStyles::ShadowColor();
        const int radius = std::min(DMStyles::CornerRadius(), std::min(value_rect_.w, value_rect_.h) / 2);
        const int bevel = std::min(DMStyles::BevelDepth(), std::max(0, std::min(value_rect_.w, value_rect_.h) / 2));
        dm_draw::DrawBeveledRect( r, value_rect_, radius, bevel, fill, highlight, shadow, false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
        SDL_Color border = hovered_value_ ? DMStyles::TextboxHoverOutline() : DMStyles::Border();
        dm_draw::DrawRoundedOutline( r, value_rect_, radius, kControlOutlineThickness, border);

        std::string value_text = std::to_string(value_);
        SDL_Point size = DMFontCache::instance().measure_text(slider_style.label, value_text);
        int text_x = value_rect_.x + (value_rect_.w - size.x) / 2;
        int text_y = value_rect_.y + (value_rect_.h - size.y) / 2;
        DMFontCache::instance().draw_text(r, slider_style.label, value_text, text_x, text_y);
    }
    if (tooltip_state_) {
        DMWidgetTooltipRender(r, rect_, *tooltip_state_);
    }
}

DMSlider::DMSlider(const std::string& label, int min_val, int max_val, int value)
    : label_(label), min_(min_val), max_(max_val), value_(value) {
    if (min_ > max_) {
        std::swap(min_, max_);
    }
    set_value(value_);
}

DMSlider::~DMSlider() {
    commit_pending_value();
    focused_ = false;
    set_slider_scroll_capture(this, false);
}

int DMSlider::clamp_value(int v) const {
    if (min_ <= max_) {
        return std::max(min_, std::min(max_, v));
    }
    return std::min(min_, std::max(max_, v));
}

bool DMSlider::apply_interaction_value(int v) {
    int clamped = clamp_value(v);
    if (!defer_commit_until_unfocus_) {
        int prev = value_;
        value_ = clamped;
        pending_value_ = value_;
        has_pending_value_ = false;
        if (value_ != prev) {
            notify_value_changed();
        }
        return value_ != prev;
    }
    int prev_display = pending_value_;
    pending_value_ = clamped;
    has_pending_value_ = (pending_value_ != value_);
    if (pending_value_ != prev_display) {
        notify_value_changed();
    }
    return pending_value_ != prev_display;
}

bool DMSlider::commit_pending_value() {
    if (!defer_commit_until_unfocus_ || !has_pending_value_) {
        return false;
    }
    has_pending_value_ = false;
    if (value_ == pending_value_) {
        return false;
    }
    value_ = pending_value_;
    notify_value_changed();
    return true;
}

int DMSlider::display_value() const {
    return defer_commit_until_unfocus_ ? pending_value_ : value_;
}

void DMSlider::set_rect(const SDL_Rect& r) {
    rect_ = r;
    label_height_ = compute_label_height(rect_.w);
    const int header_height = std::max(label_height_, slider_value_height());
    const int header_y = rect_.y + kBoxTopPadding;
    const int value_gap = DMSpacing::small_gap();

    int value_w = std::min(kSliderValueWidth, rect_.w);
    value_w = std::max(0, std::min(value_w, rect_.w));

    int label_w = std::max(0, rect_.w - value_w - value_gap);
    if (label_w <= 0) {
        value_w = std::min(rect_.w, value_w);
        label_w = std::max(0, rect_.w - value_w);
    }
    if (label_height_ <= 0 || label_.empty()) {
        label_w = 0;
    }

    int label_y = header_y + (header_height - label_height_) / 2;
    label_rect_ = SDL_Rect{ rect_.x, label_y, label_w, label_height_ };

    int value_y = header_y + (header_height - slider_value_height()) / 2;
    int value_x = rect_.x + rect_.w - std::max(value_w, 0);
    if (label_height_ > 0 && label_w > 0) {
        value_x = rect_.x + label_w + value_gap;
    }
    value_rect_ = SDL_Rect{ value_x, value_y, std::max(value_w, 0), slider_value_height() };
    if (value_rect_.x + value_rect_.w > rect_.x + rect_.w) {
        value_rect_.x = rect_.x + rect_.w - value_rect_.w;
    }

    int content_y = header_y + header_height + kLabelControlGap;
    int available = rect_.h - (content_y - rect_.y) - kBoxBottomPadding;
    int content_h = std::max(kSliderControlHeight, available);
    content_rect_ = SDL_Rect{ rect_.x, content_y, rect_.w, content_h };

    if (edit_box_) {
        edit_box_->set_rect(value_rect_);
    }

    rect_.h = (content_rect_.y - rect_.y) + content_rect_.h + kBoxBottomPadding;
}

void DMSlider::set_tooltip_state(DMWidgetTooltipState* state) {
    tooltip_state_ = state;
    if (tooltip_state_) {
        DMWidgetTooltipResetHover(*tooltip_state_);
    }
}

void DMSlider::set_on_value_changed(std::function<void(int)> callback) {
    value_changed_callback_ = std::move(callback);
    last_notified_value_ = display_value();
}

void DMSlider::notify_value_changed() {
    if (!value_changed_callback_) {
        return;
    }
    int current = display_value();
    if (current == last_notified_value_) {
        return;
    }
    last_notified_value_ = current;
    value_changed_callback_(current);
}

void DMSlider::set_enabled(bool enabled) {
    if (enabled_ == enabled) {
        return;
    }
    enabled_ = enabled;
    if (!enabled_) {
        dragging_ = false;
        hovered_ = false;
        knob_hovered_ = false;
        if (focused_) {
            focused_ = false;
            set_slider_scroll_capture(this, false);
            commit_pending_value();
        }
        edit_box_.reset();
    }
}

void DMSlider::set_value(int v) {
    int clamped = clamp_value(v);
    value_ = clamped;
    pending_value_ = clamped;
    has_pending_value_ = false;
    last_notified_value_ = value_;
}

void DMSlider::set_min_value(int v) {
    set_range(v, max_);
}

void DMSlider::set_max_value(int v) {
    set_range(min_, v);
}

void DMSlider::set_range(int min_value, int max_value) {
    min_ = min_value;
    max_ = max_value;
    if (min_ > max_) {
        std::swap(min_, max_);
    }

    value_ = clamp_value(value_);
    pending_value_ = clamp_value(pending_value_);
    if (!defer_commit_until_unfocus_) {
        pending_value_ = value_;
        has_pending_value_ = false;
    } else {
        has_pending_value_ = (pending_value_ != value_);
    }
    last_notified_value_ = display_value();
}

int DMSlider::displayed_value() const {
    return display_value();
}

int DMSlider::label_space() const {
    return label_height_;
}

SDL_Rect DMSlider::content_rect() const {
    return content_rect_;
}

SDL_Rect DMSlider::value_rect() const {
    return value_rect_;
}

SDL_Rect DMSlider::track_rect() const {
    int track_width = std::max(0, content_rect_.w);
    return SDL_Rect{ content_rect_.x, content_rect_.y + content_rect_.h/2 - kSliderTrackThickness / 2, track_width, kSliderTrackThickness };
}

int DMSlider::track_center_y() const {
    SDL_Rect tr = track_rect();
    return tr.y + tr.h / 2;
}

SDL_Rect DMSlider::knob_rect() const {
    SDL_Rect tr = track_rect();
    int usable = std::max(1, tr.w - kSliderKnobWidth);
    int x = tr.x + (int)((display_value() - min_) * usable / (double)(std::max(1, max_ - min_)));
    return SDL_Rect{ x, tr.y - kSliderKnobVerticalInset, kSliderKnobWidth, kSliderKnobHeight };
}

SDL_Rect DMSlider::interaction_rect() const {
    SDL_Rect knob = knob_rect();
    const int pad_x = 8;
    const int pad_y = 6;
    knob.x -= pad_x;
    knob.y -= pad_y;
    knob.w += pad_x * 2;
    knob.h += pad_y * 2;
    const int bounds_x = rect_.x;
    const int bounds_y = rect_.y;
    const int bounds_w = std::max(0, rect_.w);
    const int bounds_h = std::max(0, rect_.h);
    const int bounds_right = bounds_x + bounds_w;
    const int bounds_bottom = bounds_y + bounds_h;
    knob.x = std::clamp(knob.x, bounds_x, bounds_right);
    knob.y = std::clamp(knob.y, bounds_y, bounds_bottom);
    knob.w = std::max(0, std::min(knob.w, bounds_right - knob.x));
    knob.h = std::max(0, std::min(knob.h, bounds_bottom - knob.y));
    return knob;
}

int DMSlider::value_for_x(int x) const {
    SDL_Rect tr = track_rect();
    int usable = std::max(1, tr.w - kSliderKnobWidth);
    double t = (x - tr.x) / (double)usable;
    int range = std::max(1, max_ - min_);
    int v = min_ + (int)std::round(t * range);
    return std::max(min_, std::min(max_, v));
}

bool DMSlider::handle_event(const SDL_Event& e) {
    if (tooltip_state_ && DMWidgetTooltipHandleEvent(e, rect_, *tooltip_state_)) {
        return true;
    }
    if (!enabled_) {
        return false;
    }
    if (e.type == SDL_EVENT_KEY_DOWN && focused_) {
        switch (e.key.key) {
        case SDLK_LEFT:
        case SDLK_A: {
            apply_interaction_value(display_value() - 1);
            return true;
        }
        case SDLK_RIGHT:
        case SDLK_D: {
            apply_interaction_value(display_value() + 1);
            return true;
        }
        case SDLK_RETURN:
        case SDLK_KP_ENTER: {
            commit_pending_value();
            focused_ = false;
            set_slider_scroll_capture(this, false);
            return true;
        }
        default:
            break;
        }
    }
    if (edit_box_) {
        bool was_editing = edit_box_->is_editing();
        bool consumed = edit_box_->handle_event(e);
        bool now_editing = edit_box_->is_editing();
        if (!now_editing) {
            std::optional<int> parsed = parse_value(edit_box_->value());
            if (parsed) {
                set_value(*parsed);
            }
            edit_box_->set_value(format_value(display_value()));
            edit_box_.reset();
            return true;
        }
        if (consumed) {
            return true;
        }
        if (was_editing != now_editing) {
            return true;
        }
    }
    auto set_focus = [this](bool focus) {
        if (focused_ == focus) {
            return;
        }
        focused_ = focus;
        set_slider_scroll_capture(this, focused_);
        if (!focused_) {
            commit_pending_value();
        }
};
    auto update_hover = [this](SDL_Point p) {
        bool inside = SDL_PointInRect(&p, &rect_);
        hovered_ = inside || dragging_;
        if (!inside) {
            if (!dragging_) {
                knob_hovered_ = false;
            }
            return inside;
        }
        if (dragging_) {
            knob_hovered_ = true;
        } else {
            SDL_Rect knob = knob_rect();
            knob_hovered_ = SDL_PointInRect(&p, &knob);
        }
        return inside;
};

    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        SDL_Point p = sdl_mouse_util::MotionPoint(e.motion);
        update_hover(p);
        if (!dragging_ && focused_ && !hovered_) {
            set_focus(false);
        }
        if (dragging_) {
            apply_interaction_value(value_for_x(p.x));
            return true;
        }
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
        bool inside = update_hover(p);
        if (inside) {
            bool was_focused = focused_;
            set_focus(true);
            if (!was_focused) {
                return true;
            }
            SDL_Rect vr = value_rect();
            if (SDL_PointInRect(&p, &vr)) {
                edit_box_ = std::make_unique<DMTextBox>("", format_value(display_value()));
                edit_box_->set_rect(vr);
                edit_box_->handle_event(e);
                return true;
            }
            SDL_Rect tr = track_rect();
            SDL_Rect knob = knob_rect();
            if (SDL_PointInRect(&p, &knob) || SDL_PointInRect(&p, &tr)) {
                dragging_ = true;
                knob_hovered_ = true;
                apply_interaction_value(value_for_x(p.x));
                return true;
            }
            return false;
        } else if (!dragging_) {
            set_focus(false);
        }
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
        bool was_dragging = dragging_;
        dragging_ = false;
        SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
        update_hover(p);
        if (!SDL_PointInRect(&p, &rect_) && focused_) {
            set_focus(false);
        }
        if (was_dragging) {
            return true;
        }
    } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        if (!focused_) {
            return false;
        }
        SDL_Point mouse{0, 0};
        sdl_mouse_util::GetMouseState(&mouse.x, &mouse.y);
        const bool pointer_inside = SDL_PointInRect(&mouse, &rect_);
        if (pointer_inside) {
            update_hover(mouse);
        } else if (!dragging_) {
            hovered_ = false;
            knob_hovered_ = false;
        }
        int delta = e.wheel.integer_y;
        if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            delta = -delta;
        }
        if (delta == 0) {
            delta = static_cast<int>(std::round(e.wheel.y));
        }
        if (delta == 0) {
            return false;
        }
        const int prev_display = display_value();
        if (!apply_interaction_value(prev_display + delta)) {
            return false;
        }
        return display_value() != prev_display;
    }
    return false;
}

void DMSlider::draw_text(SDL_Renderer* r, const std::string& s, int x, int y) const {
    const DMSliderStyle& st = DMStyles::Slider();
    DMFontCache::instance().draw_text(r, st.label, s, x, y);
}

void DMSlider::render(SDL_Renderer* r) const {
    const DMSliderStyle& st = DMStyles::Slider();
    const bool disabled = !enabled_;
    if (!label_.empty() && label_height_ > 0) {
        draw_text(r, label_, label_rect_.x, label_rect_.y);
    }
    const bool active = !disabled && (focused_ || dragging_);
    if (active) {
        const SDL_Color& focus_outline = DMStyles::SliderFocusOutline();
        dm_draw::DrawRoundedFocusRing( r, rect_, DMStyles::CornerRadius(), kFocusRingThickness, focus_outline);
    } else if (!disabled && hovered_) {
        const SDL_Color& hover_outline = DMStyles::SliderHoverOutline();
        dm_draw::DrawRoundedOutline( r, rect_, DMStyles::CornerRadius(), kControlOutlineThickness, hover_outline);
    }
    SDL_Rect tr = track_rect();
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    const SDL_Color& highlight = DMStyles::HighlightColor();
    const SDL_Color& shadow = DMStyles::ShadowColor();
    const int radius = std::min(DMStyles::CornerRadius(), std::min(tr.w, tr.h) / 2);
    const int bevel = std::min(DMStyles::BevelDepth(), std::max(0, std::min(tr.w, tr.h) / 2));
    dm_draw::DrawBeveledRect( r, tr, radius, bevel, DMStyles::SliderTrackBackground(), highlight, shadow, false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
    int range = std::max(1, max_ - min_);
    int current_value = display_value();
    SDL_Rect fill{ tr.x, tr.y, (int)((current_value - min_) * tr.w / (double)range), tr.h };
    if (fill.w > 0) {
        SDL_Rect fill_rect = fill;
        SDL_Color track_fill = active ? st.track_fill_active : st.track_fill;
        if (disabled) {
            track_fill = dm_draw::DarkenColor(track_fill, 0.2f);
        }
        dm_draw::DrawBeveledRect( r, fill_rect, radius, bevel, track_fill, highlight, shadow, false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
    }
    SDL_Rect krect = knob_rect();
    SDL_Color knob_col = st.knob;
    SDL_Color kborder = st.knob_border;
    if (active) {
        knob_col = st.knob_accent;
        kborder = st.knob_accent_border;
    } else if (!disabled && knob_hovered_) {
        knob_col = st.knob_hover;
        kborder = st.knob_border_hover;
    }
    if (disabled) {
        knob_col = dm_draw::DarkenColor(knob_col, 0.25f);
        kborder = dm_draw::DarkenColor(kborder, 0.15f);
    }
    const int knob_radius = std::min(DMStyles::CornerRadius(), std::min(krect.w, krect.h) / 2);
    const int knob_bevel = std::min(DMStyles::BevelDepth(), std::max(0, std::min(krect.w, krect.h) / 2));
    dm_draw::DrawBeveledRect( r, krect, knob_radius, knob_bevel, knob_col, highlight, shadow, false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
    dm_draw::DrawRoundedOutline( r, krect, knob_radius, kKnobOutlineThickness, kborder);
    if (edit_box_) {
        edit_box_->render(r);
    } else {
        SDL_Rect vr = value_rect();
        const std::string& value_text = format_value(current_value);
        SDL_Point size = DMFontCache::instance().measure_text(st.label, value_text);
        int text_x = vr.x + kSliderValueHorizontalPadding;
        int text_y = vr.y + (vr.h - size.y) / 2;
        DMFontCache::instance().draw_text(r, st.label, value_text, text_x, text_y);
    }
    if (disabled) {
        SDL_Color overlay = dm_draw::LightenColor(DMStyles::PanelBG(), 0.12f);
        overlay.a = 180;
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(r, overlay.r, overlay.g, overlay.b, overlay.a);
        sdl_render::FillRect(r, &rect_);
        SDL_Color outline = DMStyles::Border();
        SDL_SetRenderDrawColor(r, outline.r, outline.g, outline.b, 160);
        sdl_render::Rect(r, &rect_);
    }
    if (tooltip_state_) {
        DMWidgetTooltipRender(r, rect_, *tooltip_state_);
    }
}

void DMSlider::set_value_formatter(SliderValueFormatter formatter) {
    value_formatter_ = std::move(formatter);
    formatted_value_cache_.clear();
    if (edit_box_) {
        edit_box_->set_value(format_value(display_value()));
    }
}

void DMSlider::set_value_parser(std::function<std::optional<int>(const std::string&)> parser) {
    value_parser_ = std::move(parser);
}

const std::string& DMSlider::format_value(int v) const {
    auto& stats = slider_format_stats();
    ++stats.format_calls;

    if (value_formatter_) {
        const std::size_t before_capacity = formatted_value_cache_.capacity();
        std::string_view view = value_formatter_(v, value_buffer_);
        if (!view.empty()) {
            formatted_value_cache_.assign(view.data(), view.size());
        } else {
            std::string fallback = std::to_string(v);
            formatted_value_cache_.assign(fallback.data(), fallback.size());
        }
        if (formatted_value_cache_.capacity() > before_capacity) {
            ++stats.allocations;
            SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, "[DMSlider] format allocation grew: before=%zu after=%zu", before_capacity, formatted_value_cache_.capacity());
        }
        stats.log_if_needed();
        return formatted_value_cache_;
    }

    const std::size_t before_capacity = formatted_value_cache_.capacity();
    auto* begin = value_buffer_.data();
    auto* end = value_buffer_.data() + value_buffer_.size();
    auto result = std::to_chars(begin, end, v);
    if (result.ec == std::errc{}) {
        formatted_value_cache_.assign(begin, static_cast<std::size_t>(result.ptr - begin));
    } else {
        std::string fallback = std::to_string(v);
        formatted_value_cache_.assign(fallback.data(), fallback.size());
    }
    if (formatted_value_cache_.capacity() > before_capacity) {
        ++stats.allocations;
        SDL_LogVerbose(SDL_LOG_CATEGORY_APPLICATION, "[DMSlider] integer format allocation grew: before=%zu after=%zu", before_capacity, formatted_value_cache_.capacity());
    }
    stats.log_if_needed();
    return formatted_value_cache_;
}

std::optional<int> DMSlider::parse_value(const std::string& text) const {
    if (value_parser_) {
        return value_parser_(text);
    }
    try {
        return std::stoi(text);
    } catch (...) {
        return std::nullopt;
    }
}

int DMSlider::preferred_height(int width) const {
    int label_h = compute_label_height(width);
    int header_h = std::max(label_h, slider_value_height());
    return kBoxTopPadding + header_h + kLabelControlGap + kSliderControlHeight + kBoxBottomPadding;
}

int DMSlider::compute_label_height(int width) const {
    if (label_.empty()) return 0;
    const DMSliderStyle& st = DMStyles::Slider();
    SDL_Point size = DMFontCache::instance().measure_text(st.label, label_);
    (void)width;
    return size.y;
}

int DMSlider::height() {
    const DMSliderStyle& st = DMStyles::Slider();
    return kBoxTopPadding + st.label.font_size + kLabelControlGap + kSliderControlHeight + kBoxBottomPadding;
}

DMRangeSlider::DMRangeSlider(int min_val, int max_val, int min_value, int max_value)
    : min_(min_val), max_(max_val) {
    if (min_ > max_) std::swap(min_, max_);

    set_max_value(max_value);
    set_min_value(min_value);
}

DMRangeSlider::~DMRangeSlider() {
    commit_pending_values();
    focused_ = false;
    set_slider_scroll_capture(this, false);
}

int DMRangeSlider::clamp_min_value(int v) const {
    int hi = defer_commit_until_unfocus_ ? pending_max_value_ : max_value_;
    hi = std::max(min_, std::min(max_, hi));
    return std::max(min_, std::min(hi, v));
}

int DMRangeSlider::clamp_max_value(int v) const {
    int lo = defer_commit_until_unfocus_ ? pending_min_value_ : min_value_;
    lo = std::max(min_, std::min(max_, lo));
    return std::max(lo, std::min(max_, v));
}

bool DMRangeSlider::apply_min_interaction(int v) {
    int clamped = clamp_min_value(v);
    if (!defer_commit_until_unfocus_) {
        int prev = min_value_;
        min_value_ = clamped;
        if (min_value_ > max_value_) min_value_ = max_value_;
        pending_min_value_ = min_value_;
        pending_max_value_ = max_value_;
        pending_dirty_ = false;
        return min_value_ != prev;
    }
    int prev_display = pending_min_value_;
    pending_min_value_ = clamped;
    if (pending_min_value_ > pending_max_value_) pending_min_value_ = pending_max_value_;
    bool changed = pending_min_value_ != prev_display;
    pending_dirty_ = pending_dirty_ || (pending_min_value_ != min_value_);
    return changed;
}

bool DMRangeSlider::apply_max_interaction(int v) {
    int clamped = clamp_max_value(v);
    if (!defer_commit_until_unfocus_) {
        int prev = max_value_;
        max_value_ = clamped;
        if (max_value_ < min_value_) max_value_ = min_value_;
        pending_min_value_ = min_value_;
        pending_max_value_ = max_value_;
        pending_dirty_ = false;
        return max_value_ != prev;
    }
    int prev_display = pending_max_value_;
    pending_max_value_ = clamped;
    if (pending_max_value_ < pending_min_value_) pending_max_value_ = pending_min_value_;
    bool changed = pending_max_value_ != prev_display;
    pending_dirty_ = pending_dirty_ || (pending_max_value_ != max_value_);
    return changed;
}

bool DMRangeSlider::commit_pending_values() {
    if (!defer_commit_until_unfocus_) {
        return false;
    }
    if (!pending_dirty_ && pending_min_value_ == min_value_ && pending_max_value_ == max_value_) {
        return false;
    }
    pending_dirty_ = false;
    bool changed = false;
    if (min_value_ != pending_min_value_) {
        min_value_ = pending_min_value_;
        changed = true;
    }
    if (max_value_ != pending_max_value_) {
        max_value_ = pending_max_value_;
        changed = true;
    }
    if (min_value_ > max_value_) {
        max_value_ = min_value_;
    }
    return changed;
}

int DMRangeSlider::display_min_value() const {
    return defer_commit_until_unfocus_ ? pending_min_value_ : min_value_;
}

int DMRangeSlider::display_max_value() const {
    return defer_commit_until_unfocus_ ? pending_max_value_ : max_value_;
}

void DMRangeSlider::set_rect(const SDL_Rect& r) {
    rect_ = r;
    const int header_height = slider_value_height();
    const int header_y = rect_.y + kBoxTopPadding;
    const int gap = DMSpacing::small_gap();

    int total_width = std::max(0, rect_.w);
    int available_each = std::max(0, (total_width - gap) / 2);
    int desired = std::min(range_value_width(rect_.w), available_each);
    int label_w = std::max(available_each / 2, desired);
    label_w = std::min(label_w, available_each);

    min_value_rect_ = SDL_Rect{ rect_.x, header_y, label_w, header_height };
    max_value_rect_ = SDL_Rect{ rect_.x + total_width - label_w, header_y, label_w, header_height };

    int content_y = header_y + header_height + kLabelControlGap;
    int available = rect_.h - (content_y - rect_.y) - kBoxBottomPadding;
    int content_h = std::max(kSliderControlHeight, available);
    content_rect_ = SDL_Rect{ rect_.x, content_y, rect_.w, content_h };

    rect_.h = (content_rect_.y - rect_.y) + content_rect_.h + kBoxBottomPadding;

    if (edit_min_) edit_min_->set_rect(min_value_rect_);
    if (edit_max_) edit_max_->set_rect(max_value_rect_);
}

void DMRangeSlider::set_tooltip_state(DMWidgetTooltipState* state) {
    tooltip_state_ = state;
    if (tooltip_state_) {
        DMWidgetTooltipResetHover(*tooltip_state_);
    }
}

void DMRangeSlider::set_min_value(int v) {
    min_value_ = std::max(min_, std::min(max_, v));
    if (min_value_ > max_value_) min_value_ = max_value_;
    pending_min_value_ = min_value_;
    if (!defer_commit_until_unfocus_) {
        pending_max_value_ = max_value_;
    }
    pending_dirty_ = false;
}

void DMRangeSlider::set_max_value(int v) {
    max_value_ = std::max(min_, std::min(max_, v));
    if (max_value_ < min_value_) max_value_ = min_value_;
    pending_max_value_ = max_value_;
    if (!defer_commit_until_unfocus_) {
        pending_min_value_ = min_value_;
    }
    pending_dirty_ = false;
}

SDL_Rect DMRangeSlider::content_rect() const {
    return content_rect_;
}

SDL_Rect DMRangeSlider::track_rect() const {
    int width = std::max(0, content_rect_.w);
    return SDL_Rect{ content_rect_.x, content_rect_.y + content_rect_.h/2 - kSliderTrackThickness / 2, width, kSliderTrackThickness };
}

SDL_Rect DMRangeSlider::min_knob_rect() const {
    SDL_Rect tr = track_rect();
    int usable = std::max(1, tr.w - kSliderKnobWidth);
    int range = std::max(1, max_ - min_);
    int x = tr.x + (int)((display_min_value() - min_) * usable / (double)range);
    return SDL_Rect{ x, tr.y - kSliderKnobVerticalInset, kSliderKnobWidth, kSliderKnobHeight };
}

SDL_Rect DMRangeSlider::max_knob_rect() const {
    SDL_Rect tr = track_rect();
    int usable = std::max(1, tr.w - kSliderKnobWidth);
    int range = std::max(1, max_ - min_);
    int x = tr.x + (int)((display_max_value() - min_) * usable / (double)range);
    return SDL_Rect{ x, tr.y - kSliderKnobVerticalInset, kSliderKnobWidth, kSliderKnobHeight };
}

int DMRangeSlider::value_for_x(int x) const {
    SDL_Rect tr = track_rect();
    double t = (x - tr.x) / (double)(std::max(1, tr.w - kSliderKnobWidth));
    int v = min_ + (int)std::round(t * (max_ - min_));
    return std::max(min_, std::min(max_, v));
}

bool DMRangeSlider::handle_event(const SDL_Event& e) {
    if (tooltip_state_ && DMWidgetTooltipHandleEvent(e, rect_, *tooltip_state_)) {
        return true;
    }
    if (edit_min_) {
        bool was_editing = edit_min_->is_editing();
        bool consumed = edit_min_->handle_event(e);
        bool now_editing = edit_min_->is_editing();
        if (!now_editing) {
            try {
                int nv = std::stoi(edit_min_->value());
                set_min_value(nv);
            } catch (...) {

            }
            edit_min_->set_value(std::to_string(display_min_value()));
            edit_min_.reset();
            return true;
        }
        if (consumed) {
            return true;
        }
        if (was_editing != now_editing) {
            return true;
        }
    }
    if (edit_max_) {
        bool was_editing = edit_max_->is_editing();
        bool consumed = edit_max_->handle_event(e);
        bool now_editing = edit_max_->is_editing();
        if (!now_editing) {
            try {
                int nv = std::stoi(edit_max_->value());
                set_max_value(nv);
            } catch (...) {

            }
            edit_max_->set_value(std::to_string(display_max_value()));
            edit_max_.reset();
            return true;
        }
        if (consumed) {
            return true;
        }
        if (was_editing != now_editing) {
            return true;
        }
    }
    auto set_focus = [this](bool focus) {
        if (focused_ == focus) {
            return;
        }
        focused_ = focus;
        set_slider_scroll_capture(this, focused_);
        if (!focused_) {
            wheel_target_max_ = false;
            commit_pending_values();
        }
};
    auto update_hover = [this](SDL_Point p) {
        if (dragging_min_) {
            min_hovered_ = true;
            max_hovered_ = false;
            wheel_target_max_ = false;
        } else if (dragging_max_) {
            min_hovered_ = false;
            max_hovered_ = true;
            wheel_target_max_ = true;
        }
        bool inside = SDL_PointInRect(&p, &rect_);
        hovered_ = inside || dragging_min_ || dragging_max_;
        if (!inside) {
            if (!dragging_min_ && !dragging_max_) {
                min_hovered_ = false;
                max_hovered_ = false;
            }
            return inside;
        }
        SDL_Rect kmin = min_knob_rect();
        SDL_Rect kmax = max_knob_rect();
        bool on_min = SDL_PointInRect(&p, &kmin);
        bool on_max = SDL_PointInRect(&p, &kmax);
        if (dragging_min_) {
            min_hovered_ = true;
            max_hovered_ = false;
            wheel_target_max_ = false;
            return inside;
        }
        if (dragging_max_) {
            min_hovered_ = false;
            max_hovered_ = true;
            wheel_target_max_ = true;
            return inside;
        }
        if (on_min || on_max) {
            min_hovered_ = on_min;
            max_hovered_ = on_max;
            if (on_min != on_max) {
                wheel_target_max_ = on_max;
            }
            return inside;
        }
        SDL_Point min_center{ kmin.x + kmin.w / 2, kmin.y + kmin.h / 2 };
        SDL_Point max_center{ kmax.x + kmax.w / 2, kmax.y + kmax.h / 2 };
        const bool overlap = (min_center.x == max_center.x) && (min_center.y == max_center.y);
        if (overlap) {
            if (p.x >= max_center.x) {
                min_hovered_ = false;
                max_hovered_ = true;
                wheel_target_max_ = true;
            } else {
                min_hovered_ = true;
                max_hovered_ = false;
                wheel_target_max_ = false;
            }
            return inside;
        }
        const auto sqr = [](int v) { return v * v; };
        const int min_dist = sqr(p.x - min_center.x) + sqr(p.y - min_center.y);
        const int max_dist = sqr(p.x - max_center.x) + sqr(p.y - max_center.y);
        if (min_dist <= max_dist) {
            min_hovered_ = true;
            max_hovered_ = false;
            wheel_target_max_ = false;
        } else {
            min_hovered_ = false;
            max_hovered_ = true;
            wheel_target_max_ = true;
        }
        return inside;
};

    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        SDL_Point p = sdl_mouse_util::MotionPoint(e.motion);
        update_hover(p);
        bool dragging = false;
        if (dragging_min_) {
            apply_min_interaction(value_for_x(p.x));
            dragging = true;
        }
        if (dragging_max_) {
            apply_max_interaction(value_for_x(p.x));
            dragging = true;
        }
        if (dragging) {
            return true;
        }
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
        bool inside = update_hover(p);
        bool focus_changed = false;
        if (inside) {
            bool was_focused = focused_;
            set_focus(true);
            focus_changed = !was_focused;
        } else if (!dragging_min_ && !dragging_max_) {
            set_focus(false);
        }

        if (inside) {
            const bool on_min_value = SDL_PointInRect(&p, &min_value_rect_);
            const bool on_max_value = SDL_PointInRect(&p, &max_value_rect_);
            if (on_min_value) {
                wheel_target_max_ = false;
                edit_min_ = std::make_unique<DMTextBox>("", std::to_string(display_min_value()));
                edit_min_->set_rect(min_value_rect_);
                edit_min_->handle_event(e);
                return true;
            }
            if (on_max_value) {
                wheel_target_max_ = true;
                edit_max_ = std::make_unique<DMTextBox>("", std::to_string(display_max_value()));
                edit_max_->set_rect(max_value_rect_);
                edit_max_->handle_event(e);
                return true;
            }
        }
        if (inside) {
            SDL_Rect track = track_rect();
            SDL_Rect min_knob = min_knob_rect();
            SDL_Rect max_knob = max_knob_rect();
            bool on_track = SDL_PointInRect(&p, &track);
            bool on_min = SDL_PointInRect(&p, &min_knob);
            bool on_max = SDL_PointInRect(&p, &max_knob);
            if (on_min || (on_track && min_hovered_ && !max_hovered_)) {
                dragging_min_ = true;
                min_hovered_ = true;
                max_hovered_ = false;
                wheel_target_max_ = false;
                apply_min_interaction(value_for_x(p.x));
                return true;
            }
            if (on_max || (on_track && max_hovered_ && !min_hovered_)) {
                dragging_max_ = true;
                min_hovered_ = false;
                max_hovered_ = true;
                wheel_target_max_ = true;
                apply_max_interaction(value_for_x(p.x));
                return true;
            }
            if (on_track) {
                int target = value_for_x(p.x);
                int midpoint = (display_min_value() + display_max_value()) / 2;
                if (target <= midpoint) {
                    dragging_min_ = true;
                    min_hovered_ = true;
                    max_hovered_ = false;
                    wheel_target_max_ = false;
                    apply_min_interaction(target);
                } else {
                    dragging_max_ = true;
                    min_hovered_ = false;
                    max_hovered_ = true;
                    wheel_target_max_ = true;
                    apply_max_interaction(target);
                }
                return true;
            }
        }
        if (focus_changed) {
            return true;
        }
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
        bool was_dragging = dragging_min_ || dragging_max_;
        dragging_min_ = false;
        dragging_max_ = false;
        SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
        update_hover(p);
        if (!SDL_PointInRect(&p, &rect_) && focused_) {
            set_focus(false);
        }
        if (was_dragging) {
            bool committed = commit_pending_values();
            (void)committed;
            return true;
        }
    } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        if (!focused_) {
            return false;
        }
        if (SDL_GetMouseFocus() != nullptr) {
            SDL_Point mouse{0, 0};
            sdl_mouse_util::GetMouseState(&mouse.x, &mouse.y);
            update_hover(mouse);
        }
        int delta = e.wheel.integer_y;
        if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            delta = -delta;
        }
        if (delta == 0) {
            delta = static_cast<int>(std::round(e.wheel.y));
        }
        if (delta == 0) {
            return false;
        }
        const int prev_min = display_min_value();
        const int prev_max = display_max_value();
        bool target_max = wheel_target_max_;
        if (max_hovered_ != min_hovered_) {
            target_max = max_hovered_;
        }
        wheel_target_max_ = target_max;
        bool changed = false;
        if (target_max) {
            changed = apply_max_interaction(prev_max + delta);
        } else {
            changed = apply_min_interaction(prev_min + delta);
        }
        if (!changed) {
            changed = display_min_value() != prev_min || display_max_value() != prev_max;
        }
        return changed;
    }
    return false;
}

void DMRangeSlider::draw_text(SDL_Renderer* r, const std::string& s, int x, int y) const {
    const DMSliderStyle& st = DMStyles::Slider();
    DMFontCache::instance().draw_text(r, st.label, s, x, y);
}

void DMRangeSlider::render(SDL_Renderer* r) const {
    const DMSliderStyle& st = DMStyles::Slider();
    const bool dragging = dragging_min_ || dragging_max_;
    const bool active = focused_ || dragging;
    if (active) {
        const SDL_Color& focus_outline = DMStyles::SliderFocusOutline();
        dm_draw::DrawRoundedFocusRing( r, rect_, DMStyles::CornerRadius(), kFocusRingThickness, focus_outline);
    } else if (hovered_) {
        const SDL_Color& hover_outline = DMStyles::SliderHoverOutline();
        dm_draw::DrawRoundedOutline( r, rect_, DMStyles::CornerRadius(), kControlOutlineThickness, hover_outline);
    }
    SDL_Rect tr = track_rect();
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    const SDL_Color& highlight = DMStyles::HighlightColor();
    const SDL_Color& shadow = DMStyles::ShadowColor();
    const int radius = std::min(DMStyles::CornerRadius(), std::min(tr.w, tr.h) / 2);
    const int bevel = std::min(DMStyles::BevelDepth(), std::max(0, std::min(tr.w, tr.h) / 2));
    dm_draw::DrawBeveledRect( r, tr, radius, bevel, DMStyles::SliderTrackBackground(), highlight, shadow, false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
    SDL_Rect kmin = min_knob_rect();
    SDL_Rect kmax = max_knob_rect();
    int fill_x = kmin.x + kSliderKnobWidth / 2;
    int fill_w = (kmax.x + kSliderKnobWidth / 2) - fill_x;
    SDL_Rect fill{ fill_x, tr.y, std::max(0, fill_w), tr.h };
    if (fill.w > 0) {
        const SDL_Color track_fill = active ? st.track_fill_active : st.track_fill;
        dm_draw::DrawBeveledRect( r, fill, radius, bevel, track_fill, highlight, shadow, false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
    }
    const bool min_active = dragging_min_ || (focused_ && !wheel_target_max_);
    const bool max_active = dragging_max_ || (focused_ && wheel_target_max_);
    SDL_Color col_min = st.knob;
    SDL_Color border_min = st.knob_border;
    if (min_active) {
        col_min = st.knob_accent;
        border_min = st.knob_accent_border;
    } else if (min_hovered_) {
        col_min = st.knob_hover;
        border_min = st.knob_border_hover;
    }
    SDL_Color col_max = dm_draw::DarkenColor(st.knob_accent, 0.12f);
    SDL_Color border_max = dm_draw::DarkenColor(st.knob_accent_border, 0.12f);
    if (max_active) {
        col_max = st.knob_accent;
        border_max = st.knob_accent_border;
    } else if (max_hovered_) {
        col_max = dm_draw::LightenColor(st.knob_accent, 0.08f);
        border_max = st.knob_accent_border;
    }
    const int knob_radius = std::min(DMStyles::CornerRadius(), std::min(kmin.w, kmin.h) / 2);
    const int knob_bevel = std::min(DMStyles::BevelDepth(), std::max(0, std::min(kmin.w, kmin.h) / 2));
    dm_draw::DrawBeveledRect( r, kmin, knob_radius, knob_bevel, col_min, highlight, shadow, false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
    dm_draw::DrawRoundedOutline( r, kmin, knob_radius, kKnobOutlineThickness, border_min);
    dm_draw::DrawBeveledRect( r, kmax, knob_radius, knob_bevel, col_max, highlight, shadow, false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
    dm_draw::DrawRoundedOutline( r, kmax, knob_radius, kKnobOutlineThickness, border_max);
    auto draw_knob_band = [&](const SDL_Rect& knob, SDL_Color color, bool align_right) {
        const int inset = 3;
        const int band_w = std::max(2, knob.w / 5);
        SDL_Rect band{ knob.x + inset, knob.y + 3, band_w, std::max(2, knob.h - 6) };
        if (align_right) band.x = knob.x + knob.w - band_w - inset;
        SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);
        sdl_render::FillRect(r, &band);
};
    draw_knob_band(kmin, border_min, false);
    draw_knob_band(kmax, border_max, true);
    if (edit_min_) {
        edit_min_->render(r);
    } else {
        std::string value = std::to_string(display_min_value());
        SDL_Point size = DMFontCache::instance().measure_text(st.label, value);
        int text_y = min_value_rect_.y + (min_value_rect_.h - size.y) / 2;
        int text_x = min_value_rect_.x + kSliderValueHorizontalPadding;
        DMFontCache::instance().draw_text(r, st.label, value, text_x, text_y);
    }
    if (edit_max_) {
        edit_max_->render(r);
    } else {
        std::string value = std::to_string(display_max_value());
        SDL_Point size = DMFontCache::instance().measure_text(st.label, value);
        int text_y = max_value_rect_.y + (max_value_rect_.h - size.y) / 2;
        int text_x = std::max(max_value_rect_.x + kSliderValueHorizontalPadding, max_value_rect_.x + max_value_rect_.w - size.x - kSliderValueHorizontalPadding);
        DMFontCache::instance().draw_text(r, st.label, value, text_x, text_y);
    }
    if (tooltip_state_) {
        DMWidgetTooltipRender(r, rect_, *tooltip_state_);
    }
}

int DMRangeSlider::height() {
    return kBoxTopPadding + slider_value_height() + kLabelControlGap + kSliderControlHeight + kBoxBottomPadding;
}

namespace {
constexpr int kWeightedRangeTopPadding = 8;
constexpr int kWeightedRangeBottomPadding = 8;
constexpr int kWeightedRangeHeaderHeight = 28;
constexpr int kWeightedRangeCheckboxSize = 18;
constexpr int kWeightedRangeCheckboxGap = 8;
constexpr int kWeightedRangeHistogramTopGap = 10;
constexpr int kWeightedRangeHistogramBottomGap = 12;
constexpr int kWeightedRangeLineThickness = 2;
constexpr int kWeightedRangeHandleSize = 12;
constexpr int kWeightedRangeHandleHitSize = 24;
constexpr int kWeightedRangeColumnPad = 18;
constexpr int kWeightedRangeWeightTopPad = 14;
constexpr int kWeightedRangeWeightBottomPad = 30;
constexpr double kWeightedRangeDefaultSpanScreenRatio = 0.88;
constexpr int kWeightedRangeMinorTickHeight = 5;
constexpr int kWeightedRangeMidTickHeight = 9;
constexpr int kWeightedRangeMajorTickHeight = 14;
constexpr int kWeightedRangeImportantTickHeight = 19;
constexpr int kWeightedRangePopupWidth = 440;
constexpr int kWeightedRangePopupHeight = 316;
constexpr int kWeightedRangeModalWidth = 740;
constexpr int kWeightedRangeModalHeight = 430;

double weighted_range_smoothstep(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - (2.0 * t));
}

std::int64_t weighted_range_floor_to_step(std::int64_t value, std::int64_t step) {
    if (step <= 0) {
        return value;
    }
    std::int64_t remainder = value % step;
    if (remainder < 0) {
        remainder += step;
    }
    return value - remainder;
}
}

DMWeightedRangeWidget* DMWeightedRangeWidget::active_selected_ = nullptr;
DMWeightedRangeWidget* DMWeightedRangeWidget::active_expanded_ = nullptr;

DMWeightedRangeWidget::DMWeightedRangeWidget(const std::string& label,
                                             const vibble::weighted_range::WeightedIntRange& value,
                                             std::int64_t min_allowed,
                                             std::int64_t max_allowed,
                                             bool loop)
    : label_(label), value_(value), min_allowed_(min_allowed), max_allowed_(max_allowed), loop_(loop) {
    if (max_allowed_ < min_allowed_) {
        std::swap(min_allowed_, max_allowed_);
    }
    sanitize_value();
    sync_visual_range_from_value();
    update_geometry();
}

DMWeightedRangeWidget::~DMWeightedRangeWidget() {
    clear_selection();
}

void DMWeightedRangeWidget::set_rect(const SDL_Rect& r) {
    rect_ = r;
    clamp_visual_range();
    update_geometry();
}

void DMWeightedRangeWidget::set_label(const std::string& label) {
    label_ = label;
    update_geometry();
}

void DMWeightedRangeWidget::set_value(const vibble::weighted_range::WeightedIntRange& value) {
    value_ = value;
    sanitize_value();
    sync_visual_range_from_value();
    update_geometry();
}

void DMWeightedRangeWidget::set_on_value_changed(ValueChangedCallback callback) {
    on_value_changed_ = std::move(callback);
}

void DMWeightedRangeWidget::set_tooltip_state(DMWidgetTooltipState* state) {
    tooltip_state_ = state;
    if (popup_center_stepper_) popup_center_stepper_->set_tooltip_state(state);
    if (popup_span_stepper_) popup_span_stepper_->set_tooltip_state(state);
    if (popup_falloff_stepper_) popup_falloff_stepper_->set_tooltip_state(state);
    if (popup_center_weight_stepper_) popup_center_weight_stepper_->set_tooltip_state(state);
    if (popup_falloff_weight_stepper_) popup_falloff_weight_stepper_->set_tooltip_state(state);
    if (popup_edge_weight_stepper_) popup_edge_weight_stepper_->set_tooltip_state(state);
}

void DMWeightedRangeWidget::set_enabled(bool enabled) {
    enabled_ = enabled;
    if (!enabled_ && active_selected_ == this) {
        clear_selection();
    }
}

void DMWeightedRangeWidget::clear_selection() {
    if (active_selected_ == this) {
        active_selected_ = nullptr;
    }
    if (dragging_) {
        end_drag();
    } else {
        set_slider_scroll_capture(this, false);
    }
    wheel_scroll_accumulator_ = 0.0;
    selected_node_index_ = -1;
    if (active_expanded_ == this) {
        active_expanded_ = nullptr;
    }
}

int DMWeightedRangeWidget::height() {
    return kWeightedRangeTopPadding + kWeightedRangeHeaderHeight + kWeightedRangeHistogramTopGap + 108 + kWeightedRangeHistogramBottomGap + kWeightedRangeBottomPadding;
}

int DMWeightedRangeWidget::preferred_height(int) const {
    return height();
}

SDL_Rect DMWeightedRangeWidget::content_rect() const {
    return content_rect_;
}

SDL_Rect DMWeightedRangeWidget::checkbox_rect() const {
    return SDL_Rect{
        rect_.x + 8,
        rect_.y + 6,
        kWeightedRangeCheckboxSize,
        kWeightedRangeCheckboxSize,
    };
}

SDL_Rect DMWeightedRangeWidget::histogram_rect() const {
    const int header_bottom = rect_.y + kWeightedRangeTopPadding + kWeightedRangeHeaderHeight;
    const int top = header_bottom + kWeightedRangeHistogramTopGap;
    const int bottom = rect_.y + rect_.h - kWeightedRangeHistogramBottomGap;
    return SDL_Rect{ rect_.x + 4, top, std::max(0, rect_.w - 8), std::max(0, bottom - top) };
}

SDL_Rect DMWeightedRangeWidget::column_label_rect(int index) const {
    const SDL_Rect hist = histogram_rect();
    const int x = control_x_for_index(index);
    const int y = hist.y + hist.h - 18;
    return SDL_Rect{ x - 42, y, 84, 14 };
}

SDL_Rect DMWeightedRangeWidget::column_value_rect(int index) const {
    const SDL_Rect hist = histogram_rect();
    const int x = control_x_for_index(index);
    return SDL_Rect{ x - 42, hist.y + hist.h - 16, 84, 14 };
}

SDL_Rect DMWeightedRangeWidget::histogram_line_rect(int index) const {
    const SDL_Rect hist = histogram_rect();
    const int x = control_x_for_index(index);
    const int top = hist.y + kWeightedRangeWeightTopPad;
    const int bottom = hist.y + hist.h - kWeightedRangeWeightBottomPad;
    return SDL_Rect{ x - (kWeightedRangeLineThickness / 2), top, kWeightedRangeLineThickness, std::max(0, bottom - top) };
}

SDL_Rect DMWeightedRangeWidget::histogram_handle_rect(int index) const {
    const SDL_Rect line = histogram_line_rect(index);
    const int handle_y = weight_y_for_value(display_weight_for_index(index));
    return SDL_Rect{ line.x - (kWeightedRangeHandleHitSize - line.w) / 2,
                     handle_y - kWeightedRangeHandleHitSize / 2,
                     kWeightedRangeHandleHitSize,
                     kWeightedRangeHandleHitSize };
}

void DMWeightedRangeWidget::update_geometry() {
    content_rect_ = rect_;
    clamp_visual_range();
    const SDL_Rect hist = histogram_rect();
    for (int i = 0; i < 5; ++i) {
        const int x = control_x_for_index(i);
        columns_[i].x = x;
        const int line_top = hist.y + kWeightedRangeWeightTopPad;
        const int line_bottom = hist.y + hist.h - kWeightedRangeWeightBottomPad;
        columns_[i].line = SDL_Rect{ x - (kWeightedRangeLineThickness / 2),
                                      line_top,
                                      kWeightedRangeLineThickness,
                                      std::max(0, line_bottom - line_top) };
        const int weight_y = weight_y_for_value(display_weight_for_index(i));
        columns_[i].weight_handle = SDL_Rect{ x - (kWeightedRangeHandleHitSize / 2),
                                              weight_y - (kWeightedRangeHandleHitSize / 2),
                                              kWeightedRangeHandleHitSize,
                                              kWeightedRangeHandleHitSize };
        columns_[i].value_hitbox = SDL_Rect{ x - 18, hist.y, 36, hist.h };
    }
    update_popup_geometry();
}

void DMWeightedRangeWidget::ensure_popup_controls() {
    if (popup_center_stepper_) {
        return;
    }
    popup_center_stepper_ = std::make_unique<DMNumericStepper>("Center", static_cast<int>(min_allowed_), static_cast<int>(max_allowed_), static_cast<int>(value_.center));
    popup_span_stepper_ = std::make_unique<DMNumericStepper>("Span", 0, static_cast<int>(std::max<std::int64_t>(0, max_allowed_ - min_allowed_)), static_cast<int>(value_.span));
    popup_falloff_stepper_ = std::make_unique<DMNumericStepper>("Falloff", 0, static_cast<int>(std::max<std::int64_t>(0, max_allowed_ - min_allowed_)), static_cast<int>(value_.falloff));
    popup_center_weight_stepper_ = std::make_unique<DMNumericStepper>("Center Weight", 0, 100, static_cast<int>(std::lround(value_.weights.center * 100.0)));
    popup_falloff_weight_stepper_ = std::make_unique<DMNumericStepper>("Falloff Weight", 0, 100, static_cast<int>(std::lround(value_.weights.falloff * 100.0)));
    popup_edge_weight_stepper_ = std::make_unique<DMNumericStepper>("Edge Weight", 0, 100, static_cast<int>(std::lround(value_.weights.edge * 100.0)));
    set_tooltip_state(tooltip_state_);
    sync_popup_controls_from_value();
}

void DMWeightedRangeWidget::sync_popup_controls_from_value() {
    ensure_popup_controls();
    popup_center_stepper_->set_range(static_cast<int>(min_allowed_), static_cast<int>(max_allowed_));
    popup_center_stepper_->set_value(static_cast<int>(value_.center));
    popup_span_stepper_->set_value(static_cast<int>(value_.span));
    popup_falloff_stepper_->set_value(static_cast<int>(value_.falloff));
    popup_center_weight_stepper_->set_value(static_cast<int>(std::lround(value_.weights.center * 100.0)));
    popup_falloff_weight_stepper_->set_value(static_cast<int>(std::lround(value_.weights.falloff * 100.0)));
    popup_edge_weight_stepper_->set_value(static_cast<int>(std::lround(value_.weights.edge * 100.0)));
}

void DMWeightedRangeWidget::sync_value_from_popup_controls() {
    if (!popup_center_stepper_) {
        return;
    }
    value_.center = popup_center_stepper_->value();
    value_.span = popup_span_stepper_->value();
    value_.falloff = popup_falloff_stepper_->value();
    value_.weights.center = static_cast<double>(popup_center_weight_stepper_->value()) / 100.0;
    value_.weights.falloff = static_cast<double>(popup_falloff_weight_stepper_->value()) / 100.0;
    value_.weights.edge = static_cast<double>(popup_edge_weight_stepper_->value()) / 100.0;
    sanitize_value();
    sync_popup_controls_from_value();
    sync_visual_range_from_value();
    update_geometry();
}

void DMWeightedRangeWidget::update_popup_geometry() {
    const int fallback_w = std::max(rect_.w + 48, kWeightedRangeModalWidth);
    const int fallback_h = std::max(rect_.h + 48, kWeightedRangeModalHeight);
    popup_rect_ = SDL_Rect{
        rect_.x + (rect_.w - fallback_w) / 2,
        rect_.y + (rect_.h - fallback_h) / 2,
        fallback_w,
        fallback_h
    };
    if (!popup_open_) {
        return;
    }
    ensure_popup_controls();
    const int column_gap = 8;
    const int col_w = (popup_rect_.w - 30 - column_gap) / 2;
    const int left_x = popup_rect_.x + 10;
    const int right_x = left_x + col_w + column_gap;
    int row_y = popup_rect_.y + 250;
    popup_center_stepper_->set_rect(SDL_Rect{left_x, row_y, col_w, DMNumericStepper::height()});
    popup_span_stepper_->set_rect(SDL_Rect{right_x, row_y, col_w, DMNumericStepper::height()});
    row_y += DMNumericStepper::height() + 6;
    popup_falloff_stepper_->set_rect(SDL_Rect{left_x, row_y, col_w, DMNumericStepper::height()});
    popup_center_weight_stepper_->set_rect(SDL_Rect{right_x, row_y, col_w, DMNumericStepper::height()});
    row_y += DMNumericStepper::height() + 6;
    popup_falloff_weight_stepper_->set_rect(SDL_Rect{left_x, row_y, col_w, DMNumericStepper::height()});
    popup_edge_weight_stepper_->set_rect(SDL_Rect{right_x, row_y, col_w, DMNumericStepper::height()});
}

void DMWeightedRangeWidget::update_hover(SDL_Point point) {
    hovered_ = SDL_PointInRect(&point, &rect_);
    checkbox_hovered_ = SDL_PointInRect(&point, &checkbox_rect());
    random_hovered_ = checkbox_hovered_;
    popup_toggle_hovered_ = SDL_PointInRect(&point, &popup_toggle_rect());
    hovered_handle_index_ = hovered_ ? weight_index_for_point(point) : -1;
    hovered_popup_handle_index_ = (popup_open_ && SDL_PointInRect(&point, &popup_rect())) ? weight_index_for_point(point) : -1;
}

SDL_Rect DMWeightedRangeWidget::popup_toggle_rect() const {
    return SDL_Rect{
        rect_.x + rect_.w - 96,
        rect_.y + 5,
        88,
        20
    };
}

SDL_Rect DMWeightedRangeWidget::popup_rect() const {
    return popup_rect_;
}

SDL_Rect DMWeightedRangeWidget::popup_histogram_rect() const {
    if (!popup_open_) {
        return histogram_rect();
    }
    return SDL_Rect{
        popup_rect_.x + 14,
        popup_rect_.y + 36,
        std::max(0, popup_rect_.w - 28),
        130
    };
}

void DMWeightedRangeWidget::sync_visual_range_from_value() {
    const SDL_Rect hist = histogram_rect();
    const double max_span_px = std::max(1.0, static_cast<double>(std::max(1, hist.w - (kWeightedRangeColumnPad * 2))) * 0.5);
    visual_span_px_ = max_span_px * kWeightedRangeDefaultSpanScreenRatio;
    if (value_.random && value_.span > 0) {
        visual_falloff_px_ = visual_span_px_ * std::clamp(static_cast<double>(value_.falloff) / static_cast<double>(value_.span), 0.0, 1.0);
    } else {
        visual_falloff_px_ = 0.0;
    }
    clamp_visual_range();
}

void DMWeightedRangeWidget::clamp_visual_range() {
    const SDL_Rect hist = histogram_rect();
    const double max_span_px = std::max(1.0, static_cast<double>(std::max(1, hist.w - (kWeightedRangeColumnPad * 2))) * 0.5);
    visual_span_px_ = std::clamp(visual_span_px_, 1.0, max_span_px);
    visual_falloff_px_ = std::clamp(visual_falloff_px_, 0.0, visual_span_px_);
}

void DMWeightedRangeWidget::sync_falloff_value_from_visual() {
    if (!value_.random || value_.span <= 0) {
        value_.falloff = 0;
        return;
    }
    clamp_visual_range();
    const double ratio = std::clamp(visual_falloff_px_ / std::max(1.0, visual_span_px_), 0.0, 1.0);
    value_.falloff = std::clamp<std::int64_t>(
        static_cast<std::int64_t>(std::llround(static_cast<double>(value_.span) * ratio)),
        0,
        value_.span);
}

double DMWeightedRangeWidget::units_per_pixel() const {
    return static_cast<double>(std::max<std::int64_t>(1, value_.span)) / std::max(1.0, visual_span_px_);
}

int DMWeightedRangeWidget::control_x_for_index(int index) const {
    const SDL_Rect hist = histogram_rect();
    const int center_x = hist.x + hist.w / 2;
    switch (index) {
    case 0:
        return center_x - static_cast<int>(std::lround(visual_span_px_));
    case 1:
        return center_x - static_cast<int>(std::lround(visual_falloff_px_));
    case 3:
        return center_x + static_cast<int>(std::lround(visual_falloff_px_));
    case 4:
        return center_x + static_cast<int>(std::lround(visual_span_px_));
    case 2:
    default:
        return center_x;
    }
}

std::int64_t DMWeightedRangeWidget::raw_value_for_index(int index) const {
    switch (index) {
    case 0: return value_.center - value_.span;
    case 1: return value_.center - value_.falloff;
    case 3: return value_.center + value_.falloff;
    case 4: return value_.center + value_.span;
    case 2:
    default:
        return value_.center;
    }
}

std::int64_t DMWeightedRangeWidget::display_value(std::int64_t raw) const {
    if (loop_) {
        return vibble::weighted_range::wrap_inclusive(raw, min_allowed_, max_allowed_);
    }
    return std::clamp(raw, min_allowed_, max_allowed_);
}

double DMWeightedRangeWidget::density_for_raw_value(double raw) const {
    if (!value_.random || value_.span <= 0) {
        return std::abs(raw - static_cast<double>(value_.center)) <= 0.5 ? 1.0 : 0.0;
    }
    const double center = static_cast<double>(value_.center);
    const double span = static_cast<double>(std::max<std::int64_t>(1, value_.span));
    const double falloff = static_cast<double>(std::clamp<std::int64_t>(value_.falloff, 0, value_.span));
    const double d = std::abs(raw - center);
    if (d > span) {
        return 0.0;
    }
    const auto weights = value_.weights;
    if (d <= falloff || falloff >= span) {
        const double t = falloff <= 0.0 ? 1.0 : weighted_range_smoothstep(d / falloff);
        return weights.center + (weights.falloff - weights.center) * t;
    }
    const double denom = std::max(1e-9, span - falloff);
    const double t = weighted_range_smoothstep((d - falloff) / denom);
    return weights.falloff + (weights.edge - weights.falloff) * t;
}

std::int64_t DMWeightedRangeWidget::ruler_tick_step(double target_px) const {
    const double raw_step = std::max(1.0, units_per_pixel() * std::max(1.0, target_px));
    const double exponent = std::floor(std::log10(raw_step));
    const double scale = std::pow(10.0, exponent);
    const double normalized = raw_step / scale;
    double nice = 10.0;
    if (normalized <= 1.0) {
        nice = 1.0;
    } else if (normalized <= 2.0) {
        nice = 2.0;
    } else if (normalized <= 5.0) {
        nice = 5.0;
    }
    return std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(nice * scale)));
}

double DMWeightedRangeWidget::display_weight_for_index(int index) const {
    const vibble::weighted_range::WeightedRangeWeights weights = value_.weights;
    switch (index) {
    case 0:
    case 4:
        return weights.edge;
    case 1:
    case 3:
        return weights.falloff;
    case 2:
    default:
        return weights.center;
    }
}

void DMWeightedRangeWidget::set_weight_for_index(int index, double weight) {
    weight = std::clamp(weight, 0.0, kMaxRawWeight);
    switch (index) {
    case 0:
    case 4:
        value_.weights.edge = weight;
        break;
    case 1:
    case 3:
        value_.weights.falloff = weight;
        break;
    case 2:
    default:
        value_.weights.center = weight;
        break;
    }
}

std::string DMWeightedRangeWidget::format_value(std::int64_t value) const {
    return std::to_string(value);
}

int DMWeightedRangeWidget::weight_y_for_value(double weight) const {
    const SDL_Rect hist = histogram_rect();
    const int top = hist.y + kWeightedRangeWeightTopPad;
    const int bottom = hist.y + hist.h - kWeightedRangeWeightBottomPad;
    const int span = std::max(1, bottom - top);
    const double normalized = std::clamp(weight, 0.0, 1.0);
    const int y = bottom - static_cast<int>(std::lround(normalized * static_cast<double>(span)));
    return std::clamp(y, top, bottom);
}

double DMWeightedRangeWidget::weight_for_y(int y) const {
    const SDL_Rect hist = histogram_rect();
    const int top = hist.y + kWeightedRangeWeightTopPad;
    const int bottom = hist.y + hist.h - kWeightedRangeWeightBottomPad;
    const int span = std::max(1, bottom - top);
    const int clamped = std::clamp(y, top, bottom);
    return static_cast<double>(bottom - clamped) / static_cast<double>(span);
}

int DMWeightedRangeWidget::weight_index_for_point(SDL_Point point) const {
    int nearest_index = -1;
    int nearest_distance = std::numeric_limits<int>::max();
    for (int i = 0; i < 5; ++i) {
        if (!value_.random && i != 2) {
            continue;
        }
        if (SDL_PointInRect(&point, &histogram_handle_rect(i))) {
            return i;
        }
        const SDL_Rect line = histogram_line_rect(i);
        const int expanded_top = line.y - 6;
        const int expanded_bottom = line.y + line.h + 6;
        if (point.y < expanded_top || point.y > expanded_bottom) {
            continue;
        }
        const int dx = std::abs(point.x - columns_[i].x);
        if (dx <= 14 && dx < nearest_distance) {
            nearest_distance = dx;
            nearest_index = i;
        }
    }
    return nearest_index;
}

void DMWeightedRangeWidget::sanitize_value() {
    value_.span = std::llabs(value_.span);
    value_.falloff = std::llabs(value_.falloff);
    auto clean_weight = [](double weight) {
        if (!std::isfinite(weight) || weight < 0.0) {
            return 0.5;
        }
        return std::clamp(weight, 0.0, 1.0);
    };
    if (value_.random) {
        value_.weights.edge = clean_weight(value_.weights.edge);
        value_.weights.falloff = clean_weight(value_.weights.falloff);
        value_.weights.center = clean_weight(value_.weights.center);
    } else {
        value_.weights = vibble::weighted_range::WeightedRangeWeights{0.5, 0.5, 0.5};
    }
    if (loop_) {
        value_.center = vibble::weighted_range::wrap_inclusive(value_.center, min_allowed_, max_allowed_);
        const std::int64_t domain = std::max<std::int64_t>(1, (max_allowed_ - min_allowed_) + 1);
        value_.span = std::min<std::int64_t>(value_.span, domain / 2);
    } else {
        value_.center = std::clamp(value_.center, min_allowed_, max_allowed_);
        const std::int64_t left_room = value_.center - min_allowed_;
        const std::int64_t right_room = max_allowed_ - value_.center;
        value_.span = std::min<std::int64_t>(value_.span, std::min(left_room, right_room));
    }
    value_.span = std::max<std::int64_t>(0, value_.span);
    value_.falloff = std::clamp<std::int64_t>(value_.falloff, 0, value_.span);
    if (value_.span == 0) {
        value_.falloff = 0;
    }
}

void DMWeightedRangeWidget::notify_value_changed() {
    if (on_value_changed_) {
        on_value_changed_(value_);
    }
}

bool DMWeightedRangeWidget::toggle_random() {
    if (!enabled_) {
        return false;
    }
    if (value_.random) {
        random_snapshot_ = value_;
        has_random_snapshot_ = true;
        value_.random = false;
        value_.span = 0;
        value_.falloff = 0;
        value_.weights = vibble::weighted_range::WeightedRangeWeights{0.5, 0.5, 0.5};
    } else {
        if (has_random_snapshot_) {
            value_ = random_snapshot_;
        } else {
            value_.random = true;
            if (value_.span <= 0) {
                if (loop_) {
                    value_.span = 1;
                } else {
                    const std::int64_t room = std::min<std::int64_t>(value_.center - min_allowed_, max_allowed_ - value_.center);
                    value_.span = std::max<std::int64_t>(0, std::min<std::int64_t>(1, room));
                }
            }
            if (value_.falloff > value_.span) {
                value_.falloff = value_.span;
            }
            value_.weights = vibble::weighted_range::WeightedRangeWeights{0.5, 0.5, 0.5};
        }
    }
    sanitize_value();
    sync_visual_range_from_value();
    if (popup_open_) {
        sync_popup_controls_from_value();
    }
    update_geometry();
    notify_value_changed();
    return true;
}

bool DMWeightedRangeWidget::toggle_popup_editor() {
    if (!enabled_) {
        return false;
    }
    popup_open_ = !popup_open_;
    if (popup_open_) {
        active_expanded_ = this;
        ensure_popup_controls();
        sync_popup_controls_from_value();
        set_slider_scroll_capture(this, true);
    } else if (active_expanded_ == this) {
        active_expanded_ = nullptr;
    }
    update_popup_geometry();
    return true;
}

double DMWeightedRangeWidget::velocity_scaled_pixels(int delta_pixels) const {
    const double sign = (delta_pixels < 0) ? -1.0 : 1.0;
    const double distance = std::abs(static_cast<double>(delta_pixels));
    if (distance <= 0.0) {
        return 0.0;
    }
    const double fine_zone = std::min(36.0, std::max(18.0, static_cast<double>(rect_.w) * 0.08));
    if (distance <= fine_zone) {
        return sign * (distance * 0.34);
    }
    const double accelerated = (fine_zone * 0.34) + std::pow(distance - fine_zone, 1.12);
    return sign * accelerated;
}

bool DMWeightedRangeWidget::apply_wheel_delta(int wheel_y) {
    if (!enabled_ || !value_.random) {
        return false;
    }
    if (wheel_y == 0) {
        return false;
    }
    std::int64_t span = value_.span;
    const int steps = std::abs(wheel_y);
    const int direction = (wheel_y > 0) ? -1 : 1;
    bool changed = false;
    for (int i = 0; i < steps; ++i) {
        if (direction > 0 && span == 0) {
            span = 1;
            changed = true;
            continue;
        }
        const std::int64_t step_size = std::max<std::int64_t>(1, span / 10);
        const std::int64_t next = std::max<std::int64_t>(0, span + (direction * step_size));
        if (next == span) {
            continue;
        }
        span = next;
        changed = true;
    }
    if (!changed) {
        return false;
    }
    value_.span = span;
    sanitize_value();
    sync_falloff_value_from_visual();
    if (popup_open_) {
        sync_popup_controls_from_value();
    }
    update_geometry();
    notify_value_changed();
    return true;
}

bool DMWeightedRangeWidget::begin_drag(SDL_Point point) {
    if (!enabled_) {
        return false;
    }
    const auto index_for_surface = [this, &point](const SDL_Rect& surface) -> int {
        const int center_x = surface.x + surface.w / 2;
        const int top = surface.y + kWeightedRangeWeightTopPad;
        const int bottom = surface.y + surface.h - kWeightedRangeWeightBottomPad;
        int nearest_index = -1;
        int nearest_distance = std::numeric_limits<int>::max();
        for (int i = 0; i < 5; ++i) {
            if (!value_.random && i != 2) {
                continue;
            }
            const int x = center_x +
                          ((i == 0) ? -static_cast<int>(std::lround(visual_span_px_)) :
                           (i == 1) ? -static_cast<int>(std::lround(visual_falloff_px_)) :
                           (i == 3) ? static_cast<int>(std::lround(visual_falloff_px_)) :
                           (i == 4) ? static_cast<int>(std::lround(visual_span_px_)) : 0);
            const int y = std::clamp(weight_y_for_value(display_weight_for_index(i)), top, bottom);
            SDL_Rect handle{x - (kWeightedRangeHandleHitSize / 2), y - (kWeightedRangeHandleHitSize / 2), kWeightedRangeHandleHitSize, kWeightedRangeHandleHitSize};
            if (SDL_PointInRect(&point, &handle)) {
                return i;
            }
            const int dx = std::abs(point.x - x);
            const bool y_match = point.y >= (top - 6) && point.y <= (bottom + 6);
            if (y_match && dx <= 14 && dx < nearest_distance) {
                nearest_distance = dx;
                nearest_index = i;
            }
        }
        return nearest_index;
    };
    drag_in_popup_ = popup_open_ && SDL_PointInRect(&point, &popup_histogram_rect());
    const int weight_idx = index_for_surface(drag_in_popup_ ? popup_histogram_rect() : histogram_rect());
    if (weight_idx >= 0) {
        if (!value_.random && weight_idx != 2) {
            return false;
        }
        drag_mode_ = DragMode::NodeHandle;
        selected_node_index_ = weight_idx;
        drag_handle_index_ = weight_idx;
        drag_start_x_ = point.x;
        drag_start_y_ = point.y;
        drag_start_value_ = value_;
        drag_start_visual_span_px_ = visual_span_px_;
        drag_start_visual_falloff_px_ = visual_falloff_px_;
        dragging_ = true;
        drag_started_ = true;
        set_slider_scroll_capture(this, true);
        return true;
    }
    return false;
}

void DMWeightedRangeWidget::end_drag() {
    dragging_ = false;
    drag_started_ = false;
    drag_in_popup_ = false;
    drag_mode_ = DragMode::None;
    set_slider_scroll_capture(this, active_selected_ == this);
}

bool DMWeightedRangeWidget::apply_node_wheel_delta(int wheel_y, int node_index) {
    if (!enabled_ || wheel_y == 0) {
        return false;
    }
    const int steps = std::abs(wheel_y);
    const int direction = (wheel_y > 0) ? -1 : 1;
    bool changed = false;
    if (node_index == 2 || !value_.random) {
        value_.center += static_cast<std::int64_t>(direction) * static_cast<std::int64_t>(steps);
        changed = true;
    } else if (node_index == 0 || node_index == 4) {
        value_.span = std::max<std::int64_t>(0, value_.span + static_cast<std::int64_t>(direction) * static_cast<std::int64_t>(steps));
        if (value_.falloff > value_.span) {
            value_.falloff = value_.span;
        }
        changed = true;
    } else if (node_index == 1 || node_index == 3) {
        value_.falloff = std::clamp<std::int64_t>(
            value_.falloff + static_cast<std::int64_t>(direction) * static_cast<std::int64_t>(steps),
            0,
            value_.span);
        changed = true;
    }
    if (!changed) {
        return false;
    }
    sanitize_value();
    sync_visual_range_from_value();
    if (popup_open_) {
        sync_popup_controls_from_value();
    }
    update_geometry();
    notify_value_changed();
    return true;
}

bool DMWeightedRangeWidget::apply_drag_delta_in_surface(SDL_Point point, const SDL_Rect& surface) {
    if (!dragging_ || !enabled_) {
        return false;
    }
    const int dx = point.x - drag_start_x_;
    const int dy = point.y - drag_start_y_;
    bool changed = false;
    const double start_units_per_pixel =
        static_cast<double>(std::max<std::int64_t>(1, drag_start_value_.span)) / std::max(1.0, drag_start_visual_span_px_);
    switch (drag_mode_) {
    case DragMode::NodeHandle: {
        const double scaled_dx = velocity_scaled_pixels(dx);
        if (drag_handle_index_ == 2) {
            const double center_units_per_pixel = std::max(1.0, start_units_per_pixel);
            value_.center = drag_start_value_.center +
                            static_cast<std::int64_t>(std::llround(scaled_dx * center_units_per_pixel));
        } else if (drag_handle_index_ == 0 || drag_handle_index_ == 4) {
            if (drag_handle_index_ == 0) {
                visual_span_px_ = drag_start_visual_span_px_ - scaled_dx;
            } else {
                visual_span_px_ = drag_start_visual_span_px_ + scaled_dx;
            }
            clamp_visual_range();
            value_.span = std::max<std::int64_t>(0, static_cast<std::int64_t>(std::llround(visual_span_px_ * start_units_per_pixel)));
        } else {
            if (drag_handle_index_ == 1) {
                visual_falloff_px_ = drag_start_visual_falloff_px_ - scaled_dx;
            } else {
                visual_falloff_px_ = drag_start_visual_falloff_px_ + scaled_dx;
            }
            clamp_visual_range();
        }
        if (value_.random) {
            const int center_x = surface.x + surface.w / 2;
            const int x = center_x +
                          ((drag_handle_index_ == 0) ? -static_cast<int>(std::lround(visual_span_px_)) :
                           (drag_handle_index_ == 1) ? -static_cast<int>(std::lround(visual_falloff_px_)) :
                           (drag_handle_index_ == 3) ? static_cast<int>(std::lround(visual_falloff_px_)) :
                           (drag_handle_index_ == 4) ? static_cast<int>(std::lround(visual_span_px_)) : 0);
            const int line_top = surface.y + kWeightedRangeWeightTopPad;
            const int line_bottom = surface.y + surface.h - kWeightedRangeWeightBottomPad;
            const SDL_Rect line{x - (kWeightedRangeLineThickness / 2), line_top, kWeightedRangeLineThickness, std::max(0, line_bottom - line_top)};
            const int adjusted_y = std::clamp(drag_start_y_ + dy, line.y, line.y + line.h);
            const int span = std::max(1, (line.y + line.h) - line.y);
            const double normalized = static_cast<double>((line.y + line.h) - adjusted_y) / static_cast<double>(span);
            set_weight_for_index(drag_handle_index_, std::clamp(normalized, 0.0, 1.0));
        }
        changed = true;
        break;
    }
    case DragMode::None:
    default:
        break;
    }
    if (changed) {
        sanitize_value();
        sync_falloff_value_from_visual();
        if (popup_open_) {
            sync_popup_controls_from_value();
        }
        update_geometry();
        notify_value_changed();
    }
    return changed;
}

bool DMWeightedRangeWidget::apply_drag_delta(SDL_Point point) {
    return apply_drag_delta_in_surface(point, drag_in_popup_ ? popup_histogram_rect() : histogram_rect());
}

bool DMWeightedRangeWidget::handle_event(const SDL_Event& e) {
    if (tooltip_state_) {
        DMWidgetTooltipHandleEvent(e, rect_, *tooltip_state_);
    }
    bool used = false;
    const auto is_selected = [this]() { return active_selected_ == this; };
    const auto deselect = [this]() {
        if (active_selected_ != this) {
            return;
        }
        clear_selection();
    };
    const auto select_widget = [this]() {
        active_selected_ = this;
        set_slider_scroll_capture(this, true);
        wheel_scroll_accumulator_ = 0.0;
    };
    const bool modal_active = popup_open_ && (active_expanded_ == this);
    switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION: {
        SDL_Point p{static_cast<int>(std::lround(e.motion.x)), static_cast<int>(std::lround(e.motion.y))};
        update_hover(p);
        set_slider_scroll_capture(this, is_selected() || dragging_);
        if (dragging_) {
            used = apply_drag_delta(p);
        } else if (is_selected()) {
            used = true;
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        SDL_Point p{static_cast<int>(std::lround(e.button.x)), static_cast<int>(std::lround(e.button.y))};
        update_hover(p);
        const bool inside_widget = SDL_PointInRect(&p, &rect_);
        const bool inside_popup = popup_open_ && SDL_PointInRect(&p, &popup_rect());
        if (modal_active && !inside_popup && !inside_widget) {
            used = true;
            break;
        }
        if (!inside_widget) {
            if (inside_popup) {
                used = true;
                break;
            }
            if (e.button.button == SDL_BUTTON_LEFT && is_selected()) {
                deselect();
            }
            break;
        }
        if (!enabled_) {
            used = true;
            break;
        }
        if (e.button.button == SDL_BUTTON_LEFT && !is_selected()) {
            select_widget();
            used = true;
        }
        if (is_selected()) {
            used = true;
        }
        if (e.button.button != SDL_BUTTON_LEFT) {
            break;
        }
        if (SDL_PointInRect(&p, &popup_toggle_rect())) {
            used = toggle_popup_editor() || used;
            break;
        }
        if (SDL_PointInRect(&p, &checkbox_rect())) {
            used = toggle_random() || used;
            break;
        }
        if (!begin_drag(p)) {
            selected_node_index_ = -1;
        }
        used = true;
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (is_selected()) {
            used = true;
            if (dragging_ && e.button.button == SDL_BUTTON_LEFT) {
                end_drag();
            }
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        {
            if (!is_selected()) {
                break;
            }
            used = true;
            int mx = 0;
            int my = 0;
            sdl_mouse_util::GetMouseState(&mx, &my);
            update_hover(SDL_Point{mx, my});
            SDL_Point wheel_point{mx, my};
            const bool pointer_over_widget = SDL_PointInRect(&wheel_point, &rect_);
            const bool pointer_over_popup = popup_open_ && SDL_PointInRect(&wheel_point, &popup_rect());
            set_slider_scroll_capture(this, true);
            if (pointer_over_widget || pointer_over_popup || dragging_ || modal_active) {
                double delta = static_cast<double>(e.wheel.y);
                if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                    delta = -delta;
                }
                if (std::abs(delta) > 0.0) {
                    wheel_scroll_accumulator_ += delta;
                }
                int steps = 0;
                while (wheel_scroll_accumulator_ >= 1.0) {
                    wheel_scroll_accumulator_ -= 1.0;
                    ++steps;
                }
                while (wheel_scroll_accumulator_ <= -1.0) {
                    wheel_scroll_accumulator_ += 1.0;
                    --steps;
                }
                if (steps != 0) {
                    if (selected_node_index_ >= 0) {
                        (void)apply_node_wheel_delta(steps, selected_node_index_);
                    } else {
                        (void)apply_wheel_delta(steps);
                    }
                }
            } else {
                wheel_scroll_accumulator_ = 0.0;
            }
        }
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        hovered_ = false;
        checkbox_hovered_ = false;
        random_hovered_ = false;
        hovered_handle_index_ = -1;
        wheel_scroll_accumulator_ = 0.0;
        set_slider_scroll_capture(this, is_selected());
        if (dragging_) {
            end_drag();
        }
        break;
    case SDL_EVENT_KEY_DOWN:
        if (is_selected() && e.key.key == SDLK_ESCAPE && popup_open_) {
            popup_open_ = false;
            if (active_expanded_ == this) {
                active_expanded_ = nullptr;
            }
            used = true;
        }
        break;
    default:
        break;
    }
    if (popup_open_) {
        ensure_popup_controls();
        const auto before_json = vibble::weighted_range::to_json(value_);
        used = popup_center_stepper_->handle_event(e) || used;
        used = popup_span_stepper_->handle_event(e) || used;
        used = popup_falloff_stepper_->handle_event(e) || used;
        used = popup_center_weight_stepper_->handle_event(e) || used;
        used = popup_falloff_weight_stepper_->handle_event(e) || used;
        used = popup_edge_weight_stepper_->handle_event(e) || used;
        sync_value_from_popup_controls();
        if (vibble::weighted_range::to_json(value_) != before_json) {
            notify_value_changed();
        }
    }
    return used;
}

void DMWeightedRangeWidget::draw_text(SDL_Renderer* r, const std::string& s, int x, int y) const {
    const DMLabelStyle& style = DMStyles::Label();
    DMFontCache::instance().draw_text(r, style, s, x, y);
}

void DMWeightedRangeWidget::render(SDL_Renderer* r) const {
    const SDL_Rect box = rect_;
    const SDL_Color fill = enabled_ ? DMStyles::PanelHeader() : SDL_Color{32, 36, 46, 220};
    const bool is_selected = (active_selected_ == this);
    const SDL_Color border = is_selected ? SDL_Color{255, 199, 120, 255}
                                         : (hovered_ ? DMStyles::HighlightColor() : DMStyles::Border());
    dm_draw::DrawBeveledRect(r, box, DMStyles::CornerRadius(), DMStyles::BevelDepth(), fill, DMStyles::HighlightColor(), DMStyles::ShadowColor(), false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
    dm_draw::DrawRoundedOutline(r, box, DMStyles::CornerRadius(), kControlOutlineThickness, border);

    const SDL_Rect cb = checkbox_rect();
    SDL_Color cb_fill = value_.random ? DMStyles::AccentColor() : DMStyles::ButtonBaseFill();
    if (!enabled_) {
        cb_fill.a = 160;
    } else if (checkbox_hovered_) {
        cb_fill = DMStyles::ButtonHoverFill();
    }
    SDL_SetRenderDrawColor(r, cb_fill.r, cb_fill.g, cb_fill.b, cb_fill.a);
    sdl_render::FillRect(r, &cb);
    SDL_SetRenderDrawColor(r, border.r, border.g, border.b, border.a);
    sdl_render::Rect(r, &cb);
    if (value_.random) {
        SDL_Rect check_inner{ cb.x + 4, cb.y + 4, std::max(0, cb.w - 8), std::max(0, cb.h - 8) };
        SDL_SetRenderDrawColor(r, DMStyles::AccentColor().r, DMStyles::AccentColor().g, DMStyles::AccentColor().b, 255);
        sdl_render::FillRect(r, &check_inner);
    }

    const DMLabelStyle& label_style = DMStyles::Label();
    const int label_x = cb.x + cb.w + kWeightedRangeCheckboxGap;
    const int label_y = box.y + 6;
    DMFontCache::instance().draw_text(r, label_style, std::string("Random"), label_x, label_y);
    if (!label_.empty()) {
        SDL_Point label_size = DMFontCache::instance().measure_text(label_style, label_);
        const int title_x = std::max(box.x + 8, box.x + box.w - label_size.x - 8);
        DMFontCache::instance().draw_text(r, label_style, label_, title_x, label_y);
    }
    const SDL_Rect popup_btn = popup_toggle_rect();
    SDL_Color popup_fill = popup_open_ ? SDL_Color{80, 124, 190, 235} : DMStyles::ButtonBaseFill();
    if (popup_toggle_hovered_) {
        popup_fill = DMStyles::ButtonHoverFill();
    }
    dm_draw::DrawRoundedSolidRect(r, popup_btn, 4, popup_fill);
    dm_draw::DrawRoundedOutline(r, popup_btn, 4, 1, border);
    DMFontCache::instance().draw_text(r, label_style, popup_open_ ? "Close" : "Expand", popup_btn.x + 18, popup_btn.y + 3);

    const SDL_Rect hist = histogram_rect();
    SDL_Color grid{76, 86, 110, 120};
    SDL_SetRenderDrawColor(r, grid.r, grid.g, grid.b, grid.a);
    sdl_render::Rect(r, &hist);

    const int top = hist.y + kWeightedRangeWeightTopPad;
    const int baseline = hist.y + hist.h - kWeightedRangeWeightBottomPad;
    const int center_x = control_x_for_index(2);
    SDL_Rect range_bar{hist.x + kWeightedRangeColumnPad, baseline, std::max(1, hist.w - (kWeightedRangeColumnPad * 2)), 4};
    SDL_Color bar_col = DMStyles::Border();
    bar_col.a = 190;
    SDL_SetRenderDrawColor(r, bar_col.r, bar_col.g, bar_col.b, bar_col.a);
    sdl_render::FillRect(r, &range_bar);

    const double units = units_per_pixel();
    const std::int64_t minor_step = ruler_tick_step(16.0);
    const std::int64_t major_step = std::max<std::int64_t>(minor_step, minor_step * 5);
    const std::int64_t important_step = std::max<std::int64_t>(major_step, major_step * 2);
    const double visible_raw_min = static_cast<double>(value_.center) + static_cast<double>(range_bar.x - center_x) * units;
    const double visible_raw_max = static_cast<double>(value_.center) + static_cast<double>((range_bar.x + range_bar.w) - center_x) * units;
    const std::int64_t first_tick = weighted_range_floor_to_step(static_cast<std::int64_t>(std::floor(visible_raw_min)), minor_step);
    DMLabelStyle small_tick_style = label_style;
    small_tick_style.font_size = std::max(8, label_style.font_size - 2);
    DMLabelStyle important_tick_style = label_style;
    important_tick_style.font_size = label_style.font_size + 1;
    int last_label_right = hist.x - 100;
    for (std::int64_t tick = first_tick; tick <= static_cast<std::int64_t>(std::ceil(visible_raw_max)) + minor_step; tick += minor_step) {
        const int tick_x = center_x + static_cast<int>(std::lround((static_cast<double>(tick - value_.center)) / std::max(1e-9, units)));
        if (tick_x < range_bar.x || tick_x > range_bar.x + range_bar.w) {
            continue;
        }
        const bool important = (tick % important_step) == 0;
        const bool major = (tick % major_step) == 0;
        const int tick_h = important ? kWeightedRangeImportantTickHeight
                         : major ? kWeightedRangeMajorTickHeight
                         : ((tick % (minor_step * 2)) == 0 ? kWeightedRangeMidTickHeight : kWeightedRangeMinorTickHeight);
        SDL_Color tick_col = important ? SDL_Color{238, 244, 255, 230}
                           : major ? SDL_Color{184, 195, 216, 205}
                                   : SDL_Color{126, 138, 164, 165};
        SDL_SetRenderDrawColor(r, tick_col.r, tick_col.g, tick_col.b, tick_col.a);
        SDL_RenderLine(r, tick_x, baseline + 4, tick_x, baseline + 4 + tick_h);
        if (major) {
            const std::string label = format_value(display_value(tick));
            const DMLabelStyle& tick_style = important ? important_tick_style : small_tick_style;
            SDL_Point size = DMFontCache::instance().measure_text(tick_style, label);
            const int text_x = std::clamp(tick_x - (size.x / 2), hist.x, std::max(hist.x, hist.x + hist.w - size.x));
            if (text_x > last_label_right + 8) {
                DMFontCache::instance().draw_text(r, tick_style, label, text_x, baseline + 7 + tick_h);
                last_label_right = text_x + size.x;
            }
        }
    }

    SDL_Color fill_col{245, 132, 42, static_cast<Uint8>(value_.random ? 92 : 112)};
    SDL_Color curve_col{255, 174, 86, 230};
    int prev_x = hist.x;
    int prev_y = baseline;
    for (int x = hist.x + 1; x < hist.x + hist.w; ++x) {
        const double raw = static_cast<double>(value_.center) + static_cast<double>(x - center_x) * units;
        const double density = std::clamp(density_for_raw_value(raw), 0.0, 1.0);
        const int y = baseline - static_cast<int>(std::lround(density * static_cast<double>(std::max(1, baseline - top))));
        SDL_Rect column{x, y, 1, std::max(1, baseline - y)};
        SDL_SetRenderDrawColor(r, fill_col.r, fill_col.g, fill_col.b, fill_col.a);
        sdl_render::FillRect(r, &column);
        SDL_SetRenderDrawColor(r, curve_col.r, curve_col.g, curve_col.b, curve_col.a);
        SDL_RenderLine(r, prev_x, prev_y, x, y);
        prev_x = x;
        prev_y = y;
    }

    for (int i = 0; i < 5; ++i) {
        if (!value_.random && i != 2) {
            continue;
        }
        const int x = control_x_for_index(i);
        const double weight = value_.random ? display_weight_for_index(i) : 1.0;
        const int handle_y = weight_y_for_value(weight);
        const bool handle_active = dragging_ && drag_handle_index_ == i;
        const bool line_hovered = hovered_handle_index_ == i || handle_active;
        SDL_Color line_col = (i == 2) ? DMStyles::AccentColor() : SDL_Color{154, 164, 188, 220};
        if (line_hovered) {
            line_col = SDL_Color{255, 207, 136, 255};
        }
        SDL_SetRenderDrawColor(r, line_col.r, line_col.g, line_col.b, line_col.a);
        SDL_RenderLine(r, x, baseline, x, handle_y);
        if (line_hovered) {
            SDL_RenderLine(r, x - 1, baseline, x - 1, handle_y);
            SDL_RenderLine(r, x + 1, baseline, x + 1, handle_y);
        }

        SDL_Rect value_rect = column_value_rect(i);
        const std::string value_text = format_value(display_value(raw_value_for_index(i)));
        SDL_Point value_size = DMFontCache::instance().measure_text(label_style, value_text);
        const int text_x = std::clamp(value_rect.x + (value_rect.w - value_size.x) / 2,
                                      hist.x,
                                      std::max(hist.x, hist.x + hist.w - value_size.x));
        DMLabelStyle value_style = label_style;
        if (line_hovered) {
            value_style.color = SDL_Color{255, 225, 176, 255};
        }
        DMFontCache::instance().draw_text(r, value_style, value_text, text_x, std::max(top, baseline - 18));

        SDL_Rect handle{ x - (kWeightedRangeHandleSize / 2),
                         handle_y - (kWeightedRangeHandleSize / 2),
                         kWeightedRangeHandleSize,
                         kWeightedRangeHandleSize };
        SDL_Color handle_fill = (i == 2) ? DMStyles::AccentColor() : DMStyles::HighlightColor();
        if (hovered_handle_index_ == i || handle_active) {
            handle_fill = SDL_Color{255, 184, 78, 255};
        }
        if (handle_active) {
            handle.x -= 2;
            handle.y -= 2;
            handle.w += 4;
            handle.h += 4;
        } else if (hovered_handle_index_ == i) {
            handle.x -= 1;
            handle.y -= 1;
            handle.w += 2;
            handle.h += 2;
        }
        dm_draw::DrawRoundedSolidRect(r, handle, 5, handle_fill);
        dm_draw::DrawRoundedOutline(r, handle, 5, line_hovered ? 2 : 1, line_hovered ? SDL_Color{255, 235, 194, 255} : border);
    }

    if (popup_open_ && active_expanded_ != this) {
        // Keep internal state consistent if a different widget took modal ownership.
        const_cast<DMWeightedRangeWidget*>(this)->popup_open_ = false;
    }
    if (false) {
        const SDL_Rect popup = popup_rect();
        dm_draw::DrawBeveledRect(r, popup, DMStyles::CornerRadius(), DMStyles::BevelDepth(),
                                 SDL_Color{23, 28, 38, 245},
                                 DMStyles::HighlightColor(), DMStyles::ShadowColor(), false,
                                 DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
        dm_draw::DrawRoundedOutline(r, popup, DMStyles::CornerRadius(), 2, SDL_Color{114, 158, 236, 255});
        DMFontCache::instance().draw_text(r, label_style, "Expanded Weighted Range Editor", popup.x + 12, popup.y + 10);

        const SDL_Rect pop_hist = popup_histogram_rect();
        SDL_SetRenderDrawColor(r, grid.r, grid.g, grid.b, grid.a);
        sdl_render::Rect(r, &pop_hist);
        const int pop_top = pop_hist.y + kWeightedRangeWeightTopPad;
        const int pop_baseline = pop_hist.y + pop_hist.h - kWeightedRangeWeightBottomPad;
        SDL_SetRenderDrawColor(r, bar_col.r, bar_col.g, bar_col.b, bar_col.a);
        SDL_Rect pop_bar{pop_hist.x + kWeightedRangeColumnPad, pop_baseline, std::max(1, pop_hist.w - (kWeightedRangeColumnPad * 2)), 4};
        sdl_render::FillRect(r, &pop_bar);
        for (int i = 0; i < 5; ++i) {
            if (!value_.random && i != 2) continue;
            const int center_x = pop_hist.x + pop_hist.w / 2;
            const int x = center_x +
                          ((i == 0) ? -static_cast<int>(std::lround(visual_span_px_)) :
                           (i == 1) ? -static_cast<int>(std::lround(visual_falloff_px_)) :
                           (i == 3) ? static_cast<int>(std::lround(visual_falloff_px_)) :
                           (i == 4) ? static_cast<int>(std::lround(visual_span_px_)) : 0);
            const double normalized = std::clamp(value_.random ? display_weight_for_index(i) : 1.0, 0.0, 1.0);
            const int y = pop_baseline - static_cast<int>(std::lround(normalized * static_cast<double>(std::max(1, pop_baseline - pop_top))));
            SDL_SetRenderDrawColor(r, 255, 207, 136, 220);
            SDL_RenderLine(r, x, pop_baseline, x, y);
            SDL_Rect handle{x - (kWeightedRangeHandleSize / 2), y - (kWeightedRangeHandleSize / 2), kWeightedRangeHandleSize, kWeightedRangeHandleSize};
            dm_draw::DrawRoundedSolidRect(r, handle, 5, SDL_Color{255, 184, 78, 255});
            dm_draw::DrawRoundedOutline(r, handle, 5, 1, SDL_Color{255, 235, 194, 255});
        }
        if (popup_center_stepper_ && popup_span_stepper_ && popup_falloff_stepper_ &&
            popup_center_weight_stepper_ && popup_falloff_weight_stepper_ && popup_edge_weight_stepper_) {
            popup_center_stepper_->render(r);
            popup_span_stepper_->render(r);
            popup_falloff_stepper_->render(r);
            popup_center_weight_stepper_->render(r);
            popup_falloff_weight_stepper_->render(r);
            popup_edge_weight_stepper_->render(r);
        }
    }

    if (tooltip_state_) {
        DMWidgetTooltipRender(r, rect_, *tooltip_state_);
    }
}

bool DMWeightedRangeWidget::has_active_expanded() {
    return active_expanded_ && active_expanded_->popup_open_;
}

bool DMWeightedRangeWidget::handle_active_expanded_event(const SDL_Event& e) {
    if (!has_active_expanded()) {
        return false;
    }
    return active_expanded_->handle_event(e);
}

void DMWeightedRangeWidget::render_active_expanded(SDL_Renderer* r) {
    if (!has_active_expanded() || !r) {
        return;
    }
    DMWeightedRangeWidget* w = active_expanded_;
    int rw = 0;
    int rh = 0;
    SDL_GetRenderOutputSize(r, &rw, &rh);
    if (rw <= 0 || rh <= 0) {
        return;
    }
    w->popup_rect_ = SDL_Rect{
        std::max(8, (rw - kWeightedRangeModalWidth) / 2),
        std::max(8, (rh - kWeightedRangeModalHeight) / 2),
        std::min(kWeightedRangeModalWidth, rw - 16),
        std::min(kWeightedRangeModalHeight, rh - 16)
    };
    w->update_popup_geometry();
    w->ensure_popup_controls();

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 6, 10, 16, 168);
    SDL_FRect scrim{0.0f, 0.0f, static_cast<float>(rw), static_cast<float>(rh)};
    SDL_RenderFillRect(r, &scrim);

    const SDL_Rect popup = w->popup_rect();
    const DMLabelStyle& label_style = DMStyles::Label();
    dm_draw::DrawBeveledRect(r, popup, DMStyles::CornerRadius(), DMStyles::BevelDepth(),
                             SDL_Color{20, 26, 38, 248},
                             DMStyles::HighlightColor(), DMStyles::ShadowColor(), false,
                             DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
    dm_draw::DrawRoundedOutline(r, popup, DMStyles::CornerRadius(), 2, SDL_Color{120, 172, 255, 255});
    DMFontCache::instance().draw_text(r, label_style, "Weighted Random Range Editor", popup.x + 16, popup.y + 12);

    const SDL_Rect pop_hist = w->popup_histogram_rect();
    SDL_SetRenderDrawColor(r, 82, 94, 122, 170);
    sdl_render::Rect(r, &pop_hist);
    const int pop_top = pop_hist.y + kWeightedRangeWeightTopPad;
    const int pop_baseline = pop_hist.y + pop_hist.h - kWeightedRangeWeightBottomPad;
    SDL_Rect pop_bar{pop_hist.x + kWeightedRangeColumnPad, pop_baseline, std::max(1, pop_hist.w - (kWeightedRangeColumnPad * 2)), 4};
    SDL_SetRenderDrawColor(r, 122, 138, 168, 210);
    sdl_render::FillRect(r, &pop_bar);
    for (int i = 0; i < 5; ++i) {
        if (!w->value_.random && i != 2) continue;
        const int center_x = pop_hist.x + pop_hist.w / 2;
        const int x = center_x +
                      ((i == 0) ? -static_cast<int>(std::lround(w->visual_span_px_)) :
                       (i == 1) ? -static_cast<int>(std::lround(w->visual_falloff_px_)) :
                       (i == 3) ? static_cast<int>(std::lround(w->visual_falloff_px_)) :
                       (i == 4) ? static_cast<int>(std::lround(w->visual_span_px_)) : 0);
        const double normalized = std::clamp(w->value_.random ? w->display_weight_for_index(i) : 1.0, 0.0, 1.0);
        const int y = pop_baseline - static_cast<int>(std::lround(normalized * static_cast<double>(std::max(1, pop_baseline - pop_top))));
        SDL_SetRenderDrawColor(r, 255, 207, 136, 220);
        SDL_RenderLine(r, x, pop_baseline, x, y);
        SDL_Rect handle{x - (kWeightedRangeHandleSize / 2), y - (kWeightedRangeHandleSize / 2), kWeightedRangeHandleSize, kWeightedRangeHandleSize};
        dm_draw::DrawRoundedSolidRect(r, handle, 5, SDL_Color{255, 184, 78, 255});
        dm_draw::DrawRoundedOutline(r, handle, 5, (w->selected_node_index_ == i) ? 2 : 1, SDL_Color{255, 235, 194, 255});
    }
    if (w->popup_center_stepper_ && w->popup_span_stepper_ && w->popup_falloff_stepper_ &&
        w->popup_center_weight_stepper_ && w->popup_falloff_weight_stepper_ && w->popup_edge_weight_stepper_) {
        w->popup_center_stepper_->render(r);
        w->popup_span_stepper_->render(r);
        w->popup_falloff_stepper_->render(r);
        w->popup_center_weight_stepper_->render(r);
        w->popup_falloff_weight_stepper_->render(r);
        w->popup_edge_weight_stepper_->render(r);
    }
}

DMDropdown::DMDropdown(const std::string& label, const std::vector<std::string>& options, int idx)
    : label_(label), options_(options), index_(idx) {
    set_selected(idx);
}

DMDropdown::~DMDropdown() {
    if (active_ == this) active_ = nullptr;
}

DMDropdown* DMDropdown::active_ = nullptr;

DMDropdown* DMDropdown::active_dropdown() { return active_; }

struct DMDropdown::OptionEntry {
    int index = 0;
    int delta = 0;
    float scale = 1.0f;
    float alpha = 1.0f;
    SDL_Rect rect{ 0, 0, 0, 0 };
};

bool DMDropdown::build_option_entries(std::vector<OptionEntry>& entries) const {
    entries.clear();
    if (options_.empty()) {
        return false;
    }

    const int base_index = clamp_index(has_pending_index_ ? pending_index_ : index_);
    entries.reserve(std::size(kDropdownCandidates));
    for (const DropdownCandidate& candidate : kDropdownCandidates) {
        const int idx = base_index + candidate.delta;
        if (idx < 0 || idx >= static_cast<int>(options_.size())) {
            continue;
        }
        OptionEntry entry;
        entry.index = idx;
        entry.delta = candidate.delta;
        entry.scale = candidate.scale;
        entry.alpha = candidate.alpha;
        entries.push_back(entry);
    }

    if (entries.empty()) {
        return false;
    }

    const int spacing = 6;
    const int base_w = box_rect_.w;
    const int base_h = box_rect_.h;
    const int center_x = box_rect_.x + base_w / 2;
    const int center_y = box_rect_.y + base_h / 2;

    const auto compute_size = [&](const OptionEntry& e) {
        const int w = std::max(1, static_cast<int>(std::round(base_w * e.scale)));
        const int h = std::max(1, static_cast<int>(std::round(base_h * e.scale)));
        SDL_Rect rect{ center_x - w / 2, center_y - h / 2, w, h };
        return rect;
};

    OptionEntry* center_entry = nullptr;
    for (OptionEntry& entry : entries) {
        if (entry.delta == 0) {
            entry.rect = compute_size(entry);
            center_entry = &entry;
            break;
        }
    }

    if (!center_entry) {
        entries.front().rect = compute_size(entries.front());
        center_entry = &entries.front();
    }

    int current_top = center_entry->rect.y;
    std::vector<OptionEntry*> above;
    std::vector<OptionEntry*> below;
    above.reserve(entries.size());
    below.reserve(entries.size());
    for (OptionEntry& entry : entries) {
        if (&entry == center_entry) continue;
        if (entry.delta < 0) {
            above.push_back(&entry);
        } else {
            below.push_back(&entry);
        }
    }

    std::sort(above.begin(), above.end(), [](const OptionEntry* a, const OptionEntry* b) { return a->delta > b->delta; });
    std::sort(below.begin(), below.end(), [](const OptionEntry* a, const OptionEntry* b) { return a->delta < b->delta; });

    for (OptionEntry* entry : above) {
        SDL_Rect rect = compute_size(*entry);
        rect.y = current_top - spacing - rect.h;
        entry->rect = rect;
        current_top = rect.y;
    }

    int current_bottom = center_entry->rect.y + center_entry->rect.h;
    for (OptionEntry* entry : below) {
        SDL_Rect rect = compute_size(*entry);
        rect.y = current_bottom + spacing;
        entry->rect = rect;
        current_bottom = rect.y + rect.h;
    }

    return true;
}

void DMDropdown::render_active_options(SDL_Renderer* r) {
    if (active_ && active_->focused_) active_->render_options(r);
}

void DMDropdown::set_rect(const SDL_Rect& r) {
    rect_ = r;
    label_height_ = compute_label_height(rect_.w);
    int y = rect_.y + kBoxTopPadding;
    label_rect_ = SDL_Rect{ rect_.x, y, rect_.w, label_height_ };
    int box_y = y + label_height_ + (label_height_ > 0 ? kLabelControlGap : 0);
    int available = rect_.h - (box_y - rect_.y) - kBoxBottomPadding;
    int box_h = std::max(kDropdownControlHeight, available);
    box_rect_ = SDL_Rect{ rect_.x, box_y, rect_.w, box_h };
    rect_.h = (box_rect_.y - rect_.y) + box_rect_.h + kBoxBottomPadding;
}

void DMDropdown::set_tooltip_state(DMWidgetTooltipState* state) {
    tooltip_state_ = state;
    if (tooltip_state_) {
        DMWidgetTooltipResetHover(*tooltip_state_);
    }
}

void DMDropdown::set_selected(int idx) {
    const int clamped = clamp_index(idx);
    const bool changed = clamped != index_;
    index_ = clamped;
    pending_index_ = index_;
    has_pending_index_ = focused_;
    hovered_option_index_ = focused_ ? pending_index_ : -1;

    if (changed && focused_ && active_ == this) {
        focused_ = false;
        has_pending_index_ = false;
        hovered_option_index_ = -1;
        if (active_ == this) active_ = nullptr;
    }
    if (changed && on_selection_changed_) {
        on_selection_changed_(index_);
    }
}

void DMDropdown::set_label(const std::string& label) {
    label_ = label;
    if (rect_.w > 0) {
        set_rect(rect_);
    }
}

bool DMDropdown::handle_event(const SDL_Event& e) {
    if (tooltip_state_ && DMWidgetTooltipHandleEvent(e, rect_, *tooltip_state_)) {
        return true;
    }
    if (e.type == SDL_EVENT_KEY_DOWN && focused_) {
        switch (e.key.key) {
        case SDLK_UP:
        case SDLK_W: {
            if (!has_pending_index_) {
                pending_index_ = index_;
                has_pending_index_ = true;
            }
            int new_index = pending_index_ - 1;
            if (new_index < 0) {
                new_index = static_cast<int>(options_.size()) - 1;
            }
            pending_index_ = new_index;
            hovered_option_index_ = pending_index_;
            return true;
        }
        case SDLK_DOWN:
        case SDLK_S: {
            if (!has_pending_index_) {
                pending_index_ = index_;
                has_pending_index_ = true;
            }
            int new_index = pending_index_ + 1;
            if (new_index >= static_cast<int>(options_.size())) {
                new_index = 0;
            }
            pending_index_ = new_index;
            hovered_option_index_ = pending_index_;
            return true;
        }
        case SDLK_RETURN:
        case SDLK_KP_ENTER: {
            commit_pending_selection();
            focused_ = false;
            has_pending_index_ = false;
            hovered_option_index_ = -1;
            if (active_ == this) active_ = nullptr;
            return true;
        }
        default:
            break;
        }
    }
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        SDL_Point p = sdl_mouse_util::MotionPoint(e.motion);
        const bool inside_box = SDL_PointInRect(&p, &box_rect_);
        hovered_ = inside_box;

        bool inside_options = false;
        bool inside_options_area = false;
        int hovered_option = -1;
        std::vector<OptionEntry> entries;
        if (focused_ && active_ == this && build_option_entries(entries)) {
            SDL_Rect options_bounds{0, 0, 0, 0};
            bool have_bounds = false;
            for (const OptionEntry& entry : entries) {
                if (!have_bounds) {
                    options_bounds = entry.rect;
                    have_bounds = true;
                } else {
                    const int min_x = std::min(options_bounds.x, entry.rect.x);
                    const int min_y = std::min(options_bounds.y, entry.rect.y);
                    const int max_x = std::max(options_bounds.x + options_bounds.w, entry.rect.x + entry.rect.w);
                    const int max_y = std::max(options_bounds.y + options_bounds.h, entry.rect.y + entry.rect.h);
                    options_bounds.x = min_x;
                    options_bounds.y = min_y;
                    options_bounds.w = max_x - min_x;
                    options_bounds.h = max_y - min_y;
                }

                if (!inside_options && SDL_PointInRect(&p, &entry.rect)) {
                    inside_options = true;
                    hovered_option = entry.index;
                }
            }
            if (have_bounds) {
                inside_options_area = SDL_PointInRect(&p, &options_bounds);
            }
        }

        bool consumed = false;
        if (inside_options) {
            hovered_option_index_ = hovered_option;
            consumed = true;
        } else {
            hovered_option_index_ = -1;
        }

        if (focused_ && active_ == this && !inside_box && !inside_options_area) {
            const bool applied = commit_pending_selection();
            focused_ = false;
            has_pending_index_ = false;
            hovered_option_index_ = -1;
            if (active_ == this) active_ = nullptr;
            return applied || consumed;
        }
        return consumed;
    }

    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
        const bool inside = SDL_PointInRect(&p, &box_rect_);
        if (inside) {
            if (focused_ && active_ == this) {
                commit_pending_selection();
                focused_ = false;
                has_pending_index_ = false;
                hovered_option_index_ = -1;
                if (active_ == this) active_ = nullptr;
                return true;
            }
            begin_focus();
            return true;
        }
        if (focused_ && active_ == this) {
            std::vector<OptionEntry> entries;
            if (build_option_entries(entries)) {
                for (const OptionEntry& entry : entries) {
                    if (SDL_PointInRect(&p, &entry.rect)) {
                        pending_index_ = entry.index;
                        has_pending_index_ = true;
                        hovered_option_index_ = -1;
                        const bool applied = commit_pending_selection();
                        focused_ = false;
                        has_pending_index_ = false;
                        if (active_ == this) active_ = nullptr;
                        return true;
                    }
                }
            }
            const bool applied = commit_pending_selection();
            focused_ = false;
            has_pending_index_ = false;
            hovered_option_index_ = -1;
            if (active_ == this) active_ = nullptr;
            return applied;
        }
        return false;
    }

    if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        // Capture all wheel events while this dropdown is active to prevent
        // scroll events from leaking to underlying widgets (e.g., sliders).
        if (active_ != this) {
            return false;
        }
        if (options_.empty()) {
            return true;
        }
        if (!has_pending_index_) {
            pending_index_ = index_;
            has_pending_index_ = true;
        }
        int raw_delta = e.wheel.integer_y;
        if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            raw_delta = -raw_delta;
        }
        const int delta = -raw_delta;
        if (delta == 0) return false;
        int target = pending_index_ + delta;
        int clamped = clamp_index(target);
        if (clamped == pending_index_) {
            pending_index_ = clamped;
            return false;
        }
        pending_index_ = clamped;
        hovered_option_index_ = pending_index_;
        return true;
    }

    return false;
}

void DMDropdown::render(SDL_Renderer* r) const {
    const DMTextBoxStyle& st = DMStyles::TextBox();
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    const bool has_focus = focused_ && active_ == this;
    const SDL_Color fill = has_focus ? DMStyles::TextboxHoverFill() : (hovered_ ? DMStyles::TextboxHoverFill() : DMStyles::TextboxBaseFill());
    dm_draw::DrawBeveledRect( r, box_rect_, DMStyles::CornerRadius(), DMStyles::BevelDepth(), fill, DMStyles::HighlightColor(), DMStyles::ShadowColor(), false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
    if (!label_.empty() && label_height_ > 0) {
        DMLabelStyle lbl = DMStyles::Label();
        TTF_Font* f = TTF_OpenFont(lbl.font_path.c_str(), lbl.font_size);
        if (f) {
            SDL_Surface* surf = ttf_util::RenderTextBlended(f, label_.c_str(), lbl.color);
            if (surf) {
                SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
                if (tex) {
                    SDL_Rect dst{ label_rect_.x, label_rect_.y, surf->w, surf->h };
                    sdl_render::Texture(r, tex, nullptr, &dst);
                    SDL_DestroyTexture(tex);
                }
                SDL_DestroySurface(surf);
            }
            TTF_CloseFont(f);
        }
    }
    SDL_Color border = st.border;
    if (hovered_ && !has_focus) {
        border = DMStyles::TextboxHoverOutline();
    }
    if (has_focus) {
        const SDL_Color focus = DMStyles::TextboxFocusOutline();
        dm_draw::DrawRoundedFocusRing( r, box_rect_, DMStyles::CornerRadius(), kFocusRingThickness, focus);
        border = DMStyles::TextboxActiveOutline();
    }
    dm_draw::DrawRoundedOutline( r, box_rect_, DMStyles::CornerRadius(), kControlOutlineThickness, border);
    DMLabelStyle labelStyle{ st.label.font_path, st.label.font_size, st.text };

    const DMButtonStyle& arrow_style = DMStyles::IconButton();
    DMLabelStyle arrow_label{arrow_style.label.font_path, arrow_style.label.font_size, arrow_style.text};
    const std::string arrow_icon = std::string(DMIcons::NavDown());
    const SDL_Point arrow_size = DMFontCache::instance().measure_text(arrow_label, arrow_icon);
    int arrow_space = box_rect_.w > 0 ? std::max({arrow_size.x + 12, box_rect_.h / 2, 24}) : 0;
    arrow_space = std::min(arrow_space, std::max(12, box_rect_.w / 2));
    arrow_space = std::min(arrow_space, box_rect_.w);
    TTF_Font* f = TTF_OpenFont(labelStyle.font_path.c_str(), labelStyle.font_size);
    if (f) {
        int safe_idx = 0;
        if (!options_.empty()) {
            int display_idx = has_focus && has_pending_index_ ? pending_index_ : index_;
            display_idx = clamp_index(display_idx);
            safe_idx = display_idx;
        }
        const char* display = options_.empty() ? "" : options_[safe_idx].c_str();
        SDL_Surface* surf = ttf_util::RenderTextBlended(f, display, labelStyle.color);
        if (surf) {
            SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
            if (tex) {
                const int text_area_width = std::max(0, box_rect_.w - arrow_space);
                const int text_x_min = box_rect_.x;
                const int text_x_max = box_rect_.x + std::max(0, text_area_width - surf->w);
                const int centered = box_rect_.x + std::max(0, (text_area_width - surf->w) / 2);
                const int dst_x = std::clamp(centered, text_x_min, text_x_max);
                SDL_Rect dst{ dst_x, box_rect_.y + (box_rect_.h - surf->h)/2, surf->w, surf->h };
                sdl_render::Texture(r, tex, nullptr, &dst);
                SDL_DestroyTexture(tex);
            }
            SDL_DestroySurface(surf);
        }
        TTF_CloseFont(f);
    }

    if (arrow_space > 0) {
        arrow_label.color = arrow_style.text;
        if (has_focus) {
            arrow_label.color = DMStyles::ButtonFocusOutline();
        } else if (hovered_) {
            arrow_label.color = dm_draw::LightenColor(arrow_label.color, 0.08f);
        } else {
            arrow_label.color = dm_draw::DarkenColor(arrow_label.color, 0.05f);
        }
        SDL_Point rendered_size = DMFontCache::instance().measure_text(arrow_label, arrow_icon);
        const int arrow_x = box_rect_.x + box_rect_.w - arrow_space + std::max(0, (arrow_space - rendered_size.x) / 2);
        const int arrow_y = box_rect_.y + (box_rect_.h - rendered_size.y) / 2;
        DMFontCache::instance().draw_text(r, arrow_label, arrow_icon, arrow_x, arrow_y);
    }
    if (tooltip_state_) {
        DMWidgetTooltipRender(r, rect_, *tooltip_state_);
    }
}

void DMDropdown::render_options(SDL_Renderer* r) const {
    if (!(focused_ && active_ == this)) return;
    if (options_.empty()) return;

    const DMTextBoxStyle& tb = DMStyles::TextBox();
    DMLabelStyle labelStyle{ tb.label.font_path, tb.label.font_size, tb.text };
    const SDL_Color focus_border = DMStyles::TextboxActiveOutline();
    const SDL_Color base_border = DMStyles::TextboxHoverOutline();
    const SDL_Color base_fill = DMStyles::TextboxBaseFill();
    const SDL_Color focus_fill = DMStyles::TextboxHoverFill();
    const SDL_Color highlight = DMStyles::HighlightColor();
    const SDL_Color shadow = DMStyles::ShadowColor();

    std::vector<OptionEntry> entries;
    if (!build_option_entries(entries)) {
        return;
    }

    const int selected_index = clamp_index(has_pending_index_ ? pending_index_ : index_);

    TTF_Font* font = TTF_OpenFont(labelStyle.font_path.c_str(), labelStyle.font_size);
    for (const OptionEntry& entry : entries) {
        SDL_Rect rect = entry.rect;
        const bool is_selected = (entry.index == selected_index);
        const bool is_hovered = (entry.index == hovered_option_index_);
        const bool emphasize = is_selected || is_hovered;

        SDL_Color fill = emphasize ? focus_fill : base_fill;
        SDL_Color border = emphasize ? focus_border : base_border;
        SDL_Color hl = highlight;
        SDL_Color sh = shadow;
        if (!emphasize) {
            fill = ApplyAlpha(fill, entry.alpha);
            border = ApplyAlpha(border, entry.alpha);
            hl = ApplyAlpha(hl, entry.alpha);
            sh = ApplyAlpha(sh, entry.alpha);
        }

        dm_draw::DrawBeveledRect( r, rect, DMStyles::CornerRadius(), DMStyles::BevelDepth(), fill, hl, sh, false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());

        if (emphasize) {
            const SDL_Color focus_ring = DMStyles::TextboxFocusOutline();
            dm_draw::DrawRoundedFocusRing( r, rect, DMStyles::CornerRadius(), kFocusRingThickness, focus_ring);
        }

        dm_draw::DrawRoundedOutline( r, rect, DMStyles::CornerRadius(), kControlOutlineThickness, border);

        if (!font) {
            continue;
        }

        SDL_Color text_color = labelStyle.color;
        if (!emphasize) {
            text_color = ApplyAlpha(text_color, entry.alpha);
        }

        SDL_Surface* surf = ttf_util::RenderTextBlended(font, options_[entry.index].c_str(), text_color);
        if (surf) {
            SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
            if (tex) {
                SDL_Rect dst{ rect.x + (rect.w - surf->w) / 2, rect.y + (rect.h - surf->h) / 2, surf->w, surf->h };
                sdl_render::Texture(r, tex, nullptr, &dst);
                SDL_DestroyTexture(tex);
            }
            SDL_DestroySurface(surf);
        }
    }
    if (font) {
        TTF_CloseFont(font);
    }
}

int DMDropdown::preferred_height(int width) const {
    int label_h = compute_label_height(width);
    return kBoxTopPadding + label_h + (label_h > 0 ? kLabelControlGap : 0) + kDropdownControlHeight + kBoxBottomPadding;
}

int DMDropdown::compute_label_height(int width) const {
    if (label_.empty()) return 0;
    DMLabelStyle lbl = DMStyles::Label();
    TTF_Font* f = TTF_OpenFont(lbl.font_path.c_str(), lbl.font_size);
    if (!f) return lbl.font_size;
    int text_w = 0;
    int text_h = 0;
    ttf_util::GetStringSize(f, label_, &text_w, &text_h);
    TTF_CloseFont(f);
    (void)width;
    return text_h;
}

int DMDropdown::height() {
    DMLabelStyle lbl = DMStyles::Label();
    return kBoxTopPadding + lbl.font_size + kLabelControlGap + kDropdownControlHeight + kBoxBottomPadding;
}

int DMDropdown::label_space() const { return label_height_; }

SDL_Rect DMDropdown::box_rect() const { return box_rect_; }

SDL_Rect DMDropdown::label_rect() const { return label_rect_; }

int DMDropdown::clamp_index(int idx) const {
    if (options_.empty()) {
        return 0;
    }
    if (idx < 0) return 0;
    int max_index = static_cast<int>(options_.size()) - 1;
    if (idx > max_index) idx = max_index;
    return idx;
}

bool DMDropdown::commit_pending_selection() {
    if (options_.empty()) {
        has_pending_index_ = false;
        return false;
    }
    const int target = clamp_index(has_pending_index_ ? pending_index_ : index_);
    const bool changed = target != index_;
    index_ = target;
    pending_index_ = target;
    has_pending_index_ = false;
    if (changed && on_selection_changed_) {
        on_selection_changed_(index_);
    }
    return changed;
}

void DMDropdown::begin_focus() {
    focused_ = true;
    DMDropdown* previous_active = active_;
    if (previous_active && previous_active != this) {
        previous_active->commit_pending_selection();
        if (active_ == previous_active) {
            previous_active->focused_ = false;
            previous_active->has_pending_index_ = false;
            previous_active->hovered_option_index_ = -1;
        }
    }
    active_ = this;
    pending_index_ = index_;
    has_pending_index_ = true;
    hovered_option_index_ = pending_index_;
}


