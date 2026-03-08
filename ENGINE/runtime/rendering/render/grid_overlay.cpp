#include "rendering/render/grid_overlay.hpp"

#include "utils/grid.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace render_overlay {

float clamp_spacing_multiplier(float value) {
    if (!std::isfinite(value)) {
        return kMinGridMultiplier;
    }
    return std::clamp(value, kMinGridMultiplier, kMaxGridMultiplier);
}

float clamp_base_size_scale(float value) {
    if (!std::isfinite(value)) {
        return kMinBaseScale;
    }
    return std::clamp(value, kMinBaseScale, kMaxBaseScale);
}

float clamp_vertical_offset(float value) {
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return std::clamp(value, kMinVerticalOffset, kMaxVerticalOffset);
}

float clamp_random_jitter(float value) {
    if (!std::isfinite(value)) {
        return kMinRandomJitter;
    }
    return std::clamp(value, kMinRandomJitter, kMaxRandomJitter);
}

int scaled_spacing(int base_spacing, float multiplier) {
    const double scaled = static_cast<double>(base_spacing) * static_cast<double>(multiplier);
    if (!std::isfinite(scaled) || scaled <= 0.0) {
        return std::max(1, base_spacing);
    }
    const long long rounded = static_cast<long long>(std::llround(scaled));
    if (rounded <= 0) {
        return 1;
    }
    if (rounded > static_cast<long long>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(rounded);
}

int spacing_for_resolution(int resolution) {
    const int clamped = vibble::grid::clamp_resolution(resolution);
    long long value = 1;
    for (int i = 0; i < clamped; ++i) {
        if (value > static_cast<long long>(std::numeric_limits<int>::max()) / 3LL) {
            return std::numeric_limits<int>::max();
        }
        value *= 3LL;
    }
    return static_cast<int>(value);
}

std::uint64_t mix_uint64(std::uint64_t seed, std::uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed;
}

std::uint64_t xorshift64(std::uint64_t value) {
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    return value;
}

std::uint64_t hash_grid_cell(int grid_x,
                             int grid_y,
                             int world_z,
                             int resolution_layer,
                             int group,
                             std::uint64_t seed) {
    std::uint64_t hash = mix_uint64(seed, 0x9e3779b97f4a7c15ULL);
    hash = mix_uint64(hash, static_cast<std::uint64_t>(group));
    hash = mix_uint64(hash, static_cast<std::uint64_t>(resolution_layer));
    hash = mix_uint64(hash, static_cast<std::uint64_t>(grid_x));
    hash = mix_uint64(hash, static_cast<std::uint64_t>(grid_y));
    hash = mix_uint64(hash, static_cast<std::uint64_t>(world_z));
    return hash;
}

SDL_FPoint jitter_from_hash(std::uint64_t hash, float max_jitter) {
    if (max_jitter <= 0.0f) {
        return SDL_FPoint{0.0f, 0.0f};
    }

    hash ^= 0x9e3779b97f4a7c15ULL;

    auto next_uniform = [&hash]() -> double {
        hash = xorshift64(hash);
        const std::uint32_t value = static_cast<std::uint32_t>(hash & 0xFFFFFFFFULL);
        return static_cast<double>(value) / static_cast<double>(std::numeric_limits<std::uint32_t>::max());
    };

    const double jitter_x = (next_uniform() * 2.0 - 1.0) * static_cast<double>(max_jitter);
    const double jitter_y = (next_uniform() * 2.0 - 1.0) * static_cast<double>(max_jitter);
    return SDL_FPoint{static_cast<float>(jitter_x), static_cast<float>(jitter_y)};
}

int hashed_roll(std::uint64_t hash, int total_weight) {
    if (total_weight <= 0) {
        return -1;
    }
    hash = xorshift64(hash ^ 0x9e3779b97f4a7c15ULL);
    return static_cast<int>(hash % static_cast<std::uint64_t>(total_weight));
}

int choose_weighted_index(std::uint64_t hash, const std::vector<int>& weights, int total_weight) {
    if (weights.empty()) {
        return -1;
    }
    if (total_weight <= 0) {
        total_weight = 0;
        for (int w : weights) {
            if (w > 0) {
                total_weight += w;
            }
        }
    }
    if (total_weight <= 0) {
        return -1;
    }

    const int roll = hashed_roll(hash, total_weight);
    if (roll < 0) {
        return -1;
    }

    int cumulative = 0;
    for (std::size_t i = 0; i < weights.size(); ++i) {
        const int w = weights[i];
        if (w <= 0) {
            continue;
        }
        cumulative += w;
        if (roll < cumulative) {
            return static_cast<int>(i);
        }
    }
    return static_cast<int>(weights.size() - 1);
}

}  // namespace render_overlay
