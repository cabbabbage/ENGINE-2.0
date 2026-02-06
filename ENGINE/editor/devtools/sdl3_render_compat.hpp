#pragma once

// Compatibility helpers that keep existing SDL2-style render calls building
// against the SDL3 API. These wrappers convert integer rects/points to their
// floating-point equivalents and translate renamed/updated functions.

#include "sdl3_render_compat.hpp"

#include <cmath>
#include <cstring>
#include <vector>

// SDL_RendererFlip was renamed to SDL_FlipMode in SDL3.
using SDL_RendererFlip = SDL_FlipMode;

namespace dm::sdl3_compat {

inline SDL_FPoint to_fpoint(const SDL_Point& p) {
    return SDL_FPoint{static_cast<float>(p.x), static_cast<float>(p.y)};
}

inline SDL_FRect to_frect(const SDL_Rect& r) {
    SDL_FRect fr;
    SDL_RectToFRect(&r, &fr);
    return fr;
}

inline std::vector<SDL_FPoint> to_fpoints(const SDL_Point* points, int count) {
    std::vector<SDL_FPoint> result;
    if (!points || count <= 0) {
        return result;
    }
    result.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        result.push_back(to_fpoint(points[i]));
    }
    return result;
}

inline std::vector<SDL_FRect> to_frects(const SDL_Rect* rects, int count) {
    std::vector<SDL_FRect> result;
    if (!rects || count <= 0) {
        return result;
    }
    result.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        result.push_back(to_frect(rects[i]));
    }
    return result;
}

}  // namespace dm::sdl3_compat

inline int SDL_RenderDrawLine(SDL_Renderer* renderer, int x1, int y1, int x2, int y2) {
    return SDL_RenderLine(renderer,
                          static_cast<float>(x1),
                          static_cast<float>(y1),
                          static_cast<float>(x2),
                          static_cast<float>(y2)) ? 0 : -1;
}

inline int SDL_RenderDrawLineF(SDL_Renderer* renderer, float x1, float y1, float x2, float y2) {
    return SDL_RenderLine(renderer, x1, y1, x2, y2) ? 0 : -1;
}

inline int SDL_RenderDrawLines(SDL_Renderer* renderer, const SDL_Point* points, int count) {
    auto fpoints = dm::sdl3_compat::to_fpoints(points, count);
    if (fpoints.empty()) {
        return 0;
    }
    return SDL_RenderLines(renderer, fpoints.data(), static_cast<int>(fpoints.size())) ? 0 : -1;
}

inline int SDL_RenderDrawLinesF(SDL_Renderer* renderer, const SDL_FPoint* points, int count) {
    if (!points || count <= 0) {
        return 0;
    }
    return SDL_RenderLines(renderer, points, count) ? 0 : -1;
}

inline int SDL_RenderDrawPoint(SDL_Renderer* renderer, int x, int y) {
    return SDL_RenderPoint(renderer, static_cast<float>(x), static_cast<float>(y)) ? 0 : -1;
}

inline int SDL_RenderDrawPointF(SDL_Renderer* renderer, float x, float y) {
    return SDL_RenderPoint(renderer, x, y) ? 0 : -1;
}

inline int SDL_RenderDrawPoints(SDL_Renderer* renderer, const SDL_Point* points, int count) {
    auto fpoints = dm::sdl3_compat::to_fpoints(points, count);
    if (fpoints.empty()) {
        return 0;
    }
    return SDL_RenderPoints(renderer, fpoints.data(), static_cast<int>(fpoints.size())) ? 0 : -1;
}

inline int SDL_RenderDrawPointsF(SDL_Renderer* renderer, const SDL_FPoint* points, int count) {
    if (!points || count <= 0) {
        return 0;
    }
    return SDL_RenderPoints(renderer, points, count) ? 0 : -1;
}

inline int SDL_RenderDrawRect(SDL_Renderer* renderer, const SDL_Rect* rect) {
    if (!rect) {
        return SDL_RenderRect(renderer, nullptr) ? 0 : -1;
    }
    SDL_FRect frect = dm::sdl3_compat::to_frect(*rect);
    return SDL_RenderRect(renderer, &frect) ? 0 : -1;
}

inline int SDL_RenderDrawRectF(SDL_Renderer* renderer, const SDL_FRect* rect) {
    return SDL_RenderRect(renderer, rect) ? 0 : -1;
}

inline int SDL_RenderDrawRects(SDL_Renderer* renderer, const SDL_Rect* rects, int count) {
    auto frects = dm::sdl3_compat::to_frects(rects, count);
    if (frects.empty()) {
        return 0;
    }
    return SDL_RenderRects(renderer, frects.data(), static_cast<int>(frects.size())) ? 0 : -1;
}

inline int SDL_RenderDrawRectsF(SDL_Renderer* renderer, const SDL_FRect* rects, int count) {
    if (!rects || count <= 0) {
        return 0;
    }
    return SDL_RenderRects(renderer, rects, count) ? 0 : -1;
}

inline int SDL_RenderFillRect(SDL_Renderer* renderer, const SDL_Rect* rect) {
    if (!rect) {
        return SDL_RenderFillRect(renderer, static_cast<const SDL_FRect*>(nullptr)) ? 0 : -1;
    }
    SDL_FRect frect = dm::sdl3_compat::to_frect(*rect);
    return SDL_RenderFillRect(renderer, &frect) ? 0 : -1;
}

inline int SDL_RenderFillRectF(SDL_Renderer* renderer, const SDL_FRect* rect) {
    return SDL_RenderFillRect(renderer, rect) ? 0 : -1;
}

inline int SDL_RenderFillRects(SDL_Renderer* renderer, const SDL_Rect* rects, int count) {
    auto frects = dm::sdl3_compat::to_frects(rects, count);
    if (frects.empty()) {
        return 0;
    }
    return SDL_RenderFillRects(renderer, frects.data(), static_cast<int>(frects.size())) ? 0 : -1;
}

inline int SDL_RenderFillRectsF(SDL_Renderer* renderer, const SDL_FRect* rects, int count) {
    if (!rects || count <= 0) {
        return 0;
    }
    return SDL_RenderFillRects(renderer, rects, count) ? 0 : -1;
}

inline int SDL_RenderCopy(SDL_Renderer* renderer,
                          SDL_Texture* texture,
                          const SDL_Rect* srcrect,
                          const SDL_Rect* dstrect) {
    SDL_FRect src_frect{};
    SDL_FRect dst_frect{};
    const SDL_FRect* src_ptr = nullptr;
    const SDL_FRect* dst_ptr = nullptr;
    if (srcrect) {
        SDL_RectToFRect(srcrect, &src_frect);
        src_ptr = &src_frect;
    }
    if (dstrect) {
        SDL_RectToFRect(dstrect, &dst_frect);
        dst_ptr = &dst_frect;
    }
    return SDL_RenderTexture(renderer, texture, src_ptr, dst_ptr) ? 0 : -1;
}

inline int SDL_RenderCopyEx(SDL_Renderer* renderer,
                            SDL_Texture* texture,
                            const SDL_Rect* srcrect,
                            const SDL_Rect* dstrect,
                            double angle,
                            const SDL_Point* center,
                            SDL_FlipMode flip) {
    SDL_FRect src_frect{};
    SDL_FRect dst_frect{};
    SDL_FPoint center_f{};
    const SDL_FRect* src_ptr = nullptr;
    const SDL_FRect* dst_ptr = nullptr;
    const SDL_FPoint* center_ptr = nullptr;
    if (srcrect) {
        SDL_RectToFRect(srcrect, &src_frect);
        src_ptr = &src_frect;
    }
    if (dstrect) {
        SDL_RectToFRect(dstrect, &dst_frect);
        dst_ptr = &dst_frect;
    }
    if (center) {
        center_f = dm::sdl3_compat::to_fpoint(*center);
        center_ptr = &center_f;
    }
    return SDL_RenderTextureRotated(renderer, texture, src_ptr, dst_ptr, angle, center_ptr, flip) ? 0 : -1;
}

inline int SDL_RenderSetClipRect(SDL_Renderer* renderer, const SDL_Rect* rect) {
    return SDL_SetRenderClipRect(renderer, rect) ? 0 : -1;
}

inline int SDL_RenderGetClipRect(SDL_Renderer* renderer, SDL_Rect* rect) {
    return SDL_GetRenderClipRect(renderer, rect) ? 0 : -1;
}

inline SDL_bool SDL_RenderIsClipEnabled(SDL_Renderer* renderer) {
    return SDL_RenderClipEnabled(renderer) ? SDL_TRUE : SDL_FALSE;
}

inline int SDL_QueryTexture(SDL_Texture* texture, Uint32* format, int* access, int* w, int* h) {
    SDL_PropertiesID props = SDL_GetTextureProperties(texture);
    if (!props) {
        return -1;
    }
    if (format) {
        *format = static_cast<Uint32>(SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_FORMAT_NUMBER, 0));
    }
    if (access) {
        *access = static_cast<int>(SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_ACCESS_NUMBER, 0));
    }
    float fw = 0.0f;
    float fh = 0.0f;
    if ((w || h) && !SDL_GetTextureSize(texture, w ? &fw : nullptr, h ? &fh : nullptr)) {
        return -1;
    }
    if (w) {
        *w = static_cast<int>(std::lround(fw));
    }
    if (h) {
        *h = static_cast<int>(std::lround(fh));
    }
    return 0;
}

inline int SDL_RenderReadPixels(SDL_Renderer* renderer,
                                const SDL_Rect* rect,
                                Uint32 format,
                                void* pixels,
                                int pitch) {
    if (!pixels || pitch <= 0) {
        return -1;
    }

    SDL_Surface* captured = SDL_RenderReadPixels(renderer, rect);
    if (!captured) {
        return -1;
    }

    SDL_Surface* working = captured;
    if (format != 0 && format != static_cast<Uint32>(captured->format)) {
        working = SDL_ConvertSurfaceFormat(captured, static_cast<SDL_PixelFormat>(format));
        SDL_DestroySurface(captured);
        captured = nullptr;
        if (!working) {
            return -1;
        }
    }

    const int bytes_per_row = working->w * SDL_BYTESPERPIXEL(working->format);
    if (pitch < bytes_per_row) {
        SDL_DestroySurface(working);
        SDL_SetError("Pitch too small for SDL_RenderReadPixels data copy");
        return -1;
    }

    const Uint8* src = static_cast<const Uint8*>(working->pixels);
    Uint8* dst = static_cast<Uint8*>(pixels);
    for (int y = 0; y < working->h; ++y) {
        std::memcpy(dst + y * pitch, src + y * working->pitch, bytes_per_row);
    }

    SDL_DestroySurface(working);
    return 0;
}
