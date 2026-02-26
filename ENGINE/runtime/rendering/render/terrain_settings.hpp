#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// Immutable-like configuration used by TerrainField and related terrain shading helpers.
// Mirrors the static config access style used by DynamicFogSystem/DynamicBoundarySystem.
struct TerrainSettings {
    // Enable/disable terrain synthesis entirely (short-circuits to flat ground when false).
    bool enabled = true;

    // Peak displacement in world-space pixels for synthesized height.
    float max_elevation_world = 520.0f;

    // Radius (in world pixels) around rooms/trails where elevation eases back to zero.
    float edge_falloff_distance_world = 360.0f;

    // Coarse-to-fine transition control: higher smoothness biases towards larger features.
    float smoothness = 0.9f;

    // Base variation amplitude; also influences octave count.
    float noise_variation = 1.1f;

    // Persistence between octaves (0 = only base layer, 1 = equal energy at all octaves).
    float roughness = 0.78f;

    // Softens the blend between octaves and edge falloffs (0 = sharp, 1 = very smooth).
    float blend_strength = 0.6f;

    // Scales sampling frequency relative to the GridPoint spacing for the active resolution layer.
    // A value of 1.0 samples one feature per grid spacing; <1 densifies, >1 widens features.
    float resolution_density_scale = 0.85f;

    struct LightSettings {
        // Stable RNG seed controlling directional light jitter/placement.
        std::uint32_t base_seed = 0x4ba52u;
        // If true, the world position of a grid cell contributes to the seed for stable lighting.
        bool lock_seed_to_world = true;
        // Static world-facing light direction (normalized XY plane; Z assumed up).
        SDL_FPoint direction_world{ -0.52f, -0.85f };
        // Optional world anchor for positional falloff (XY, Z implicitly handled in shading).
        SDL_FPoint position_world{ 0.0f, -1200.0f };
        float light_strength = 1.2f;
        float contrast = 1.3f;

        inline void clamp();
    } light;

    // Clamp all fields to sane numeric ranges, handling NaN/inf gracefully.
    inline void clamp();

    // Sanitized copy helper.
    static inline TerrainSettings sanitized(TerrainSettings s) {
        s.clamp();
        return s;
    }

    // Global mutable config accessors (pattern mirrors DynamicFogSystem/DynamicBoundarySystem).
    static inline TerrainSettings& global() {
        static TerrainSettings settings{};
        settings.clamp();
        return settings;
    }

    static inline const TerrainSettings& readonly() {
        return global();
    }

    static inline std::uint64_t& revision_counter() {
        static std::uint64_t revision = 1;
        return revision;
    }

    static inline std::uint64_t revision() { return revision_counter(); }

    static inline void apply(const TerrainSettings& incoming) {
        global() = sanitized(incoming);
        auto& rev = revision_counter();
        rev = (rev == std::numeric_limits<std::uint64_t>::max()) ? 1 : (rev + 1);
    }

    static inline float clamp_scalar(float value, float lo, float hi, float fallback) {
        if (!std::isfinite(value)) {
            return fallback;
        }
        if (value < lo) return lo;
        if (value > hi) return hi;
        return value;
    }
};

inline void TerrainSettings::LightSettings::clamp() {
    base_seed = base_seed == 0 ? 0x4ba52u : base_seed;
    if (!std::isfinite(direction_world.x) || !std::isfinite(direction_world.y)) {
        direction_world = SDL_FPoint{-0.45f, -0.88f};
    }
    if (!std::isfinite(position_world.x) || !std::isfinite(position_world.y)) {
        position_world = SDL_FPoint{0.0f, -800.0f};
    }
    light_strength = TerrainSettings::clamp_scalar(light_strength, 0.0f, 4.0f, 1.2f);
    contrast = TerrainSettings::clamp_scalar(contrast, 0.25f, 4.0f, 1.3f);
}

inline void TerrainSettings::clamp() {
    enabled = !!enabled;
    max_elevation_world = clamp_scalar(max_elevation_world, 0.0f, 4000.0f, 520.0f);
    edge_falloff_distance_world = clamp_scalar(edge_falloff_distance_world, 0.0f, 3000.0f, 360.0f);
    smoothness = clamp_scalar(smoothness, 0.0f, 2.0f, 0.9f);
    noise_variation = clamp_scalar(noise_variation, 0.0f, 2.0f, 1.1f);
    roughness = clamp_scalar(roughness, 0.05f, 1.5f, 0.78f);
    blend_strength = clamp_scalar(blend_strength, 0.0f, 1.0f, 0.6f);
    resolution_density_scale = clamp_scalar(resolution_density_scale, 0.1f, 8.0f, 0.85f);
    light.clamp();
}
