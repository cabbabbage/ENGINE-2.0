#include "core/popup_manager.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/ttf_render_utils.hpp"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace {

struct SDLSurfaceDeleter {
    void operator()(SDL_Surface* surface) const {
        if (surface) {
            SDL_DestroySurface(surface);
        }
    }
};

constexpr Uint32 kIndicatorFadeMs = 400;
constexpr Uint32 kRoomChangeHoldMs = 1200;
constexpr Uint32 kMinToastDurationMs = 100;
constexpr int kPanelPadding = 14;
constexpr int kSmallGap = 8;

TTF_Font* open_popup_font(int point_size) {
    const int clamped_size = std::max(10, point_size);
#ifdef _WIN32
    static constexpr std::array<const char*, 2> kCandidates{
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf"
    };
#else
    static constexpr std::array<const char*, 2> kCandidates{
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"
    };
#endif
    for (const char* path : kCandidates) {
        if (!path) {
            continue;
        }
        if (TTF_Font* font = TTF_OpenFont(path, clamped_size)) {
            return font;
        }
    }
    return nullptr;
}

void draw_popup_box(SDL_Renderer* renderer,
                    const SDL_Rect& rect,
                    SDL_Color fill,
                    SDL_Color border) {
    if (!renderer || rect.w <= 0 || rect.h <= 0) {
        return;
    }
    SDL_BlendMode previous_blend = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(renderer, &previous_blend);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, fill.r, fill.g, fill.b, fill.a);
    sdl_render::FillRect(renderer, &rect);
    SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, border.a);
    sdl_render::Rect(renderer, &rect);
    SDL_SetRenderDrawBlendMode(renderer, previous_blend);
}

Uint8 alpha_from_fraction(float value) {
    const int clamped = std::clamp(static_cast<int>(std::lround(value * 255.0f)), 0, 255);
    return static_cast<Uint8>(clamped);
}

std::string sanitize_room_label(const std::string& room_name) {
    return room_name;
}

} // namespace

PopupManager::PopupManager()
    : toast_style_(),
      indicator_style_() {
    toast_style_.font_size = 24;
    toast_style_.color = SDL_Color{236, 245, 255, 255};
    indicator_style_.font_size = 14;
    indicator_style_.color = SDL_Color{221, 228, 240, 255};
}

void PopupManager::show_toast(const std::string& message, Uint32 duration_ms) {
    const Uint32 now = SDL_GetTicks();
    if (message.empty()) {
        toast_.visible = false;
        toast_.texture.reset();
        return;
    }
    toast_.message = message;
    toast_.dirty = true;
    toast_.visible = true;
    const Uint32 clamped_duration = std::max(duration_ms, kMinToastDurationMs);
    toast_.expiry_ms = now + clamped_duration;
}

void PopupManager::notify_camera_activity(const std::string& room_name, bool active, Uint32 timestamp_ms) {
    if (room_name.empty()) {
        if (!active) {
            indicator_.showing = false;
            indicator_.texture.reset();
        }
        return;
    }

    const std::string label = sanitize_room_label(room_name);
    if (indicator_.room_name != room_name || indicator_.label_text != label) {
        indicator_.room_name = room_name;
        indicator_.label_text = label;
        indicator_.dirty = true;
        indicator_.texture.reset();
    }

    indicator_.showing = true;
    indicator_.hold_until_ms = 0;

    if (active) {
        indicator_.active = true;
        indicator_.fade_start_ms = 0;
        indicator_.fade_end_ms = 0;
    } else {
        indicator_.active = false;
        indicator_.fade_start_ms = timestamp_ms;
        indicator_.fade_end_ms = timestamp_ms + kIndicatorFadeMs;
    }
}

void PopupManager::notify_room_change(const std::string& room_name, Uint32 timestamp_ms) {
    if (room_name.empty()) {
        indicator_.showing = false;
        indicator_.texture.reset();
        return;
    }

    const std::string label = sanitize_room_label(room_name);
    if (indicator_.room_name != room_name || indicator_.label_text != label) {
        indicator_.room_name = room_name;
        indicator_.label_text = label;
        indicator_.dirty = true;
        indicator_.texture.reset();
    }

    indicator_.active = false;
    indicator_.showing = true;
    indicator_.hold_until_ms = timestamp_ms + kRoomChangeHoldMs;
    indicator_.fade_start_ms = indicator_.hold_until_ms;
    indicator_.fade_end_ms = indicator_.fade_start_ms + kIndicatorFadeMs;
}

void PopupManager::update(Uint32 now) {
    if (toast_.visible && now >= toast_.expiry_ms) {
        toast_.visible = false;
        toast_.texture.reset();
    }

    if (indicator_.showing && !indicator_.active && indicator_.fade_end_ms > 0 && now >= indicator_.fade_end_ms) {
        indicator_.showing = false;
        indicator_.texture.reset();
    }
}

void PopupManager::render(SDL_Renderer* renderer, int screen_w, int screen_h, Uint32 now) {
    if (!renderer || screen_w <= 0 || screen_h <= 0) {
        return;
    }

    if (toast_.visible) {
        rebuild_toast_texture(renderer);
        if (toast_.texture && toast_.width > 0 && toast_.height > 0) {
            const int padding_x = kPanelPadding;
            const int padding_y = kSmallGap;

            SDL_Rect dest{0, 0, toast_.width, toast_.height};
            dest.x = (screen_w - dest.w) / 2;
            dest.x = std::clamp(dest.x, 0, std::max(0, screen_w - dest.w));
            dest.y = kPanelPadding;
            SDL_Rect background{
                dest.x - padding_x,
                dest.y - padding_y,
                dest.w + padding_x * 2,
                dest.h + padding_y * 2
            };
            background.x = std::clamp(background.x, 0, std::max(0, screen_w - background.w));
            background.y = std::clamp(background.y, 0, std::max(0, screen_h - background.h));
            dest.x = background.x + (background.w - dest.w) / 2;
            dest.y = background.y + (background.h - dest.h) / 2;

            SDL_Color fill{22, 28, 40, 220};
            SDL_Color border{88, 129, 191, 245};
            draw_popup_box(renderer, background, fill, border);

            SDL_SetTextureBlendMode(toast_.texture.get(), SDL_BLENDMODE_BLEND);
            sdl_render::Texture(renderer, toast_.texture.get(), nullptr, &dest);
        }
    }

    if (indicator_.showing) {
        rebuild_indicator_texture(renderer);
        if (indicator_.texture && indicator_.width > 0 && indicator_.height > 0) {
            const float alpha = compute_indicator_alpha(now);
            if (alpha > 0.0f) {
                const Uint8 alpha_byte = alpha_from_fraction(alpha);
                const int padding = kSmallGap;
                SDL_Rect dest{0, 0, indicator_.width, indicator_.height};
                dest.x = kPanelPadding;
                dest.y = kPanelPadding;
                SDL_Rect background{
                    dest.x - padding,
                    dest.y - padding,
                    dest.w + padding * 2,
                    dest.h + padding * 2
                };
                background.x = std::clamp(background.x, 0, std::max(0, screen_w - background.w));
                background.y = std::clamp(background.y, 0, std::max(0, screen_h - background.h));
                dest.x = background.x + (background.w - dest.w) / 2;
                dest.y = background.y + (background.h - dest.h) / 2;

                SDL_Color fill{16, 22, 32, alpha_byte};
                SDL_Color border{118, 130, 152, alpha_byte};
                fill.a = alpha_byte;
                draw_popup_box(renderer, background, fill, border);

                SDL_SetTextureBlendMode(indicator_.texture.get(), SDL_BLENDMODE_BLEND);
                SDL_SetTextureAlphaMod(indicator_.texture.get(), alpha_byte);
                sdl_render::Texture(renderer, indicator_.texture.get(), nullptr, &dest);
            }
        }
    }
}

void PopupManager::rebuild_toast_texture(SDL_Renderer* renderer) {
    if (!toast_.visible || !toast_.dirty) {
        return;
    }
    toast_.texture.reset();
    toast_.width = 0;
    toast_.height = 0;
    if (toast_.message.empty()) {
        toast_.dirty = false;
        toast_.visible = false;
        return;
    }

    std::unique_ptr<TTF_Font, decltype(&TTF_CloseFont)> font(
        open_popup_font(toast_style_.font_size),
        TTF_CloseFont);
    if (!font) {
        toast_.dirty = false;
        toast_.visible = false;
        return;
    }

    std::unique_ptr<SDL_Surface, SDLSurfaceDeleter> surface(
        ttf_util::RenderTextBlended(font.get(), toast_.message.c_str(), toast_style_.color));
    if (!surface) {
        toast_.dirty = false;
        toast_.visible = false;
        return;
    }

    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface.get());
    if (!texture) {
        toast_.dirty = false;
        toast_.visible = false;
        return;
    }

    toast_.texture.reset(texture);
    toast_.width = surface->w;
    toast_.height = surface->h;
    toast_.dirty = false;
}

void PopupManager::rebuild_indicator_texture(SDL_Renderer* renderer) {
    if (!indicator_.showing || !indicator_.dirty) {
        return;
    }
    indicator_.texture.reset();
    indicator_.width = 0;
    indicator_.height = 0;
    if (indicator_.label_text.empty()) {
        indicator_.dirty = false;
        indicator_.showing = false;
        return;
    }

    std::unique_ptr<TTF_Font, decltype(&TTF_CloseFont)> font(
        open_popup_font(indicator_style_.font_size),
        TTF_CloseFont);
    if (!font) {
        indicator_.dirty = false;
        indicator_.showing = false;
        return;
    }

    std::unique_ptr<SDL_Surface, SDLSurfaceDeleter> surface(
        ttf_util::RenderTextBlended(font.get(), indicator_.label_text.c_str(), indicator_style_.color));
    if (!surface) {
        indicator_.dirty = false;
        indicator_.showing = false;
        return;
    }

    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface.get());
    if (!texture) {
        indicator_.dirty = false;
        indicator_.showing = false;
        return;
    }

    indicator_.texture.reset(texture);
    indicator_.width = surface->w;
    indicator_.height = surface->h;
    indicator_.dirty = false;
}

float PopupManager::compute_indicator_alpha(Uint32 now) const {
    if (!indicator_.showing) {
        return 0.0f;
    }
    if (indicator_.active) {
        return 1.0f;
    }
    if (indicator_.fade_end_ms == 0) {
        return 1.0f;
    }
    if (now < indicator_.fade_start_ms) {
        return 1.0f;
    }
    if (now >= indicator_.fade_end_ms) {
        return 0.0f;
    }
    const float span = static_cast<float>(indicator_.fade_end_ms - indicator_.fade_start_ms);
    if (span <= 0.0f) {
        return 0.0f;
    }
    const float elapsed = static_cast<float>(now - indicator_.fade_start_ms);
    return 1.0f - std::clamp(elapsed / span, 0.0f, 1.0f);
}

bool PopupManager::has_active_content() const {
    return toast_.visible || indicator_.showing;
}



