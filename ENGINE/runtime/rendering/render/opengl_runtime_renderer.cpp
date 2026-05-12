#include "rendering/render/opengl_runtime_renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_types.hpp"
#include "core/AssetsManager.hpp"
#include "gameplay/map_generation/room.hpp"
#include "gameplay/world/chunk.hpp"
#include "rendering/render/render_diagnostics.hpp"
#include "rendering/render/layer_depth_bins.hpp"
#include "rendering/render/render_depth_policy.hpp"
#include "rendering/render/render_object_builder.hpp"
#include "rendering/render/render_object_projection.hpp"
#include "rendering/render/opengl_runtime_renderer.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "utils/log.hpp"
#include "utils/ranged_color.hpp"

namespace {

constexpr float kFloorBlendRadius = 2400.0f;
constexpr float kFloorLerpWhenMoving = 0.08f;

std::string safe_string(const char* value) {
    return value ? std::string(value) : std::string();
}

int renderer_max_texture_size(SDL_Renderer* renderer) {
    if (!renderer) {
        return 0;
    }
    const SDL_PropertiesID renderer_props = SDL_GetRendererProperties(renderer);
    if (!renderer_props) {
        return 0;
    }
    return static_cast<int>(SDL_GetNumberProperty(renderer_props, SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 0));
}

int clamp_dimension_to_renderer_limit(int value, int renderer_limit, const char* axis_label) {
    const int safe_value = std::max(1, value);
    if (renderer_limit <= 0 || safe_value <= renderer_limit) {
        return safe_value;
    }
    vibble::log::warn(std::string{"[OpenGLRuntimeRenderer] Clamping "} + axis_label +
                      " dimension from " + std::to_string(safe_value) +
                      " to renderer max texture size " + std::to_string(renderer_limit) + ".");
    return renderer_limit;
}

std::string describe_asset_sprite_source(const Asset* asset,
                                         const char* texture_id = nullptr,
                                         int frame_index = -1,
                                         int variant_index = -1) {
    const std::string asset_name = (asset && asset->info && !asset->info->name.empty())
        ? asset->info->name
        : std::string{"<unknown-asset>"};
    const std::string animation_name = (asset && !asset->current_animation.empty())
        ? asset->current_animation
        : std::string{"<none>"};
    const int resolved_frame_index = (frame_index >= 0)
        ? frame_index
        : ((asset && asset->current_frame) ? asset->current_frame->frame_index : -1);
    const int resolved_variant_index = (variant_index >= 0)
        ? variant_index
        : (asset ? asset->current_variant_index : -1);

    std::string description = "asset='" + asset_name +
                              "' animation='" + animation_name +
                              "' frame=" + std::to_string(resolved_frame_index) +
                              " variant=" + std::to_string(resolved_variant_index);
    if (texture_id && *texture_id) {
        description += " texture='" + std::string(texture_id) + "'";
    }
    return description;
}

std::string describe_draw_packet_source(const GpuSpriteDrawPacket& draw) {
    return "asset='" + draw.source_asset_name +
           "' animation='" + (draw.source_animation_name.empty()
               ? std::string{"<none>"}
               : draw.source_animation_name) +
           "' frame=" + std::to_string(draw.source_frame_index) +
           " variant=" + std::to_string(draw.source_variant_index) +
           " texture='" + draw.source_texture_id + "'";
}

std::string join_ints(const std::vector<int>& values) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            oss << ", ";
        }
        oss << values[i];
    }
    return oss.str();
}

double compute_asset_camera_depth_key(const WarpedScreenGrid& camera, const Asset& asset) {
    const auto projection = camera.projection_params();
    const double focus_world_z = camera.current_focus_plane_world_z();
    const double effective_world_z =
        static_cast<double>(asset.world_z()) +
        static_cast<double>(asset.world_z_offset()) +
        static_cast<double>(asset.render_anchor_offset_z());
    const double bias = asset.render_depth_bias();
    const double depth_from_anchor = render_depth::depth_from_anchor(
        focus_world_z,
        effective_world_z,
        bias);
    const double depth_axis_sign = static_cast<double>(render_depth::normalize_depth_axis_sign(
        static_cast<float>(projection.forward_z)));
    return depth_from_anchor * depth_axis_sign;
}

float to_clip_x(float screen_x, float target_width) {
    if (!std::isfinite(screen_x) || !std::isfinite(target_width) || target_width <= 0.0f) {
        return 0.0f;
    }
    return (screen_x / target_width) * 2.0f - 1.0f;
}

float to_clip_y(float screen_y, float target_height) {
    if (!std::isfinite(screen_y) || !std::isfinite(target_height) || target_height <= 0.0f) {
        return 0.0f;
    }
    return 1.0f - (screen_y / target_height) * 2.0f;
}

void fill_quad_packet_vertices(const SDL_FPoint& tl,
                               const SDL_FPoint& tr,
                               const SDL_FPoint& br,
                               const SDL_FPoint& bl,
                               float u0,
                               float v0,
                               float u1,
                               float v1,
                               float target_width,
                               float target_height,
                               GpuSpriteDrawPacket& out_packet) {
    const auto make_vertex = [target_width, target_height](const SDL_FPoint& point, float u, float v) {
        GpuSpriteVertex vertex{};
        vertex.clip_x = to_clip_x(point.x, target_width);
        vertex.clip_y = to_clip_y(point.y, target_height);
        vertex.uv_x = u;
        vertex.uv_y = v;
        return vertex;
    };

    out_packet.vertices[0] = make_vertex(tl, u0, v0);
    out_packet.vertices[1] = make_vertex(tr, u1, v0);
    out_packet.vertices[2] = make_vertex(br, u1, v1);
    out_packet.vertices[3] = make_vertex(tl, u0, v0);
    out_packet.vertices[4] = make_vertex(br, u1, v1);
    out_packet.vertices[5] = make_vertex(bl, u0, v1);
}

void fill_sprite_packet_vertices(const render_projection::ProjectedSpriteFrame& projected,
                                 float u0,
                                 float v0,
                                 float u1,
                                 float v1,
                                 float target_width,
                                 float target_height,
                                 GpuSpriteDrawPacket& out_packet) {
    fill_quad_packet_vertices(projected.screen_tl,
                              projected.screen_tr,
                              projected.screen_br,
                              projected.screen_bl,
                              u0,
                              v0,
                              u1,
                              v1,
                              target_width,
                              target_height,
                              out_packet);
}

bool tag_requests_floor_render_pass(const std::string& tag) {
    return tag == "render_pass:floor" ||
           tag == "render-pass:floor" ||
           tag == "render_floor_pass" ||
           tag == "floor_render_pass";
}

bool info_requests_floor_pass_tag_for_diagnostics_impl(const std::string& type,
                                                       const std::vector<std::string>& tags) {
    if (tag_requests_floor_render_pass(type)) {
        return true;
    }
    for (const std::string& tag : tags) {
        if (tag_requests_floor_render_pass(tag)) {
            return true;
        }
    }
    return false;
}

void emit_floor_pass_tag_ignored_diagnostic(const Asset* asset) {
    if (!asset || !asset->info) {
        return;
    }
    if (!info_requests_floor_pass_tag_for_diagnostics_impl(asset->info->type, asset->info->tags)) {
        return;
    }
    const std::string asset_name = asset->info->name.empty()
        ? std::string{"<unnamed-asset>"}
        : asset->info->name;
    static std::unordered_set<std::string> warned_assets{};
    if (!warned_assets.insert(asset_name).second) {
        return;
    }
    vibble::log::warn(
        "[OpenGLRuntimeRenderer] Ignoring floor render-pass tag on XY sprite candidate asset='" +
        asset_name +
        "' because floor pass is strict XZ-only (tiles + floor markers).");
}

std::uintptr_t floor_sort_id(bool sprite_packet, std::uintptr_t sequence) {
    constexpr std::uintptr_t kSpritePacketSortOffset =
        std::uintptr_t{1} << ((sizeof(std::uintptr_t) * 8u) - 1u);
    return sprite_packet ? (kSpritePacketSortOffset + sequence) : sequence;
}

void fill_geometry_vertices(const GpuSpriteDrawPacket& packet,
                            std::uint32_t target_width,
                            std::uint32_t target_height,
                            std::array<SDL_Vertex, 6>& out_vertices) {
    const float width = static_cast<float>(std::max<std::uint32_t>(1u, target_width));
    const float height = static_cast<float>(std::max<std::uint32_t>(1u, target_height));
    const auto convert_position = [width, height](float clip_x, float clip_y) -> SDL_FPoint {
        const float x = (clip_x + 1.0f) * 0.5f * width;
        const float y = (1.0f - clip_y) * 0.5f * height;
        return SDL_FPoint{x, y};
    };

    for (std::size_t i = 0; i < out_vertices.size(); ++i) {
        const GpuSpriteVertex& src = packet.vertices[i];
        SDL_Vertex dst{};
        dst.position = convert_position(src.clip_x, src.clip_y);
        dst.tex_coord = SDL_FPoint{src.uv_x, src.uv_y};
        dst.color = packet.modulate;
        out_vertices[i] = dst;
    }
}

} // namespace

bool opengl_runtime_renderer_detail::draw_packet_sort_predicate_floor(const GpuSpriteDrawPacket& lhs,
                                                                      const GpuSpriteDrawPacket& rhs) {
    if (lhs.projected_foot_y_key != rhs.projected_foot_y_key) {
        return lhs.projected_foot_y_key < rhs.projected_foot_y_key;
    }
    if (lhs.camera_depth_key != rhs.camera_depth_key) {
        return lhs.camera_depth_key < rhs.camera_depth_key;
    }
    return lhs.stable_sort_id < rhs.stable_sort_id;
}

bool opengl_runtime_renderer_detail::draw_packet_sort_predicate_xy(const GpuSpriteDrawPacket& lhs,
                                                                   const GpuSpriteDrawPacket& rhs) {
    // Strict far-to-near ordering in XY pass.
    if (lhs.camera_depth_key != rhs.camera_depth_key) {
        return lhs.camera_depth_key > rhs.camera_depth_key;
    }
    if (lhs.projected_foot_y_key != rhs.projected_foot_y_key) {
        return lhs.projected_foot_y_key < rhs.projected_foot_y_key;
    }
    return lhs.stable_sort_id < rhs.stable_sort_id;
}

namespace opengl_runtime_renderer_detail {

bool info_requests_floor_pass_tag_for_diagnostics(const std::string& type,
                                                  const std::vector<std::string>& tags) {
    return info_requests_floor_pass_tag_for_diagnostics_impl(type, tags);
}

bool info_is_xy_sprite_pass_eligible(bool tillable) {
    return !tillable;
}

int classify_depth_layer_for_asset(const WarpedScreenGrid& camera, const Asset& asset) {
    const auto& settings = camera.get_settings();
    const double signed_distance = compute_asset_camera_depth_key(camera, asset);
    const double distance = std::fabs(signed_distance);
    if (!std::isfinite(distance) || distance <= 1.0e-4) {
        return 0;
    }

    const double interval = std::max(1.0, static_cast<double>(settings.layer_depth_interval));
    const double curve = std::max(0.0, static_cast<double>(settings.layer_depth_curve));
    const double effective_max_depth = std::max(interval, static_cast<double>(settings.max_cull_depth));
    const std::vector<double> edges =
        render_depth::build_background_depth_edges(effective_max_depth, interval, curve);

    std::size_t bin_index = 0;
    for (std::size_t i = 1; i < edges.size(); ++i) {
        if (distance <= edges[i]) {
            bin_index = i - 1;
            break;
        }
        bin_index = i - 1;
    }

    const std::uint32_t max_layer = static_cast<std::uint32_t>(render_depth::kMaxLayersPerSide);
    const std::uint32_t clamped_layer = static_cast<std::uint32_t>(
        std::min<std::size_t>(bin_index + 1u, static_cast<std::size_t>(max_layer)));
    const int signed_layer = (signed_distance >= 0.0) ? static_cast<int>(clamped_layer)
                                                      : -static_cast<int>(clamped_layer);
    return signed_layer;
}

bool build_floor_tile_draw_packets(const WarpedScreenGrid& camera,
                                   const std::vector<world::Chunk*>& chunks,
                                   std::uint32_t target_width,
                                   std::uint32_t target_height,
                                   std::vector<GpuSpriteDrawPacket>& out_packets) {
    out_packets.clear();
    const float output_w = static_cast<float>(std::max<std::uint32_t>(1u, target_width));
    const float output_h = static_cast<float>(std::max<std::uint32_t>(1u, target_height));
    std::uintptr_t tile_sequence = 0u;

    for (const world::Chunk* chunk : chunks) {
        if (!chunk) {
            continue;
        }
        for (const GridTile& tile : chunk->tiles) {
            if (!tile.texture || tile.world_rect.w <= 0 || tile.world_rect.h <= 0) {
                continue;
            }

            const float left = static_cast<float>(tile.world_rect.x);
            const float top_z = static_cast<float>(tile.world_rect.y);
            const float right = static_cast<float>(tile.world_rect.x + tile.world_rect.w);
            const float bottom_z = static_cast<float>(tile.world_rect.y + tile.world_rect.h);

            SDL_FPoint screen_tl{};
            SDL_FPoint screen_tr{};
            SDL_FPoint screen_br{};
            SDL_FPoint screen_bl{};
            if (!camera.project_world_point(SDL_FPoint{left, 0.0f}, top_z, screen_tl) ||
                !camera.project_world_point(SDL_FPoint{right, 0.0f}, top_z, screen_tr) ||
                !camera.project_world_point(SDL_FPoint{right, 0.0f}, bottom_z, screen_br) ||
                !camera.project_world_point(SDL_FPoint{left, 0.0f}, bottom_z, screen_bl)) {
                continue;
            }

            GpuSpriteDrawPacket packet{};
            packet.source_texture = tile.texture;
            packet.source_asset_name = "<floor-tile>";
            packet.source_animation_name = "<floor-tile>";
            packet.source_texture_id = "tile_texture_ptr=" + std::to_string(reinterpret_cast<std::uintptr_t>(tile.texture));
            packet.source_frame_index = -1;
            packet.source_variant_index = -1;
            packet.modulate = SDL_FColor{1.0f, 1.0f, 1.0f, 1.0f};
            packet.projected_foot_y_key = std::max(screen_br.y, screen_bl.y);
            packet.camera_depth_key = 0.5f * (top_z + bottom_z);
            packet.stable_sort_id = floor_sort_id(false, tile_sequence++);
            packet.is_floor_packet = true;
            packet.depth_layer = 0;
            fill_quad_packet_vertices(screen_tl,
                                      screen_tr,
                                      screen_br,
                                      screen_bl,
                                      0.0f,
                                      0.0f,
                                      1.0f,
                                      1.0f,
                                      output_w,
                                      output_h,
                                      packet);
            out_packets.push_back(packet);
        }
    }

    std::sort(out_packets.begin(), out_packets.end(), opengl_runtime_renderer_detail::draw_packet_sort_predicate_floor);
    return true;
}

bool build_floor_marker_draw_packets(const WarpedScreenGrid& camera,
                                     const std::vector<Assets::DevFloorProjectionMarker>& markers,
                                     SDL_Texture* marker_texture,
                                     std::uint32_t target_width,
                                     std::uint32_t target_height,
                                     std::vector<GpuSpriteDrawPacket>& out_packets,
                                     std::string& out_error) {
    out_packets.clear();
    out_error.clear();
    if (!marker_texture) {
        if (!markers.empty()) {
            out_error = "Missing floor marker texture.";
            return false;
        }
        return true;
    }
    out_packets.reserve(markers.size());
    const float output_w = static_cast<float>(std::max<std::uint32_t>(1u, target_width));
    const float output_h = static_cast<float>(std::max<std::uint32_t>(1u, target_height));
    std::uintptr_t marker_sequence = 0u;

    for (const Assets::DevFloorProjectionMarker& marker : markers) {
        const float extent = std::max(1.0f, marker.world_half_extent);
        const float left = marker.floor_world_xz.x - extent;
        const float right = marker.floor_world_xz.x + extent;
        const float top_z = marker.floor_world_xz.y - extent;
        const float bottom_z = marker.floor_world_xz.y + extent;

        SDL_FPoint screen_tl{};
        SDL_FPoint screen_tr{};
        SDL_FPoint screen_br{};
        SDL_FPoint screen_bl{};
        if (!camera.project_world_point(SDL_FPoint{left, 0.0f}, top_z, screen_tl) ||
            !camera.project_world_point(SDL_FPoint{right, 0.0f}, top_z, screen_tr) ||
            !camera.project_world_point(SDL_FPoint{right, 0.0f}, bottom_z, screen_br) ||
            !camera.project_world_point(SDL_FPoint{left, 0.0f}, bottom_z, screen_bl)) {
            continue;
        }

        GpuSpriteDrawPacket packet{};
        packet.source_texture = marker_texture;
        packet.source_asset_name = "<floor-marker>";
        packet.source_animation_name = "<floor-marker>";
        packet.source_texture_id =
            "floor_marker_texture_ptr=" + std::to_string(reinterpret_cast<std::uintptr_t>(marker_texture));
        packet.source_frame_index = -1;
        packet.source_variant_index = -1;
        packet.modulate = SDL_FColor{
            static_cast<float>(marker.color.r) / 255.0f,
            static_cast<float>(marker.color.g) / 255.0f,
            static_cast<float>(marker.color.b) / 255.0f,
            static_cast<float>(marker.color.a) / 255.0f,
        };
        packet.projected_foot_y_key = std::max(screen_br.y, screen_bl.y);
        packet.camera_depth_key = marker.floor_world_xz.y;
        packet.stable_sort_id = floor_sort_id(true, marker_sequence++);
        packet.is_floor_packet = true;
        packet.depth_layer = 0;
        fill_quad_packet_vertices(screen_tl,
                                  screen_tr,
                                  screen_br,
                                  screen_bl,
                                  0.0f,
                                  0.0f,
                                  1.0f,
                                  1.0f,
                                  output_w,
                                  output_h,
                                  packet);
        out_packets.push_back(packet);
    }

    std::stable_sort(out_packets.begin(), out_packets.end(), opengl_runtime_renderer_detail::draw_packet_sort_predicate_floor);
    return true;
}

} // namespace opengl_runtime_renderer_detail

bool opengl_runtime_renderer_detail::build_xy_sprite_draw_packets(
    const WarpedScreenGrid& camera,
    const std::vector<Asset*>& visible_assets,
    std::uint32_t target_width,
    std::uint32_t target_height,
    std::vector<GpuSpriteDrawPacket>& out_xy_sprite_draws,
    std::string& out_error) {
    out_xy_sprite_draws.clear();
    out_xy_sprite_draws.reserve(visible_assets.size());
    out_error.clear();

    const float output_w = static_cast<float>(std::max<std::uint32_t>(1u, target_width));
    const float output_h = static_cast<float>(std::max<std::uint32_t>(1u, target_height));
    std::uintptr_t xy_sequence = 0u;

    for (Asset* asset : visible_assets) {
        if (!asset || asset->dead || !asset->info) {
            continue;
        }
        if (!opengl_runtime_renderer_detail::info_is_xy_sprite_pass_eligible(asset->info->tillable)) {
            continue;
        }

        emit_floor_pass_tag_ignored_diagnostic(asset);

        RenderObject object{};
        if (!render_build::build_direct_asset_render_object(asset, object) || !object.texture) {
            continue;
        }

        const Asset::PerspectiveSample perspective = asset->runtime_perspective_sample();
        const float perspective_scale = perspective.scale;
        const float world_z = static_cast<float>(asset->world_z()) + object.world_z_offset;
        const double camera_depth_key = compute_asset_camera_depth_key(camera, *asset);

        render_projection::ProjectedSpriteFrame projected{};
        if (!render_projection::build_render_object_projected_frame(camera,
                                                                    object,
                                                                    perspective_scale,
                                                                    world_z,
                                                                    projected) ||
            !projected.valid) {
            continue;
        }

        float u0 = 0.0f;
        float v0 = 0.0f;
        float u1 = 1.0f;
        float v1 = 1.0f;
        if (object.has_src_rect && object.atlas_w > 0 && object.atlas_h > 0) {
            const float atlas_w = static_cast<float>(object.atlas_w);
            const float atlas_h = static_cast<float>(object.atlas_h);
            u0 = static_cast<float>(object.src_rect.x) / atlas_w;
            v0 = static_cast<float>(object.src_rect.y) / atlas_h;
            u1 = static_cast<float>(object.src_rect.x + object.src_rect.w) / atlas_w;
            v1 = static_cast<float>(object.src_rect.y + object.src_rect.h) / atlas_h;
        }
        if ((object.flip & SDL_FLIP_HORIZONTAL) != 0) {
            std::swap(u0, u1);
        }
        if ((object.flip & SDL_FLIP_VERTICAL) != 0) {
            std::swap(v0, v1);
        }

        GpuSpriteDrawPacket packet{};
        packet.source_texture = object.texture;
        packet.source_asset_name = asset->info ? asset->info->name : "<unknown-asset>";
        packet.source_animation_name = asset ? asset->current_animation : std::string{};
        packet.source_texture_id = "sdl_texture_ptr=" + std::to_string(reinterpret_cast<std::uintptr_t>(object.texture));
        packet.source_frame_index = asset && asset->current_frame ? asset->current_frame->frame_index : -1;
        packet.source_variant_index = asset ? asset->current_variant_index : -1;
        packet.modulate = SDL_FColor{
            static_cast<float>(object.color_mod.r) / 255.0f,
            static_cast<float>(object.color_mod.g) / 255.0f,
            static_cast<float>(object.color_mod.b) / 255.0f,
            static_cast<float>(object.color_mod.a) / 255.0f,
        };
        packet.projected_foot_y_key = std::max(projected.screen_bl.y, projected.screen_br.y);
        packet.camera_depth_key = static_cast<float>(camera_depth_key);
        packet.stable_sort_id = xy_sequence++;
        packet.is_floor_packet = false;
        packet.depth_layer = opengl_runtime_renderer_detail::classify_depth_layer_for_asset(camera, *asset);
        fill_sprite_packet_vertices(projected, u0, v0, u1, v1, output_w, output_h, packet);
        out_xy_sprite_draws.push_back(packet);
    }

    std::stable_sort(out_xy_sprite_draws.begin(), out_xy_sprite_draws.end(), opengl_runtime_renderer_detail::draw_packet_sort_predicate_xy);
    return true;
}

const std::vector<Asset*>& opengl_runtime_renderer_detail::select_visible_assets_for_gpu_frame(
    bool dev_mode,
    bool focus_filter_active,
    const std::vector<Asset*>& active_assets,
    const std::vector<Asset*>& filtered_active_assets,
    bool& out_used_active_fallback) {
    out_used_active_fallback = false;
    if (!dev_mode) {
        return active_assets;
    }
    if (!focus_filter_active) {
        return active_assets;
    }
    if (!filtered_active_assets.empty()) {
        return filtered_active_assets;
    }
    if (!active_assets.empty()) {
        out_used_active_fallback = true;
        return active_assets;
    }
    return filtered_active_assets;
}


void OpenGLRuntimeRenderer::RenderTargetLifecycleManager::set_requested_size(int screen_width,
                                                                             int screen_height) {
    requested_width = std::max(1, screen_width);
    requested_height = std::max(1, screen_height);
    active_width = requested_width;
    active_height = requested_height;
}

bool OpenGLRuntimeRenderer::RenderTargetLifecycleManager::synchronize_to_output(int width,
                                                                               int height,
                                                                               std::string& out_error) {
    out_error.clear();
    if (width <= 0 || height <= 0) {
        out_error = "Output dimensions are invalid.";
        return false;
    }
    active_width = width;
    active_height = height;
    return true;
}

std::optional<SDL_Point> OpenGLRuntimeRenderer::RenderTargetLifecycleManager::current_size() const {
    const int width = active_width > 0 ? active_width : requested_width;
    const int height = active_height > 0 ? active_height : requested_height;
    if (width <= 0 || height <= 0) {
        return std::nullopt;
    }
    return SDL_Point{width, height};
}

OpenGLRuntimeRenderer::OpenGLRuntimeRenderer(SDL_Renderer* renderer,
                                             Assets* assets,
                                             int screen_width,
                                             int screen_height)
    : renderer_(renderer),
      assets_(assets),
      screen_width_(std::max(1, screen_width)),
      screen_height_(std::max(1, screen_height)) {
    render_target_manager_.set_requested_size(screen_width_, screen_height_);
}

OpenGLRuntimeRenderer::~OpenGLRuntimeRenderer() {
    destroy_render_targets();
}

std::unique_ptr<OpenGLRuntimeRenderer> OpenGLRuntimeRenderer::Create(SDL_Renderer* renderer,
                                                                     Assets* assets,
                                                                     int screen_width,
                                                                     int screen_height,
                                                                     std::string& out_error) {
    auto runtime_renderer = std::unique_ptr<OpenGLRuntimeRenderer>(
        new OpenGLRuntimeRenderer(renderer, assets, screen_width, screen_height));
    if (!runtime_renderer->initialize(out_error)) {
        return nullptr;
    }
    return runtime_renderer;
}

bool OpenGLRuntimeRenderer::initialize(std::string& out_error) {
    out_error.clear();
    if (!renderer_) {
        out_error = "Renderer is null.";
        return false;
    }
    renderer_name_ = renderer_name_.empty() ? std::string("unknown") : renderer_name_;
    if (const char* name = SDL_GetRendererName(renderer_)) {
        renderer_name_ = name;
    }

    int vsync = 0;
    if (SDL_GetRenderVSync(renderer_, &vsync)) {
        present_mode_name_ = vsync != 0 ? "vsync" : "immediate";
    } else {
        present_mode_name_ = "unknown";
    }

    SDL_SetDefaultTextureScaleMode(renderer_, SDL_SCALEMODE_LINEAR);
    render_target_manager_.set_requested_size(screen_width_, screen_height_);

    std::string size_error;
    if (!render_target_manager_.synchronize_to_output(screen_width_, screen_height_, size_error)) {
        out_error = size_error;
        return false;
    }
    return true;
}

void OpenGLRuntimeRenderer::destroy_render_targets() {
    render_diagnostics::destroy_texture(floor_target_);
    render_diagnostics::destroy_texture(xy_sprite_target_);
    render_diagnostics::destroy_texture(composite_target_);
    render_diagnostics::destroy_texture(floor_marker_texture_);
    for (auto& entry : depth_layer_targets_) {
        render_diagnostics::destroy_texture(entry.second);
    }
    depth_layer_targets_.clear();
    cached_depth_layer_ids_.clear();
    output_target_width_ = 1;
    output_target_height_ = 1;
}

SDL_Texture* OpenGLRuntimeRenderer::create_render_target(SDL_Renderer* renderer,
                                                         int width,
                                                         int height,
                                                         const std::string& label,
                                                         std::string& out_error) {
    out_error.clear();
    if (!renderer || width <= 0 || height <= 0) {
        out_error = "Invalid render target size for " + label + ".";
        return nullptr;
    }

    SDL_Texture* texture = render_diagnostics::create_texture(renderer,
                                                              SDL_PIXELFORMAT_RGBA32,
                                                              SDL_TEXTUREACCESS_TARGET,
                                                              width,
                                                              height);
    if (!texture) {
        out_error = "Failed to create render target '" + label + "': " + safe_string(SDL_GetError());
        return nullptr;
    }

    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    return texture;
}

SDL_Texture* OpenGLRuntimeRenderer::create_solid_white_texture(SDL_Renderer* renderer,
                                                               std::string& out_error) {
    out_error.clear();
    if (!renderer) {
        out_error = "Renderer is null for solid white texture creation.";
        return nullptr;
    }

    SDL_Texture* texture = render_diagnostics::create_texture(renderer,
                                                              SDL_PIXELFORMAT_RGBA32,
                                                              SDL_TEXTUREACCESS_STATIC,
                                                              1,
                                                              1);
    if (!texture) {
        out_error = "Failed to create floor marker texture: " + safe_string(SDL_GetError());
        return nullptr;
    }

    const std::uint32_t white_pixel = 0xFFFFFFFFu;
    if (!SDL_UpdateTexture(texture, nullptr, &white_pixel, sizeof(white_pixel))) {
        out_error = "Failed to upload floor marker texture pixel: " + safe_string(SDL_GetError());
        render_diagnostics::destroy_texture(texture);
        return nullptr;
    }

    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    return texture;
}

void OpenGLRuntimeRenderer::configure_render_target(SDL_Texture* texture) {
    if (!texture) {
        return;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
}

SDL_FPoint OpenGLRuntimeRenderer::clip_to_screen(float clip_x, float clip_y, float target_width, float target_height) {
    const float width = std::max(1.0f, target_width);
    const float height = std::max(1.0f, target_height);
    return SDL_FPoint{
        (clip_x + 1.0f) * 0.5f * width,
        (1.0f - clip_y) * 0.5f * height
    };
}

void OpenGLRuntimeRenderer::packet_to_vertices(const GpuSpriteDrawPacket& packet,
                                               std::uint32_t target_width,
                                               std::uint32_t target_height,
                                               std::array<SDL_Vertex, 6>& out_vertices) {
    fill_geometry_vertices(packet, target_width, target_height, out_vertices);
}

void OpenGLRuntimeRenderer::set_output_dimensions(int screen_width, int screen_height) {
    screen_width_ = std::max(1, screen_width);
    screen_height_ = std::max(1, screen_height);
    if (renderer_) {
        const int max_texture_size = renderer_max_texture_size(renderer_);
        screen_width_ = clamp_dimension_to_renderer_limit(screen_width_, max_texture_size, "scene width");
        screen_height_ = clamp_dimension_to_renderer_limit(screen_height_, max_texture_size, "scene height");
    }
    render_target_manager_.set_requested_size(screen_width_, screen_height_);
    std::string size_error;
    (void)render_target_manager_.synchronize_to_output(screen_width_, screen_height_, size_error);
}

std::optional<SDL_Point> OpenGLRuntimeRenderer::scene_target_size() const {
    return render_target_manager_.current_size();
}

const std::string& OpenGLRuntimeRenderer::present_mode() const {
    return present_mode_name_;
}

const std::string& OpenGLRuntimeRenderer::backend_name() const {
    return renderer_name_;
}

std::vector<world::Chunk*> OpenGLRuntimeRenderer::runtime_floor_chunks() const {
    if (!assets_) {
        return {};
    }
    const std::vector<world::Chunk*>& active_chunks = assets_->active_chunks();
    if (!active_chunks.empty()) {
        bool active_has_tiles = false;
        for (const world::Chunk* chunk : active_chunks) {
            if (chunk && !chunk->tiles.empty()) {
                active_has_tiles = true;
                break;
            }
        }
        if (active_has_tiles) {
            return std::vector<world::Chunk*>(active_chunks.begin(), active_chunks.end());
        }
        vibble::log::warn("[OpenGLRuntimeRenderer] Active chunk set had no floor tiles; falling back to all world chunks.");
    }
    return assets_->world_grid().all_chunks();
}

bool OpenGLRuntimeRenderer::build_gpu_scene_frame_data(std::uint32_t target_width,
                                                       std::uint32_t target_height,
                                                       GpuSceneFrameData& out_data,
                                                       std::string& out_error) const {
    // Two-pass content contract (must match render_frame merge order):
    // 1. Floor Pass (XZ only): background clear + chunk floor tiles + dev floor projection markers.
    // 2. XY Sprite Pass: non-tiled XY sprite assets only, grouped by depth layer.
    // 3. Merge Stage: floor target + xy sprite target + ui overlay target into composite.
    out_data = GpuSceneFrameData{};
    out_error.clear();
    if (!assets_) {
        out_error = "Assets context unavailable for OpenGL scene frame build.";
        return false;
    }

    const std::vector<Asset*>& active_assets = assets_->getActive();
    const std::vector<Asset*>& filtered_active_assets = assets_->getFilteredActiveAssets();
    const std::vector<Asset*>& all_assets = assets_->all;
    bool used_active_fallback = false;
    const std::vector<Asset*>& selected_visible_assets =
        opengl_runtime_renderer_detail::select_visible_assets_for_gpu_frame(
            assets_->is_dev_mode(),
            assets_->focus_filter_active(),
            active_assets,
            filtered_active_assets,
            used_active_fallback);
    const std::vector<Asset*>* visible_assets = &selected_visible_assets;
    bool used_all_assets_visibility_fallback = false;
    if (visible_assets->empty() && !all_assets.empty()) {
        visible_assets = &all_assets;
        used_all_assets_visibility_fallback = true;
    }
    const WarpedScreenGrid& camera = assets_->getView();
    const std::size_t traversal_count = camera.visible_traversal_entries().size();

    std::vector<Asset*> render_assets = *visible_assets;
    if (assets_->boundary_assets_visible() && assets_->is_dev_mode() && assets_->focus_filter_active()) {
        std::unordered_set<Asset*> selected_assets(render_assets.begin(), render_assets.end());
        for (Asset* asset : active_assets) {
            if (!asset || !asset->info) {
                continue;
            }
            if (asset_types::canonicalize(asset->info->type) != std::string(asset_types::boundary)) {
                continue;
            }
            if (selected_assets.insert(asset).second) {
                render_assets.push_back(asset);
            }
        }
    }

    out_data.target_width = target_width;
    out_data.target_height = target_height;
    out_data.active_asset_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        active_assets.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.filtered_active_asset_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        filtered_active_assets.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.selected_asset_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        render_assets.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.visible_traversal_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        traversal_count, static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.dev_mode = assets_->is_dev_mode();
    out_data.focus_filter_active = assets_->focus_filter_active();
    out_data.used_active_asset_fallback = used_active_fallback;

    if (used_active_fallback) {
        vibble::log::warn("[OpenGLRuntimeRenderer] Dev filtered render list was empty; using active assets for this frame. "
                          "active_count=" + std::to_string(active_assets.size()) +
                          " filtered_count=" + std::to_string(filtered_active_assets.size()) +
                          " focus_filter_active=" + std::string(assets_->focus_filter_active() ? "true" : "false") +
                          " traversal_count=" + std::to_string(traversal_count));
    }

    if (!opengl_runtime_renderer_detail::build_floor_tile_draw_packets(
            camera,
            runtime_floor_chunks(),
            target_width,
            target_height,
            out_data.floor_draws)) {
        out_error = "Failed to build map-floor tile packets.";
        return false;
    }

    std::vector<GpuSpriteDrawPacket> floor_marker_draws{};
    const std::vector<Assets::DevFloorProjectionMarker> floor_markers = assets_->dev_floor_projection_markers();
    if (!opengl_runtime_renderer_detail::build_floor_marker_draw_packets(camera,
                                                                         floor_markers,
                                                                         floor_marker_texture_,
                                                                         target_width,
                                                                         target_height,
                                                                         floor_marker_draws,
                                                                         out_error)) {
        return false;
    }
    if (!floor_marker_draws.empty()) {
        out_data.floor_draws.insert(out_data.floor_draws.end(),
                                    std::make_move_iterator(floor_marker_draws.begin()),
                                    std::make_move_iterator(floor_marker_draws.end()));
        std::stable_sort(out_data.floor_draws.begin(),
                         out_data.floor_draws.end(),
                         opengl_runtime_renderer_detail::draw_packet_sort_predicate_floor);
    }

    if (!opengl_runtime_renderer_detail::build_xy_sprite_draw_packets(
            camera,
            render_assets,
            target_width,
            target_height,
            out_data.xy_sprite_draws,
            out_error)) {
        return false;
    }

    // Emergency fallback for movement-time visibility/culling gaps: retry sprite packet build
    // against all tracked assets before presenting a floor-only frame.
    if (out_data.xy_sprite_draws.empty() && !all_assets.empty() && !used_all_assets_visibility_fallback) {
        std::vector<GpuSpriteDrawPacket> fallback_xy_sprite_draws{};
        std::string fallback_error;
        if (!opengl_runtime_renderer_detail::build_xy_sprite_draw_packets(
                camera,
                all_assets,
                target_width,
                target_height,
                fallback_xy_sprite_draws,
                fallback_error)) {
            out_error = fallback_error;
            return false;
        }
        if (!fallback_xy_sprite_draws.empty()) {
            out_data.xy_sprite_draws = std::move(fallback_xy_sprite_draws);
            out_data.selected_asset_count = static_cast<std::uint32_t>(std::min<std::size_t>(
                all_assets.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
            vibble::log::warn("[OpenGLRuntimeRenderer] Rebuilt XY sprite packets from all assets after primary visibility pass produced no XY packets.");
        }
    }

    out_data.floor_draw_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        out_data.floor_draws.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.xy_sprite_draw_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        out_data.xy_sprite_draws.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));

    std::unordered_map<int, std::vector<GpuSpriteDrawPacket>> depth_xy_sprite_packets{};
    depth_xy_sprite_packets.reserve(out_data.xy_sprite_draws.size());
    for (const GpuSpriteDrawPacket& packet : out_data.xy_sprite_draws) {
        depth_xy_sprite_packets[packet.depth_layer].push_back(packet);
    }

    std::vector<int> depth_layer_ids{};
    depth_layer_ids.reserve(depth_xy_sprite_packets.size());
    for (const auto& entry : depth_xy_sprite_packets) {
        depth_layer_ids.push_back(entry.first);
    }
    std::sort(depth_layer_ids.begin(), depth_layer_ids.end(), [](int lhs, int rhs) {
        return lhs > rhs;
    });

    out_data.depth_layers.clear();
    out_data.depth_layers.reserve(depth_layer_ids.size());
    int max_layer_distance = 0;
    for (int layer_id : depth_layer_ids) {
        max_layer_distance = std::max(max_layer_distance, std::abs(layer_id));
    }
    for (int layer_id : depth_layer_ids) {
        GpuDepthLayerDrawPackets layer{};
        layer.depth_layer = layer_id;
        layer.packets = std::move(depth_xy_sprite_packets[layer_id]);
        std::stable_sort(layer.packets.begin(), layer.packets.end(), opengl_runtime_renderer_detail::draw_packet_sort_predicate_xy);
        const float blur_distance = max_layer_distance > 0
            ? static_cast<float>(std::abs(layer_id)) / static_cast<float>(max_layer_distance)
            : 0.0f;
        const float blur_strength = std::clamp(
            static_cast<float>(assets_->getView().get_settings().blur_px) * blur_distance,
            0.0f,
            static_cast<float>(assets_->getView().get_settings().blur_px));
        layer.blur_strength_px = blur_strength;
        out_data.depth_layers.push_back(std::move(layer));
    }

    out_data.active_depth_layer_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        out_data.depth_layers.size(),
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.debug_overlay_draw_count = 0;
    out_data.has_valid_composite_source = true;

    render_diagnostics::set_pass_packet_counts(out_data.floor_draw_count, out_data.xy_sprite_draw_count);
    render_diagnostics::set_active_depth_layer_count(out_data.active_depth_layer_count);
    render_diagnostics::set_gpu_scene_packet_stats(out_data.floor_draw_count,
                                                   out_data.xy_sprite_draw_count,
                                                   out_data.has_valid_composite_source);
    {
        std::ostringstream packets_summary;
        std::ostringstream blur_summary;
        bool first = true;
        for (const GpuDepthLayerDrawPackets& layer : out_data.depth_layers) {
            if (!first) {
                packets_summary << ", ";
                blur_summary << ", ";
            }
            first = false;
            packets_summary << layer.depth_layer << '=' << layer.packets.size();
            blur_summary << layer.depth_layer << '=' << layer.blur_strength_px;
        }
        render_diagnostics::set_packets_per_depth_layer(packets_summary.str());
        render_diagnostics::set_blur_strength_per_layer(blur_summary.str());
        render_diagnostics::set_blur_pass_count(0);
    }

    out_data.suspected_incomplete_scene =
        out_data.xy_sprite_draw_count == 0 &&
        out_data.active_asset_count > 0 &&
        (out_data.selected_asset_count > 0 || out_data.visible_traversal_count > 0);

    return true;
}

bool OpenGLRuntimeRenderer::ensure_render_targets(const GpuSceneFrameData& frame_data, std::string& out_error) {
    out_error.clear();
    const int width = static_cast<int>(frame_data.target_width);
    const int height = static_cast<int>(frame_data.target_height);
    if (width <= 0 || height <= 0) {
        out_error = "Invalid OpenGL scene target size.";
        return false;
    }

    const bool size_changed = width != output_target_width_ || height != output_target_height_;
    if (!size_changed && floor_target_ && xy_sprite_target_ && composite_target_ && floor_marker_texture_) {
        return true;
    }

    destroy_render_targets();

    output_target_width_ = width;
    output_target_height_ = height;

    floor_target_ = create_render_target(renderer_, width, height, "floor", out_error);
    if (!floor_target_) {
        destroy_render_targets();
        return false;
    }

    xy_sprite_target_ = create_render_target(renderer_, width, height, "xy_sprite", out_error);
    if (!xy_sprite_target_) {
        destroy_render_targets();
        return false;
    }

    composite_target_ = create_render_target(renderer_, width, height, "composite", out_error);
    if (!composite_target_) {
        destroy_render_targets();
        return false;
    }

    floor_marker_texture_ = create_solid_white_texture(renderer_, out_error);
    if (!floor_marker_texture_) {
        destroy_render_targets();
        return false;
    }

    return true;
}

bool OpenGLRuntimeRenderer::render_packet_batch(const std::vector<GpuSpriteDrawPacket>& packets,
                                                std::uint32_t target_width,
                                                std::uint32_t target_height,
                                                std::string& out_error) {
    out_error.clear();
    if (packets.empty()) {
        return true;
    }
    if (!renderer_) {
        out_error = "Renderer is null.";
        return false;
    }

    std::array<SDL_Vertex, 6> vertices{};
    for (const GpuSpriteDrawPacket& packet : packets) {
        if (!packet.source_texture) {
            out_error = "Missing source SDL texture; " + describe_draw_packet_source(packet);
            render_diagnostics::add_skipped_texture_count();
            render_diagnostics::set_failed_texture_names(describe_draw_packet_source(packet));
            return false;
        }
        packet_to_vertices(packet, target_width, target_height, vertices);
        if (!render_diagnostics::render_geometry(renderer_,
                                                 packet.source_texture,
                                                 vertices.data(),
                                                 static_cast<int>(vertices.size()),
                                                 nullptr,
                                                 0)) {
            out_error = "SDL_RenderGeometry failed for " + describe_draw_packet_source(packet) +
                        ": " + safe_string(SDL_GetError());
            return false;
        }
    }

    return true;
}

bool OpenGLRuntimeRenderer::render_frame(std::string& out_error, SDL_Texture* ui_overlay_texture) {
    out_error.clear();
    render_diagnostics::begin_frame();
    render_diagnostics::set_texture_memory_usage(render_diagnostics::tracked_texture_bytes(), false);

    if (!renderer_ || !assets_ || screen_width_ <= 0 || screen_height_ <= 0) {
        out_error = "OpenGL runtime renderer is not ready.";
        render_diagnostics::set_submit_result(false);
        render_diagnostics::end_frame();
        return false;
    }

    std::string size_error;
    if (!render_target_manager_.synchronize_to_output(screen_width_, screen_height_, size_error)) {
        out_error = size_error;
        render_diagnostics::set_submit_result(false);
        render_diagnostics::end_frame();
        return false;
    }

    const std::optional<SDL_Point> effective_target = render_target_manager_.current_size();
    if (!effective_target.has_value() || effective_target->x <= 0 || effective_target->y <= 0) {
        out_error = "Invalid render target dimensions.";
        render_diagnostics::set_submit_result(false);
        render_diagnostics::end_frame();
        return false;
    }

    GpuSceneFrameData target_size_only{};
    target_size_only.target_width = static_cast<std::uint32_t>(effective_target->x);
    target_size_only.target_height = static_cast<std::uint32_t>(effective_target->y);
    if (!ensure_render_targets(target_size_only, out_error)) {
        render_diagnostics::set_submit_result(false);
        render_diagnostics::end_frame();
        return false;
    }

    if (last_complete_scene_frame_data_.has_value() &&
        (last_complete_scene_width_ != static_cast<std::uint32_t>(effective_target->x) ||
         last_complete_scene_height_ != static_cast<std::uint32_t>(effective_target->y))) {
        last_complete_scene_frame_data_.reset();
        last_complete_scene_width_ = 0;
        last_complete_scene_height_ = 0;
        consecutive_held_incomplete_scene_frames_ = 0;
    }

    GpuSceneFrameData frame_data{};
    if (!build_gpu_scene_frame_data(static_cast<std::uint32_t>(effective_target->x),
                                    static_cast<std::uint32_t>(effective_target->y),
                                    frame_data,
                                    out_error)) {
        render_diagnostics::set_submit_result(false);
        render_diagnostics::end_frame();
        return false;
    }

    if (ui_overlay_texture) {
        frame_data.ui_overlay_texture = ui_overlay_texture;
    }
    const SDL_Color floor_clear_color = update_smoothed_floor_clear_color(resolve_runtime_floor_clear_color());

    const bool can_hold_previous_scene =
        last_complete_scene_frame_data_.has_value() &&
        last_complete_scene_width_ == frame_data.target_width &&
        last_complete_scene_height_ == frame_data.target_height;
    const bool hold_incomplete_scene_frame =
        frame_data.suspected_incomplete_scene && can_hold_previous_scene;
    const bool hold_zero_sprite_scene_frame =
        frame_data.xy_sprite_draw_count == 0 &&
        can_hold_previous_scene &&
        last_complete_scene_frame_data_.has_value() &&
        last_complete_scene_frame_data_->xy_sprite_draw_count > 0;
    const bool hold_empty_scene_frame =
        frame_data.floor_draw_count == 0 &&
        frame_data.xy_sprite_draw_count == 0 &&
        can_hold_previous_scene;

    GpuSceneFrameData held_frame_data{};
    const GpuSceneFrameData* frame_to_render = &frame_data;
    if (hold_incomplete_scene_frame || hold_zero_sprite_scene_frame || hold_empty_scene_frame) {
        held_frame_data = *last_complete_scene_frame_data_;
        held_frame_data.ui_overlay_texture = frame_data.ui_overlay_texture;
        held_frame_data.ui_overlay_gpu_texture = frame_data.ui_overlay_gpu_texture;
        held_frame_data.debug_overlay_draw_count = frame_data.debug_overlay_draw_count;
        frame_to_render = &held_frame_data;

        ++consecutive_held_incomplete_scene_frames_;
    } else {
        consecutive_held_incomplete_scene_frames_ = 0;
    }

    render_diagnostics::set_renderer_runtime_info("opengl", backend_name(), present_mode());
    render_diagnostics::set_floor_pass_target_dimensions(frame_to_render->target_width, frame_to_render->target_height);
    render_diagnostics::set_xy_sprite_pass_target_dimensions(frame_to_render->target_width, frame_to_render->target_height);
    render_diagnostics::set_gpu_scene_packet_stats(frame_to_render->floor_draw_count,
                                                   frame_to_render->xy_sprite_draw_count,
                                                   frame_to_render->has_valid_composite_source);
    render_diagnostics::set_clear_executed(true);
    render_diagnostics::set_submit_result(false);

    const SDL_FRect full_rect{0.0f,
                              0.0f,
                              static_cast<float>(frame_to_render->target_width),
                              static_cast<float>(frame_to_render->target_height)};
    if (!ensure_render_targets(*frame_to_render, out_error)) {
        render_diagnostics::end_frame();
        return false;
    }

    auto bind_target = [&](SDL_Texture* target, const SDL_Color& clear_color) -> bool {
        if (!render_diagnostics::set_render_target(renderer_, target)) {
            out_error = std::string("Failed to bind ") + (target ? "render target" : "backbuffer") +
                        ": " + safe_string(SDL_GetError());
            return false;
        }
        SDL_SetRenderViewport(renderer_, nullptr);
        SDL_SetRenderClipRect(renderer_, nullptr);
        SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
        SDL_SetRenderDrawColor(renderer_, clear_color.r, clear_color.g, clear_color.b, clear_color.a);
        SDL_RenderClear(renderer_);
        return true;
    };

    if (!bind_target(floor_target_, floor_clear_color)) {
        render_diagnostics::end_frame();
        return false;
    }
    if (!render_packet_batch(frame_to_render->floor_draws,
                             frame_to_render->target_width,
                             frame_to_render->target_height,
                             out_error)) {
        render_diagnostics::end_frame();
        return false;
    }

    const SDL_Color transparent_clear{0, 0, 0, 0};
    if (!bind_target(xy_sprite_target_, transparent_clear)) {
        render_diagnostics::end_frame();
        return false;
    }

    std::ostringstream xy_depth_layer_summary;
    bool first_layer = true;
    for (const GpuDepthLayerDrawPackets& layer : frame_to_render->depth_layers) {
        if (!first_layer) {
            xy_depth_layer_summary << ", ";
        }
        first_layer = false;
        xy_depth_layer_summary << layer.depth_layer << '(' << layer.packets.size() << ')';
        if (!render_packet_batch(layer.packets,
                                 frame_to_render->target_width,
                                 frame_to_render->target_height,
                                 out_error)) {
            render_diagnostics::end_frame();
            return false;
        }
    }

    if (!bind_target(composite_target_, transparent_clear)) {
        render_diagnostics::end_frame();
        return false;
    }

    const SDL_FRect floor_full_rect{0.0f,
                                    0.0f,
                                    static_cast<float>(frame_to_render->target_width),
                                    static_cast<float>(frame_to_render->target_height)};
    if (floor_target_) {
        if (!render_diagnostics::render_texture(renderer_, floor_target_, nullptr, &floor_full_rect)) {
            out_error = "Failed to composite floor pass target: " + safe_string(SDL_GetError());
            render_diagnostics::end_frame();
            return false;
        }
    }
    if (xy_sprite_target_) {
        if (!render_diagnostics::render_texture(renderer_, xy_sprite_target_, nullptr, &floor_full_rect)) {
            out_error = "Failed to composite XY sprite pass target: " + safe_string(SDL_GetError());
            render_diagnostics::end_frame();
            return false;
        }
    }
    if (frame_to_render->ui_overlay_texture) {
        if (!render_diagnostics::render_texture(renderer_, frame_to_render->ui_overlay_texture, nullptr, &full_rect)) {
            out_error = "Failed to composite UI overlay texture: " + safe_string(SDL_GetError());
            render_diagnostics::end_frame();
            return false;
        }
    }

    if (!bind_target(nullptr, floor_clear_color)) {
        render_diagnostics::end_frame();
        return false;
    }
    if (!render_diagnostics::render_texture(renderer_, composite_target_, nullptr, &full_rect)) {
        out_error = "Failed to present composite target: " + safe_string(SDL_GetError());
        render_diagnostics::end_frame();
        return false;
    }

    render_diagnostics::set_composite_layers_submitted("floor_pass->xy_sprite_pass->ui_overlay");
    render_diagnostics::set_submit_result(true);

    if (!hold_incomplete_scene_frame &&
        !hold_zero_sprite_scene_frame &&
        !hold_empty_scene_frame &&
        (frame_data.xy_sprite_draw_count > 0 || frame_data.floor_draw_count > 0)) {
        last_complete_scene_frame_data_ = frame_data;
        last_complete_scene_width_ = frame_data.target_width;
        last_complete_scene_height_ = frame_data.target_height;
    }

    render_diagnostics::end_frame();
    return true;
}

SDL_Color OpenGLRuntimeRenderer::resolve_runtime_floor_clear_color() const {
    SDL_Color map_default{0, 0, 0, 255};
    if (!assets_) {
        return map_default;
    }
    const nlohmann::json& map_info = assets_->map_info_json();
    if (map_info.is_object() && map_info.contains("dev_map_settings") && map_info["dev_map_settings"].is_object()) {
        const nlohmann::json& dev = map_info["dev_map_settings"];
        if (dev.contains("default_floor_color")) {
            if (auto parsed = utils::color::color_from_json(dev["default_floor_color"])) {
                map_default = *parsed;
                map_default.a = 255;
            }
        } else if (dev.contains("map_color")) {
            if (auto parsed = utils::color::color_from_json(dev["map_color"])) {
                map_default = *parsed;
                map_default.a = 255;
            }
        }
    }

    const Asset* player = assets_->player;
    if (!player) {
        return map_default;
    }

    const float px = static_cast<float>(player->world_x());
    const float pz = static_cast<float>(player->world_z());
    const std::vector<Room*>& rooms = assets_->rooms();
    if (rooms.empty()) {
        return map_default;
    }

    double weighted_r = 0.0;
    double weighted_g = 0.0;
    double weighted_b = 0.0;
    double total_weight = 0.0;
    for (Room* room : rooms) {
        if (!room || !room->room_area) {
            continue;
        }
        const SDL_Point c = room->room_area->get_center();
        const float dx = px - static_cast<float>(c.x);
        const float dz = pz - static_cast<float>(c.y);
        const float distance = std::sqrt(dx * dx + dz * dz);
        const float t = std::clamp(distance / kFloorBlendRadius, 0.0f, 1.0f);
        const float weight = 1.0f - t;
        if (weight <= 0.0f) {
            continue;
        }
        SDL_Color room_color = room->inherit_map_floor_color() ? map_default : room->room_floor_color(map_default);
        weighted_r += static_cast<double>(room_color.r) * static_cast<double>(weight);
        weighted_g += static_cast<double>(room_color.g) * static_cast<double>(weight);
        weighted_b += static_cast<double>(room_color.b) * static_cast<double>(weight);
        total_weight += static_cast<double>(weight);
    }

    if (total_weight <= 1e-6) {
        return map_default;
    }
    SDL_Color out{
        static_cast<Uint8>(std::clamp<int>(static_cast<int>(std::lround(weighted_r / total_weight)), 0, 255)),
        static_cast<Uint8>(std::clamp<int>(static_cast<int>(std::lround(weighted_g / total_weight)), 0, 255)),
        static_cast<Uint8>(std::clamp<int>(static_cast<int>(std::lround(weighted_b / total_weight)), 0, 255)),
        255};
    return out;
}

SDL_Color OpenGLRuntimeRenderer::update_smoothed_floor_clear_color(SDL_Color target) {
    target.a = 255;
    const Asset* player = assets_ ? assets_->player : nullptr;
    SDL_Point player_xz{0, 0};
    bool has_player = false;
    if (player) {
        player_xz = SDL_Point{player->world_x(), player->world_z()};
        has_player = true;
    }

    if (!smoothed_floor_color_valid_) {
        smoothed_floor_color_valid_ = true;
        smoothed_floor_clear_color_ = target;
        if (has_player) {
            last_floor_color_player_xz_ = player_xz;
            last_floor_color_player_xz_valid_ = true;
        }
        return smoothed_floor_clear_color_;
    }

    bool moved = false;
    if (has_player) {
        if (!last_floor_color_player_xz_valid_) {
            moved = true;
            last_floor_color_player_xz_valid_ = true;
        } else {
            moved = (last_floor_color_player_xz_.x != player_xz.x) || (last_floor_color_player_xz_.y != player_xz.y);
        }
        last_floor_color_player_xz_ = player_xz;
    } else {
        last_floor_color_player_xz_valid_ = false;
    }

    if (moved) {
        auto blend_channel = [](Uint8 current, Uint8 goal) {
            const float c = static_cast<float>(current);
            const float g = static_cast<float>(goal);
            return static_cast<Uint8>(std::clamp<int>(static_cast<int>(std::lround(c + ((g - c) * kFloorLerpWhenMoving))), 0, 255));
        };
        smoothed_floor_clear_color_.r = blend_channel(smoothed_floor_clear_color_.r, target.r);
        smoothed_floor_clear_color_.g = blend_channel(smoothed_floor_clear_color_.g, target.g);
        smoothed_floor_clear_color_.b = blend_channel(smoothed_floor_clear_color_.b, target.b);
    }
    smoothed_floor_clear_color_.a = 255;
    return smoothed_floor_clear_color_;
}

