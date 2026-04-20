#include "rendering/render/render.hpp"
#include "rendering/render/render_depth_policy.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "rendering/render/render_object_projection.hpp"
#include "rendering/render/floor_composer.hpp"
#include "rendering/render/blur_chain_renderer.hpp"
#include "rendering/render/layer_stack_renderer.hpp"
#include "rendering/render/layer_submission_builder.hpp"
#include "rendering/render/render_object.hpp"
#include "rendering/render/render_object_builder.hpp"
#include "rendering/render/scene_composite_pass.hpp"
#include "rendering/render/render_texture_utils.hpp"
#include "rendering/render/debug_overlay_renderer.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/log.hpp"
#include "utils/input.hpp"
#include "utils/AnchorPointResolver.hpp"
#include "animation/controllers/shared/anchor_bound_asset_helper.hpp"
#include "animation/controllers/shared/anchored_child_placement.hpp"
#include "animation/controllers/shared/oval_anchor_heading.hpp"
#include "animation/movement_rotation.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/animation_frame.hpp"
#include "assets/asset/asset_library.hpp"
#include "core/AssetsManager.hpp"
#include "gameplay/world/tiling/grid_tile.hpp"
#include "gameplay/world/chunk.hpp"
#include "gameplay/world/world_grid.hpp"
#include "gameplay/map_generation/map_layers_geometry.hpp"

#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {
constexpr double kDepthBucketSize = 0.0625;
constexpr double kDepthBucketScale = 1.0 / kDepthBucketSize;
constexpr float kQuadEpsilon = 1.0e-5f;

inline std::int64_t quantize_depth(double depth) {
    const double scaled = std::floor(depth * kDepthBucketScale);
    const double min_value = static_cast<double>(std::numeric_limits<std::int64_t>::lowest());
    const double max_value = static_cast<double>(std::numeric_limits<std::int64_t>::max());
    return static_cast<std::int64_t>(std::clamp(scaled, min_value, max_value));
}

void destroy_texture(SDL_Texture*& texture) {
    if (texture) {
        SDL_DestroyTexture(texture);
        texture = nullptr;
    }
}


bool project_floor_point_to_screen(const WarpedScreenGrid& cam,
                                   float world_x,
                                   float world_z,
                                   SDL_FPoint& out_screen) {
    SDL_FPoint linear_screen{};
    if (!cam.project_world_point(SDL_FPoint{world_x, 0.0f}, world_z, linear_screen) ||
        !std::isfinite(linear_screen.x) ||
        !std::isfinite(linear_screen.y)) {
        return false;
    }
    linear_screen.y = cam.warp_floor_screen_y(0.0f, linear_screen.y);
    if (!std::isfinite(linear_screen.y)) {
        return false;
    }
    out_screen = linear_screen;
    return true;
}

float sample_floor_axis_radius_px(const WarpedScreenGrid& cam,
                                  const SDL_FPoint& floor_center_screen,
                                  float floor_world_x,
                                  float floor_world_z,
                                  float offset_world_x,
                                  float offset_world_z,
                                  bool& out_has_sample) {
    out_has_sample = false;
    if (!std::isfinite(floor_center_screen.x) ||
        !std::isfinite(floor_center_screen.y) ||
        !std::isfinite(floor_world_x) ||
        !std::isfinite(floor_world_z) ||
        !std::isfinite(offset_world_x) ||
        !std::isfinite(offset_world_z)) {
        return 0.0f;
    }

    float distance_sum = 0.0f;
    int distance_count = 0;
    auto sample_side = [&](float wx, float wz) {
        SDL_FPoint sample_screen{};
        if (!project_floor_point_to_screen(cam, wx, wz, sample_screen)) {
            return;
        }
        const float dx = sample_screen.x - floor_center_screen.x;
        const float dy = sample_screen.y - floor_center_screen.y;
        const float distance_px = std::sqrt(dx * dx + dy * dy);
        if (!std::isfinite(distance_px) || distance_px <= 0.0f) {
            return;
        }
        distance_sum += distance_px;
        ++distance_count;
    };

    sample_side(floor_world_x + offset_world_x, floor_world_z + offset_world_z);
    sample_side(floor_world_x - offset_world_x, floor_world_z - offset_world_z);
    out_has_sample = distance_count > 0;
    return out_has_sample ? (distance_sum / static_cast<float>(distance_count)) : 0.0f;
}

bool sample_world_distance_scale(const WarpedScreenGrid& cam,
                                 float world_x,
                                 float world_y,
                                 float world_z,
                                 float& out_scale) {
    out_scale = 1.0f;
    if (!std::isfinite(world_x) || !std::isfinite(world_y) || !std::isfinite(world_z)) {
        return false;
    }

    const SDL_Point sample_world{
        static_cast<int>(std::lround(world_x)),
        static_cast<int>(std::lround(world_y))};
    const int sample_world_z = static_cast<int>(std::lround(world_z));
    const WarpedScreenGrid::RenderEffects effects =
        cam.compute_render_effects(sample_world,
                                   0.0f,
                                   0.0f,
                                   WarpedScreenGrid::RenderSmoothingKey{},
                                   sample_world_z);
    if (!std::isfinite(effects.distance_scale) || effects.distance_scale <= 0.0f) {
        return false;
    }

    out_scale = std::max(0.0001f, effects.distance_scale);
    return true;
}

bool project_world_point(const WarpedScreenGrid& cam,
                         float world_x,
                         float world_y,
                         float world_z,
                         SDL_FPoint& out) {
    return cam.project_world_point(SDL_FPoint{world_x, world_y}, world_z, out) &&
           std::isfinite(out.x) &&
           std::isfinite(out.y);
}

struct WarpedMesh {
    std::array<SDL_Vertex, 4> vertices{};
    std::array<int, 6> indices{0, 1, 2, 0, 2, 3};
    bool valid = false;
};

bool build_perspective_mesh(RenderObject& obj,
                            const WarpedScreenGrid& cam,
                            float perspective_scale,
                            float base_world_z,
                            WarpedMesh& mesh) {
    if (!obj.texture || obj.screen_rect.w <= 0 || obj.screen_rect.h <= 0) {
        return false;
    }

    const float safe_perspective = render_projection::sanitize_perspective_scale(perspective_scale);
    const std::uint64_t camera_version = cam.camera_state_version();
    auto quantize = [](float value) -> std::int64_t {
        if (!std::isfinite(value)) {
            return std::numeric_limits<std::int64_t>::min();
        }
        return static_cast<std::int64_t>(std::llround(static_cast<double>(value) * 256.0));
    };

    const float anchor_world_x = std::isfinite(obj.world_anchor_x)
        ? obj.world_anchor_x
        : static_cast<float>(obj.screen_rect.x);
    const float anchor_world_y = std::isfinite(obj.world_anchor_y)
        ? obj.world_anchor_y
        : static_cast<float>(obj.screen_rect.y);

    const std::int64_t key_x = quantize(anchor_world_x);
    const std::int64_t key_y = quantize(anchor_world_y);
    const std::int64_t key_z = quantize(base_world_z);
    const std::int64_t key_scale = quantize(safe_perspective);
    if (!obj.mesh_dirty &&
        obj.has_cached_mesh &&
        obj.cached_mesh_texture == obj.texture &&
        obj.cached_projection_key_valid &&
        obj.cached_position_key_x == key_x &&
        obj.cached_position_key_y == key_y &&
        obj.cached_world_z_key == key_z &&
        obj.cached_scale_key == key_scale &&
        obj.cached_camera_state_version == camera_version) {
        mesh.vertices = obj.cached_vertices;
        mesh.indices = obj.cached_indices;
        mesh.valid = true;
        return true;
    }

    render_projection::SpriteProjectionInput input{};
    if (!render_projection::assemble_render_object_projection_input(obj, safe_perspective, base_world_z, input)) {
        return false;
    }

    if (obj.dimension_cache_texture != obj.texture) {
        obj.dimension_cache_texture = obj.texture;
        obj.has_atlas_size = false;
        obj.has_texture_size = false;
        obj.has_cached_mesh = false;
    }

    if (!obj.has_atlas_size) {
        float atlas_wf = 0.0f;
        float atlas_hf = 0.0f;
        if (!SDL_GetTextureSize(obj.texture, &atlas_wf, &atlas_hf)) {
            return false;
        }
        obj.atlas_w = static_cast<int>(std::lround(atlas_wf));
        obj.atlas_h = static_cast<int>(std::lround(atlas_hf));
        obj.has_atlas_size = true;
        if (!obj.has_texture_size) {
            obj.texture_w = obj.atlas_w;
            obj.texture_h = obj.atlas_h;
            obj.has_texture_size = obj.texture_w > 0 && obj.texture_h > 0;
        }
    }

    int tex_w = obj.texture_w;
    int tex_h = obj.texture_h;
    if (!obj.has_texture_size) {
        tex_w = obj.atlas_w;
        tex_h = obj.atlas_h;
        obj.texture_w = tex_w;
        obj.texture_h = tex_h;
        obj.has_texture_size = tex_w > 0 && tex_h > 0;
    }
    if (obj.atlas_w <= 0 || obj.atlas_h <= 0 || tex_w <= 0 || tex_h <= 0) {
        return false;
    }

    float u0 = 0.0f;
    float u1 = 1.0f;
    float v0 = 0.0f;
    float v1 = 1.0f;
    if (obj.has_src_rect) {
        const float pad_x = 0.5f / static_cast<float>(obj.atlas_w);
        const float pad_y = 0.5f / static_cast<float>(obj.atlas_h);
        u0 = (static_cast<float>(obj.src_rect.x) + pad_x) / static_cast<float>(obj.atlas_w);
        u1 = (static_cast<float>(obj.src_rect.x + obj.src_rect.w) - pad_x) / static_cast<float>(obj.atlas_w);
        v0 = (static_cast<float>(obj.src_rect.y) + pad_y) / static_cast<float>(obj.atlas_h);
        v1 = (static_cast<float>(obj.src_rect.y + obj.src_rect.h) - pad_y) / static_cast<float>(obj.atlas_h);
    } else {
        const float pad_x = 0.5f / static_cast<float>(tex_w);
        const float pad_y = 0.5f / static_cast<float>(tex_h);
        u0 = pad_x;
        u1 = 1.0f - pad_x;
        v0 = pad_y;
        v1 = 1.0f - pad_y;
    }
    if ((obj.flip & SDL_FLIP_HORIZONTAL) != 0) std::swap(u0, u1);
    if ((obj.flip & SDL_FLIP_VERTICAL) != 0) std::swap(v0, v1);

    render_projection::ProjectedSpriteFrame projection{};
    if (!render_projection::build_projected_sprite_frame(cam, input, projection)) {
        return false;
    }

    const SDL_FColor color{
        obj.color_mod.r / 255.0f,
        obj.color_mod.g / 255.0f,
        obj.color_mod.b / 255.0f,
        obj.color_mod.a / 255.0f};

    mesh.vertices[0] = SDL_Vertex{projection.screen_tl, color, SDL_FPoint{u0, v0}};
    mesh.vertices[1] = SDL_Vertex{projection.screen_tr, color, SDL_FPoint{u1, v0}};
    mesh.vertices[2] = SDL_Vertex{projection.screen_br, color, SDL_FPoint{u1, v1}};
    mesh.vertices[3] = SDL_Vertex{projection.screen_bl, color, SDL_FPoint{u0, v1}};
    mesh.valid = true;

    obj.cached_vertices = mesh.vertices;
    obj.cached_indices = mesh.indices;
    obj.cached_position_key_x = key_x;
    obj.cached_position_key_y = key_y;
    obj.cached_world_z_key = key_z;
    obj.cached_scale_key = key_scale;
    obj.cached_projection_key_valid = true;
    obj.cached_camera_state_version = camera_version;
    obj.cached_mesh_texture = obj.texture;
    obj.has_cached_mesh = true;
    obj.mesh_dirty = false;
    return true;
}

std::uint32_t stable_path_color_hash(const std::string& animation_id, std::size_t path_index) {
    std::uint32_t hash = 2166136261u;
    for (unsigned char c : animation_id) {
        hash ^= c;
        hash *= 16777619u;
    }
    hash ^= static_cast<std::uint32_t>(path_index & 0xffffffffu);
    hash *= 16777619u;
    return hash;
}

SDL_Color stable_movement_path_color(const std::string& animation_id, std::size_t path_index) {
    constexpr std::array<SDL_Color, 12> palette{{
        SDL_Color{48, 200, 255, 220}, SDL_Color{255, 214, 64, 220}, SDL_Color{120, 235, 110, 220},
        SDL_Color{255, 128, 96, 220}, SDL_Color{130, 170, 255, 220}, SDL_Color{255, 156, 220, 220},
        SDL_Color{96, 240, 210, 220}, SDL_Color{240, 178, 96, 220}, SDL_Color{190, 240, 112, 220},
        SDL_Color{255, 96, 150, 220}, SDL_Color{140, 210, 255, 220}, SDL_Color{255, 196, 120, 220},
    }};
    return palette[stable_path_color_hash(animation_id, path_index) % palette.size()];
}

struct GeometryBatcherDepthBucketCache {
    const GeometryBatcher* batcher = nullptr;
    std::int64_t quantized_depth = 0;
    void* bucket = nullptr;
};

GeometryBatcherDepthBucketCache& geometry_batcher_depth_bucket_cache() {
    static thread_local GeometryBatcherDepthBucketCache cache;
    return cache;
}


inline float ticks_to_seconds(Uint64 ticks) {
    return static_cast<float>(ticks) * 0.001f;
}

} // namespace

namespace render_internal {

bool clear_gameplay_target_to_color(SDL_Renderer* renderer,
                                    SDL_Texture* gameplay_target,
                                    SDL_Color clear_color) {
    if (!renderer || !gameplay_target) {
        return false;
    }
    SDL_SetRenderTarget(renderer, gameplay_target);
    SDL_SetRenderViewport(renderer, nullptr);
    SDL_SetRenderClipRect(renderer, nullptr);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, clear_color.r, clear_color.g, clear_color.b, clear_color.a);
    SDL_RenderClear(renderer);
    return true;
}

FloorLightContact resolve_floor_light_contact(float flat_world_x,
                                              float flat_world_z,
                                              float displaced_world_x,
                                              float displaced_world_z,
                                              float world_height) {
    (void)displaced_world_x;
    (void)displaced_world_z;
    FloorLightContact contact{};
    if (!std::isfinite(flat_world_x) || !std::isfinite(flat_world_z) || !std::isfinite(world_height)) {
        return contact;
    }
    contact.world_x = flat_world_x;
    contact.world_z = flat_world_z;
    contact.world_height = std::max(0.0f, world_height);
    contact.valid = true;
    return contact;
}

bool project_floor_contact_to_screen(const WarpedScreenGrid& cam,
                                     const FloorLightContact& contact,
                                     SDL_FPoint& out_screen) {
    return contact.valid && project_floor_point_to_screen(cam, contact.world_x, contact.world_z, out_screen);
}

bool sample_floor_light_footprint_axes_px(const WarpedScreenGrid& cam,
                                          const FloorLightContact& contact,
                                          const SDL_FPoint& floor_screen_center,
                                          float base_radius_world,
                                          float height_spread_scale,
                                          float& out_radius_x_px,
                                          float& out_radius_y_px) {
    out_radius_x_px = 0.0f;
    out_radius_y_px = 0.0f;
    if (!contact.valid ||
        !std::isfinite(base_radius_world) ||
        base_radius_world <= 0.0f ||
        !std::isfinite(height_spread_scale) ||
        height_spread_scale <= 0.0f) {
        return false;
    }

    bool has_x = false;
    bool has_y = false;
    const float sx = sample_floor_axis_radius_px(cam,
                                                 floor_screen_center,
                                                 contact.world_x,
                                                 contact.world_z,
                                                 base_radius_world,
                                                 0.0f,
                                                 has_x);
    const float sy = sample_floor_axis_radius_px(cam,
                                                 floor_screen_center,
                                                 contact.world_x,
                                                 contact.world_z,
                                                 0.0f,
                                                 base_radius_world,
                                                 has_y);
    if (!has_x && !has_y) {
        return false;
    }
    const float rx = std::clamp((has_x ? sx : sy) * height_spread_scale, 1.0f, 8192.0f);
    const float ry = std::clamp((has_y ? sy : sx) * height_spread_scale, 1.0f, 8192.0f);
    out_radius_x_px = rx;
    out_radius_y_px = ry;
    return std::isfinite(rx) && std::isfinite(ry);
}

float floor_light_depth_weight(float abs_depth_from_anchor, float floor_light_cull_depth) {
    const float safe_depth = std::max(1.0f, floor_light_cull_depth);
    const float t = std::clamp(std::fabs(abs_depth_from_anchor) / safe_depth, 0.0f, 1.0f);
    return 1.0f - (t * t * (3.0f - 2.0f * t));
}

float floor_light_height_normalized(float world_height, float base_radius_world) {
    return std::clamp(std::max(0.0f, world_height) / std::max(1.0f, base_radius_world), 0.0f, 8.0f);
}

float floor_light_height_weight(float world_height, float base_radius_world) {
    const float norm = floor_light_height_normalized(world_height, base_radius_world);
    return 1.0f / (1.0f + norm * norm);
}

float floor_light_height_spread_scale(float world_height, float base_radius_world) {
    const float norm = floor_light_height_normalized(world_height, base_radius_world);
    return std::clamp(std::sqrt(1.0f + norm * norm), 1.0f, 6.0f);
}

float floor_light_footprint_radius(float base_radius_px, float world_height) {
    return std::max(4.0f, base_radius_px) * floor_light_height_spread_scale(world_height, std::max(4.0f, base_radius_px));
}

float layer_light_strength_multiplier_for_depth(double depth_from_camera_plane,
                                                 float front_multiplier,
                                                 float behind_multiplier,
                                                 float transition_world) {
    const float safe_front = std::clamp(std::isfinite(front_multiplier) ? front_multiplier : 1.0f, 0.0f, 4.0f);
    const float safe_behind = std::clamp(std::isfinite(behind_multiplier) ? behind_multiplier : 1.0f, 0.0f, 4.0f);
    const float safe_transition = std::max(0.0f, transition_world);
    if (!std::isfinite(depth_from_camera_plane)) {
        return safe_front;
    }
    if (safe_transition <= 1.0e-4f) {
        return (depth_from_camera_plane <= 0.0) ? safe_front : safe_behind;
    }
    const float t = std::clamp(
        static_cast<float>((depth_from_camera_plane + static_cast<double>(safe_transition)) /
                           (2.0 * static_cast<double>(safe_transition))),
        0.0f,
        1.0f);
    const float smooth_t = t * t * (3.0f - 2.0f * t);
    return safe_front + ((safe_behind - safe_front) * smooth_t);
}

float apply_layer_light_strength_bias(float intensity,
                                      double depth_from_camera_plane,
                                      float front_multiplier,
                                      float behind_multiplier,
                                      float transition_world) {
    const float safe_intensity = (std::isfinite(intensity) && intensity > 0.0f) ? intensity : 0.0f;
    return safe_intensity * layer_light_strength_multiplier_for_depth(depth_from_camera_plane,
                                                                      front_multiplier,
                                                                      behind_multiplier,
                                                                      transition_world);
}

bool light_overlaps_layer_slice(const LayerEffectProcessor::RuntimeLight& light,
                                double layer_depth_min,
                                double layer_depth_max,
                                float layer_bounds_min_x,
                                float layer_bounds_min_y,
                                float layer_bounds_max_x,
                                float layer_bounds_max_y,
                                float screen_padding_px,
                                float depth_padding_world) {
    if (!std::isfinite(layer_depth_min) || !std::isfinite(layer_depth_max) || layer_depth_min > layer_depth_max ||
        !std::isfinite(layer_bounds_min_x) || !std::isfinite(layer_bounds_min_y) ||
        !std::isfinite(layer_bounds_max_x) || !std::isfinite(layer_bounds_max_y) ||
        layer_bounds_min_x > layer_bounds_max_x || layer_bounds_min_y > layer_bounds_max_y ||
        !std::isfinite(light.world_z)) {
        return false;
    }

    const bool has_world_radius = std::isfinite(light.radius_world) && light.radius_world > 0.0f;
    const bool has_screen_radius = std::isfinite(light.radius_px) && light.radius_px > 0.0f;
    if (!has_world_radius && !has_screen_radius) {
        return false;
    }

    const double safe_depth_padding = static_cast<double>(std::max(0.0f, depth_padding_world));
    const float safe_screen_padding = std::max(0.0f, screen_padding_px);
    const double depth_radius = static_cast<double>(has_world_radius ? light.radius_world : light.radius_px) + safe_depth_padding;
    const double light_depth_min = static_cast<double>(light.world_z) - depth_radius;
    const double light_depth_max = static_cast<double>(light.world_z) + depth_radius;
    if (light_depth_max < (layer_depth_min - safe_depth_padding) ||
        light_depth_min > (layer_depth_max + safe_depth_padding)) {
        return false;
    }
    if (!std::isfinite(light.screen_center.x) || !std::isfinite(light.screen_center.y)) {
        return false;
    }

    const bool center_inside = light.screen_center.x >= (layer_bounds_min_x - safe_screen_padding) &&
                               light.screen_center.x <= (layer_bounds_max_x + safe_screen_padding) &&
                               light.screen_center.y >= (layer_bounds_min_y - safe_screen_padding) &&
                               light.screen_center.y <= (layer_bounds_max_y + safe_screen_padding);
    if (center_inside) {
        return true;
    }
    if (!has_screen_radius) {
        return false;
    }

    const float padded_radius = light.radius_px + safe_screen_padding;
    const float min_x = light.screen_center.x - padded_radius;
    const float min_y = light.screen_center.y - padded_radius;
    const float max_x = light.screen_center.x + padded_radius;
    const float max_y = light.screen_center.y + padded_radius;
    return max_x >= (layer_bounds_min_x - safe_screen_padding) &&
           min_x <= (layer_bounds_max_x + safe_screen_padding) &&
           max_y >= (layer_bounds_min_y - safe_screen_padding) &&
           min_y <= (layer_bounds_max_y + safe_screen_padding);
}

bool dof_blur_chain_enabled(bool depth_of_field_enabled,
                            float blur_px,
                            float radial_blur_px) {
    return depth_of_field_enabled && (std::max(0.0f, blur_px) > 1.0e-4f || std::max(0.0f, radial_blur_px) > 1.0e-4f);
}

std::vector<int> distributed_blur_repeat_counts(std::size_t target_blur_pass_count,
                                                std::size_t layer_count) {
    std::vector<int> counts;
    if (layer_count == 0) {
        return counts;
    }
    counts.assign(layer_count, 0);
    if (target_blur_pass_count == 0) {
        return counts;
    }
    const std::size_t base = target_blur_pass_count / layer_count;
    const std::size_t remainder = target_blur_pass_count % layer_count;
    std::size_t error = 0;
    for (std::size_t i = 0; i < layer_count; ++i) {
        std::size_t repeats = base;
        error += remainder;
        if (error >= layer_count) {
            ++repeats;
            error -= layer_count;
        }
        counts[i] = static_cast<int>(repeats);
    }
    return counts;
}

std::vector<int> background_chain_layers(const std::vector<int>& non_empty_layers, int player_layer_index) {
    std::vector<int> result;
    result.reserve(non_empty_layers.size());
    for (auto it = non_empty_layers.rbegin(); it != non_empty_layers.rend(); ++it) {
        if (*it >= player_layer_index) {
            result.push_back(*it);
        }
    }
    return result;
}

std::vector<int> foreground_chain_layers(const std::vector<int>& non_empty_layers, int player_layer_index) {
    std::vector<int> result;
    result.reserve(non_empty_layers.size());
    for (auto it = non_empty_layers.rbegin(); it != non_empty_layers.rend(); ++it) {
        if (*it < player_layer_index) {
            result.push_back(*it);
        }
    }
    return result;
}

} // namespace render_internal

GeometryBatcher::GeometryBatcher(SDL_Renderer* renderer) : renderer_(renderer) {
    vertex_buffer_.reserve(40000);
    index_buffer_.reserve(60000);
}

void GeometryBatcher::addQuad(SDL_Texture* texture,
                              const SDL_Vertex vertices[4],
                              const int indices[6],
                              SDL_BlendMode blend_mode,
                              double depth) {
    (void)indices;
    if (!texture) {
        return;
    }

    DrawItem item{};
    item.texture = texture;
    item.blend_mode = blend_mode;
    item.vertices[0] = vertices[0];
    item.vertices[1] = vertices[1];
    item.vertices[2] = vertices[2];
    item.vertices[3] = vertices[3];
    item.depth = depth;

    DepthBucket* bucket = nullptr;
    auto& cache = geometry_batcher_depth_bucket_cache();
    if (std::isfinite(depth)) {
        const auto quantized = quantize_depth(depth);
        if (cache.batcher == this && cache.bucket && cache.quantized_depth == quantized) {
            bucket = static_cast<DepthBucket*>(cache.bucket);
        } else {
            bucket = &depth_buckets_[quantized];
            cache.batcher = this;
            cache.quantized_depth = quantized;
            cache.bucket = bucket;
        }
    } else {
        bucket = &invalid_depth_bucket_;
        if (cache.batcher == this) {
            cache.bucket = nullptr;
        }
    }
    bucket->items.push_back(item);
}

void GeometryBatcher::flush() {
    if (depth_buckets_.empty() && invalid_depth_bucket_.items.empty()) {
        last_flush_cpu_ms_ = 0.0;
        return;
    }

    const auto start = std::chrono::steady_clock::now();
    draw_call_count_ = 0;
    total_vertices_ = 0;
    static constexpr int kQuadIndices[6] = {0, 1, 2, 0, 2, 3};
    vertex_buffer_.clear();
    index_buffer_.clear();

    auto emit_current_batch = [&](SDL_Texture* current_texture, SDL_BlendMode current_blend) {
        if (!current_texture || vertex_buffer_.empty() || index_buffer_.empty()) {
            return;
        }
        SDL_SetTextureBlendMode(current_texture, current_blend);
        SDL_RenderGeometry(renderer_,
                           current_texture,
                           vertex_buffer_.data(),
                           static_cast<int>(vertex_buffer_.size()),
                           index_buffer_.data(),
                           static_cast<int>(index_buffer_.size()));
        ++draw_call_count_;
        total_vertices_ += vertex_buffer_.size();
        vertex_buffer_.clear();
        index_buffer_.clear();
    };

    auto emit_item = [&](const DrawItem& item, SDL_Texture*& current_texture, SDL_BlendMode& current_blend) {
        if (item.texture != current_texture || item.blend_mode != current_blend) {
            emit_current_batch(current_texture, current_blend);
            current_texture = item.texture;
            current_blend = item.blend_mode;
        }
        const int base = static_cast<int>(vertex_buffer_.size());
        vertex_buffer_.push_back(item.vertices[0]);
        vertex_buffer_.push_back(item.vertices[1]);
        vertex_buffer_.push_back(item.vertices[2]);
        vertex_buffer_.push_back(item.vertices[3]);
        for (int idx : kQuadIndices) {
            index_buffer_.push_back(base + idx);
        }
    };

    SDL_Texture* current_texture = nullptr;
    SDL_BlendMode current_blend = SDL_BLENDMODE_BLEND;
    for (auto it = depth_buckets_.rbegin(); it != depth_buckets_.rend(); ++it) {
        for (const DrawItem& item : it->second.items) {
            emit_item(item, current_texture, current_blend);
        }
    }
    for (const DrawItem& item : invalid_depth_bucket_.items) {
        emit_item(item, current_texture, current_blend);
    }
    emit_current_batch(current_texture, current_blend);

    const auto end = std::chrono::steady_clock::now();
    last_flush_cpu_ms_ = std::chrono::duration<double, std::milli>(end - start).count();
}

void GeometryBatcher::clear() {
    depth_buckets_.clear();
    invalid_depth_bucket_ = DepthBucket{};
    draw_call_count_ = 0;
    total_vertices_ = 0;
    last_flush_cpu_ms_ = 0.0;
    vertex_buffer_.clear();
    index_buffer_.clear();
    auto& cache = geometry_batcher_depth_bucket_cache();
    if (cache.batcher == this) {
        cache = GeometryBatcherDepthBucketCache{};
    }
}

void GeometryBatcher::for_each_item_far_to_near(const std::function<void(const DrawItem&)>& fn) const {
    if (!fn) {
        return;
    }
    for (auto it = depth_buckets_.rbegin(); it != depth_buckets_.rend(); ++it) {
        for (const DrawItem& item : it->second.items) {
            fn(item);
        }
    }
    for (const DrawItem& item : invalid_depth_bucket_.items) {
        fn(item);
    }
}

SceneRenderer::SceneRenderer(SDL_Renderer* renderer,
                             Assets* assets,
                             int screen_width,
                             int screen_height,
                             const nlohmann::json& map_manifest,
                             const std::string& map_id)
    : SceneRenderer(require_prerequisites(renderer, assets),
                    renderer,
                    assets,
                    screen_width,
                    screen_height,
                    map_manifest,
                    map_id) {}

SceneRenderer::PrevalidatedTag SceneRenderer::require_prerequisites(SDL_Renderer* renderer,
                                                                    Assets* assets) {
    std::string reason;
    if (!SceneRenderer::prerequisites_ready(renderer, assets, &reason)) {
        const std::string message = reason.empty() ? "SceneRenderer prerequisites missing." : reason;
        vibble::log::error(std::string{"[SceneRenderer] Initialization aborted: "} + message);
        if (!renderer) SDL_assert(renderer != nullptr);
        if (!assets) SDL_assert(assets != nullptr);
        throw std::invalid_argument(message);
    }
    return PrevalidatedTag{};
}

SceneRenderer::SceneRenderer(PrevalidatedTag,
                             SDL_Renderer* renderer,
                             Assets* assets,
                             int screen_width,
                             int screen_height,
                             const nlohmann::json& map_manifest,
                             const std::string& map_id)
    : renderer_(renderer),
      assets_(assets),
      screen_width_(screen_width),
      screen_height_(screen_height),
      geometry_batcher_(std::make_unique<GeometryBatcher>(renderer)),
      dynamic_boundary_system_(std::make_unique<DynamicBoundarySystem>()),
      floor_composer_(std::make_unique<FloorComposer>(renderer, assets)),
      blur_chain_renderer_(std::make_unique<BlurChainRenderer>(renderer)),
      layer_stack_renderer_(std::make_unique<LayerStackRenderer>(renderer)),
      layer_submission_builder_(std::make_unique<LayerSubmissionBuilder>()),
      scene_composite_pass_(std::make_unique<SceneCompositePass>(renderer)),
      debug_overlay_renderer_(std::make_unique<DebugOverlayRenderer>(renderer)) {
    map_clear_color_ = SDL_Color{69, 101, 74, 255};

    if (dynamic_boundary_system_ && !dynamic_boundary_system_->initialize(renderer_, &assets_->library())) {
        vibble::log::warn("[SceneRenderer] Failed to initialize dynamic boundary system");
    }

    map_radius_world_ = map_layers::map_radius_from_map_info(map_manifest);
    if (!std::isfinite(map_radius_world_) || map_radius_world_ <= 0.0) {
        map_radius_world_ = static_cast<double>(std::max(screen_width_, screen_height_));
    }

    if (floor_composer_) floor_composer_->set_output_dimensions(screen_width_, screen_height_);
    if (blur_chain_renderer_) blur_chain_renderer_->set_output_dimensions(screen_width_, screen_height_);
    if (layer_stack_renderer_) layer_stack_renderer_->set_output_dimensions(screen_width_, screen_height_);

    vibble::log::debug(std::string{"[SceneRenderer] Initializing for map '"} + map_id +
                       "' with screen " + std::to_string(screen_width_) + "x" + std::to_string(screen_height_) + ".");
}

SceneRenderer::~SceneRenderer() {
    destroy_texture(scene_composite_tex_);
}

bool SceneRenderer::ensure_scene_target() {
    if (!renderer_ || screen_width_ <= 0 || screen_height_ <= 0) {
        return false;
    }
    if (scene_composite_tex_) {
        float w = 0.0f;
        float h = 0.0f;
        if (SDL_GetTextureSize(scene_composite_tex_, &w, &h) &&
            static_cast<int>(std::lround(w)) == screen_width_ &&
            static_cast<int>(std::lround(h)) == screen_height_) {
            return true;
        }
        destroy_texture(scene_composite_tex_);
    }
    scene_composite_tex_ = SDL_CreateTexture(renderer_,
                                             SDL_PIXELFORMAT_RGBA8888,
                                             SDL_TEXTUREACCESS_TARGET,
                                             screen_width_,
                                             screen_height_);
    if (scene_composite_tex_) {
        SDL_SetTextureBlendMode(scene_composite_tex_, SDL_BLENDMODE_BLEND);
    }
    return scene_composite_tex_ != nullptr;
}

void SceneRenderer::invalidate_dynamic_boundary_system() {
    if (dynamic_boundary_system_) {
        dynamic_boundary_system_->invalidate_config();
    }
}

const std::vector<DynamicBoundarySystem::BoundarySprite>& SceneRenderer::dynamic_boundary_sprites() const {
    static const std::vector<DynamicBoundarySystem::BoundarySprite> empty;
    return dynamic_boundary_system_ ? dynamic_boundary_system_->get_boundary_sprites() : empty;
}

SDL_Renderer* SceneRenderer::get_renderer() const {
    return renderer_;
}

void SceneRenderer::set_output_dimensions(int screen_width, int screen_height) {
    const int safe_w = std::max(1, screen_width);
    const int safe_h = std::max(1, screen_height);
    if (safe_w == screen_width_ && safe_h == screen_height_) {
        return;
    }

    screen_width_ = safe_w;
    screen_height_ = safe_h;
    destroy_texture(scene_composite_tex_);
    if (floor_composer_) floor_composer_->set_output_dimensions(screen_width_, screen_height_);
    if (blur_chain_renderer_) blur_chain_renderer_->set_output_dimensions(screen_width_, screen_height_);
    if (layer_stack_renderer_) layer_stack_renderer_->set_output_dimensions(screen_width_, screen_height_);
}

std::optional<SDL_Point> SceneRenderer::postprocess_target_size() const {
    if (!scene_composite_tex_) {
        return std::nullopt;
    }
    float w = 0.0f;
    float h = 0.0f;
    if (!SDL_GetTextureSize(scene_composite_tex_, &w, &h) || w <= 0.0f || h <= 0.0f) {
        return std::nullopt;
    }
    return SDL_Point{static_cast<int>(std::lround(w)), static_cast<int>(std::lround(h))};
}

void SceneRenderer::set_movement_debug_enabled(bool enabled) {
    if (debug_auto_paths_ == enabled) {
        return;
    }
    debug_auto_paths_ = enabled;
    if (!enabled) {
        movement_debug_snapshots_.clear();
        movement_debug_observed_state_.clear();
    }
}

void SceneRenderer::set_movement_debug_visible(bool visible) {
    movement_debug_visible_ = visible;
}

void SceneRenderer::set_anchor_point_debug_enabled(bool enabled) {
    anchor_point_debug_enabled_ = enabled;
}

void SceneRenderer::collect_frame_geometry(const WarpedScreenGrid& cam,
                                           world::WorldGrid& grid,
                                           double focus_plane_world_z,
                                           double max_cull_depth,
                                           std::vector<Asset*>& rendered_assets_for_debug) {
    if (!geometry_batcher_ || !dynamic_boundary_system_) {
        return;
    }

    geometry_batcher_->clear();
    rendered_assets_for_debug.clear();

    const bool boundary_assets_visible = assets_->boundary_assets_visible();
    const bool runtime_updates_enabled = assets_->should_run_runtime_updates();
    const bool should_render_boundaries = boundary_assets_visible && dynamic_boundary_system_->is_initialized();
    const float boundary_delta_ms = runtime_updates_enabled
        ? static_cast<float>(assets_->frame_delta_seconds() * 1000.0)
        : 0.0f;
    if (should_render_boundaries) {
        dynamic_boundary_system_->update(cam, grid, assets_, boundary_delta_ms);
    }

    static const std::vector<DynamicBoundarySystem::BoundarySprite> empty_boundary_sprites;
    const auto& boundary_sprites = should_render_boundaries
        ? dynamic_boundary_system_->get_boundary_sprites()
        : empty_boundary_sprites;

    static constexpr int kQuadIndices[6] = {0, 1, 2, 0, 2, 3};
    const float boundary_vertical_offset = DynamicBoundarySystem::vertical_offset();
    const float boundary_cull_margin = 64.0f;
    const float boundary_min_visible_px = static_cast<float>(screen_height_) * std::clamp(assets_->boundary_min_visible_screen_ratio(), 0.0f, 0.5f);

    auto queue_boundary_sprite = [&](const DynamicBoundarySystem::BoundarySprite& sprite, double depth_from_focus_plane) {
        const double depth_distance = std::fabs(depth_from_focus_plane);
        if (!std::isfinite(depth_distance) || depth_distance > max_cull_depth || !sprite.texture ||
            sprite.texture_w <= 0 || sprite.texture_h <= 0 || sprite.world_width <= 0.0f || sprite.world_height <= 0.0f ||
            !assets_->is_spawn_id_in_focus_filter(sprite.spawn_id)) {
            return;
        }

        SDL_FPoint base_screen{};
        if (!project_world_point(cam,
                                 sprite.world_pos.x,
                                 sprite.world_pos.y,
                                 static_cast<float>(sprite.world_z),
                                 base_screen)) {
            return;
        }

        const float half_width = sprite.world_width * 0.5f;
        const float adjusted_y = base_screen.y + boundary_vertical_offset;
        if (base_screen.x + half_width < -boundary_cull_margin ||
            base_screen.x - half_width > static_cast<float>(screen_width_) + boundary_cull_margin ||
            adjusted_y < -boundary_cull_margin ||
            adjusted_y - sprite.world_height > static_cast<float>(screen_height_) + boundary_cull_margin) {
            return;
        }
        if (boundary_min_visible_px > 0.0f && std::max(sprite.world_width, sprite.world_height) < boundary_min_visible_px) {
            return;
        }

        const float pad_x = 0.5f / static_cast<float>(sprite.texture_w);
        const float pad_y = 0.5f / static_cast<float>(sprite.texture_h);
        SDL_Vertex vertices[4]{};
        const SDL_FColor white{1.0f, 1.0f, 1.0f, 1.0f};
        vertices[0].position = SDL_FPoint{base_screen.x - half_width, adjusted_y - sprite.world_height};
        vertices[1].position = SDL_FPoint{base_screen.x + half_width, adjusted_y - sprite.world_height};
        vertices[2].position = SDL_FPoint{base_screen.x + half_width, adjusted_y};
        vertices[3].position = SDL_FPoint{base_screen.x - half_width, adjusted_y};
        vertices[0].color = vertices[1].color = vertices[2].color = vertices[3].color = white;
        vertices[0].tex_coord = SDL_FPoint{pad_x, pad_y};
        vertices[1].tex_coord = SDL_FPoint{1.0f - pad_x, pad_y};
        vertices[2].tex_coord = SDL_FPoint{1.0f - pad_x, 1.0f - pad_y};
        vertices[3].tex_coord = SDL_FPoint{pad_x, 1.0f - pad_y};
        geometry_batcher_->addQuad(sprite.texture, vertices, kQuadIndices, SDL_BLENDMODE_BLEND, depth_from_focus_plane);
    };

    auto normalize_depth = [](double depth) {
        return std::isfinite(depth) ? depth : std::numeric_limits<double>::lowest();
    };
    auto depth_for_traversal = [&](const Assets::ActiveTraversalEntry& entry) {
        if (!entry.asset) {
            return std::numeric_limits<double>::lowest();
        }
        if (std::isfinite(entry.depth_from_anchor)) {
            return normalize_depth(entry.depth_from_anchor);
        }
        return normalize_depth(render_depth::depth_from_anchor(focus_plane_world_z,
                                                               static_cast<double>(entry.asset->world_z()),
                                                               entry.asset->render_depth_bias()));
    };
    auto depth_for_boundary = [&](const DynamicBoundarySystem::BoundarySprite& sprite) {
        return normalize_depth(render_depth::depth_from_anchor(focus_plane_world_z,
                                                               static_cast<double>(sprite.world_z)));
    };

    rendered_assets_for_debug.reserve(assets_->active_traversal().size());

    std::size_t traversal_index = 0;
    std::size_t boundary_index = 0;
    const auto& active_traversal = assets_->active_traversal();
    while (traversal_index < active_traversal.size() || boundary_index < boundary_sprites.size()) {
        const double asset_depth = traversal_index < active_traversal.size()
            ? depth_for_traversal(active_traversal[traversal_index])
            : std::numeric_limits<double>::lowest();
        const double boundary_depth = boundary_index < boundary_sprites.size()
            ? depth_for_boundary(boundary_sprites[boundary_index])
            : std::numeric_limits<double>::lowest();

        if (traversal_index < active_traversal.size() && asset_depth >= boundary_depth) {
            const Assets::ActiveTraversalEntry& traversal_entry = active_traversal[traversal_index++];
            Asset* asset = traversal_entry.asset;
            if (!asset || asset->dead || !assets_->is_asset_in_focus_filter(asset)) {
                continue;
            }

            rendered_assets_for_debug.push_back(asset);

            RenderObject obj{};
            if (!render_build::build_direct_asset_render_object(asset, obj)) {
                continue;
            }

            const Asset::PerspectiveSample perspective_sample = asset->runtime_perspective_sample();
            const float perspective_scale = perspective_sample.scale;
            const float base_world_z = static_cast<float>(asset->world_z());
            const RuntimeCameraMetrics& camera_metrics = asset->runtime_camera_metrics;
            const bool has_camera_metrics =
                asset->has_fresh_runtime_camera_metrics(assets_->frame_id(), cam.camera_state_version());
            const double asset_depth_from_focus_plane = has_camera_metrics
                ? camera_metrics.world_z_depth_from_anchor
                : render_depth::depth_from_anchor(focus_plane_world_z,
                                                  static_cast<double>(asset->world_z()),
                                                  asset->render_depth_bias());

        WarpedMesh mesh{};
        if (!obj.texture ||
            !build_perspective_mesh(obj, cam, perspective_scale, base_world_z + obj.world_z_offset, mesh) ||
            !mesh.valid) {
            continue;
        }

        // IMPORTANT:
        // Project the quad using the render Z offset so it appears in the right place,
        // but keep DOF/layer bucketing tied to the asset's focus-plane depth, not the
        // sprite's render-anchor Z offset. Otherwise the player/focus object can get
        // pushed into an adjacent blurred layer even when the camera focus plane is correct.
        geometry_batcher_->addQuad(obj.texture,
                                mesh.vertices.data(),
                                mesh.indices.data(),
                                obj.blend_mode,
                                asset_depth_from_focus_plane);
            continue;
        }

        if (boundary_index < boundary_sprites.size()) {
            queue_boundary_sprite(boundary_sprites[boundary_index++], boundary_depth);
        }
    }
}


void SceneRenderer::gather_runtime_lights(const WarpedScreenGrid& cam,
                                          double focus_plane_world_z,
                                          const std::vector<Asset*>& rendered_assets,
                                          std::vector<LayerEffectProcessor::RuntimeLight>& out_lights) {
    out_lights.clear();
    runtime_light_debug_overlay_.clear();
    runtime_light_rendered_count_ = 0;
    runtime_light_culled_count_ = 0;
    if (!assets_) {
        return;
    }

    const WarpedScreenGrid::RealismSettings realism = cam.get_settings();
    const bool overlap_culling_enabled = realism.light_radius_overlap_culling_enabled;
    const bool fade_smoothing_enabled = realism.light_fade_smoothing_enabled;
    const float min_fade_seconds = std::max(0.0f, realism.light_min_fade_seconds);
    const float fade_in_seconds = std::max(min_fade_seconds, std::max(0.0f, realism.light_fade_in_seconds));
    const float fade_out_seconds = std::max(min_fade_seconds, std::max(0.0f, realism.light_fade_out_seconds));
    const float dt_seconds = std::clamp(assets_->frame_delta_seconds(), 0.0f, 0.25f);
    const std::uint64_t frame_token = static_cast<std::uint64_t>(assets_->frame_id());

    std::vector<Asset*> candidate_assets;
    candidate_assets.reserve(rendered_assets.size() + (overlap_culling_enabled ? assets_->all.size() : 0));
    std::unordered_set<Asset*> seen_assets;
    seen_assets.reserve(rendered_assets.size() + (overlap_culling_enabled ? assets_->all.size() : 0));
    for (Asset* asset : rendered_assets) {
        if (asset && seen_assets.insert(asset).second) {
            candidate_assets.push_back(asset);
        }
    }
    if (overlap_culling_enabled) {
        for (Asset* asset : assets_->all) {
            if (asset && seen_assets.insert(asset).second) {
                candidate_assets.push_back(asset);
            }
        }
    }
    out_lights.reserve(candidate_assets.size());

    constexpr float kCullingMargin = 128.0f;
    std::unordered_set<std::string> seen_light_keys;
    seen_light_keys.reserve(candidate_assets.size() * 2);
    const std::uint64_t gather_start_ticks = SDL_GetTicks();

    for (Asset* asset : candidate_assets) {
        if (!asset || asset->dead || !asset->current_frame || !assets_->is_asset_in_focus_filter(asset)) {
            continue;
        }

        for (const DisplacedAssetAnchorPoint& anchor : asset->current_frame->anchor_points) {
            if (!anchor.is_valid() || !anchor.has_light_data) {
                continue;
            }

            const std::optional<AnchorPoint> resolved = asset->anchor_state(anchor.name,
                                                                            anchor_points::GridMaterialization::None,
                                                                            Asset::AnchorResolveMode::Cached);
            if (!resolved.has_value() || !resolved->exists) {
                continue;
            }

            const float anchor_world_x = resolved->world_exact_pos_2d.x;
            const float anchor_world_y = resolved->world_exact_pos_2d.y;
            const float anchor_world_z = resolved->world_exact_z;
            SDL_FPoint screen = resolved->screen_pos_2d;
            if ((!std::isfinite(screen.x) || !std::isfinite(screen.y)) &&
                (!cam.project_world_point(SDL_FPoint{anchor_world_x, anchor_world_y}, anchor_world_z, screen) ||
                 !std::isfinite(screen.x) || !std::isfinite(screen.y))) {
                continue;
            }

            AnchorLightData light = anchor.light;
            light.sanitize();
            const float fallback_scale = resolved->has_flat_perspective_scale
                ? std::max(0.05f, resolved->flat_perspective_scale)
                : 1.0f;
            float sampled_scale = fallback_scale;
            float world_sampled_scale = 1.0f;
            if (sample_world_distance_scale(cam, anchor_world_x, anchor_world_y, anchor_world_z, world_sampled_scale)) {
                sampled_scale = std::max(0.05f, world_sampled_scale);
            }

            const float radius_world = std::max(AnchorLightData::kMinRadius, light.radius);
            const float radius_px = std::max(4.0f, radius_world * sampled_scale);
            const bool overlaps_view = screen.x + radius_px >= -kCullingMargin &&
                                       screen.x - radius_px <= static_cast<float>(screen_width_) + kCullingMargin &&
                                       screen.y + radius_px >= -kCullingMargin &&
                                       screen.y - radius_px <= static_cast<float>(screen_height_) + kCullingMargin;
            const bool enabled_and_overlapping = !anchor.hidden && overlaps_view;
            if (!enabled_and_overlapping) {
                ++runtime_light_culled_count_;
            }

            std::string light_key = std::to_string(reinterpret_cast<std::uintptr_t>(asset));
            light_key.push_back('|');
            light_key.append(anchor.name);
            seen_light_keys.insert(light_key);
            RuntimeLightCacheEntry& cache_entry = runtime_light_cache_[light_key];
            const bool first_seen = cache_entry.last_seen_frame == 0;
            cache_entry.last_seen_frame = frame_token;
            cache_entry.fade.last_seen_frame = frame_token;

            const float target_intensity = enabled_and_overlapping ? light.intensity : 0.0f;
            if (!fade_smoothing_enabled) {
                cache_entry.fade.intensity_current = target_intensity;
            } else {
                if (first_seen) {
                    cache_entry.fade.intensity_current = 0.0f;
                }
                const float duration = target_intensity > cache_entry.fade.intensity_current
                    ? std::max(0.0001f, fade_in_seconds)
                    : std::max(0.0001f, fade_out_seconds);
                const float alpha = std::clamp(dt_seconds / duration, 0.0f, 1.0f);
                cache_entry.fade.intensity_current += (target_intensity - cache_entry.fade.intensity_current) * alpha;
            }

            const float effective_intensity = std::max(0.0f, cache_entry.fade.intensity_current);
            const bool renderable = effective_intensity > 0.0005f && overlaps_view;
            if (realism.light_culling_debug_overlay) {
                runtime_light_debug_overlay_.push_back(render_debug::RuntimeLightDebugOverlayEntry{screen, radius_px, renderable});
            }
            if (!renderable) {
                continue;
            }

            LayerEffectProcessor::RuntimeLight& instance = cache_entry.instance;
            instance.stable_light_id = static_cast<std::uint64_t>(std::hash<std::string>{}(light_key));
            instance.screen_center = screen;
            instance.color = SDL_Color{light.color_r, light.color_g, light.color_b, 255};
            instance.intensity = effective_intensity;
            instance.opacity = light.opacity;
            instance.radius_px = radius_px;
            instance.radius_world = radius_world;
            instance.falloff = light.falloff;
            instance.world_z = static_cast<float>(
                render_depth::depth_from_anchor(focus_plane_world_z,
                                                static_cast<double>(anchor_world_z)));
            const render_internal::FloorLightContact floor_contact = render_internal::resolve_floor_light_contact(
                resolved->flat_world_exact_pos_2d.x,
                resolved->flat_world_exact_z,
                anchor_world_x,
                anchor_world_z,
                resolved->world_exact_pos_2d.y);
            instance.floor_world_x = floor_contact.world_x;
            instance.floor_world_z = floor_contact.world_z;
            instance.world_height = floor_contact.world_height;
            instance.has_floor_projection = false;
            instance.retained_by_hysteresis = false;
            instance.depth_blended = false;
            if (floor_contact.valid) {
                SDL_FPoint floor_screen{};
                if (render_internal::project_floor_contact_to_screen(cam, floor_contact, floor_screen)) {
                    instance.floor_screen_center = floor_screen;
                    instance.has_floor_projection = true;
                }
            }
            out_lights.push_back(cache_entry.instance);
            ++runtime_light_rendered_count_;
        }
    }

    for (auto it = runtime_light_cache_.begin(); it != runtime_light_cache_.end();) {
        if (seen_light_keys.find(it->first) == seen_light_keys.end() && frame_token > it->second.last_seen_frame + 120) {
            it = runtime_light_cache_.erase(it);
        } else {
            ++it;
        }
    }

    if (realism.light_culling_debug_overlay) {
        const std::uint64_t now_ticks = SDL_GetTicks();
        const std::uint64_t elapsed_ticks = SDL_GetTicks() - gather_start_ticks;
        if (runtime_light_profile_last_log_ticks_ == 0 || now_ticks - runtime_light_profile_last_log_ticks_ >= 1000) {
            runtime_light_profile_last_log_ticks_ = now_ticks;
            vibble::log::debug("[SceneRenderer] light gather profile: candidates=" +
                               std::to_string(candidate_assets.size()) +
                               " rendered=" + std::to_string(runtime_light_rendered_count_) +
                               " culled=" + std::to_string(runtime_light_culled_count_) +
                               " ms=" + std::to_string(ticks_to_seconds(elapsed_ticks) * 1000.0f));
        }
    }
}

void SceneRenderer::refresh_movement_debug_snapshots(const std::vector<Asset*>& visible_assets) {
    std::unordered_set<const Asset*> visible_set;
    visible_set.reserve(visible_assets.size());
    for (const Asset* asset : visible_assets) {
        if (asset && !asset->dead) {
            visible_set.insert(asset);
        }
    }

    for (auto it = movement_debug_snapshots_.begin(); it != movement_debug_snapshots_.end();) {
        if (visible_set.find(it->first) == visible_set.end()) {
            it = movement_debug_snapshots_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = movement_debug_observed_state_.begin(); it != movement_debug_observed_state_.end();) {
        if (visible_set.find(it->first) == visible_set.end()) {
            it = movement_debug_observed_state_.erase(it);
        } else {
            ++it;
        }
    }

    std::unordered_map<const Asset*, std::optional<float>> oval_child_headings_by_asset;
    oval_child_headings_by_asset.reserve(visible_assets.size());
    {
        const auto bindings =
            anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().debug_bindings_snapshot();
        for (const auto& binding : bindings) {
            if (!binding.owner || !binding.child_asset || !binding.owner->info || binding.anchor_name.empty()) {
                continue;
            }
            if (!binding.owner->info->find_oval_anchor_mapping(binding.anchor_name, true)) {
                continue;
            }

            // Oval-bound children are dynamic with mouse heading/radius state, so movement debug paths
            // are rebuilt every frame for these assets.
            std::optional<float> heading{};
            const auto anchor = binding.owner->anchor_state(binding.anchor_name,
                                                            anchor_points::GridMaterialization::None,
                                                            Asset::AnchorResolveMode::ForceRecompute);
            if (anchor.has_value() && anchor->exists) {
                heading = oval_anchor_heading::resolve_effective_oval_heading_radians(binding.owner, *anchor);
            }
            oval_child_headings_by_asset[binding.child_asset] = heading;
        }
    }

    for (Asset* asset : visible_assets) {
        if (!asset || asset->dead || !asset->info || !asset->current_frame) {
            continue;
        }

        const std::string current_animation = asset->current_animation;
        const AnimationFrame* current_frame = asset->current_frame;
        const bool current_is_first = current_frame->is_first;
        const bool current_is_last = current_frame->is_last;

        const auto observed_it = movement_debug_observed_state_.find(asset);
        const bool has_previous_state = observed_it != movement_debug_observed_state_.end();
        const bool has_snapshot = movement_debug_snapshots_.find(asset) != movement_debug_snapshots_.end();
        bool loop_trigger = false;
        bool end_trigger = false;
        if (has_previous_state) {
            const auto& previous = observed_it->second;
            loop_trigger = previous.frame_is_last && previous.animation_id == current_animation && current_is_first;
            end_trigger = previous.frame_is_last && previous.animation_id != current_animation;
        }
        const auto oval_heading_it = oval_child_headings_by_asset.find(asset);
        const bool is_oval_bound_child = oval_heading_it != oval_child_headings_by_asset.end();
        const std::optional<float> oval_heading_radians = is_oval_bound_child
            ? oval_heading_it->second
            : std::optional<float>{};

        if (!has_snapshot || loop_trigger || end_trigger || is_oval_bound_child) {
            render_debug::MovementDebugAssetSnapshot snapshot{};
            const SDL_Point origin = asset->world_xz_point();
            for (const auto& [animation_id, animation] : asset->info->animations) {
                const std::size_t path_count = animation.movement_path_count();
                for (std::size_t path_index = 0; path_index < path_count; ++path_index) {
                    const auto& movement_path = animation.movement_path(path_index);
                    if (movement_path.empty()) {
                        continue;
                    }
                    render_debug::MovementDebugPathSnapshot path_snapshot{};
                    path_snapshot.color = stable_movement_path_color(animation_id, path_index);
                    path_snapshot.world_points.reserve(movement_path.size() + 1);
                    path_snapshot.world_points.push_back(origin);

                    float world_x = static_cast<float>(origin.x);
                    float world_z = static_cast<float>(origin.y);
                    bool has_movement = false;
                    for (const AnimationFrame& frame : movement_path) {
                        if (frame.dx != 0 || frame.dz != 0) {
                            has_movement = true;
                        }
                        SDL_Point step_delta =
                            animation_update::movement_rotation::frame_floor_delta_absolute_yaw(frame);
                        float step_x = static_cast<float>(step_delta.x);
                        float step_z = static_cast<float>(step_delta.y);
                        if (oval_heading_radians.has_value()) {
                            oval_anchor_heading::rotate_xz_about_world_y(
                                *oval_heading_radians,
                                step_x,
                                step_z);
                        }
                        world_x += step_x;
                        world_z += step_z;
                        const SDL_Point next{
                            static_cast<int>(std::lround(world_x)),
                            static_cast<int>(std::lround(world_z))};
                        if (path_snapshot.world_points.empty() ||
                            next.x != path_snapshot.world_points.back().x ||
                            next.y != path_snapshot.world_points.back().y) {
                            path_snapshot.world_points.push_back(next);
                        }
                    }
                    if (has_movement && path_snapshot.world_points.size() >= 2) {
                        snapshot.paths.push_back(std::move(path_snapshot));
                    }
                }
            }
            if (snapshot.paths.empty()) {
                movement_debug_snapshots_.erase(asset);
            } else {
                movement_debug_snapshots_[asset] = std::move(snapshot);
            }
        }

        movement_debug_observed_state_[asset] = render_debug::MovementDebugObservedState{
            current_animation,
            current_frame,
            current_is_first,
            current_is_last};
    }
}

void SceneRenderer::render() {
    if (!renderer_ || !assets_ || screen_width_ <= 0 || screen_height_ <= 0 || !ensure_scene_target()) {
        return;
    }

    render_internal::clear_gameplay_target_to_color(renderer_, scene_composite_tex_, map_clear_color_);
    WarpedScreenGrid& cam = assets_->getView();
    world::WorldGrid& grid = assets_->world_grid();
    const WarpedScreenGrid::RealismSettings realism = cam.get_settings();
    const double depth_anchor_world_z = cam.anchor_world_z();
    double player_split_world_z = depth_anchor_world_z;
    if (assets_->is_dev_mode()) {
        if (const Input* input = assets_->get_input()) {
            const SDL_FPoint cursor_world = cam.screen_to_map(SDL_Point{input->getX(), input->getY()});
            if (std::isfinite(cursor_world.y)) {
                player_split_world_z = static_cast<double>(cursor_world.y);
            }
        }
    } else if (assets_->player) {
        const double player_world_z = static_cast<double>(assets_->player->world_z());
        if (std::isfinite(player_world_z)) {
            player_split_world_z = player_world_z;
        }
    }
    if (!std::isfinite(player_split_world_z)) {
        player_split_world_z = depth_anchor_world_z;
    }
    const double max_cull_depth = std::max(1.0, static_cast<double>(realism.max_cull_depth));

    std::vector<Asset*> rendered_assets_for_debug;
    collect_frame_geometry(cam, grid, depth_anchor_world_z, max_cull_depth, rendered_assets_for_debug);

    std::vector<LayerEffectProcessor::RuntimeLight> runtime_lights;
    const bool runtime_lighting_enabled = assets_->should_render_runtime_lighting();
    if (runtime_lighting_enabled) {
        gather_runtime_lights(cam, depth_anchor_world_z, rendered_assets_for_debug, runtime_lights);
    }
    SDL_Texture* floor_texture = nullptr;
    if (floor_composer_) {
        floor_texture = floor_composer_->compose(cam,
                                                 grid,
                                                 runtime_lights,
                                                 runtime_lighting_enabled,
                                                 max_cull_depth,
                                                 map_clear_color_,
                                                 true);
    }

    SDL_SetRenderTarget(renderer_, scene_composite_tex_);
    SDL_SetRenderViewport(renderer_, nullptr);
    SDL_SetRenderClipRect(renderer_, nullptr);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);

    if (floor_texture) {
        render_texture_utils::draw_fullscreen_texture(renderer_, floor_texture);
    }

    const render_pipeline::LayerBuildResult layer_build = layer_submission_builder_
        ? layer_submission_builder_->build(*geometry_batcher_, cam, player_split_world_z, max_cull_depth)
        : render_pipeline::LayerBuildResult{};

    bool composed = false;
    if (layer_build.valid && !layer_build.non_empty_layers.empty() && layer_stack_renderer_ && scene_composite_pass_) {
        const float front_mult = std::clamp(realism.front_layer_light_strength_multiplier, 0.0f, 4.0f);
        const float behind_mult = std::clamp(realism.behind_layer_light_strength_multiplier, 0.0f, 4.0f);
        const render_pipeline::LayerRenderResult layer_render =
            layer_stack_renderer_->render(layer_build,
                                          runtime_lights,
                                          runtime_lighting_enabled,
                                          front_mult,
                                          behind_mult,
                                          realism.layer_light_overlap_padding_px,
                                          realism.layer_light_depth_padding_world,
                                          realism.layer_light_membership_hold_frames,
                                          realism.layer_light_depth_transition_world,
                                          realism.dark_mask_temporal_enabled,
                                          realism.dark_mask_temporal_prev_weight);

        SDL_Point screen_center = cam.get_focus_override_point();
        const SDL_FPoint optical_center{
            std::clamp(static_cast<float>(screen_center.x), 0.0f, static_cast<float>(screen_width_)),
            std::clamp(static_cast<float>(screen_center.y), 0.0f, static_cast<float>(screen_height_))};

        const render_pipeline::BlurCompositeResult blur_result = blur_chain_renderer_
            ? blur_chain_renderer_->compose(layer_render,
                                            floor_texture,
                                            realism.depth_of_field_enabled,
                                            realism.blur_px,
                                            realism.radial_blur_px,
                                            optical_center)
            : render_pipeline::BlurCompositeResult{};

        composed = scene_composite_pass_->compose(scene_composite_tex_, layer_render, blur_result);
    }

    if (!composed) {
        geometry_batcher_->flush();
    }

    SDL_SetRenderTarget(renderer_, nullptr);
    SDL_SetRenderViewport(renderer_, nullptr);
    SDL_SetRenderClipRect(renderer_, nullptr);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, map_clear_color_.r, map_clear_color_.g, map_clear_color_.b, map_clear_color_.a);
    SDL_RenderClear(renderer_);
    render_texture_utils::draw_fullscreen_texture(renderer_, scene_composite_tex_);

    if (debug_overlay_renderer_) {
        if (realism.light_culling_debug_overlay) {
            debug_overlay_renderer_->render_light_culling(runtime_light_debug_overlay_);
        }
        if (debug_auto_paths_ && movement_debug_visible_) {
            refresh_movement_debug_snapshots(rendered_assets_for_debug);
            debug_overlay_renderer_->render_movement_debug(cam,
                                                           screen_width_,
                                                           screen_height_,
                                                           movement_debug_snapshots_,
                                                           rendered_assets_for_debug);
        }
        if (anchor_point_debug_enabled_) {
            debug_overlay_renderer_->render_anchor_debug(cam,
                                                         screen_width_,
                                                         screen_height_,
                                                         rendered_assets_for_debug,
                                                         assets_->is_dev_mode());
        }
    }

    if (assets_->dev_grid_overlay_callback_) {
        assets_->dev_grid_overlay_callback_();
    }
}
