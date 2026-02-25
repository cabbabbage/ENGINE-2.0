#include "rendering/render/terrain_runtime_state.hpp"

#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <random>
#include <string>

namespace {
std::uint64_t mix_uint64(std::uint64_t seed, std::uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    seed ^= seed >> 33;
    seed *= 0xff51afd7ed558ccdULL;
    seed ^= seed >> 33;
    seed *= 0xc4ceb9fe1a85ec53ULL;
    seed ^= seed >> 33;
    return seed;
}

SDL_FPoint normalize_direction(SDL_FPoint dir) {
    const float len_sq = dir.x * dir.x + dir.y * dir.y;
    if (len_sq <= 1e-8f || !std::isfinite(len_sq)) {
        return SDL_FPoint{-0.45f, -0.88f};
    }
    const float inv_len = 1.0f / std::sqrt(len_sq);
    return SDL_FPoint{dir.x * inv_len, dir.y * inv_len};
}

std::uint64_t next_revision() {
    static std::uint64_t rev = 1;
    rev = (rev == std::numeric_limits<std::uint64_t>::max()) ? 1 : (rev + 1);
    return rev;
}
} // namespace

TerrainRuntimeState TerrainRuntimeState::from_settings(const TerrainSettings& incoming,
                                                       const std::string& map_id,
                                                       bool randomize_session_seed) {
    TerrainRuntimeState state{};
    state.settings = TerrainSettings::sanitized(incoming);

    std::uint64_t seed = static_cast<std::uint64_t>(state.settings.light.base_seed);
    seed = mix_uint64(seed, std::hash<std::string>{}(map_id));
    if (randomize_session_seed) {
        const std::uint64_t time_component =
            static_cast<std::uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        std::uint64_t random_component = 0;
        try {
            std::random_device rd;
            random_component = static_cast<std::uint64_t>(rd());
        } catch (...) {
            random_component = 0;
        }
        seed = mix_uint64(seed, mix_uint64(time_component, random_component));
    }
    if (seed == 0) {
        seed = 0x4ba52u; // Fallback to default if somehow zero.
    }

    state.session_seed = seed;
    state.light_direction_world = normalize_direction(state.settings.light.direction_world);
    state.light_anchor_world = state.settings.light.position_world;
    state.revision = next_revision();
    return state;
}

