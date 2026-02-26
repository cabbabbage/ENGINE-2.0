#include "widget_text_renderer.hpp"

#include <memory>

#include "utils/sdl_render_conversions.hpp"
#include "utils/ttf_render_utils.hpp"

namespace {

struct SurfaceDeleter {
    void operator()(SDL_Surface* s) const {
        if (s) SDL_DestroySurface(s);
    }
};

struct TextureDeleter {
    void operator()(SDL_Texture* t) const {
        if (t) SDL_DestroyTexture(t);
    }
};

using SurfacePtr = std::unique_ptr<SDL_Surface, SurfaceDeleter>;
using TexturePtr = std::unique_ptr<SDL_Texture, TextureDeleter>;

}  // namespace

WidgetTextRenderer::WidgetTextRenderer(const TextStyle& style) : style_(style) {
    font_ = style_.open_font();
}

WidgetTextRenderer::~WidgetTextRenderer() {
    if (font_) {
        TTF_CloseFont(font_);
        font_ = nullptr;
    }
}

SDL_Color WidgetTextRenderer::ResolveColor(std::optional<SDL_Color> color_override) const {
    return color_override ? *color_override : style_.color;
}

int WidgetTextRenderer::line_height() const {
    if (!font_) return style_.font_size;
    int h = TTF_GetFontHeight(font_);
    return h > 0 ? h : style_.font_size;
}

bool WidgetTextRenderer::Measure(std::string_view text, int* w, int* h) const {
    if (!font_) return false;
    return ttf_util::GetStringSize(font_, text, w, h);
}

bool WidgetTextRenderer::Render(SDL_Renderer* renderer,
                                std::string_view text,
                                int x,
                                int y,
                                std::optional<SDL_Color> color_override,
                                SDL_Rect* out_bounds) const {
    if (!font_) return false;

    SurfacePtr surf{ ttf_util::RenderTextBlended(font_, text, ResolveColor(color_override)) };
    if (!surf) return false;

    TexturePtr tex{ SDL_CreateTextureFromSurface(renderer, surf.get()) };
    if (!tex) return false;

    SDL_Rect dst{ x, y, surf->w, surf->h };
    if (out_bounds) {
        *out_bounds = dst;
    }

    sdl_render::Texture(renderer, tex.get(), nullptr, &dst);
    return true;
}
