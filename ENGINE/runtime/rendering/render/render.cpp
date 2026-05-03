#include "rendering/render/render.hpp"
#include "rendering/render/render_depth_policy.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "rendering/render/render_object_projection.hpp"
#include "rendering/render/floor_composer.hpp"
#include "rendering/render/blur_chain_renderer.hpp"
#include "rendering/render/layer_stack_renderer.hpp"
#include "rendering/render/layer_submission_builder.hpp"
#include "rendering/render/sink_clip.hpp"
#include "rendering/render/render_object.hpp"
#include "rendering/render/render_object_builder.hpp"
#include "rendering/render/scene_composite_pass.hpp"
#include "rendering/render/gpu_scene_renderer.hpp"
#include "rendering/render/render_texture_utils.hpp"
#include "rendering/render/debug_overlay_renderer.hpp"
#include "rendering/render/render_diagnostics.hpp"
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
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <cstdlib>

namespace {
constexpr double kDepthBucketSize = 0.0625;
constexpr double kDepthBucketScale = 1.0 / kDepthBucketSize;
constexpr float kQuadEpsilon = 1.0e-5f;

std::vector<std::filesystem::path> runtime_shader_manifest_candidates(const char* override_manifest_path) {
    std::vector<std::filesystem::path> candidates;
    auto push_unique = [&](const std::filesystem::path& path) {
        if (path.empty()) {
            return;
        }
        const auto it = std::find(candidates.begin(), candidates.end(), path);
        if (it == candidates.end()) {
            candidates.push_back(path);
        }
    };

    if (override_manifest_path && *override_manifest_path) {
        push_unique(std::filesystem::path(override_manifest_path));
        return candidates;
    }

    push_unique(std::filesystem::path("ENGINE/runtime/rendering/shaders/runtime_shaders.json"));
    push_unique(std::filesystem::path("build/runtime_shaders/runtime_shaders.json"));
    push_unique(std::filesystem::path("build_codex/runtime_shaders/runtime_shaders.json"));
    push_unique(std::filesystem::path("build-vs/runtime_shaders/runtime_shaders.json"));
    push_unique(std::filesystem::path("build_vs_test/runtime_shaders/runtime_shaders.json"));

    if (const char* base_path_raw = SDL_GetBasePath()) {
        const std::filesystem::path base_path = std::filesystem::path(base_path_raw);
        push_unique(base_path / "runtime_shaders" / "runtime_shaders.json");
        push_unique(base_path.parent_path() / "runtime_shaders" / "runtime_shaders.json");
        push_unique(base_path.parent_path() / "build" / "runtime_shaders" / "runtime_shaders.json");
        push_unique(base_path.parent_path() / "build_codex" / "runtime_shaders" / "runtime_shaders.json");
    }

    return candidates;
}

std::filesystem::path runtime_project_root_path() {
#ifdef PROJECT_ROOT
    return std::filesystem::path(PROJECT_ROOT);
#else
    return std::filesystem::current_path();
#endif
}

std::optional<float> project_depth_guide_screen_y(const WarpedScreenGrid& cam,
                                                  float depth_distance,
                                                  int screen_height) {
    if (!(screen_height > 0) || !std::isfinite(depth_distance)) {
        return std::nullopt;
    }

    const world::CameraProjectionParams projection = cam.projection_params();
    const float depth_axis_sign =
        render_depth::normalize_depth_axis_sign(static_cast<float>(projection.forward_z));
    const float world_z = render_depth::world_z_from_depth_offset(
        std::max(0.0f, depth_distance),
        static_cast<float>(projection.anchor_world_z),
        depth_axis_sign);

    const SDL_FPoint center = cam.get_view_center_f();
    SDL_FPoint screen{};
    if (!cam.project_world_point(SDL_FPoint{center.x, 0.0f}, world_z, screen)) {
        return std::nullopt;
    }

    const float y = cam.warp_floor_screen_y(0.0f, screen.y);
    if (!std::isfinite(y) || y < 0.0f || y >= static_cast<float>(screen_height)) {
        return std::nullopt;
    }
    return y;
}

inline std::int64_t quantize_depth(double depth) {
    const double scaled = std::floor(depth * kDepthBucketScale);
    const double min_value = static_cast<double>(std::numeric_limits<std::int64_t>::lowest());
    const double max_value = static_cast<double>(std::numeric_limits<std::int64_t>::max());
    return static_cast<std::int64_t>(std::clamp(scaled, min_value, max_value));
}

void destroy_texture(SDL_Texture*& texture) {
    render_diagnostics::destroy_texture(texture);
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

SDL_FPoint sanitize_anchor_uv_for_sink_clip(SDL_FPoint uv) {
    if (!std::isfinite(uv.x) || !std::isfinite(uv.y)) {
        return SDL_FPoint{0.5f, 1.0f};
    }
    return SDL_FPoint{
        std::clamp(uv.x, 0.0f, 1.0f),
        std::clamp(uv.y, 0.0f, 1.0f)};
}

enum class SinkClipSubmitResult {
    Submitted,
    FullyClipped,
    Failed
};

SinkClipSubmitResult submit_sink_clipped_geometry(const RenderObject& obj,
                                                  const WarpedMesh& mesh,
                                                  double depth_from_focus_plane,
                                                  GeometryBatcher& batcher) {
    if (!obj.sink_clip_enabled) {
        return SinkClipSubmitResult::Failed;
    }
    if (!std::isfinite(obj.sink_height_offset_px)) {
        return SinkClipSubmitResult::Failed;
    }

    const SDL_FPoint anchor_uv = sanitize_anchor_uv_for_sink_clip(obj.projection_anchor_uv);
    const int frame_height_px = obj.has_src_rect
        ? std::max(1, obj.src_rect.h)
        : std::max(1, obj.texture_h);
    const float sink_line_uv_y = anchor_uv.y + (obj.sink_height_offset_px / static_cast<float>(frame_height_px));
    if (!std::isfinite(sink_line_uv_y)) {
        return SinkClipSubmitResult::Failed;
    }

    render_projection::ProjectedSpriteFrame projected{};
    projected.screen_tl = mesh.vertices[0].position;
    projected.screen_tr = mesh.vertices[1].position;
    projected.screen_br = mesh.vertices[2].position;
    projected.screen_bl = mesh.vertices[3].position;
    const SDL_FPoint sink_uv{anchor_uv.x, sink_line_uv_y};
    const SDL_FPoint sink_screen = projected.sample_screen_from_uv(sink_uv);
    if (!std::isfinite(sink_screen.x) || !std::isfinite(sink_screen.y)) {
        return SinkClipSubmitResult::Failed;
    }

    const float sink_line_y = sink_screen.y;
    if (!std::isfinite(sink_line_y)) {
        return SinkClipSubmitResult::Failed;
    }

    const SDL_Vertex quad_vertices[4]{
        mesh.vertices[0],
        mesh.vertices[1],
        mesh.vertices[2],
        mesh.vertices[3]};
    const render_sink::ClipResult clip =
        render_sink::clip_quad_against_horizontal_sink_line(quad_vertices, sink_line_y, 1.0e-3f);

    if (clip.fully_clipped) {
        return SinkClipSubmitResult::FullyClipped;
    }
    if (!clip.valid) {
        return SinkClipSubmitResult::Failed;
    }
    if (!clip.clipped) {
        batcher.addQuad(obj.texture, mesh.vertices.data(), mesh.indices.data(), obj.blend_mode, depth_from_focus_plane);
        return SinkClipSubmitResult::Submitted;
    }

    batcher.addGeometry(obj.texture,
                        clip.vertices.data(),
                        clip.vertex_count,
                        clip.indices.data(),
                        clip.index_count,
                        obj.blend_mode,
                        depth_from_focus_plane);
    return SinkClipSubmitResult::Submitted;
}

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

void clear_backbuffer_to_color(SDL_Renderer* renderer,
                               int screen_width,
                               int screen_height,
                               SDL_Color color) {
    if (!renderer) {
        return;
    }
    SDL_SetRenderTarget(renderer, nullptr);
    SDL_SetRenderViewport(renderer, nullptr);
    SDL_SetRenderClipRect(renderer, nullptr);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderClear(renderer);
    (void)screen_width;
    (void)screen_height;
}

void draw_gpu_failure_overlay(SDL_Renderer* renderer,
                              int screen_width,
                              int screen_height) {
    if (!renderer || screen_width <= 0 || screen_height <= 0) {
        return;
    }

    clear_backbuffer_to_color(renderer, screen_width, screen_height, SDL_Color{8, 8, 12, 255});

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 190, 20, 20, 215);
    SDL_Rect top_bar{0, 0, screen_width, std::max(16, screen_height / 7)};
    sdl_render::FillRect(renderer, &top_bar);

    SDL_SetRenderDrawColor(renderer, 255, 96, 96, 255);
    const int margin = std::max(24, std::min(screen_width, screen_height) / 10);
    SDL_RenderLine(renderer, margin, margin, screen_width - margin, screen_height - margin);
    SDL_RenderLine(renderer, screen_width - margin, margin, margin, screen_height - margin);
}

} // namespace

namespace render_internal {

bool clear_gameplay_target_to_color(SDL_Renderer* renderer,
                                    SDL_Texture* gameplay_target,
                                    SDL_Color clear_color) {
    if (!renderer || !gameplay_target) {
        return false;
    }
    if (!render_diagnostics::set_render_target(renderer, gameplay_target)) {
        return false;
    }
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

DepthInterval make_sorted_depth_interval(double depth_min, double depth_max) {
    if (!std::isfinite(depth_min) || !std::isfinite(depth_max)) {
        return {};
    }
    if (depth_min <= depth_max) {
        return DepthInterval{depth_min, depth_max};
    }
    return DepthInterval{depth_max, depth_min};
}

DepthInterval light_depth_interval(const LayerEffectProcessor::RuntimeLight& light) {
    if (!std::isfinite(light.world_z)) {
        return {};
    }
    const bool has_world_radius = std::isfinite(light.radius_world) && light.radius_world > 0.0f;
    const bool has_screen_radius = std::isfinite(light.radius_px) && light.radius_px > 0.0f;
    if (!has_world_radius && !has_screen_radius) {
        return {};
    }
    const double depth_radius = static_cast<double>(has_world_radius ? light.radius_world : light.radius_px);
    return make_sorted_depth_interval(static_cast<double>(light.world_z) - depth_radius,
                                      static_cast<double>(light.world_z) + depth_radius);
}

int compare_depth_intervals_signed(const DepthInterval& light_interval, const DepthInterval& layer_interval) {
    if (!std::isfinite(light_interval.min) || !std::isfinite(light_interval.max) ||
        !std::isfinite(layer_interval.min) || !std::isfinite(layer_interval.max) ||
        light_interval.min > light_interval.max || layer_interval.min > layer_interval.max) {
        return 0;
    }
    if (light_interval.max < layer_interval.min) {
        return -1;
    }
    if (light_interval.min > layer_interval.max) {
        return 1;
    }
    return 0;
}

float apply_layer_light_strength_bias(float base_intensity,
                                      int signed_depth_separation,
                                      float front_layer_multiplier,
                                      float behind_layer_multiplier) {
    const float base = std::max(0.0f, base_intensity);
    if (signed_depth_separation < 0) {
        return base * std::max(0.0f, front_layer_multiplier);
    }
    if (signed_depth_separation > 0) {
        return base * std::max(0.0f, behind_layer_multiplier);
    }
    return base;
}

bool screen_aabb_overlaps(const ScreenAabb& lhs, const ScreenAabb& rhs) {
    if (!std::isfinite(lhs.min_x) || !std::isfinite(lhs.min_y) ||
        !std::isfinite(lhs.max_x) || !std::isfinite(lhs.max_y) ||
        !std::isfinite(rhs.min_x) || !std::isfinite(rhs.min_y) ||
        !std::isfinite(rhs.max_x) || !std::isfinite(rhs.max_y) ||
        lhs.min_x > lhs.max_x || lhs.min_y > lhs.max_y ||
        rhs.min_x > rhs.max_x || rhs.min_y > rhs.max_y) {
        return false;
    }

    return lhs.max_x >= rhs.min_x &&
           lhs.min_x <= rhs.max_x &&
           lhs.max_y >= rhs.min_y &&
           lhs.min_y <= rhs.max_y;
}

bool light_overlaps_layer_slice(const LayerEffectProcessor::RuntimeLight& light,
                                const DepthInterval& light_interval,
                                const DepthInterval& layer_interval,
                                const ScreenAabb& light_bounds,
                                const ScreenAabb& layer_bounds) {
    if (!std::isfinite(light.world_z)) {
        return false;
    }

    if (compare_depth_intervals_signed(light_interval, layer_interval) != 0) {
        return false;
    }
    if (!std::isfinite(light.screen_center.x) || !std::isfinite(light.screen_center.y)) {
        return false;
    }

    const bool center_inside = light.screen_center.x >= layer_bounds.min_x &&
                               light.screen_center.x <= layer_bounds.max_x &&
                               light.screen_center.y >= layer_bounds.min_y &&
                               light.screen_center.y <= layer_bounds.max_y;
    if (center_inside) {
        return true;
    }
    if (!(std::isfinite(light.radius_px) && light.radius_px > 0.0f)) {
        return false;
    }
    return screen_aabb_overlaps(light_bounds, layer_bounds);
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

float dof_quality_scale(int screen_width,
                        int screen_height,
                        float blur_px,
                        float radial_blur_px) {
    const float safe_blur_px = std::max(0.0f, std::isfinite(blur_px) ? blur_px : 0.0f);
    const float safe_radial_blur_px = std::max(0.0f, std::isfinite(radial_blur_px) ? radial_blur_px : 0.0f);
    const float max_radius = std::max(safe_blur_px, safe_radial_blur_px);
    const int min_dim = std::max(1, std::min(screen_width, screen_height));
    if (max_radius <= 2.0f || min_dim <= 540) {
        return 1.0f;
    }
    struct RadiusThreshold {
        float radius;
        float quality;
    };
    static constexpr std::array<RadiusThreshold, 3> kThresholds{{
        {6.0f, 0.92f},
        {12.0f, 0.84f},
        {24.0f, 0.72f},
    }};
    for (const RadiusThreshold& threshold : kThresholds) {
        if (max_radius <= threshold.radius) {
            return threshold.quality;
        }
    }
    return (min_dim <= 720) ? 0.62f : 0.54f;
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
    addGeometry(texture, vertices, 4, indices, 6, blend_mode, depth);
}

void GeometryBatcher::addGeometry(SDL_Texture* texture,
                                  const SDL_Vertex* vertices,
                                  int vertex_count,
                                  const int* indices,
                                  int index_count,
                                  SDL_BlendMode blend_mode,
                                  double depth) {
    if (!texture ||
        !vertices ||
        !indices ||
        vertex_count < 3 ||
        vertex_count > kMaxVerticesPerItem ||
        index_count < 3 ||
        index_count > kMaxIndicesPerItem ||
        (index_count % 3) != 0) {
        return;
    }
    for (int i = 0; i < index_count; ++i) {
        if (indices[i] < 0 || indices[i] >= vertex_count) {
            return;
        }
    }

    DrawItem item{};
    item.texture = texture;
    item.blend_mode = blend_mode;
    item.vertex_count = vertex_count;
    item.index_count = index_count;
    for (int i = 0; i < vertex_count; ++i) {
        item.vertices[i] = vertices[i];
    }
    for (int i = 0; i < index_count; ++i) {
        item.indices[i] = indices[i];
    }
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
    vertex_buffer_.clear();
    index_buffer_.clear();

    auto emit_current_batch = [&](SDL_Texture* current_texture, SDL_BlendMode current_blend) {
        if (!current_texture || vertex_buffer_.empty() || index_buffer_.empty()) {
            return;
        }
        SDL_SetTextureBlendMode(current_texture, current_blend);
        render_diagnostics::render_geometry(renderer_,
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
        if (item.vertex_count < 3 || item.index_count < 3) {
            return;
        }
        const int base = static_cast<int>(vertex_buffer_.size());
        for (int vi = 0; vi < item.vertex_count; ++vi) {
            vertex_buffer_.push_back(item.vertices[vi]);
        }
        for (int ii = 0; ii < item.index_count; ++ii) {
            index_buffer_.push_back(base + item.indices[ii]);
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

std::size_t GeometryBatcher::item_count() const {
    std::size_t count = invalid_depth_bucket_.items.size();
    for (const auto& entry : depth_buckets_) {
        count += entry.second.items.size();
    }
    return count;
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
        const std::string message = reason.empty() ? "חסרים תנאי קדם עבור SceneRenderer." : reason;
        vibble::log::error(std::string{"[SceneRenderer] האתחול הופסק: "} + message);
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
        vibble::log::warn("[SceneRenderer] אתחול מערכת הגבולות הדינמיים נכשל");
    }

    map_radius_world_ = map_layers::map_radius_from_map_info(map_manifest);
    if (!std::isfinite(map_radius_world_) || map_radius_world_ <= 0.0) {
        map_radius_world_ = static_cast<double>(std::max(screen_width_, screen_height_));
    }

    if (floor_composer_) floor_composer_->set_output_dimensions(screen_width_, screen_height_);
    if (blur_chain_renderer_) blur_chain_renderer_->set_output_dimensions(screen_width_, screen_height_);
    if (layer_stack_renderer_) layer_stack_renderer_->set_output_dimensions(screen_width_, screen_height_);

    std::string gpu_error;
    gpu_scene_renderer_ = GpuSceneRenderer::Create(renderer_, false, gpu_error);
    if (!gpu_scene_renderer_) {
        throw std::runtime_error("[SceneRenderer] GPU runtime renderer initialization failed: " + gpu_error);
    }

    const char* shader_manifest_env = std::getenv("VIBBLE_GPU_SHADER_MANIFEST");
    const std::vector<std::filesystem::path> shader_manifest_candidates =
        runtime_shader_manifest_candidates(shader_manifest_env);
    std::string last_load_error;
    std::filesystem::path selected_manifest;
    bool loaded_manifest = false;
    for (const std::filesystem::path& candidate : shader_manifest_candidates) {
        if (!shader_manifest_env && !std::filesystem::exists(candidate)) {
            continue;
        }
        if (gpu_scene_renderer_->load_shader_packages(candidate.string(), gpu_error)) {
            selected_manifest = candidate;
            loaded_manifest = true;
            break;
        }
        last_load_error = gpu_error;
    }

    if (!loaded_manifest) {
        std::ostringstream oss;
        oss << "[SceneRenderer] GPU shader package load failed";
        if (!last_load_error.empty()) {
            oss << ": " << last_load_error;
        }
        if (!shader_manifest_candidates.empty()) {
            oss << " (candidates:";
            for (const std::filesystem::path& candidate : shader_manifest_candidates) {
                oss << " " << candidate.string();
            }
            oss << ")";
        }
        throw std::runtime_error(oss.str());
    }

    vibble::log::info("[SceneRenderer] Runtime gameplay renderer mode: gpu-only manifest=" +
                      selected_manifest.string());

    gpu_runtime_path_enabled_ = true;
    vibble::log::info("[SceneRenderer] GPU runtime renderer active.");

    vibble::log::debug(std::string{"[SceneRenderer] מתחיל אתחול עבור מפה '"} + map_id +
                       "' עם מסך " + std::to_string(screen_width_) + "x" + std::to_string(screen_height_) + ".");
}

SceneRenderer::~SceneRenderer() {
    destroy_texture(scene_composite_tex_);
    destroy_texture(scene_preblur_tex_);
    destroy_far_backdrop_resources();
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
    scene_composite_tex_ = render_diagnostics::create_texture(renderer_,
                                                              SDL_PIXELFORMAT_RGBA8888,
                                                              SDL_TEXTUREACCESS_TARGET,
                                                              screen_width_,
                                                              screen_height_);
    if (scene_composite_tex_) {
        SDL_SetTextureBlendMode(scene_composite_tex_, SDL_BLENDMODE_BLEND);
    }
    return scene_composite_tex_ != nullptr;
}

bool SceneRenderer::ensure_far_backdrop_composite_target() {
    if (!renderer_ || screen_width_ <= 0 || screen_height_ <= 0) {
        return false;
    }
    if (scene_preblur_tex_) {
        float w = 0.0f;
        float h = 0.0f;
        if (SDL_GetTextureSize(scene_preblur_tex_, &w, &h) &&
            static_cast<int>(std::lround(w)) == screen_width_ &&
            static_cast<int>(std::lround(h)) == screen_height_) {
            return true;
        }
        destroy_texture(scene_preblur_tex_);
    }
    scene_preblur_tex_ = render_diagnostics::create_texture(renderer_,
                                                            SDL_PIXELFORMAT_RGBA8888,
                                                            SDL_TEXTUREACCESS_TARGET,
                                                            screen_width_,
                                                            screen_height_);
    if (scene_preblur_tex_) {
        SDL_SetTextureBlendMode(scene_preblur_tex_, SDL_BLENDMODE_BLEND);
    }
    return scene_preblur_tex_ != nullptr;
}

std::filesystem::path SceneRenderer::resolve_misc_content_path(const std::string& filename) const {
    const std::filesystem::path project_root = runtime_project_root_path();
    std::vector<std::filesystem::path> roots;
    roots.push_back(project_root);
    roots.push_back(std::filesystem::current_path());
    if (const char* base_path_raw = SDL_GetBasePath()) {
        const std::filesystem::path base = std::filesystem::path(base_path_raw);
        roots.push_back(base);
        roots.push_back(base.parent_path());
    }

    for (const std::filesystem::path& root : roots) {
        if (root.empty()) {
            continue;
        }
        const std::filesystem::path candidate = (root / "resources" / "misc_content" / filename).lexically_normal();
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

bool SceneRenderer::ensure_far_backdrop_resources() {
    if (!renderer_) {
        return false;
    }

    if (!far_backdrop_sky_tex_) {
        const std::filesystem::path sky_path = resolve_misc_content_path("sky.png");
        if (!sky_path.empty()) {
            far_backdrop_sky_tex_ = IMG_LoadTexture(renderer_, sky_path.string().c_str());
        }
    }
    if (!far_backdrop_mountains_tex_) {
        const std::filesystem::path mountains_path = resolve_misc_content_path("mountains.png");
        if (!mountains_path.empty()) {
            far_backdrop_mountains_tex_ = IMG_LoadTexture(renderer_, mountains_path.string().c_str());
        }
    }

    return far_backdrop_sky_tex_ != nullptr && far_backdrop_mountains_tex_ != nullptr;
}

void SceneRenderer::destroy_far_backdrop_resources() {
    destroy_texture(far_backdrop_sky_tex_);
    destroy_texture(far_backdrop_mountains_tex_);
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
    destroy_texture(scene_preblur_tex_);
    if (floor_composer_) floor_composer_->set_output_dimensions(screen_width_, screen_height_);
    if (blur_chain_renderer_) blur_chain_renderer_->set_output_dimensions(screen_width_, screen_height_);
    if (layer_stack_renderer_) layer_stack_renderer_->set_output_dimensions(screen_width_, screen_height_);
}

bool SceneRenderer::execute_gpu_frame_graph(SDL_Texture* scene_texture, std::string& out_error) {
    out_error.clear();
    if (!gpu_frame_graph_interop_supported_) {
        return true;
    }
    if (!gpu_scene_renderer_) {
        out_error = "GpuSceneRenderer is not initialized.";
        return false;
    }
    if (!scene_texture) {
        out_error = "Scene composite texture is null.";
        return false;
    }

    const SDL_PropertiesID texture_props = SDL_GetTextureProperties(scene_texture);
    if (!texture_props) {
        out_error = "Failed to query scene composite texture properties: " + std::string(SDL_GetError());
        return false;
    }
    SDL_GPUTexture* scene_gpu_texture = static_cast<SDL_GPUTexture*>(
        SDL_GetPointerProperty(texture_props, SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, nullptr));
    if (!scene_gpu_texture) {
        gpu_frame_graph_interop_supported_ = false;
        if (!gpu_frame_graph_interop_warning_logged_) {
            vibble::log::warn(
                "[SceneRenderer] Frame-graph interop disabled: scene composite texture is not backed by "
                "SDL_GPUTexture on this backend. Continuing with GPU SDL renderer presentation.");
            gpu_frame_graph_interop_warning_logged_ = true;
        }
        return true;
    }

    float scene_width_f = 0.0f;
    float scene_height_f = 0.0f;
    if (!SDL_GetTextureSize(scene_texture, &scene_width_f, &scene_height_f) ||
        scene_width_f <= 0.0f ||
        scene_height_f <= 0.0f) {
        out_error = "Failed to query scene composite texture dimensions.";
        return false;
    }
    const std::uint32_t scene_width = static_cast<std::uint32_t>(std::max(1, static_cast<int>(std::lround(scene_width_f))));
    const std::uint32_t scene_height = static_cast<std::uint32_t>(std::max(1, static_cast<int>(std::lround(scene_height_f))));

    GpuSceneRenderer::TextureResourceSpec scratch_copy_spec{};
    scratch_copy_spec.width = scene_width;
    scratch_copy_spec.height = scene_height;
    scratch_copy_spec.format = gpu_scene_renderer_->device()
        ? gpu_scene_renderer_->device()->format_policy().albedo_format
        : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    scratch_copy_spec.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    scratch_copy_spec.layer_count_or_depth = 1;
    scratch_copy_spec.num_levels = 1;
    scratch_copy_spec.sample_count = SDL_GPU_SAMPLECOUNT_1;

    std::string frame_error;
    if (!gpu_scene_renderer_->ensure_texture_resource("scene.frame_graph_copy", scratch_copy_spec, frame_error)) {
        out_error = frame_error.empty()
            ? "Failed to allocate frame-graph copy target."
            : frame_error;
        return false;
    }

    if (!gpu_scene_renderer_->begin_frame(&frame_error)) {
        out_error = frame_error.empty() ? "GpuSceneRenderer::begin_frame failed." : frame_error;
        return false;
    }

    gpu_scene_renderer_->clear_external_texture_resources();
    if (!gpu_scene_renderer_->register_external_texture_resource("scene.composite", scene_gpu_texture)) {
        gpu_scene_renderer_->abort_frame();
        out_error = "Failed to register scene composite texture for frame-graph presentation.";
        return false;
    }

    GpuFrameGraph::PassDescriptor present_pass{};
    present_pass.type = GpuFrameGraph::PassType::Copy;
    present_pass.name = "copy_scene_composite";
    present_pass.resources = {
        GpuFrameGraph::ResourceDependency{"scene.composite", false},
        GpuFrameGraph::ResourceDependency{"scene.frame_graph_copy", true}
    };
    present_pass.blit.source_texture = "scene.composite";
    present_pass.blit.destination_texture = "scene.frame_graph_copy";
    present_pass.blit.use_swapchain_destination = false;
    present_pass.blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    present_pass.blit.filter = SDL_GPU_FILTER_LINEAR;
    present_pass.blit.width = scene_width;
    present_pass.blit.height = scene_height;
    gpu_scene_renderer_->add_pass(std::move(present_pass));

    if (!gpu_scene_renderer_->end_frame(&frame_error)) {
        gpu_scene_renderer_->clear_external_texture_resources();
        out_error = frame_error.empty() ? "GpuSceneRenderer::end_frame failed." : frame_error;
        return false;
    }

    gpu_scene_renderer_->clear_external_texture_resources();
    return true;
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

        SDL_FPoint base_screen = sprite.screen_pos;
        if (!std::isfinite(base_screen.x) || !std::isfinite(base_screen.y)) {
            if (!project_world_point(cam,
                                     sprite.world_pos.x,
                                     sprite.world_pos.y,
                                     static_cast<float>(sprite.world_z),
                                     base_screen)) {
                return;
            }
        }

        const float half_width = sprite.world_width * 0.5f;
        const float adjusted_y = base_screen.y + boundary_vertical_offset + sprite.spawn_y_offset_px;
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
        if (std::isfinite(sprite.spawn_tilt_degrees) && std::fabs(sprite.spawn_tilt_degrees) > 1e-4f) {
            const float radians = sprite.spawn_tilt_degrees * (3.14159265358979323846f / 180.0f);
            const float cos_v = std::cos(radians);
            const float sin_v = std::sin(radians);
            const SDL_FPoint pivot{base_screen.x, adjusted_y};
            for (SDL_Vertex& vertex : vertices) {
                const float local_x = vertex.position.x - pivot.x;
                const float local_y = vertex.position.y - pivot.y;
                vertex.position.x = pivot.x + local_x * cos_v - local_y * sin_v;
                vertex.position.y = pivot.y + local_x * sin_v + local_y * cos_v;
            }
        }
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
    std::unordered_set<const Asset*> touched_assets;
    touched_assets.reserve(assets_->active_traversal().size());

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
            touched_assets.insert(asset);

            rendered_assets_for_debug.push_back(asset);

            // Tileable assets with valid tiling coverage are drawn by the tile system.
            // Skip direct sprite rendering to avoid drawing the same asset twice.
            if (const auto& tiling = asset->tiling_info(); tiling && tiling->is_valid()) {
                continue;
            }

            auto& cache_entry = asset_render_cache_[asset];
            SDL_Texture* current_texture_identity = asset->get_current_variant_texture();
            if (!current_texture_identity) {
                current_texture_identity = asset->get_current_frame();
            }
            const bool needs_static_refresh =
                !cache_entry.initialized ||
                cache_entry.frame_identity != (asset->current_frame ? asset->current_frame->frame_index : -1) ||
                cache_entry.variant_identity != asset->current_variant_index ||
                cache_entry.texture_identity != current_texture_identity;
            if (needs_static_refresh) {
                if (!render_build::refresh_direct_asset_render_cache(asset, cache_entry.static_record)) {
                    cache_entry = AssetRenderCacheEntry{};
                    continue;
                }
                cache_entry.frame_identity = cache_entry.static_record.frame_identity;
                cache_entry.variant_identity = cache_entry.static_record.variant_identity;
                cache_entry.texture_identity = cache_entry.static_record.texture_identity;
                cache_entry.object.mesh_dirty = true;
                cache_entry.initialized = true;
            }

            if (!render_build::build_direct_asset_render_object(asset, cache_entry.static_record, cache_entry.object)) {
                continue;
            }
            RenderObject& obj = cache_entry.object;
            const Uint32 reprojection_identity = render_build::direct_asset_reprojection_identity(asset);
            if (cache_entry.reprojection_identity != reprojection_identity) {
                obj.mesh_dirty = true;
            }
            cache_entry.reprojection_identity = reprojection_identity;
            if (asset->is_mesh_dirty()) {
                obj.mesh_dirty = true;
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
        asset->clear_mesh_dirty();

        // חשוב:
        // הקרן את הקווד באמצעות היסט Z של הרינדור כדי שיופיע במקום הנכון,
        // אבל השאר את חלוקת DOF/שכבות קשורה לעומק מישור הפוקוס של הנכס, ולא
        // להיסט Z של עוגן הרינדור של הספרייט. אחרת אובייקט השחקן או הפוקוס עלול
        // להידחף לשכבה מטושטשת סמוכה גם כאשר מישור הפוקוס של המצלמה תקין.
        const SinkClipSubmitResult sink_submit =
            submit_sink_clipped_geometry(obj, mesh, asset_depth_from_focus_plane, *geometry_batcher_);
        if (sink_submit == SinkClipSubmitResult::Failed) {
            geometry_batcher_->addQuad(obj.texture,
                                       mesh.vertices.data(),
                                       mesh.indices.data(),
                                       obj.blend_mode,
                                       asset_depth_from_focus_plane);
        }
            continue;
        }

        if (boundary_index < boundary_sprites.size()) {
            queue_boundary_sprite(boundary_sprites[boundary_index++], boundary_depth);
        }
    }

    for (auto it = asset_render_cache_.begin(); it != asset_render_cache_.end();) {
        if (!it->first || touched_assets.find(it->first) == touched_assets.end()) {
            it = asset_render_cache_.erase(it);
        } else {
            ++it;
        }
    }
}


void SceneRenderer::gather_runtime_lights(const WarpedScreenGrid& cam,
                                          double focus_plane_world_z,
                                          const std::vector<Asset*>& rendered_assets,
                                          std::vector<LayerEffectProcessor::RuntimeLight>& out_lights) {
    render_diagnostics::ScopedCpuTimer gather_timer(render_diagnostics::CpuTimerMetric::LightGather);
    (void)rendered_assets;
    out_lights.clear();
    runtime_light_debug_overlay_.clear();
    runtime_light_rendered_count_ = 0;
    runtime_light_culled_count_ = 0;
    if (!assets_) {
        return;
    }

    const WarpedScreenGrid::RealismSettings realism = cam.get_settings();
    const bool fade_smoothing_enabled = realism.light_fade_smoothing_enabled;
    const float min_fade_seconds = std::max(0.0f, realism.light_min_fade_seconds);
    const float fade_in_seconds = std::max(min_fade_seconds, std::max(0.0f, realism.light_fade_in_seconds));
    const float fade_out_seconds = std::max(min_fade_seconds, std::max(0.0f, realism.light_fade_out_seconds));
    const float light_max_cull_depth = std::max(1.0f, realism.light_max_cull_depth);
    const float distance_fade_start_ratio = std::clamp(realism.light_distance_fade_start_ratio, 0.0f, 0.999f);
    const float distance_fade_start_depth = light_max_cull_depth * distance_fade_start_ratio;
    const float distance_fade_span = std::max(32.0f, light_max_cull_depth - distance_fade_start_depth);
    const float dt_seconds = std::clamp(assets_->frame_delta_seconds(), 0.0f, 0.25f);
    const std::uint64_t frame_token = static_cast<std::uint64_t>(assets_->frame_id());

    constexpr float kCullingMargin = 128.0f;
    update_runtime_light_registry_incremental(frame_token);
    std::vector<std::uint32_t> candidate_light_ids;
    runtime_light_query_visible_cells(cam,
                                      static_cast<float>(focus_plane_world_z),
                                      kCullingMargin,
                                      candidate_light_ids);
    out_lights.reserve(candidate_light_ids.size());

    std::unordered_set<std::uint32_t> seen_light_ids;
    seen_light_ids.reserve(candidate_light_ids.size());
    const std::uint64_t gather_start_ticks = SDL_GetTicks();

    std::vector<RuntimeLightBroadphaseEntry> broadphase_entries;
    broadphase_entries.reserve(candidate_light_ids.size());
    for (const std::uint32_t light_id : candidate_light_ids) {
        if (light_id == 0 || light_id >= runtime_light_registry_entries_.size() || !seen_light_ids.insert(light_id).second) {
            continue;
        }
        RuntimeLightRegistryEntry& registry_entry = runtime_light_registry_entries_[light_id];
        if (!registry_entry.valid || !registry_entry.asset || registry_entry.asset->dead || !registry_entry.asset->current_frame) {
            continue;
        }
        if (!assets_->is_asset_in_focus_filter(registry_entry.asset)) {
            continue;
        }

        SDL_FPoint broadphase_screen{};
        if (!cam.project_world_point(SDL_FPoint{registry_entry.anchor_world_x, registry_entry.anchor_world_y},
                                     registry_entry.anchor_world_z,
                                     broadphase_screen) ||
            !std::isfinite(broadphase_screen.x) ||
            !std::isfinite(broadphase_screen.y)) {
            continue;
        }
        float broadphase_scale = 1.0f;
        sample_world_distance_scale(cam,
                                    registry_entry.anchor_world_x,
                                    registry_entry.anchor_world_y,
                                    registry_entry.anchor_world_z,
                                    broadphase_scale);
        const float broadphase_height_spread = render_internal::floor_light_height_spread_scale(
            registry_entry.anchor_world_y,
            registry_entry.radius_world);
        const float broadphase_radius_px = std::max(
            4.0f,
            registry_entry.radius_world * std::max(0.05f, broadphase_scale) * std::max(1.0f, broadphase_height_spread));
        const bool broadphase_overlap = broadphase_screen.x + broadphase_radius_px >= -kCullingMargin &&
                                        broadphase_screen.x - broadphase_radius_px <= static_cast<float>(screen_width_) + kCullingMargin &&
                                        broadphase_screen.y + broadphase_radius_px >= -kCullingMargin &&
                                        broadphase_screen.y - broadphase_radius_px <= static_cast<float>(screen_height_) + kCullingMargin;
        if (!broadphase_overlap) {
            ++runtime_light_culled_count_;
            continue;
        }
        broadphase_entries.push_back(RuntimeLightBroadphaseEntry{light_id, broadphase_screen, broadphase_radius_px});
    }
#ifndef NDEBUG
    runtime_light_debug_parity_visible_count_ = 0;
    for (std::size_t light_id = 1; light_id < runtime_light_registry_entries_.size(); ++light_id) {
        const RuntimeLightRegistryEntry& entry = runtime_light_registry_entries_[light_id];
        if (!entry.valid || !entry.asset || entry.asset->dead || !entry.asset->current_frame) {
            continue;
        }
        if (!assets_->is_asset_in_focus_filter(entry.asset)) {
            continue;
        }
        SDL_FPoint debug_screen{};
        if (!cam.project_world_point(SDL_FPoint{entry.anchor_world_x, entry.anchor_world_y}, entry.anchor_world_z, debug_screen)) {
            continue;
        }
        float debug_scale = 1.0f;
        sample_world_distance_scale(cam, entry.anchor_world_x, entry.anchor_world_y, entry.anchor_world_z, debug_scale);
        const float debug_radius_px = std::max(4.0f, entry.radius_world * std::max(0.05f, debug_scale));
        const bool debug_overlap = debug_screen.x + debug_radius_px >= -kCullingMargin &&
                                   debug_screen.x - debug_radius_px <= static_cast<float>(screen_width_) + kCullingMargin &&
                                   debug_screen.y + debug_radius_px >= -kCullingMargin &&
                                   debug_screen.y - debug_radius_px <= static_cast<float>(screen_height_) + kCullingMargin;
        if (debug_overlap) {
            ++runtime_light_debug_parity_visible_count_;
        }
    }
    SDL_assert(runtime_light_debug_parity_visible_count_ >= static_cast<int>(broadphase_entries.size()));
#endif

    for (const RuntimeLightBroadphaseEntry& broadphase : broadphase_entries) {
        RuntimeLightRegistryEntry& registry_entry = runtime_light_registry_entries_[broadphase.light_id];
        Asset* asset = registry_entry.asset;
        if (!asset) {
            continue;
        }
        std::optional<AnchorPoint> resolved = asset->anchor_state(registry_entry.anchor_name,
                                                                  anchor_points::GridMaterialization::None,
                                                                  Asset::AnchorResolveMode::Cached);
        if (!resolved.has_value() || !resolved->exists) {
            resolved = asset->anchor_state(registry_entry.anchor_name,
                                           anchor_points::GridMaterialization::None,
                                           Asset::AnchorResolveMode::ForceRecompute);
        }
        if (!resolved.has_value() || !resolved->exists) {
            ++runtime_light_culled_count_;
            continue;
        }

        SDL_FPoint screen = resolved->screen_pos_2d;
        if ((!std::isfinite(screen.x) || !std::isfinite(screen.y)) &&
            (!cam.project_world_point(SDL_FPoint{resolved->world_exact_pos_2d.x, resolved->world_exact_pos_2d.y},
                                      resolved->world_exact_z,
                                      screen) ||
             !std::isfinite(screen.x) || !std::isfinite(screen.y))) {
            ++runtime_light_culled_count_;
            continue;
        }

        const float fallback_scale = resolved->has_flat_perspective_scale
            ? std::max(0.05f, resolved->flat_perspective_scale)
            : 1.0f;
        float sampled_scale = fallback_scale;
        float world_sampled_scale = fallback_scale;
        if (sample_world_distance_scale(cam,
                                        resolved->world_exact_pos_2d.x,
                                        resolved->world_exact_pos_2d.y,
                                        resolved->world_exact_z,
                                        world_sampled_scale)) {
            sampled_scale = std::max(0.05f, world_sampled_scale);
        }

        const AnchorLightData& light = registry_entry.light;
        const float radius_world = registry_entry.radius_world;
        const float radius_px = std::max(4.0f, radius_world * sampled_scale);
        const float floor_spread_radius_px = radius_px * std::max(
            1.0f,
            render_internal::floor_light_height_spread_scale(resolved->world_exact_pos_2d.y, radius_world));
        const bool overlaps_view = screen.x + floor_spread_radius_px >= -kCullingMargin &&
                                   screen.x - floor_spread_radius_px <= static_cast<float>(screen_width_) + kCullingMargin &&
                                   screen.y + floor_spread_radius_px >= -kCullingMargin &&
                                   screen.y - floor_spread_radius_px <= static_cast<float>(screen_height_) + kCullingMargin;
        const bool enabled_and_overlapping = !registry_entry.hidden && overlaps_view;
        if (!enabled_and_overlapping) {
            ++runtime_light_culled_count_;
        }

        const float light_depth_distance = static_cast<float>(std::fabs(
            render_depth::depth_from_anchor(focus_plane_world_z, static_cast<double>(resolved->world_exact_z))));
        float depth_fade_alpha = 0.0f;
        if (std::isfinite(light_depth_distance) && light_depth_distance < light_max_cull_depth) {
            if (light_depth_distance <= distance_fade_start_depth) {
                depth_fade_alpha = 1.0f;
            } else {
                const float t = std::clamp((light_depth_distance - distance_fade_start_depth) / distance_fade_span,
                                           0.0f,
                                           1.0f);
                const float smoothstep_t = t * t * (3.0f - 2.0f * t);
                depth_fade_alpha = 1.0f - smoothstep_t;
            }
        }

        if (broadphase.light_id >= runtime_light_cache_.size()) {
            runtime_light_cache_.resize(static_cast<std::size_t>(broadphase.light_id) + 1);
        }
        RuntimeLightCacheEntry& cache_entry = runtime_light_cache_[broadphase.light_id];
        const bool first_seen = cache_entry.last_seen_frame == 0;
        cache_entry.last_seen_frame = frame_token;
        cache_entry.fade.last_seen_frame = frame_token;

        const float target_intensity = enabled_and_overlapping
            ? light.intensity * depth_fade_alpha
            : 0.0f;
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
        instance.stable_light_id = broadphase.light_id;
        instance.screen_center = screen;
        instance.color = SDL_Color{light.color_r, light.color_g, light.color_b, 255};
        instance.intensity = effective_intensity;
        instance.opacity = light.opacity;
        instance.radius_px = radius_px;
        instance.radius_world = radius_world;
        instance.falloff = light.falloff;
        instance.world_z = static_cast<float>(
            render_depth::depth_from_anchor(focus_plane_world_z,
                                            static_cast<double>(resolved->world_exact_z)));
        const render_internal::FloorLightContact floor_contact = render_internal::resolve_floor_light_contact(
            resolved->flat_world_exact_pos_2d.x,
            resolved->flat_world_exact_z,
            resolved->world_exact_pos_2d.x,
            resolved->world_exact_z,
            resolved->world_exact_pos_2d.y);
        instance.floor_world_x = floor_contact.world_x;
        instance.floor_world_z = floor_contact.world_z;
        instance.world_height = floor_contact.world_height;
        instance.has_floor_projection = false;
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

    for (std::size_t i = 1; i < runtime_light_cache_.size(); ++i) {
        RuntimeLightCacheEntry& cache_entry = runtime_light_cache_[i];
        if (cache_entry.last_seen_frame != 0 && frame_token > cache_entry.last_seen_frame + 120) {
            cache_entry = RuntimeLightCacheEntry{};
        }
    }

    if (realism.light_culling_debug_overlay) {
        const std::uint64_t now_ticks = SDL_GetTicks();
        const std::uint64_t elapsed_ticks = SDL_GetTicks() - gather_start_ticks;
        if (runtime_light_profile_last_log_ticks_ == 0 || now_ticks - runtime_light_profile_last_log_ticks_ >= 1000) {
            runtime_light_profile_last_log_ticks_ = now_ticks;
            vibble::log::debug("[SceneRenderer] פרופיל איסוף תאורה: מועמדים=" +
                               std::to_string(candidate_light_ids.size()) +
                               " צוירו=" + std::to_string(runtime_light_rendered_count_) +
                               " סוננו=" + std::to_string(runtime_light_culled_count_) +
                               " מילישניות=" + std::to_string(ticks_to_seconds(elapsed_ticks) * 1000.0f));
        }
    }
}

void SceneRenderer::enqueue_runtime_light_dirty(std::uint32_t light_id,
                                                RuntimeLightRegistryEntry& entry,
                                                bool transform_dirty,
                                                bool frame_dirty,
                                                bool light_data_dirty,
                                                bool removed) {
    if (light_id == 0 || light_id >= runtime_light_registry_entries_.size()) {
        return;
    }
    entry.transform_dirty = entry.transform_dirty || transform_dirty;
    entry.frame_dirty = entry.frame_dirty || frame_dirty;
    entry.light_data_dirty = entry.light_data_dirty || light_data_dirty;
    entry.removed = entry.removed || removed;
    if (runtime_light_dirty_set_.insert(light_id).second) {
        runtime_light_dirty_queue_.push_back(light_id);
    }
}

void SceneRenderer::discover_runtime_lights_for_asset(Asset* asset, std::uint64_t frame_token) {
    if (!asset || asset->dead || !asset->current_frame) {
        return;
    }
    for (const DisplacedAssetAnchorPoint& anchor : asset->current_frame->anchor_points) {
        if (!anchor.is_valid() || !anchor.has_light_data) {
            continue;
        }
        RuntimeLightRegistryKey registry_key{asset, anchor.name};
        auto id_it = runtime_light_registry_ids_.find(registry_key);
        std::uint32_t light_id = 0;
        if (id_it == runtime_light_registry_ids_.end()) {
            light_id = runtime_light_next_id_++;
            runtime_light_registry_ids_.emplace(std::move(registry_key), light_id);
            if (light_id >= runtime_light_registry_entries_.size()) {
                runtime_light_registry_entries_.resize(static_cast<std::size_t>(light_id) + 1);
            }
            if (light_id >= runtime_light_cache_.size()) {
                runtime_light_cache_.resize(static_cast<std::size_t>(light_id) + 1);
            }
        } else {
            light_id = id_it->second;
        }
        RuntimeLightRegistryEntry& entry = runtime_light_registry_entries_[light_id];
        entry.light_id = light_id;
        entry.asset = asset;
        entry.anchor_name = anchor.name;
        entry.last_seen_frame = frame_token;
        enqueue_runtime_light_dirty(light_id, entry, true, true, true, false);
    }
}

void SceneRenderer::prune_removed_runtime_lights(std::uint64_t frame_token) {
    std::vector<RuntimeLightRegistryKey> erased_keys;
    for (std::size_t i = 1; i < runtime_light_registry_entries_.size(); ++i) {
        RuntimeLightRegistryEntry& entry = runtime_light_registry_entries_[i];
        if (entry.light_id == 0 || !entry.asset) {
            continue;
        }
        if (!entry.removed && entry.last_seen_frame + 1 >= frame_token) {
            continue;
        }
        erased_keys.push_back(RuntimeLightRegistryKey{entry.asset, entry.anchor_name});
        entry = RuntimeLightRegistryEntry{};
        if (i < runtime_light_cache_.size()) {
            runtime_light_cache_[i] = RuntimeLightCacheEntry{};
        }
    }
    for (const RuntimeLightRegistryKey& key : erased_keys) {
        runtime_light_registry_ids_.erase(key);
    }
}

void SceneRenderer::update_runtime_light_registry_incremental(std::uint64_t frame_token) {
    if (!assets_) {
        return;
    }
    world::WorldGrid& world_grid = assets_->world_grid();
    runtime_light_spatial_cell_size_ = std::max(32, world_grid.grid_spacing_for_layer(world_grid.default_resolution_layer()));

    const std::uint64_t active_state_version = assets_->dev_active_state_version();
    if (runtime_light_observed_active_state_version_ != active_state_version) {
        runtime_light_observed_active_state_version_ = active_state_version;
        for (auto state_it = runtime_light_asset_state_.begin(); state_it != runtime_light_asset_state_.end();) {
            Asset* state_asset = state_it->first;
            if (state_asset && assets_->contains_asset(state_asset)) {
                ++state_it;
                continue;
            }
            for (const auto& [registry_key, light_id] : runtime_light_registry_ids_) {
                if (registry_key.asset != state_asset || light_id == 0 || light_id >= runtime_light_registry_entries_.size()) {
                    continue;
                }
                RuntimeLightRegistryEntry& entry = runtime_light_registry_entries_[light_id];
                enqueue_runtime_light_dirty(light_id, entry, false, false, false, true);
            }
            state_it = runtime_light_asset_state_.erase(state_it);
        }
        for (Asset* asset : assets_->all) {
            if (!asset) {
                continue;
            }
            auto [state_it, inserted] = runtime_light_asset_state_.try_emplace(asset);
            if (inserted) {
                state_it->second.anchor_revision = asset->anchor_world_revision();
                state_it->second.frame_index = asset->current_frame ? asset->current_frame->frame_index : std::numeric_limits<int>::min();
                state_it->second.anchor_light_signature = 0;
                state_it->second.alive = !asset->dead;
            }
            discover_runtime_lights_for_asset(asset, frame_token);
        }
    }

    for (auto& [asset, state] : runtime_light_asset_state_) {
        if (!asset || !assets_->contains_asset(asset)) {
            continue;
        }
        const bool alive = !asset->dead && asset->current_frame;
        if (!alive && state.alive) {
            state.alive = false;
            for (const auto& [registry_key, light_id] : runtime_light_registry_ids_) {
                if (registry_key.asset != asset || light_id == 0 || light_id >= runtime_light_registry_entries_.size()) {
                    continue;
                }
                RuntimeLightRegistryEntry& entry = runtime_light_registry_entries_[light_id];
                entry.last_seen_frame = frame_token;
                enqueue_runtime_light_dirty(light_id, entry, false, false, false, true);
            }
            continue;
        }
        if (!alive) {
            continue;
        }

        std::size_t anchor_signature = 1469598103934665603ull;
        for (const DisplacedAssetAnchorPoint& anchor : asset->current_frame->anchor_points) {
            if (!anchor.is_valid() || !anchor.has_light_data) {
                continue;
            }
            const std::size_t name_hash = std::hash<std::string>{}(anchor.name);
            const std::size_t hidden_hash = std::hash<bool>{}(anchor.hidden);
            const std::size_t radius_hash = std::hash<float>{}(anchor.light.radius);
            const std::size_t texture_x_hash = std::hash<int>{}(anchor.texture_x);
            const std::size_t texture_y_hash = std::hash<int>{}(anchor.texture_y);
            const std::size_t depth_offset_hash = std::hash<float>{}(anchor.depth_offset);
            anchor_signature ^= (name_hash + 0x9e3779b97f4a7c15ull + (anchor_signature << 6) + (anchor_signature >> 2));
            anchor_signature ^= (hidden_hash + 0x9e3779b97f4a7c15ull + (anchor_signature << 6) + (anchor_signature >> 2));
            anchor_signature ^= (radius_hash + 0x9e3779b97f4a7c15ull + (anchor_signature << 6) + (anchor_signature >> 2));
            anchor_signature ^= (texture_x_hash + 0x9e3779b97f4a7c15ull + (anchor_signature << 6) + (anchor_signature >> 2));
            anchor_signature ^= (texture_y_hash + 0x9e3779b97f4a7c15ull + (anchor_signature << 6) + (anchor_signature >> 2));
            anchor_signature ^= (depth_offset_hash + 0x9e3779b97f4a7c15ull + (anchor_signature << 6) + (anchor_signature >> 2));
        }
        const std::uint64_t anchor_revision = asset->anchor_world_revision();
        const int frame_index = asset->current_frame ? asset->current_frame->frame_index : std::numeric_limits<int>::min();
        const bool transform_dirty = state.anchor_revision != anchor_revision;
        const bool frame_dirty = state.frame_index != frame_index;
        const bool light_data_dirty = state.anchor_light_signature != anchor_signature;
        if (transform_dirty || frame_dirty || light_data_dirty) {
            discover_runtime_lights_for_asset(asset, frame_token);
            for (const auto& [registry_key, light_id] : runtime_light_registry_ids_) {
                if (registry_key.asset != asset || light_id == 0 || light_id >= runtime_light_registry_entries_.size()) {
                    continue;
                }
                RuntimeLightRegistryEntry& entry = runtime_light_registry_entries_[light_id];
                entry.last_seen_frame = frame_token;
                enqueue_runtime_light_dirty(light_id, entry, transform_dirty, frame_dirty, light_data_dirty, false);
            }
        }
        state.anchor_revision = anchor_revision;
        state.frame_index = frame_index;
        state.anchor_light_signature = anchor_signature;
        state.alive = true;
    }

    for (const std::uint32_t light_id : runtime_light_dirty_queue_) {
        if (light_id == 0 || light_id >= runtime_light_registry_entries_.size()) {
            continue;
        }
        RuntimeLightRegistryEntry& entry = runtime_light_registry_entries_[light_id];
        if (entry.light_id == 0 || !entry.asset) {
            continue;
        }
        if (entry.removed || entry.asset->dead || !entry.asset->current_frame) {
            auto old_cell_it = runtime_light_spatial_index_.find(entry.cell);
            if (old_cell_it != runtime_light_spatial_index_.end()) {
                auto& ids = old_cell_it->second;
                ids.erase(std::remove(ids.begin(), ids.end(), light_id), ids.end());
                if (ids.empty()) {
                    runtime_light_spatial_index_.erase(old_cell_it);
                }
            }
            entry.valid = false;
            continue;
        }

        std::optional<AnchorPoint> resolved = entry.asset->anchor_state(entry.anchor_name,
                                                                         anchor_points::GridMaterialization::None,
                                                                         Asset::AnchorResolveMode::Cached);
        if (!resolved.has_value() || !resolved->exists) {
            resolved = entry.asset->anchor_state(entry.anchor_name,
                                                 anchor_points::GridMaterialization::None,
                                                 Asset::AnchorResolveMode::ForceRecompute);
        }
        bool found_anchor = false;
        DisplacedAssetAnchorPoint source_anchor{};
        for (const DisplacedAssetAnchorPoint& anchor : entry.asset->current_frame->anchor_points) {
            if (anchor.name == entry.anchor_name && anchor.is_valid() && anchor.has_light_data) {
                source_anchor = anchor;
                found_anchor = true;
                break;
            }
        }
        if (!found_anchor || !resolved.has_value() || !resolved->exists) {
            enqueue_runtime_light_dirty(light_id, entry, false, false, false, true);
            continue;
        }

        AnchorLightData light = source_anchor.light;
        light.sanitize();
        const RuntimeLightSpatialCell new_cell = runtime_light_cell_for_world(resolved->world_exact_pos_2d.x, resolved->world_exact_z);
        if (entry.valid && (entry.cell.x != new_cell.x || entry.cell.z != new_cell.z)) {
            auto old_cell_it = runtime_light_spatial_index_.find(entry.cell);
            if (old_cell_it != runtime_light_spatial_index_.end()) {
                auto& ids = old_cell_it->second;
                ids.erase(std::remove(ids.begin(), ids.end(), light_id), ids.end());
                if (ids.empty()) {
                    runtime_light_spatial_index_.erase(old_cell_it);
                }
            }
        }

        entry.light = light;
        entry.anchor_world_x = resolved->world_exact_pos_2d.x;
        entry.anchor_world_y = resolved->world_exact_pos_2d.y;
        entry.anchor_world_z = resolved->world_exact_z;
        entry.hidden = source_anchor.hidden;
        entry.radius_world = std::max(AnchorLightData::kMinRadius, light.radius);
        entry.cell = new_cell;
        entry.valid = true;
        entry.removed = false;
        entry.last_seen_frame = frame_token;
        entry.transform_dirty = false;
        entry.frame_dirty = false;
        entry.light_data_dirty = false;

        auto& members = runtime_light_spatial_index_[new_cell];
        if (std::find(members.begin(), members.end(), light_id) == members.end()) {
            members.push_back(light_id);
        }
    }
    runtime_light_dirty_queue_.clear();
    runtime_light_dirty_set_.clear();
    prune_removed_runtime_lights(frame_token);
    float max_radius_world = AnchorLightData::kMinRadius;
    for (std::size_t i = 1; i < runtime_light_registry_entries_.size(); ++i) {
        const RuntimeLightRegistryEntry& entry = runtime_light_registry_entries_[i];
        if (!entry.valid) {
            continue;
        }
        max_radius_world = std::max(max_radius_world, std::max(AnchorLightData::kMinRadius, entry.radius_world));
    }
    runtime_light_max_radius_world_ = max_radius_world;
}

SceneRenderer::RuntimeLightSpatialCell SceneRenderer::runtime_light_cell_for_world(float world_x, float world_z) const {
    const float cell_size = static_cast<float>(std::max(1, runtime_light_spatial_cell_size_));
    return RuntimeLightSpatialCell{
        static_cast<int>(std::floor(world_x / cell_size)),
        static_cast<int>(std::floor(world_z / cell_size))
    };
}

void SceneRenderer::runtime_light_query_visible_cells(const WarpedScreenGrid& cam,
                                                      float world_z,
                                                      float culling_margin,
                                                      std::vector<std::uint32_t>& out_light_ids) const {
    out_light_ids.clear();
    if (runtime_light_spatial_index_.empty()) {
        return;
    }

    const float left = -culling_margin;
    const float right = static_cast<float>(screen_width_) + culling_margin;
    const float top = -culling_margin;
    const float bottom = static_cast<float>(screen_height_) + culling_margin;

    std::array<SDL_FPoint, 4> samples{
        SDL_FPoint{left, top},
        SDL_FPoint{right, top},
        SDL_FPoint{left, bottom},
        SDL_FPoint{right, bottom}
    };
    float world_min_x = std::numeric_limits<float>::max();
    float world_max_x = std::numeric_limits<float>::lowest();
    float world_min_z = std::numeric_limits<float>::max();
    float world_max_z = std::numeric_limits<float>::lowest();
    bool has_world_bounds = false;
    for (const SDL_FPoint& screen_point : samples) {
        render_projection::WorldPoint3 world_point{};
        if (!cam.screen_to_world_on_depth_plane(screen_point, world_z, world_point) || !world_point.valid) {
            continue;
        }
        world_min_x = std::min(world_min_x, world_point.x);
        world_max_x = std::max(world_max_x, world_point.x);
        world_min_z = std::min(world_min_z, world_point.z);
        world_max_z = std::max(world_max_z, world_point.z);
        has_world_bounds = true;
    }

    if (!has_world_bounds) {
        for (const auto& cell_entry : runtime_light_spatial_index_) {
            out_light_ids.insert(out_light_ids.end(), cell_entry.second.begin(), cell_entry.second.end());
        }
        return;
    }

    const float max_light_radius_world = std::max(AnchorLightData::kMinRadius, runtime_light_max_radius_world_);
    const RuntimeLightSpatialCell min_cell = runtime_light_cell_for_world(world_min_x, world_min_z);
    const RuntimeLightSpatialCell max_cell = runtime_light_cell_for_world(world_max_x, world_max_z);
    const int cell_padding = std::max(
        1,
        static_cast<int>(std::ceil(max_light_radius_world / static_cast<float>(std::max(1, runtime_light_spatial_cell_size_)))));
    const int query_min_x = min_cell.x - cell_padding;
    const int query_max_x = max_cell.x + cell_padding;
    const int query_min_z = min_cell.z - cell_padding;
    const int query_max_z = max_cell.z + cell_padding;
    for (int cell_x = query_min_x; cell_x <= query_max_x; ++cell_x) {
        for (int cell_z = query_min_z; cell_z <= query_max_z; ++cell_z) {
            const RuntimeLightSpatialCell cell{cell_x, cell_z};
            const auto it = runtime_light_spatial_index_.find(cell);
            if (it == runtime_light_spatial_index_.end()) {
                continue;
            }
            out_light_ids.insert(out_light_ids.end(), it->second.begin(), it->second.end());
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

            // ילדים שמקושרים לאליפסה הם דינמיים עם מצב כיוון ורדיוס של העכבר, ולכן נתיבי דיבאג לתנועה
            // נבנים מחדש בכל פריים עבור הנכסים האלה.
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
    if (!renderer_ || !assets_ || screen_width_ <= 0 || screen_height_ <= 0) {
        return;
    }

    const std::uint64_t render_begin_counter = SDL_GetPerformanceCounter();
    const std::uint64_t perf_freq = SDL_GetPerformanceFrequency();
    render_diagnostics::begin_frame();
    render_diagnostics::set_texture_memory_usage(render_diagnostics::tracked_texture_bytes(), false);

    auto finalize_render_cpu_timer = [&]() {
        const std::uint64_t render_end_counter = SDL_GetPerformanceCounter();
        if (perf_freq > 0 && render_end_counter >= render_begin_counter) {
            const double render_ms =
                (static_cast<double>(render_end_counter - render_begin_counter) * 1000.0) /
                static_cast<double>(perf_freq);
            render_diagnostics::set_render_thread_cpu_ms(render_ms);
        }
    };

    auto fail_gpu_frame = [&](const std::string& error_message, bool abort_open_gpu_frame) {
        if (abort_open_gpu_frame && gpu_scene_renderer_) {
            gpu_scene_renderer_->abort_frame();
        }
        draw_gpu_failure_overlay(renderer_, screen_width_, screen_height_);
        render_diagnostics::note_gpu_frame_skipped_due_to_failure();
        render_diagnostics::set_renderer_runtime_info("gpu", "failed", "error_overlay");
        finalize_render_cpu_timer();
        render_diagnostics::end_frame();
        vibble::log::error("[SceneRenderer] GPU runtime frame failed: " + error_message);
    };

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

    const std::uint64_t submission_begin_counter = SDL_GetPerformanceCounter();
    const render_pipeline::LayerBuildResult layer_build = layer_submission_builder_
        ? layer_submission_builder_->build(*geometry_batcher_, cam, player_split_world_z, max_cull_depth)
        : render_pipeline::LayerBuildResult{};
    const std::uint64_t submission_end_counter = SDL_GetPerformanceCounter();
    if (perf_freq > 0 && submission_end_counter >= submission_begin_counter) {
        const double submission_ms =
            (static_cast<double>(submission_end_counter - submission_begin_counter) * 1000.0) /
            static_cast<double>(perf_freq);
        render_diagnostics::add_draw_submission_ms(submission_ms);
    }

    if (first_gpu_submission_pending_) {
        clear_backbuffer_to_color(renderer_, screen_width_, screen_height_, map_clear_color_);
        first_gpu_submission_pending_ = false;
    }
    if (!ensure_scene_target()) {
        fail_gpu_frame("Failed to allocate the scene composite target texture.", false);
        return;
    }

    SDL_Texture* floor_texture = floor_composer_
        ? floor_composer_->compose_gpu(cam,
                                       grid,
                                       runtime_lights,
                                       runtime_lighting_enabled,
                                       max_cull_depth,
                                       map_clear_color_,
                                       true)
        : nullptr;
    const render_pipeline::CompactLayerRenderResult layer_render = layer_stack_renderer_
        ? layer_stack_renderer_->render_gpu_compact(layer_build,
                                                    runtime_lights,
                                                    runtime_lighting_enabled,
                                                    true)
        : render_pipeline::CompactLayerRenderResult{};

    SDL_Texture* scene_for_blur = layer_render.final_texture;
    if (scene_for_blur && ensure_far_backdrop_resources() && ensure_far_backdrop_composite_target()) {
        if (const std::optional<float> cull_screen_y =
                project_depth_guide_screen_y(cam, static_cast<float>(max_cull_depth), screen_height_)) {
            if (render_diagnostics::set_render_target(renderer_, scene_preblur_tex_)) {
                SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
                SDL_RenderClear(renderer_);

                auto draw_locked_width_bottom_aligned = [&](SDL_Texture* texture) {
                    if (!texture) {
                        return;
                    }
                    float tw = 0.0f;
                    float th = 0.0f;
                    if (!SDL_GetTextureSize(texture, &tw, &th) || tw <= 0.0f || th <= 0.0f || screen_width_ <= 0) {
                        return;
                    }
                    const float scale = static_cast<float>(screen_width_) / tw;
                    const int dst_h = static_cast<int>(std::lround(th * scale));
                    const int dst_y = static_cast<int>(std::lround(*cull_screen_y)) - dst_h;
                    const SDL_FRect dst{
                        0.0f,
                        static_cast<float>(dst_y),
                        static_cast<float>(screen_width_),
                        static_cast<float>(dst_h)};
                    render_texture_utils::reset_texture_state(texture);
                    SDL_RenderTexture(renderer_, texture, nullptr, &dst);
                };

                // Furthest back first: sky, then mountains, then runtime scene.
                draw_locked_width_bottom_aligned(far_backdrop_sky_tex_);
                draw_locked_width_bottom_aligned(far_backdrop_mountains_tex_);
                render_texture_utils::draw_fullscreen_texture(renderer_, layer_render.final_texture);
                scene_for_blur = scene_preblur_tex_;
            }
        }
    }

    const render_pipeline::BlurCompositeResult blur_result = blur_chain_renderer_
        ? blur_chain_renderer_->compose_gpu(scene_for_blur,
                                            realism.depth_of_field_enabled,
                                            realism.blur_px,
                                            realism.radial_blur_px,
                                            SDL_FPoint{
                                                static_cast<float>(screen_width_) * 0.5f,
                                                static_cast<float>(screen_height_) * 0.5f})
        : render_pipeline::BlurCompositeResult{};
    const bool composed = scene_composite_pass_ &&
                          scene_composite_pass_->compose_gpu(scene_composite_tex_,
                                                             floor_texture,
                                                             floor_composer_ ? floor_composer_->floor_dark_mask_texture() : nullptr,
                                                             floor_composer_ ? floor_composer_->floor_overlay_texture() : nullptr,
                                                             layer_render.final_texture,
                                                             blur_result);
    if (!composed) {
        fail_gpu_frame("Scene composite pass failed to produce an output frame.", false);
        return;
    }

    std::string frame_graph_error;
    if (!execute_gpu_frame_graph(scene_composite_tex_, frame_graph_error)) {
        fail_gpu_frame("Frame-graph execution failed: " + frame_graph_error, false);
        return;
    }

    clear_backbuffer_to_color(renderer_, screen_width_, screen_height_, map_clear_color_);
    render_texture_utils::draw_fullscreen_texture(renderer_, scene_composite_tex_);
    render_diagnostics::set_renderer_runtime_info("gpu",
                                                  gpu_frame_graph_interop_supported_ ? "frame_graph+sdl_renderer"
                                                                                     : "sdl_renderer",
                                                  "vsync");

    finalize_render_cpu_timer();
    render_diagnostics::end_frame();
    return;
}
