#include "rendering/render/terrain_shading.hpp"

#include <algorithm>
#include <cmath>

float terrain_brightness(const TerrainRuntimeState& state,
                         float slope_x,
                         float slope_y,
                         const SDL_FPoint& world_position) {
    const float strength = std::clamp(state.settings.light.light_strength, 0.0f, 4.0f);
    const float contrast = std::clamp(state.settings.light.contrast, 0.25f, 4.0f);

    // Surface normal from slope: (-dz/dx, -dz/dy, 1)
    const float nx = -slope_x;
    const float ny = -slope_y;
    const float nz = 1.0f;
    float normal_len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (!std::isfinite(normal_len) || normal_len <= 1e-6f) {
        normal_len = 1.0f;
    }
    const float inv_n = 1.0f / normal_len;
    const float nnx = nx * inv_n;
    const float nny = ny * inv_n;
    const float nnz = nz * inv_n;

    // Light direction (assume upward Z component of 1 for soft fill)
    SDL_FPoint dir_xy = state.light_direction_world;
    const float dir_len_xy = std::sqrt(dir_xy.x * dir_xy.x + dir_xy.y * dir_xy.y);
    if (!std::isfinite(dir_len_xy) || dir_len_xy <= 1e-6f) {
        dir_xy = SDL_FPoint{-0.45f, -0.88f};
    } else {
        dir_xy.x /= dir_len_xy;
        dir_xy.y /= dir_len_xy;
    }
    const float light_len = std::sqrt(dir_xy.x * dir_xy.x + dir_xy.y * dir_xy.y + 1.0f);
    const float inv_l = light_len > 1e-6f ? 1.0f / light_len : 1.0f;
    const float lx = dir_xy.x * inv_l;
    const float ly = dir_xy.y * inv_l;
    const float lz = 1.0f * inv_l;

    float ndotl = nnx * lx + nny * ly + nnz * lz;
    ndotl = std::clamp(ndotl, 0.0f, 1.0f);

    // Optional positional attenuation around anchor.
    float attenuation = 1.0f;
    if (std::isfinite(state.light_anchor_world.x) && std::isfinite(state.light_anchor_world.y)) {
        const float dx = world_position.x - state.light_anchor_world.x;
        const float dy = world_position.y - state.light_anchor_world.y;
        const float dist = std::sqrt(dx * dx + dy * dy);
        attenuation = 1.0f / (1.0f + dist * 0.0015f);
    }

    float contrasted = 0.5f + (ndotl - 0.5f) * contrast;
    contrasted = std::clamp(contrasted, 0.0f, 1.0f);
    float brightness = contrasted * strength * attenuation;
    return std::clamp(brightness, 0.0f, 1.0f);
}

