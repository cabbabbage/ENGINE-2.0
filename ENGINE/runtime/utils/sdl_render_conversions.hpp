#pragma once

#include <SDL3/SDL.h>

#include <vector>

namespace sdl_render {

inline SDL_FPoint to_fpoint(const SDL_Point& p) {
    return SDL_FPoint{static_cast<float>(p.x), static_cast<float>(p.y)};
}

inline SDL_FRect to_frect(const SDL_Rect& r) {
    return SDL_FRect{static_cast<float>(r.x),
                     static_cast<float>(r.y),
                     static_cast<float>(r.w),
                     static_cast<float>(r.h)};
}

inline bool Texture(SDL_Renderer* renderer,
                    SDL_Texture* texture,
                    const SDL_FRect* srcrect,
                    const SDL_FRect* dstrect) {
    return SDL_RenderTexture(renderer, texture, srcrect, dstrect);
}

inline bool Texture(SDL_Renderer* renderer,
                    SDL_Texture* texture,
                    const SDL_Rect* srcrect,
                    const SDL_Rect* dstrect) {
    SDL_FRect src_f{};
    SDL_FRect dst_f{};
    const SDL_FRect* src_ptr = srcrect ? (src_f = to_frect(*srcrect), &src_f) : nullptr;
    const SDL_FRect* dst_ptr = dstrect ? (dst_f = to_frect(*dstrect), &dst_f) : nullptr;
    return SDL_RenderTexture(renderer, texture, src_ptr, dst_ptr);
}

inline bool TextureRotated(SDL_Renderer* renderer,
                           SDL_Texture* texture,
                           const SDL_FRect* srcrect,
                           const SDL_FRect* dstrect,
                           double angle,
                           const SDL_FPoint* center,
                           SDL_FlipMode flip) {
    return SDL_RenderTextureRotated(renderer, texture, srcrect, dstrect, angle, center, flip);
}

inline bool TextureRotated(SDL_Renderer* renderer,
                           SDL_Texture* texture,
                           const SDL_Rect* srcrect,
                           const SDL_Rect* dstrect,
                           double angle,
                           const SDL_Point* center,
                           SDL_FlipMode flip) {
    SDL_FRect src_f{};
    SDL_FRect dst_f{};
    SDL_FPoint center_f{};
    const SDL_FRect* src_ptr = srcrect ? (src_f = to_frect(*srcrect), &src_f) : nullptr;
    const SDL_FRect* dst_ptr = dstrect ? (dst_f = to_frect(*dstrect), &dst_f) : nullptr;
    const SDL_FPoint* center_ptr = center ? (center_f = to_fpoint(*center), &center_f) : nullptr;
    return SDL_RenderTextureRotated(renderer, texture, src_ptr, dst_ptr, angle, center_ptr, flip);
}

inline bool Rect(SDL_Renderer* renderer, const SDL_FRect* rect) {
    return SDL_RenderRect(renderer, rect);
}

inline bool Rect(SDL_Renderer* renderer, const SDL_Rect* rect) {
    if (!rect) {
        return SDL_RenderRect(renderer, nullptr);
    }
    SDL_FRect frect = to_frect(*rect);
    return SDL_RenderRect(renderer, &frect);
}

inline bool Rects(SDL_Renderer* renderer, const SDL_FRect* rects, int count) {
    return SDL_RenderRects(renderer, rects, count);
}

inline bool Rects(SDL_Renderer* renderer, const SDL_Rect* rects, int count) {
    if (!rects || count <= 0) {
        return true;
    }
    std::vector<SDL_FRect> converted;
    converted.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        converted.push_back(to_frect(rects[i]));
    }
    return SDL_RenderRects(renderer, converted.data(), static_cast<int>(converted.size()));
}

inline bool FillRect(SDL_Renderer* renderer, const SDL_FRect* rect) {
    return SDL_RenderFillRect(renderer, rect);
}

inline bool FillRect(SDL_Renderer* renderer, const SDL_Rect* rect) {
    if (!rect) {
        return SDL_RenderFillRect(renderer, static_cast<const SDL_FRect*>(nullptr));
    }
    SDL_FRect frect = to_frect(*rect);
    return SDL_RenderFillRect(renderer, &frect);
}

inline bool FillRects(SDL_Renderer* renderer, const SDL_FRect* rects, int count) {
    return SDL_RenderFillRects(renderer, rects, count);
}

inline bool FillRects(SDL_Renderer* renderer, const SDL_Rect* rects, int count) {
    if (!rects || count <= 0) {
        return true;
    }
    std::vector<SDL_FRect> converted;
    converted.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        converted.push_back(to_frect(rects[i]));
    }
    return SDL_RenderFillRects(renderer, converted.data(), static_cast<int>(converted.size()));
}

inline bool Lines(SDL_Renderer* renderer, const SDL_FPoint* points, int count) {
    return SDL_RenderLines(renderer, points, count);
}

inline bool Lines(SDL_Renderer* renderer, const SDL_Point* points, int count) {
    if (!points || count <= 0) {
        return true;
    }
    std::vector<SDL_FPoint> converted;
    converted.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        converted.push_back(to_fpoint(points[i]));
    }
    return SDL_RenderLines(renderer, converted.data(), static_cast<int>(converted.size()));
}

} // namespace sdl_render



