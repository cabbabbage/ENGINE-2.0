#pragma once

#include <algorithm>
#include "utils/sdl_render_conversions.hpp"
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <SDL3/SDL.h>

class Asset;
class AssetLibrary;

namespace render_pipeline {

// Simple safe-scale helper. Replaces the old multi-variant scaling logic.
inline float safe_positive_scale(float value, float fallback = 1.0f) {
    if (!std::isfinite(value) || value <= 0.0f) {
        return fallback;
    }
    return std::max(0.01f, value);
}

// Create a scaled copy of an SDL_Surface using linear filtering.
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

// Stub types and functions kept for compilation compatibility.
// The actual multi-variant scaling logic has been removed.
// GPU mipmaps and hardware scaling now handle runtime size changes.

struct ScaleProfile {
    std::vector<float> steps;
    std::uint64_t revision = 0;
    bool had_entry = false;
    bool created_entry = false;
    bool revision_changed = false;
    float min_scale = 1.0f;
    float max_scale = 1.0f;
    bool has_custom_steps() const { return false; }
};

struct HysteresisState {
    int last_index = 0;
    float min_scale = 0.0f;
    float max_scale = std::numeric_limits<float>::max();
};

struct ScaleSelection {
    int index = 0;
    float requested_scale = 1.0f;
    float stored_scale = 1.0f;
    float remainder_scale = 1.0f;
    float hysteresis_min = 0.0f;
    float hysteresis_max = std::numeric_limits<float>::max();
    int preload_index = -1;
};

struct ScalingLogic {
    using ScaleSteps = std::vector<float>;

    static constexpr std::size_t kMaxVariantCount = 1;
    static constexpr std::size_t kDefaultVariantCount = 1;

    // Always returns { 1.0f } — single texture per frame, no multi-variant.
    static inline const ScaleSteps& DefaultScaleSteps() {
        static const ScaleSteps kDefaultSteps = { 1.0f };
        return kDefaultSteps;
    }

    // No-op in single-texture mode.
    static inline void NormalizeVariantSteps(ScaleSteps& steps) {
        steps = DefaultScaleSteps();
    }

    // Always returns index 0 with scale 1.0 — no variant selection needed.
    static inline ScaleSelection Choose(float desired_scale) {
        ScaleSelection sel;
        sel.index = 0;
        sel.requested_scale = safe_positive_scale(desired_scale);
        sel.stored_scale = 1.0f;
        sel.remainder_scale = sel.requested_scale;
        sel.preload_index = -1;
        return sel;
    }

    static inline ScaleSelection Choose(float desired_scale, const ScaleSteps& steps) {
        return Choose(desired_scale);
    }

    static inline ScaleSelection Choose(float desired_scale, const ScaleSteps& steps,
                                        const HysteresisState&, float, HysteresisOptions = {}) {
        return Choose(desired_scale);
    }

    // Always returns "100".
    static inline int ScalePercent(std::size_t index) {
        return 100;
    }

    static inline int ScalePercent(const ScaleSteps&, std::size_t index) {
        return 100;
    }

    // Always returns base path.
    static inline std::string VariantFolder(const std::string& base, std::size_t index) {
        return base;
    }

    static inline std::string VariantFolder(const std::string& base, const ScaleSteps&, std::size_t) {
        return base;
    }

    static inline std::array<int, 1> PercentSteps() {
        return { 100 };
    }

    static inline std::vector<int> PercentSteps(const ScaleSteps&) {
        return { 100 };
    }

    // No-op — no profiles to load.
    static inline void LoadPrecomputedProfiles(bool force_reload = false) {}

    static inline void ResetProfileHistory() {}

    static inline ScaleProfile ProfileForAsset(const std::string&) {
        ScaleProfile profile;
        profile.steps = DefaultScaleSteps();
        return profile;
    }

    static inline void SetQualityCap(float) {}
    static inline float QualityCap() { return 1.0f; }

private:
    struct HysteresisOptions { float margin = 0.05f; float preload_margin = 0.02f; };
};

}

namespace render_pipeline::shading {

void ClearShadowStateFor(const Asset* asset);

inline void ClearShadowStateFor(const Asset*) {}

}