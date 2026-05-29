#pragma once

#include <algorithm>
#include <cmath>

#include <SDL3/SDL.h>

class Asset;

namespace render_pipeline {

inline float safe_positive_scale(float value, float fallback = 1.0f) {
    if (!std::isfinite(value) || value <= 0.0f) {
        return fallback;
    }
    return std::max(0.01f, value);
}

inline float authored_scale_from_percent(float percent) {
    return safe_positive_scale(percent * 0.01f);
}

inline bool scale_changed_enough(float previous, float next, float epsilon = 0.001f) {
    return std::fabs(safe_positive_scale(previous) - safe_positive_scale(next)) > epsilon;
}

// Create a scaled copy of an SDL_Surface using linear filtering for editor/offline helpers.
// Runtime camera zoom uses the single resident texture and GPU sampling/mipmaps instead.
inline SDL_Surface* CreateScaledSurface(SDL_Surface* src, float scale) {
    if (!src || scale <= 0.0f) {
        return nullptr;
    }

    if (std::fabs(scale - 1.0f) <= 1e-4f) {
        SDL_Surface* copy = SDL_CreateSurface(src->w, src->h, SDL_PIXELFORMAT_RGBA32);
        if (!copy) {
            return nullptr;
        }
        SDL_Rect rect{0, 0, src->w, src->h};
        if (!SDL_BlitSurface(src, &rect, copy, &rect)) {
            SDL_DestroySurface(copy);
            return nullptr;
        }
        return copy;
    }

    const int dst_w = std::max(1, static_cast<int>(std::lround(static_cast<double>(src->w) * scale)));
    const int dst_h = std::max(1, static_cast<int>(std::lround(static_cast<double>(src->h) * scale)));

    SDL_Surface* dst = SDL_CreateSurface(dst_w, dst_h, SDL_PIXELFORMAT_RGBA32);
    if (!dst) {
        return nullptr;
    }

    SDL_Rect src_rect{0, 0, src->w, src->h};
    SDL_Rect dst_rect{0, 0, dst_w, dst_h};
    if (!SDL_BlitSurfaceScaled(src, &src_rect, dst, &dst_rect, SDL_SCALEMODE_LINEAR)) {
        SDL_DestroySurface(dst);
        return nullptr;
    }

    return dst;
}

} // namespace render_pipeline

namespace render_pipeline::shading {
inline void ClearShadowStateFor(const Asset*) {}
} // namespace render_pipeline::shading
