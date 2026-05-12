#include "dev_color_picker.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>

#include "dm_styles.hpp"
#include "draw_utils.hpp"
#include "utils/display_color.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/sdl_mouse_utils.hpp"
#include "utils/ttf_render_utils.hpp"
#include "widgets.hpp"

namespace {

constexpr int kPanelWidth = 360;
constexpr int kPanelHeight = 420;
constexpr int kPadding = 12;
constexpr int kHueRingOuterRadius = 95;
constexpr int kHueRingInnerRadius = 68;
constexpr int kHueSegments = 72;
constexpr int kSquareSize = 140;
constexpr float kPi = 3.14159265358979323846f;

float clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

SDL_Color hsv_to_rgb(float hue_deg, float saturation, float value) {
    SDL_Color c = utils::display_color::hsv_to_rgb(
        static_cast<double>(hue_deg),
        static_cast<double>(clamp01(saturation)),
        static_cast<double>(clamp01(value)));
    c.a = 255;
    return c;
}

float normalize_hue(float hue_deg) {
    float h = std::fmod(hue_deg, 360.0f);
    if (h < 0.0f) {
        h += 360.0f;
    }
    return h;
}

void draw_text(SDL_Renderer* renderer, const DMLabelStyle& style, const char* text, int x, int y) {
    if (!renderer || !text || !*text) {
        return;
    }
    TTF_Font* font = style.open_font();
    if (!font) {
        return;
    }
    SDL_Surface* surface = ttf_util::RenderTextBlended(font, text, style.color);
    if (!surface) {
        TTF_CloseFont(font);
        return;
    }
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture) {
        SDL_Rect dst{x, y, surface->w, surface->h};
        sdl_render::Texture(renderer, texture, nullptr, &dst);
        SDL_DestroyTexture(texture);
    }
    SDL_DestroySurface(surface);
    TTF_CloseFont(font);
}

} // namespace

DevColorPicker::DevColorPicker()
    : apply_button_(std::make_unique<DMButton>("Apply", &DMStyles::PrimaryButton(), 96, DMButton::height())),
      cancel_button_(std::make_unique<DMButton>("Cancel", &DMStyles::HeaderButton(), 96, DMButton::height())) {}

DevColorPicker::~DevColorPicker() = default;

void DevColorPicker::open(SDL_Color initial_color, ApplyCallback on_apply, CancelCallback on_cancel) {
    open_ = true;
    on_apply_ = std::move(on_apply);
    on_cancel_ = std::move(on_cancel);
    set_from_color(initial_color);
    layout();
}

void DevColorPicker::close(bool apply_changes) {
    if (!open_) {
        return;
    }
    open_ = false;
    if (apply_changes) {
        if (on_apply_) {
            on_apply_(preview_color_);
        }
    } else if (on_cancel_) {
        on_cancel_();
    }
    on_apply_ = nullptr;
    on_cancel_ = nullptr;
}

void DevColorPicker::set_screen_size(int width, int height) {
    screen_w_ = std::max(0, width);
    screen_h_ = std::max(0, height);
    if (open_) {
        layout();
    }
}

SDL_Color DevColorPicker::preview_color() const {
    return preview_color_;
}

void DevColorPicker::layout() {
    const int panel_w = std::min(kPanelWidth, std::max(220, screen_w_ - 40));
    const int panel_h = std::min(kPanelHeight, std::max(260, screen_h_ - 40));
    panel_rect_.w = panel_w;
    panel_rect_.h = panel_h;
    panel_rect_.x = std::max(0, (screen_w_ - panel_w) / 2);
    panel_rect_.y = std::max(0, (screen_h_ - panel_h) / 2);

    const int center_x = panel_rect_.x + kPadding + kHueRingOuterRadius;
    const int center_y = panel_rect_.y + kPadding + 18 + kHueRingOuterRadius;
    hue_ring_rect_ = SDL_Rect{
        center_x - kHueRingOuterRadius,
        center_y - kHueRingOuterRadius,
        kHueRingOuterRadius * 2,
        kHueRingOuterRadius * 2};

    sv_square_rect_ = SDL_Rect{
        panel_rect_.x + panel_rect_.w - kPadding - kSquareSize,
        center_y - (kSquareSize / 2),
        kSquareSize,
        kSquareSize};

    preview_rect_ = SDL_Rect{
        panel_rect_.x + kPadding,
        panel_rect_.y + panel_rect_.h - kPadding - 54,
        panel_rect_.w - (kPadding * 2),
        22};

    const int button_w = 96;
    const int button_h = DMButton::height();
    cancel_button_->set_rect(SDL_Rect{
        panel_rect_.x + panel_rect_.w - kPadding - button_w,
        panel_rect_.y + panel_rect_.h - kPadding - button_h,
        button_w,
        button_h});
    apply_button_->set_rect(SDL_Rect{
        panel_rect_.x + panel_rect_.w - kPadding - (button_w * 2) - 8,
        panel_rect_.y + panel_rect_.h - kPadding - button_h,
        button_w,
        button_h});
}

void DevColorPicker::set_from_color(SDL_Color color) {
    color.a = 255;
    rgb_to_hsv(color, hue_deg_, saturation_, value_);
    update_preview_from_hsv();
}

void DevColorPicker::update_preview_from_hsv() {
    hue_deg_ = normalize_hue(hue_deg_);
    saturation_ = clamp01(saturation_);
    value_ = clamp01(value_);
    preview_color_ = hsv_to_rgb(hue_deg_, saturation_, value_);
}

void DevColorPicker::rgb_to_hsv(SDL_Color color, float& out_h, float& out_s, float& out_v) {
    const float r = static_cast<float>(color.r) / 255.0f;
    const float g = static_cast<float>(color.g) / 255.0f;
    const float b = static_cast<float>(color.b) / 255.0f;

    const float max_c = std::max({r, g, b});
    const float min_c = std::min({r, g, b});
    const float delta = max_c - min_c;

    out_v = max_c;
    if (delta <= 1e-6f) {
        out_h = 0.0f;
        out_s = 0.0f;
        return;
    }

    out_s = (max_c <= 1e-6f) ? 0.0f : (delta / max_c);

    if (std::abs(max_c - r) < 1e-6f) {
        out_h = 60.0f * std::fmod(((g - b) / delta), 6.0f);
    } else if (std::abs(max_c - g) < 1e-6f) {
        out_h = 60.0f * (((b - r) / delta) + 2.0f);
    } else {
        out_h = 60.0f * (((r - g) / delta) + 4.0f);
    }
    out_h = normalize_hue(out_h);
}

bool DevColorPicker::handle_picker_pointer(int x, int y) {
    const int cx = hue_ring_rect_.x + hue_ring_rect_.w / 2;
    const int cy = hue_ring_rect_.y + hue_ring_rect_.h / 2;
    const float dx = static_cast<float>(x - cx);
    const float dy = static_cast<float>(y - cy);
    const float dist = std::sqrt(dx * dx + dy * dy);

    if (dist >= static_cast<float>(kHueRingInnerRadius) && dist <= static_cast<float>(kHueRingOuterRadius)) {
        const float angle = std::atan2(dy, dx);
        hue_deg_ = normalize_hue((angle * 180.0f / kPi) + 360.0f);
        update_preview_from_hsv();
        return true;
    }

    if (x >= sv_square_rect_.x && x < sv_square_rect_.x + sv_square_rect_.w &&
        y >= sv_square_rect_.y && y < sv_square_rect_.y + sv_square_rect_.h) {
        const float rel_x = static_cast<float>(x - sv_square_rect_.x) / static_cast<float>(std::max(1, sv_square_rect_.w - 1));
        const float rel_y = static_cast<float>(y - sv_square_rect_.y) / static_cast<float>(std::max(1, sv_square_rect_.h - 1));
        saturation_ = clamp01(rel_x);
        value_ = clamp01(1.0f - rel_y);
        update_preview_from_hsv();
        return true;
    }

    return false;
}

bool DevColorPicker::handle_event(const SDL_Event& event) {
    if (!open_) {
        return false;
    }

    bool used = false;
    if (apply_button_ && apply_button_->handle_event(event)) {
        used = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            close(true);
        }
    }
    if (!open_) {
        return true;
    }
    if (cancel_button_ && cancel_button_->handle_event(event)) {
        used = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            close(false);
        }
    }
    if (!open_) {
        return true;
    }

    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
        close(false);
        return true;
    }

    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
        const int x = static_cast<int>(std::lround(event.button.x));
        const int y = static_cast<int>(std::lround(event.button.y));
        SDL_Point p{x, y};
        if (!SDL_PointInRect(&p, &panel_rect_)) {
            close(false);
            return true;
        }
        if (handle_picker_pointer(x, y)) {
            return true;
        }
    } else if (event.type == SDL_EVENT_MOUSE_MOTION && (event.motion.state & SDL_BUTTON_LMASK) != 0) {
        const int x = static_cast<int>(std::lround(event.motion.x));
        const int y = static_cast<int>(std::lround(event.motion.y));
        if (handle_picker_pointer(x, y)) {
            return true;
        }
    }

    return used || true;
}

void DevColorPicker::render_hue_ring(SDL_Renderer* renderer) const {
    const int cx = hue_ring_rect_.x + hue_ring_rect_.w / 2;
    const int cy = hue_ring_rect_.y + hue_ring_rect_.h / 2;
    for (int i = 0; i < kHueSegments; ++i) {
        const float a0 = (static_cast<float>(i) / static_cast<float>(kHueSegments)) * 2.0f * kPi;
        const float a1 = (static_cast<float>(i + 1) / static_cast<float>(kHueSegments)) * 2.0f * kPi;
        const SDL_Color c0 = hsv_to_rgb((static_cast<float>(i) / static_cast<float>(kHueSegments)) * 360.0f, 1.0f, 1.0f);
        const SDL_Color c1 = hsv_to_rgb((static_cast<float>(i + 1) / static_cast<float>(kHueSegments)) * 360.0f, 1.0f, 1.0f);

        SDL_Vertex verts[4]{};
        verts[0].position = SDL_FPoint{cx + std::cos(a0) * kHueRingOuterRadius, cy + std::sin(a0) * kHueRingOuterRadius};
        verts[1].position = SDL_FPoint{cx + std::cos(a1) * kHueRingOuterRadius, cy + std::sin(a1) * kHueRingOuterRadius};
        verts[2].position = SDL_FPoint{cx + std::cos(a1) * kHueRingInnerRadius, cy + std::sin(a1) * kHueRingInnerRadius};
        verts[3].position = SDL_FPoint{cx + std::cos(a0) * kHueRingInnerRadius, cy + std::sin(a0) * kHueRingInnerRadius};
        verts[0].color = SDL_FColor{c0.r / 255.0f, c0.g / 255.0f, c0.b / 255.0f, 1.0f};
        verts[1].color = SDL_FColor{c1.r / 255.0f, c1.g / 255.0f, c1.b / 255.0f, 1.0f};
        verts[2].color = SDL_FColor{c1.r / 255.0f, c1.g / 255.0f, c1.b / 255.0f, 1.0f};
        verts[3].color = SDL_FColor{c0.r / 255.0f, c0.g / 255.0f, c0.b / 255.0f, 1.0f};
        const int idx[6]{0, 1, 2, 0, 2, 3};
        SDL_RenderGeometry(renderer, nullptr, verts, 4, idx, 6);
    }

    const float marker_angle = (hue_deg_ / 180.0f) * kPi;
    const int mx = static_cast<int>(std::lround(cx + std::cos(marker_angle) * (kHueRingOuterRadius - 4)));
    const int my = static_cast<int>(std::lround(cy + std::sin(marker_angle) * (kHueRingOuterRadius - 4)));
    SDL_Rect marker{mx - 4, my - 4, 8, 8};
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    sdl_render::Rect(renderer, &marker);
}

void DevColorPicker::render_sv_square(SDL_Renderer* renderer) const {
    const SDL_Color hue_color = hsv_to_rgb(hue_deg_, 1.0f, 1.0f);
    const int w = std::max(1, sv_square_rect_.w);
    const int h = std::max(1, sv_square_rect_.h);
    const int w_den = std::max(1, w - 1);
    const int h_den = std::max(1, h - 1);
    for (int y = 0; y < h; ++y) {
        float v = 1.0f - (static_cast<float>(y) / static_cast<float>(h_den));
        for (int x = 0; x < w; ++x) {
            float s = static_cast<float>(x) / static_cast<float>(w_den);
            SDL_Color c = hsv_to_rgb(hue_deg_, s, v);
            SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, 255);
            SDL_RenderPoint(renderer, static_cast<float>(sv_square_rect_.x + x), static_cast<float>(sv_square_rect_.y + y));
        }
    }
    SDL_SetRenderDrawColor(renderer, 220, 220, 220, 255);
    sdl_render::Rect(renderer, &sv_square_rect_);

    const int mx = sv_square_rect_.x + static_cast<int>(std::lround(saturation_ * static_cast<float>(w - 1)));
    const int my = sv_square_rect_.y + static_cast<int>(std::lround((1.0f - value_) * static_cast<float>(h - 1)));
    SDL_Rect marker{mx - 4, my - 4, 8, 8};
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    sdl_render::Rect(renderer, &marker);

    (void)hue_color;
}

void DevColorPicker::render(SDL_Renderer* renderer) const {
    if (!open_ || !renderer) {
        return;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 130);
    SDL_Rect full{0, 0, screen_w_, screen_h_};
    sdl_render::FillRect(renderer, &full);

    SDL_SetRenderDrawColor(renderer, DMStyles::PanelBG().r, DMStyles::PanelBG().g, DMStyles::PanelBG().b, 245);
    sdl_render::FillRect(renderer, &panel_rect_);
    SDL_SetRenderDrawColor(renderer, DMStyles::Border().r, DMStyles::Border().g, DMStyles::Border().b, 255);
    sdl_render::Rect(renderer, &panel_rect_);

    draw_text(renderer, DMStyles::Label(), "Floor Color Picker", panel_rect_.x + kPadding, panel_rect_.y + kPadding);

    render_hue_ring(renderer);
    render_sv_square(renderer);

    SDL_SetRenderDrawColor(renderer, preview_color_.r, preview_color_.g, preview_color_.b, 255);
    sdl_render::FillRect(renderer, &preview_rect_);
    SDL_SetRenderDrawColor(renderer, DMStyles::Border().r, DMStyles::Border().g, DMStyles::Border().b, 255);
    sdl_render::Rect(renderer, &preview_rect_);

    if (apply_button_) {
        apply_button_->render(renderer);
    }
    if (cancel_button_) {
        cancel_button_->render(renderer);
    }
}
