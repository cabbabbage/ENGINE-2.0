#pragma once

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <optional>
#include <string_view>

#include "utils/text_style.hpp"

// RAII text renderer for widgets: owns a font and can render/measure text.
class WidgetTextRenderer {
public:
    explicit WidgetTextRenderer(const TextStyle& style);
    ~WidgetTextRenderer();

    WidgetTextRenderer(const WidgetTextRenderer&) = delete;
    WidgetTextRenderer& operator=(const WidgetTextRenderer&) = delete;
    WidgetTextRenderer(WidgetTextRenderer&&) = delete;
    WidgetTextRenderer& operator=(WidgetTextRenderer&&) = delete;

    bool valid() const { return font_ != nullptr; }
    int line_height() const;
    bool Measure(std::string_view text, int* w, int* h) const;
    bool Render(SDL_Renderer* renderer,
                std::string_view text,
                int x,
                int y,
                std::optional<SDL_Color> color_override = std::nullopt,
                SDL_Rect* out_bounds = nullptr) const;

private:
    SDL_Color ResolveColor(std::optional<SDL_Color> color_override) const;

private:
    TextStyle style_;
    TTF_Font* font_ = nullptr;
};
