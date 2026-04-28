#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

#include "rendering/render/render_depth_policy.hpp"

// שכבת הגריד פועלת במישור X/Z הקנוני (world.Z קובע מיון עומק בעוד world.Y משמש לגובה).

namespace render_overlay {

// גבולות משותפים לפרמטרי הכוונון של שכבת הגריד.
constexpr float kMinGridMultiplier = 0.25f;
constexpr float kMaxGridMultiplier = 8.0f;
constexpr float kMinBaseScale      = 0.25f;
constexpr float kMaxBaseScale      = 12.0f;
constexpr float kMinVerticalOffset = -300.0f;
constexpr float kMaxVerticalOffset = 300.0f;
constexpr float kMinRandomJitter   = 0.0f;
constexpr float kMaxRandomJitter   = 500.0f;

float clamp_spacing_multiplier(float value);
float clamp_base_size_scale(float value);
float clamp_vertical_offset(float value);
float clamp_random_jitter(float value);

int   scaled_spacing(int base_spacing, float multiplier);
int   spacing_for_resolution(int resolution);

std::uint64_t mix_uint64(std::uint64_t seed, std::uint64_t value);
std::uint64_t xorshift64(std::uint64_t value);

std::uint64_t hash_grid_cell(int grid_x,
                             int grid_y,
                             int world_z,
                             int resolution_layer,
                             int group = 0,
                             std::uint64_t seed = 0);

SDL_FPoint    jitter_from_hash(std::uint64_t hash, float max_jitter);
int           hashed_roll(std::uint64_t hash, int total_weight);
int           choose_weighted_index(std::uint64_t hash,
                                    const std::vector<int>& weights,
                                    int total_weight = -1);

}  // namespace render_overlay

