#include "rendering/render/opengl_runtime_renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <SDL3_image/SDL_image.h>

#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_types.hpp"
#include "core/AssetsManager.hpp"
#include "gameplay/world/chunk.hpp"
#include "rendering/render/render_diagnostics.hpp"
#include "rendering/render/layer_depth_bins.hpp"
#include "rendering/render/render_depth_policy.hpp"
#include "rendering/render/render_object_builder.hpp"
#include "rendering/render/render_object_projection.hpp"
#include "rendering/render/opengl_runtime_renderer.hpp"
#include "rendering/render/debug_overlay_renderer.hpp"
#include "rendering/render/sprite_packet_geometry.hpp"
#include "rendering/render/sprite_floor_clip.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "animation/animation_update.hpp"
#include "utils/grid.hpp"
#include "utils/log.hpp"
#include "utils/ranged_color.hpp"

namespace {


std::filesystem::path project_root_path() {
#ifdef PROJECT_ROOT
    return std::filesystem::path(PROJECT_ROOT);
#else
    return std::filesystem::current_path();
#endif
}

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

double compute_asset_camera_depth_key(const WarpedScreenGrid& camera, const Asset& asset) {
    const auto projection = camera.projection_params();
    const double focus_world_z = camera.current_focus_plane_world_z();
    const double effective_world_z =
        static_cast<double>(asset.world_z()) +
        static_cast<double>(asset.world_z_offset()) +
        static_cast<double>(asset.render_anchor_offset_z());
    const double depth_axis_sign = static_cast<double>(render_depth::normalize_depth_axis_sign(
        static_cast<float>(projection.forward_z)));
    // Canonical far-distance scalar in camera-forward space. Higher = farther.
    const double signed_depth_offset = (effective_world_z - focus_world_z) * depth_axis_sign;
    return signed_depth_offset + asset.render_depth_bias();
}

bool render_packet_metadata_enabled() {
    static const bool enabled = [] {
        const char* raw = SDL_getenv("VIBBLE_RENDER_PACKET_METADATA");
        if (!raw || !*raw) {
            return false;
        }
        const std::string value(raw);
        return value == "1" || value == "true" || value == "TRUE" || value == "on" || value == "ON";
    }();
    return enabled;
}

int env_int_clamped_renderer(const char* name, int default_value, int min_value, int max_value) {
    const char* raw = SDL_getenv(name);
    if (!raw || !*raw) {
        return default_value;
    }
    char* end = nullptr;
    const long parsed = std::strtol(raw, &end, 10);
    if (end == raw) {
        return default_value;
    }
    return std::clamp(static_cast<int>(parsed), min_value, max_value);
}

double env_double_clamped_renderer(const char* name, double default_value, double min_value, double max_value) {
    const char* raw = SDL_getenv(name);
    if (!raw || !*raw) {
        return default_value;
    }
    char* end = nullptr;
    const double parsed = std::strtod(raw, &end);
    if (end == raw || !std::isfinite(parsed)) {
        return default_value;
    }
    return std::clamp(parsed, min_value, max_value);
}

std::uint32_t creation_budget_count_per_frame() {
    static const std::uint32_t count = static_cast<std::uint32_t>(
        env_int_clamped_renderer("VIBBLE_RENDER_MAX_CREATIONS_PER_FRAME", 3, 1, 1024));
    return count;
}

double creation_budget_ms_per_frame() {
    static const double budget_ms =
        env_double_clamped_renderer("VIBBLE_RENDER_MAX_CREATION_MS_PER_FRAME", 2.5, 0.0, 1000.0);
    return budget_ms;
}

bool should_emit_dynamic_asset_sprite(const WarpedScreenGrid& camera, const Asset& asset) {
    if (!asset.is_dynamic_spawned_asset()) {
        return true;
    }

    const auto& settings = camera.get_settings();
    const double depth = compute_asset_camera_depth_key(camera, asset);
    if (!std::isfinite(depth)) {
        return false;
    }

    const render_depth::DynamicDepthBand band = render_depth::classify_dynamic_depth_band(
        depth,
        static_cast<double>(settings.dynamic_renderer_depth_efficiency_depth),
        static_cast<double>(settings.max_cull_depth));
    return band != render_depth::DynamicDepthBand::Culled;
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
    out_packet.vertices[3] = make_vertex(bl, u0, v1);
    out_packet.indices = std::array<int, render_sprite_geometry::kMaxClippedIndices>{0, 1, 2, 0, 2, 3};
    out_packet.vertex_count = 4;
    out_packet.index_count = 6;
}

void fill_screen_quad_packet_vertices(float center_x,
                                      float center_y,
                                      float half_extent_px,
                                      float target_width,
                                      float target_height,
                                      GpuSpriteDrawPacket& out_packet) {
    const SDL_FPoint tl{center_x - half_extent_px, center_y - half_extent_px};
    const SDL_FPoint tr{center_x + half_extent_px, center_y - half_extent_px};
    const SDL_FPoint br{center_x + half_extent_px, center_y + half_extent_px};
    const SDL_FPoint bl{center_x - half_extent_px, center_y + half_extent_px};
    fill_quad_packet_vertices(tl, tr, br, bl, 0.0f, 0.0f, 1.0f, 1.0f, target_width, target_height, out_packet);
}

bool clip_quad_against_v_threshold(const SDL_Vertex (&quad)[4],
                                   float visible_v,
                                   std::array<SDL_Vertex, render_sprite_geometry::kMaxClippedVertices>& out_vertices,
                                   std::array<int, render_sprite_geometry::kMaxClippedIndices>& out_indices,
                                   int& out_vertex_count,
                                   int& out_index_count) {
    out_vertices = {};
    out_indices = {};
    out_vertex_count = 0;
    out_index_count = 0;

    constexpr float kEpsilon = 1.0e-6f;
    const auto inside = [visible_v, kEpsilon](const SDL_Vertex& v) {
        return v.tex_coord.y <= (visible_v + kEpsilon);
    };
    const auto interpolate = [visible_v, kEpsilon](const SDL_Vertex& a, const SDL_Vertex& b) {
        SDL_Vertex out{};
        const float dv = b.tex_coord.y - a.tex_coord.y;
        float t = 0.0f;
        if (std::fabs(dv) > kEpsilon) {
            t = std::clamp((visible_v - a.tex_coord.y) / dv, 0.0f, 1.0f);
        }
        out.position.x = a.position.x + (b.position.x - a.position.x) * t;
        out.position.y = a.position.y + (b.position.y - a.position.y) * t;
        out.tex_coord.x = a.tex_coord.x + (b.tex_coord.x - a.tex_coord.x) * t;
        out.tex_coord.y = visible_v;
        out.color = SDL_FColor{1.0f, 1.0f, 1.0f, 1.0f};
        return out;
    };

    std::array<SDL_Vertex, render_sprite_geometry::kMaxClippedVertices> poly{};
    int poly_count = 0;
    SDL_Vertex prev = quad[3];
    bool prev_inside = inside(prev);
    for (int i = 0; i < 4; ++i) {
        const SDL_Vertex curr = quad[i];
        const bool curr_inside = inside(curr);
        if (curr_inside) {
            if (!prev_inside && poly_count < static_cast<int>(poly.size())) {
                poly[static_cast<std::size_t>(poly_count++)] = interpolate(prev, curr);
            }
            if (poly_count < static_cast<int>(poly.size())) {
                poly[static_cast<std::size_t>(poly_count++)] = curr;
            }
        } else if (prev_inside) {
            if (poly_count < static_cast<int>(poly.size())) {
                poly[static_cast<std::size_t>(poly_count++)] = interpolate(prev, curr);
            }
        }
        prev = curr;
        prev_inside = curr_inside;
    }

    if (poly_count < 3) {
        return false;
    }

    out_vertex_count = poly_count;
    for (int i = 0; i < poly_count; ++i) {
        out_vertices[static_cast<std::size_t>(i)] = poly[static_cast<std::size_t>(i)];
    }

    int index_cursor = 0;
    for (int i = 1; i + 1 < poly_count; ++i) {
        if (index_cursor + 2 >= static_cast<int>(out_indices.size())) {
            break;
        }
        out_indices[static_cast<std::size_t>(index_cursor++)] = 0;
        out_indices[static_cast<std::size_t>(index_cursor++)] = i;
        out_indices[static_cast<std::size_t>(index_cursor++)] = i + 1;
    }
    out_index_count = index_cursor;
    return out_index_count >= 3;
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
                            std::array<SDL_Vertex, render_sprite_geometry::kMaxClippedVertices>& out_vertices) {
    const float width = static_cast<float>(std::max<std::uint32_t>(1u, target_width));
    const float height = static_cast<float>(std::max<std::uint32_t>(1u, target_height));
    const auto convert_position = [width, height](float clip_x, float clip_y) -> SDL_FPoint {
        const float x = (clip_x + 1.0f) * 0.5f * width;
        const float y = (1.0f - clip_y) * 0.5f * height;
        return SDL_FPoint{x, y};
    };

    const int vertex_count = std::clamp(packet.vertex_count, 0, static_cast<int>(out_vertices.size()));
    for (int i = 0; i < vertex_count; ++i) {
        const GpuSpriteVertex& src = packet.vertices[i];
        SDL_Vertex dst{};
        dst.position = convert_position(src.clip_x, src.clip_y);
        dst.tex_coord = SDL_FPoint{src.uv_x, src.uv_y};
        dst.color = packet.modulate;
        out_vertices[static_cast<std::size_t>(i)] = dst;
    }
}

} // namespace

bool opengl_runtime_renderer_detail::draw_packet_sort_predicate_floor(const GpuSpriteDrawPacket& lhs,
                                                                      const GpuSpriteDrawPacket& rhs) {
    constexpr std::uintptr_t kSpritePacketSortOffset =
        std::uintptr_t{1} << ((sizeof(std::uintptr_t) * 8u) - 1u);
    const bool lhs_sprite = lhs.stable_sort_id >= kSpritePacketSortOffset;
    const bool rhs_sprite = rhs.stable_sort_id >= kSpritePacketSortOffset;
    if (lhs_sprite != rhs_sprite) {
        // Keep floor tiles first and debug/overlay sprites after so overlays are visible.
        return !lhs_sprite;
    }
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

int classify_depth_layer_for_asset(const WarpedScreenGrid& camera,
                                   const Asset& asset,
                                   const std::vector<double>* cached_depth_edges) {
    const auto& settings = camera.get_settings();
    const double signed_distance = compute_asset_camera_depth_key(camera, asset);
    const double distance = std::fabs(signed_distance);
    if (!std::isfinite(distance) || distance <= 1.0e-4) {
        return 0;
    }

    const double interval = std::max(1.0, static_cast<double>(settings.layer_depth_interval));
    const double curve = std::max(0.0, static_cast<double>(settings.layer_depth_curve));
    const double effective_max_depth = std::max(interval, static_cast<double>(settings.max_cull_depth));
    const std::vector<double> local_edges =
        cached_depth_edges ? std::vector<double>{} : render_depth::build_background_depth_edges(effective_max_depth, interval, curve);
    const std::vector<double>& edges = cached_depth_edges ? *cached_depth_edges : local_edges;

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

float far_background_bottom_screen_y(const WarpedScreenGrid& camera, std::uint32_t target_height) {
    const auto& settings = camera.get_settings();
    const float far_world_z = static_cast<float>(
        camera.current_anchor_world_z() - static_cast<double>(settings.max_cull_depth));
    const SDL_FPoint center = camera.get_view_center_f();
    SDL_FPoint far_screen{};
    if (camera.project_world_point(SDL_FPoint{center.x, 0.0f}, far_world_z, far_screen) &&
        std::isfinite(far_screen.y)) {
        return far_screen.y;
    }

    const WarpedScreenGrid::FloorDepthParams floor_params = camera.compute_floor_depth_params();
    return std::isfinite(floor_params.horizon_screen_y)
        ? static_cast<float>(floor_params.horizon_screen_y)
        : static_cast<float>(target_height) * 0.5f;
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
                                     std::uint32_t target_width,
                                     std::uint32_t target_height,
                                     std::vector<GpuSpriteDrawPacket>& out_packets,
                                     std::string& out_error) {
    out_packets.clear();
    out_error.clear();
    out_packets.reserve(markers.size() * 3);
    const float output_w = static_cast<float>(std::max<std::uint32_t>(1u, target_width));
    const float output_h = static_cast<float>(std::max<std::uint32_t>(1u, target_height));
    std::uintptr_t marker_sequence = 0u;

    for (const Assets::DevFloorProjectionMarker& marker : markers) {
        SDL_FPoint center{};
        if (!camera.project_world_point(SDL_FPoint{marker.floor_world_xz.x, 0.0f}, marker.floor_world_xz.y, center) ||
            !std::isfinite(center.x) || !std::isfinite(center.y)) {
            continue;
        }
        const float packet_y = center.y;
        const std::uintptr_t base_sort_id = floor_sort_id(true, marker_sequence);
        marker_sequence += 4u;

        auto init_packet = [&](GpuSpriteDrawPacket& packet) {
            packet.source_texture = nullptr;
            packet.source_asset_name = "<floor-marker>";
            packet.source_animation_name = "<floor-marker>";
            packet.source_texture_id = "floor_marker_screen_primitive";
            packet.source_frame_index = -1;
            packet.source_variant_index = -1;
            packet.modulate = SDL_FColor{
                static_cast<float>(marker.color.r) / 255.0f,
                static_cast<float>(marker.color.g) / 255.0f,
                static_cast<float>(marker.color.b) / 255.0f,
                static_cast<float>(marker.color.a) / 255.0f,
            };
            packet.projected_foot_y_key = packet_y;
            packet.camera_depth_key = marker.floor_world_xz.y;
            packet.is_floor_packet = true;
            packet.depth_layer = 0;
        };

        if (marker.shape == Assets::DevFloorProjectionMarker::Shape::Crosshair) {
            const float line_half_world = static_cast<float>(std::clamp(marker.crosshair_radius, 3, 12));
            const float thickness_half_world =
                std::max(0.75f, 0.5f * static_cast<float>(std::clamp(marker.pixel_size, 2, 8)));

            auto emit_floor_arm = [&](const SDL_FPoint& world_tl,
                                      const SDL_FPoint& world_tr,
                                      const SDL_FPoint& world_br,
                                      const SDL_FPoint& world_bl,
                                      std::uintptr_t sort_id,
                                      const char* texture_id) {
                SDL_FPoint screen_tl{};
                SDL_FPoint screen_tr{};
                SDL_FPoint screen_br{};
                SDL_FPoint screen_bl{};
                if (!camera.project_world_point(SDL_FPoint{world_tl.x, 0.0f}, world_tl.y, screen_tl) ||
                    !camera.project_world_point(SDL_FPoint{world_tr.x, 0.0f}, world_tr.y, screen_tr) ||
                    !camera.project_world_point(SDL_FPoint{world_br.x, 0.0f}, world_br.y, screen_br) ||
                    !camera.project_world_point(SDL_FPoint{world_bl.x, 0.0f}, world_bl.y, screen_bl) ||
                    !std::isfinite(screen_tl.x) || !std::isfinite(screen_tl.y) ||
                    !std::isfinite(screen_tr.x) || !std::isfinite(screen_tr.y) ||
                    !std::isfinite(screen_br.x) || !std::isfinite(screen_br.y) ||
                    !std::isfinite(screen_bl.x) || !std::isfinite(screen_bl.y)) {
                    return;
                }

                GpuSpriteDrawPacket packet{};
                init_packet(packet);
                packet.source_texture_id = texture_id;
                packet.stable_sort_id = sort_id;
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
            };

            const SDL_FPoint horizontal_tl{
                marker.floor_world_xz.x - line_half_world,
                marker.floor_world_xz.y - thickness_half_world};
            const SDL_FPoint horizontal_tr{
                marker.floor_world_xz.x + line_half_world,
                marker.floor_world_xz.y - thickness_half_world};
            const SDL_FPoint horizontal_br{
                marker.floor_world_xz.x + line_half_world,
                marker.floor_world_xz.y + thickness_half_world};
            const SDL_FPoint horizontal_bl{
                marker.floor_world_xz.x - line_half_world,
                marker.floor_world_xz.y + thickness_half_world};
            emit_floor_arm(horizontal_tl,
                           horizontal_tr,
                           horizontal_br,
                           horizontal_bl,
                           base_sort_id,
                           "floor_marker_floor_crosshair_x");

            const SDL_FPoint vertical_tl{
                marker.floor_world_xz.x - thickness_half_world,
                marker.floor_world_xz.y - line_half_world};
            const SDL_FPoint vertical_tr{
                marker.floor_world_xz.x + thickness_half_world,
                marker.floor_world_xz.y - line_half_world};
            const SDL_FPoint vertical_br{
                marker.floor_world_xz.x + thickness_half_world,
                marker.floor_world_xz.y + line_half_world};
            const SDL_FPoint vertical_bl{
                marker.floor_world_xz.x - thickness_half_world,
                marker.floor_world_xz.y + line_half_world};
            emit_floor_arm(vertical_tl,
                           vertical_tr,
                           vertical_br,
                           vertical_bl,
                           base_sort_id + 1u,
                           "floor_marker_floor_crosshair_z");
        } else {
            GpuSpriteDrawPacket dot{};
            init_packet(dot);
            dot.stable_sort_id = base_sort_id;
            const float half_extent_px =
                std::max(1.0f, 0.5f * static_cast<float>(std::clamp(marker.pixel_size, 2, 10)));
            fill_screen_quad_packet_vertices(center.x, center.y, half_extent_px, output_w, output_h, dot);
            out_packets.push_back(dot);
        }
    }

    if (!std::is_sorted(out_packets.begin(), out_packets.end(), opengl_runtime_renderer_detail::draw_packet_sort_predicate_floor)) {
        std::stable_sort(out_packets.begin(), out_packets.end(), opengl_runtime_renderer_detail::draw_packet_sort_predicate_floor);
    }
    return true;
}

bool build_dev_floor_grid_overlay_draw_packets(const WarpedScreenGrid& camera,
                                               const Assets::DevGridOverlayContext& overlay_context,
                                               int grid_step_world_px,
                                               std::uint32_t target_width,
                                               std::uint32_t target_height,
                                               std::vector<GpuSpriteDrawPacket>& out_packets,
                                               std::string& out_error) {
    out_packets.clear();
    out_error.clear();

    const int grid_step = std::max(1, grid_step_world_px);
    const float output_w = static_cast<float>(std::max<std::uint32_t>(1u, target_width));
    const float output_h = static_cast<float>(std::max<std::uint32_t>(1u, target_height));

    vibble::grid::Grid& grid = vibble::grid::global_grid();
    const int overlay_resolution = vibble::grid::clamp_resolution(
        static_cast<int>(std::lround(std::log2(static_cast<double>(grid_step)))));
    SDL_Point center_world = overlay_context.snapped_floor_xz;
    if (!std::isfinite(overlay_context.exact_floor_xz.x) || !std::isfinite(overlay_context.exact_floor_xz.y)) {
        // No valid cursor-projected floor point available for this frame.
        // Skip drawing instead of anchoring to a misleading fallback location.
        return true;
    }
    const SDL_Point center_index = grid.world_to_index(center_world, overlay_resolution);

    constexpr int kMaxSamplesPerPass = 32000;
    int radius_cells = std::clamp(
        static_cast<int>(std::max(target_width, target_height)) / std::max(4, grid_step),
        24,
        320);
    while (((2 * radius_cells + 1) * (2 * radius_cells + 1)) > kMaxSamplesPerPass &&
           radius_cells > 12) {
        --radius_cells;
    }

    const float radius_cells_f = static_cast<float>(std::max(1, radius_cells));
    const float radius_cells_sq = radius_cells_f * radius_cells_f;
    const std::int64_t estimated_total =
        static_cast<std::int64_t>((radius_cells * 2) + 1) * static_cast<std::int64_t>((radius_cells * 2) + 1);
    if (estimated_total > 0) {
        out_packets.reserve(static_cast<std::size_t>(std::min<std::int64_t>(
            estimated_total * 3LL,
            static_cast<std::int64_t>(std::numeric_limits<std::uint16_t>::max()))));
    }

    constexpr float kGridDotHalfExtentPx = 1.0f;
    constexpr float kGridDotMaxAlpha = 180.0f / 255.0f;
    constexpr float kCrosshairHalfLengthPx = 5.0f;
    constexpr float kCrosshairHalfThicknessPx = 1.0f;
    std::uintptr_t sequence = 0u;

    auto emit_overlay_packet = [&](float center_x,
                                   float center_y,
                                   float half_extent_x,
                                   float half_extent_y,
                                   const SDL_FColor& modulate,
                                   float camera_depth_key,
                                   const char* texture_id) {
        GpuSpriteDrawPacket packet{};
        packet.source_texture = nullptr;
        packet.source_asset_name = "<dev-floor-grid-overlay>";
        packet.source_animation_name = "<dev-floor-grid-overlay>";
        packet.source_texture_id = texture_id;
        packet.source_frame_index = -1;
        packet.source_variant_index = -1;
        packet.modulate = modulate;
        packet.projected_foot_y_key = center_y;
        packet.camera_depth_key = camera_depth_key;
        packet.stable_sort_id = floor_sort_id(true, sequence++);
        packet.is_floor_packet = true;
        packet.depth_layer = 0;
        fill_quad_packet_vertices(SDL_FPoint{center_x - half_extent_x, center_y - half_extent_y},
                                  SDL_FPoint{center_x + half_extent_x, center_y - half_extent_y},
                                  SDL_FPoint{center_x + half_extent_x, center_y + half_extent_y},
                                  SDL_FPoint{center_x - half_extent_x, center_y + half_extent_y},
                                  0.0f,
                                  0.0f,
                                  1.0f,
                                  1.0f,
                                  output_w,
                                  output_h,
                                  packet);
        out_packets.push_back(packet);
    };

    for (int grid_dz = -radius_cells; grid_dz <= radius_cells; ++grid_dz) {
        for (int grid_dx = -radius_cells; grid_dx <= radius_cells; ++grid_dx) {
            const float dist_cells_sq = static_cast<float>(grid_dx * grid_dx + grid_dz * grid_dz);
            if (dist_cells_sq > radius_cells_sq) {
                continue;
            }
            const SDL_Point world_point =
                grid.index_to_world(center_index.x + grid_dx, center_index.y + grid_dz, overlay_resolution);
            SDL_FPoint center{};
            if (!camera.project_world_point(SDL_FPoint{static_cast<float>(world_point.x), 0.0f},
                                            static_cast<float>(world_point.y),
                                            center) ||
                !std::isfinite(center.x) ||
                !std::isfinite(center.y)) {
                continue;
            }
            if (center.x < -32.0f || center.y < -32.0f ||
                center.x > output_w + 32.0f || center.y > output_h + 32.0f) {
                continue;
            }

            const float edge_t = std::clamp(std::sqrt(std::max(0.0f, dist_cells_sq) / radius_cells_sq), 0.0f, 1.0f);
            const float alpha = (1.0f - edge_t) * kGridDotMaxAlpha;
            if (alpha <= (1.0f / 255.0f)) {
                continue;
            }

            const bool is_cursor_intersection = (grid_dx == 0 && grid_dz == 0);
            const float camera_depth_key = static_cast<float>(world_point.y);
            if (is_cursor_intersection) {
                const SDL_FColor orange{1.0f, 160.0f / 255.0f, 32.0f / 255.0f, std::max(alpha, 220.0f / 255.0f)};
                emit_overlay_packet(center.x,
                                    center.y,
                                    kCrosshairHalfLengthPx,
                                    kCrosshairHalfThicknessPx,
                                    orange,
                                    camera_depth_key,
                                    "floor_grid_overlay_screen_crosshair_h");
                emit_overlay_packet(center.x,
                                    center.y,
                                    kCrosshairHalfThicknessPx,
                                    kCrosshairHalfLengthPx,
                                    orange,
                                    camera_depth_key,
                                    "floor_grid_overlay_screen_crosshair_v");
            } else {
                emit_overlay_packet(center.x,
                                    center.y,
                                    kGridDotHalfExtentPx,
                                    kGridDotHalfExtentPx,
                                    SDL_FColor{1.0f, 1.0f, 1.0f, alpha},
                                    camera_depth_key,
                                    "floor_grid_overlay_screen_dot");
            }
        }
    }

    if (!std::is_sorted(out_packets.begin(), out_packets.end(), opengl_runtime_renderer_detail::draw_packet_sort_predicate_floor)) {
        std::stable_sort(out_packets.begin(), out_packets.end(), opengl_runtime_renderer_detail::draw_packet_sort_predicate_floor);
    }
    return true;
}

} // namespace opengl_runtime_renderer_detail

bool opengl_runtime_renderer_detail::build_xy_sprite_draw_packets(
    const WarpedScreenGrid& camera,
    const std::vector<Asset*>& visible_assets,
    std::uint32_t target_width,
    std::uint32_t target_height,
    std::vector<GpuSpriteDrawPacket>& out_xy_sprite_draws,
    std::string& out_error,
    const std::vector<double>* cached_depth_edges) {
    out_xy_sprite_draws.clear();
    out_xy_sprite_draws.reserve(visible_assets.size());
    out_error.clear();

    std::uintptr_t xy_sequence = 0u;

    for (Asset* asset : visible_assets) {
        if (!asset || asset->dead || !asset->info) {
            continue;
        }
        if (!opengl_runtime_renderer_detail::info_is_xy_sprite_pass_eligible(asset->info->tillable)) {
            continue;
        }
        if (!should_emit_dynamic_asset_sprite(camera, *asset)) {
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

        GpuSpriteDrawPacket packet{};
        packet.source_texture = object.texture;
        if (render_packet_metadata_enabled()) {
            packet.source_asset_name = asset->info ? asset->info->name : "<unknown-asset>";
            packet.source_animation_name = asset->current_animation;
            packet.source_texture_id = "sdl_texture_ptr=" + std::to_string(reinterpret_cast<std::uintptr_t>(object.texture));
            packet.source_frame_index = asset->current_frame ? asset->current_frame->frame_index : -1;
            packet.source_variant_index = asset->current_variant_index;
        }
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
        packet.depth_layer = opengl_runtime_renderer_detail::classify_depth_layer_for_asset(
            camera,
            *asset,
            cached_depth_edges);
        const bool packet_geometry_valid =
            opengl_runtime_renderer_detail::build_floor_clipped_sprite_packet(camera,
                                                                              projected,
                                                                              u0,
                                                                              v0,
                                                                              u1,
                                                                              v1,
                                                                              target_width,
                                                                              target_height,
                                                                              packet);
        if (!packet_geometry_valid) {
            continue;
        }
        out_xy_sprite_draws.push_back(packet);
    }

    if (!std::is_sorted(out_xy_sprite_draws.begin(), out_xy_sprite_draws.end(), opengl_runtime_renderer_detail::draw_packet_sort_predicate_xy)) {
        std::stable_sort(out_xy_sprite_draws.begin(), out_xy_sprite_draws.end(), opengl_runtime_renderer_detail::draw_packet_sort_predicate_xy);
    }
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
      screen_height_(std::max(1, screen_height)),
      dof_blur_chain_(renderer) {
    creation_budget_config_.max_creations_per_frame = creation_budget_count_per_frame();
    creation_budget_config_.max_creation_ms_per_frame = creation_budget_ms_per_frame();
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

    GpuSceneFrameData prewarm{};
    if (const std::optional<SDL_Point> target = render_target_manager_.current_size(); target.has_value()) {
        prewarm.target_width = static_cast<std::uint32_t>(target->x);
        prewarm.target_height = static_cast<std::uint32_t>(target->y);
        std::string prewarm_error;
        ensure_render_targets(prewarm, prewarm_error);
        ensure_far_background_textures();
    }
    return true;
}

void OpenGLRuntimeRenderer::destroy_render_targets() {
    render_diagnostics::destroy_texture(floor_target_);
    render_diagnostics::destroy_texture(xy_sprite_target_);
    render_diagnostics::destroy_texture(composite_target_);
    destroy_depth_layer_targets();
    dof_blur_chain_.destroy_targets();
    destroy_far_background_textures();
    output_target_width_ = 1;
    output_target_height_ = 1;
    clear_creation_queue();
}

void OpenGLRuntimeRenderer::destroy_depth_layer_targets() {
    for (auto& entry : depth_layer_targets_) {
        render_diagnostics::destroy_texture(entry.second);
    }
    depth_layer_targets_.clear();
    cached_depth_layer_ids_.clear();
}

void OpenGLRuntimeRenderer::destroy_far_background_textures() {
    render_diagnostics::destroy_texture(far_background_sky_texture_);
    render_diagnostics::destroy_texture(far_background_mountains_texture_);
}

void OpenGLRuntimeRenderer::clear_creation_queue() {
    deferred_creation_queue_.clear();
}

bool OpenGLRuntimeRenderer::process_creation_queue(const GpuSceneFrameData& frame_data, std::string& out_error) {
    const std::uint64_t frame_index = render_diagnostics::current_frame_stats().frame_index;
    const std::uint32_t queue_start = static_cast<std::uint32_t>(deferred_creation_queue_.size());
    std::uint32_t attempted = 0, executed = 0, deferred = 0, retried = 0, permanent_failures = 0, max_age = 0;
    const std::uint64_t begin = SDL_GetPerformanceCounter();
    const std::uint64_t freq = SDL_GetPerformanceFrequency();
    auto elapsed_ms = [begin, freq]() {
        if (freq == 0) return 0.0;
        return static_cast<double>(SDL_GetPerformanceCounter() - begin) * 1000.0 / static_cast<double>(freq);
    };

    while (!deferred_creation_queue_.empty()) {
        if (executed >= creation_budget_config_.max_creations_per_frame ||
            (creation_budget_config_.max_creation_ms_per_frame > 0.0 &&
             elapsed_ms() >= creation_budget_config_.max_creation_ms_per_frame)) {
            break;
        }
        DeferredCreationJob job = deferred_creation_queue_.front();
        deferred_creation_queue_.pop_front();
        ++attempted;
        max_age = std::max(max_age, static_cast<std::uint32_t>(frame_index - job.enqueue_frame));
        bool ok = false;
        if (job.type == DeferredCreationJob::Type::MainTarget) {
            SDL_Texture** target = nullptr;
            if (job.label == "floor") target = &floor_target_;
            else if (job.label == "xy_sprite") target = &xy_sprite_target_;
            else if (job.label == "composite") target = &composite_target_;
            if (target && !*target) {
                *target = create_render_target(renderer_, static_cast<int>(frame_data.target_width),
                                               static_cast<int>(frame_data.target_height), job.label, out_error);
            }
            ok = (target && *target);
        } else {
            SDL_Texture*& target = depth_layer_targets_[job.layer_id];
            if (!target) {
                target = create_render_target(renderer_, static_cast<int>(frame_data.target_width),
                                              static_cast<int>(frame_data.target_height), job.label, out_error);
            }
            ok = target != nullptr;
        }
        if (!ok) {
            if (job.retries < creation_budget_config_.max_retry_count) {
                ++job.retries; ++retried; job.enqueue_frame = frame_index;
                deferred_creation_queue_.push_back(job);
            } else {
                ++permanent_failures;
                vibble::log::error("[OpenGLRuntimeRenderer] Dropping creation job after retries: " + job.label);
            }
            continue;
        }
        ++executed;
    }
    deferred = static_cast<std::uint32_t>(deferred_creation_queue_.size());
    render_diagnostics::set_creation_budget_stats(creation_budget_config_.max_creations_per_frame,
                                                  creation_budget_config_.max_creation_ms_per_frame,
                                                  attempted, executed, deferred, queue_start, deferred, max_age,
                                                  retried, permanent_failures);
    return true;
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

void OpenGLRuntimeRenderer::configure_render_target(SDL_Texture* texture) {
    if (!texture) {
        return;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
}

bool OpenGLRuntimeRenderer::ensure_far_background_textures() {
    if (!renderer_) {
        return false;
    }
    if (far_background_sky_texture_ && far_background_mountains_texture_) {
        return true;
    }

    auto load_far_background_texture = [&](const char* filename, SDL_Texture*& out_texture) -> bool {
        if (out_texture) {
            return true;
        }
        const std::filesystem::path path = project_root_path() / "resources" / "misc_content" / filename;
        SDL_Surface* surface = IMG_Load(path.u8string().c_str());
        if (!surface) {
            vibble::log::warn(std::string{"[OpenGLRuntimeRenderer] Failed to load far background texture '"} +
                              path.u8string() + "': " + safe_string(SDL_GetError()));
            return false;
        }
        out_texture = SDL_CreateTextureFromSurface(renderer_, surface);
        SDL_DestroySurface(surface);
        if (!out_texture) {
            vibble::log::warn(std::string{"[OpenGLRuntimeRenderer] Failed to upload far background texture '"} +
                              path.u8string() + "': " + safe_string(SDL_GetError()));
            return false;
        }
        render_diagnostics::note_texture_created(out_texture);
        SDL_SetTextureBlendMode(out_texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(out_texture, SDL_SCALEMODE_LINEAR);
        return true;
    };

    const bool sky_ok = load_far_background_texture("sky.png", far_background_sky_texture_);
    const bool mountains_ok = load_far_background_texture("mountains.png", far_background_mountains_texture_);
    return sky_ok || mountains_ok;
}

bool OpenGLRuntimeRenderer::render_far_background(const WarpedScreenGrid& camera,
                                                  std::uint32_t target_width,
                                                  std::uint32_t target_height,
                                                  std::string& out_error) {
    out_error.clear();
    if (!renderer_ || target_width == 0 || target_height == 0) {
        return true;
    }
    if (!ensure_far_background_textures()) {
        return true;
    }

    const float bottom_y = opengl_runtime_renderer_detail::far_background_bottom_screen_y(camera, target_height);
    const float target_w = static_cast<float>(target_width);

    auto draw_far_background_texture = [&](SDL_Texture* texture, const char* label) -> bool {
        if (!texture) {
            return true;
        }
        float source_w = 0.0f;
        float source_h = 0.0f;
        if (!SDL_GetTextureSize(texture, &source_w, &source_h) || source_w <= 0.0f || source_h <= 0.0f) {
            return true;
        }
        const float scale = target_w / source_w;
        const float target_h = source_h * scale;
        const SDL_FRect dst{0.0f, bottom_y - target_h, target_w, target_h};
        if (!render_diagnostics::render_texture(renderer_, texture, nullptr, &dst)) {
            out_error = std::string{"Failed to render far background "} + label + " texture: " + safe_string(SDL_GetError());
            return false;
        }
        return true;
    };

    if (!draw_far_background_texture(far_background_sky_texture_, "sky")) {
        return false;
    }
    if (!draw_far_background_texture(far_background_mountains_texture_, "mountains")) {
        return false;
    }
    return true;
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
                                               std::array<SDL_Vertex, render_sprite_geometry::kMaxClippedVertices>& out_vertices) {
    fill_geometry_vertices(packet, target_width, target_height, out_vertices);
}

bool opengl_runtime_renderer_detail::build_floor_clipped_sprite_packet(
    const WarpedScreenGrid&,
    const render_projection::ProjectedSpriteFrame& projected,
    float u0,
    float v0,
    float u1,
    float v1,
    std::uint32_t target_width,
    std::uint32_t target_height,
    GpuSpriteDrawPacket& out_packet) {
    const float output_w = static_cast<float>(std::max<std::uint32_t>(1u, target_width));
    const float output_h = static_cast<float>(std::max<std::uint32_t>(1u, target_height));
    const render_floor_clip::ClipResult clip =
        render_floor_clip::compute_floor_clip(render_floor_clip::ClipInput{
            projected.world_y,
            projected.anchor_uv.y,
            projected.world_height
        });
    if (!clip.valid || clip.visibility == render_floor_clip::Visibility::FullyBuried) {
        return false;
    }

    const float bottom_v = clip.visibility == render_floor_clip::Visibility::PartiallyVisible
        ? clip.visible_v
        : 1.0f;
    const auto make_vertex = [](const SDL_FPoint& point, float u, float v) {
        SDL_Vertex vertex{};
        vertex.position = point;
        vertex.tex_coord = SDL_FPoint{u, v};
        vertex.color = SDL_FColor{1.0f, 1.0f, 1.0f, 1.0f};
        return vertex;
    };
    SDL_Vertex quad[4]{
        make_vertex(projected.screen_tl, u0, v0),
        make_vertex(projected.screen_tr, u1, v0),
        make_vertex(projected.screen_br, u1, v1),
        make_vertex(projected.screen_bl, u0, v1),
    };
    std::array<SDL_Vertex, render_sprite_geometry::kMaxClippedVertices> clipped_vertices{};
    std::array<int, render_sprite_geometry::kMaxClippedIndices> clipped_indices{};
    int clipped_vertex_count = 0;
    int clipped_index_count = 0;
    if (!clip_quad_against_v_threshold(
            quad, bottom_v, clipped_vertices, clipped_indices, clipped_vertex_count, clipped_index_count)) {
        return false;
    }
    const auto to_gpu_vertex = [output_w, output_h](const SDL_Vertex& vertex) {
        GpuSpriteVertex out{};
        out.clip_x = to_clip_x(vertex.position.x, output_w);
        out.clip_y = to_clip_y(vertex.position.y, output_h);
        out.uv_x = vertex.tex_coord.x;
        out.uv_y = vertex.tex_coord.y;
        return out;
    };
    out_packet.vertices = {};
    out_packet.indices = {};
    for (int i = 0; i < clipped_vertex_count; ++i) {
        out_packet.vertices[static_cast<std::size_t>(i)] =
            to_gpu_vertex(clipped_vertices[static_cast<std::size_t>(i)]);
    }
    for (int i = 0; i < clipped_index_count; ++i) {
        out_packet.indices[static_cast<std::size_t>(i)] = clipped_indices[static_cast<std::size_t>(i)];
    }
    out_packet.vertex_count = clipped_vertex_count;
    out_packet.index_count = clipped_index_count;
    return true;
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

void OpenGLRuntimeRenderer::ingest_player_damage_pulse(float now_seconds) {
    if (!assets_) {
        return;
    }

    const runtime::context::PlayerDamagePulseState& pulse = assets_->game_context().player_damage_pulse();
    if (pulse.pulse_id == 0 || pulse.pulse_id == last_damage_pulse_id_) {
        return;
    }
    last_damage_pulse_id_ = pulse.pulse_id;

    ActiveDamagePulse active{};
    active.pulse_id = pulse.pulse_id;
    active.pulse_time_seconds = pulse.pulse_time_seconds;
    active.amplitude = std::clamp(
        static_cast<float>(std::max(0, pulse.damage_amount)) / dof_blur_chain::damage_pulse_tuning::kDamageReference,
        0.0f,
        1.0f);
    active.health_ratio_after = std::clamp(pulse.health_ratio_after, 0.0f, 1.0f);
    if (active.amplitude <= 0.0f) {
        return;
    }

    active_damage_pulses_.push_back(active);
    while (active_damage_pulses_.size() > dof_blur_chain::damage_pulse_tuning::kMaxConcurrentPulses) {
        active_damage_pulses_.pop_front();
    }
    prune_expired_damage_pulses(now_seconds);
}

void OpenGLRuntimeRenderer::prune_expired_damage_pulses(float now_seconds) {
    const float lifetime = dof_blur_chain::damage_pulse_tuning::kPulseLifetimeSeconds;
    while (!active_damage_pulses_.empty()) {
        const float age = now_seconds - active_damage_pulses_.front().pulse_time_seconds;
        if (age <= lifetime) {
            break;
        }
        active_damage_pulses_.pop_front();
    }
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
    }
    return assets_->world_grid().all_chunks();
}

bool OpenGLRuntimeRenderer::build_gpu_scene_frame_data(std::uint32_t target_width,
                                                       std::uint32_t target_height,
                                                       GpuSceneFrameData& out_data,
                                                       std::string& out_error,
                                                       bool allow_dof_depth_layers) const {
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
    bool used_active_fallback = false;
    const std::vector<Asset*>& selected_visible_assets =
        opengl_runtime_renderer_detail::select_visible_assets_for_gpu_frame(
            assets_->is_dev_mode(),
            assets_->focus_filter_active(),
            active_assets,
            filtered_active_assets,
            used_active_fallback);
    const WarpedScreenGrid& camera = assets_->getView();
    const auto& camera_settings = camera.get_settings();
    const double depth_interval = std::max(1.0, static_cast<double>(camera_settings.layer_depth_interval));
    const double depth_curve = std::max(0.0, static_cast<double>(camera_settings.layer_depth_curve));
    const double effective_max_depth = std::max(depth_interval, static_cast<double>(camera_settings.max_cull_depth));
    const std::vector<double> frame_depth_edges =
        render_depth::build_background_depth_edges(effective_max_depth, depth_interval, depth_curve);
    const std::size_t traversal_count = camera.visible_traversal_entries().size();
    out_data.focus_depth_layer = assets_->player
        ? opengl_runtime_renderer_detail::classify_depth_layer_for_asset(camera, *assets_->player, &frame_depth_edges)
        : 0;

    out_data.target_width = target_width;
    out_data.target_height = target_height;
    out_data.active_asset_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        active_assets.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.filtered_active_asset_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        filtered_active_assets.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.selected_asset_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        selected_visible_assets.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
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
    if (selected_visible_assets.empty() && !active_assets.empty() && traversal_count > 0) {
        vibble::log::warn("[OpenGLRuntimeRenderer] Observed empty selected_visible_assets despite non-empty active traversal state. "
                          "active_count=" + std::to_string(active_assets.size()) +
                          " filtered_count=" + std::to_string(filtered_active_assets.size()) +
                          " selected_count=" + std::to_string(selected_visible_assets.size()) +
                          " traversal_count=" + std::to_string(traversal_count) +
                          " dev_mode=" + std::string(assets_->is_dev_mode() ? "true" : "false") +
                          " focus_filter_active=" + std::string(assets_->focus_filter_active() ? "true" : "false"));
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

    const Assets::DevGridOverlayContext dev_grid_overlay_context = assets_->dev_grid_overlay_context();
    if (assets_->dev_grid_overlay_enabled() &&
        std::isfinite(dev_grid_overlay_context.exact_floor_xz.x) &&
        std::isfinite(dev_grid_overlay_context.exact_floor_xz.y)) {
        scratch_floor_grid_overlay_draws_.clear();
        scratch_floor_grid_overlay_draws_.reserve(scratch_floor_grid_overlay_draws_.capacity());
        if (!opengl_runtime_renderer_detail::build_dev_floor_grid_overlay_draw_packets(
                camera,
                dev_grid_overlay_context,
                assets_->dev_grid_overlay_cell_size_px(),
                target_width,
                target_height,
                scratch_floor_grid_overlay_draws_,
                out_error)) {
            return false;
        }
        if (!scratch_floor_grid_overlay_draws_.empty()) {
            out_data.floor_draws.insert(out_data.floor_draws.end(),
                                        std::make_move_iterator(scratch_floor_grid_overlay_draws_.begin()),
                                        std::make_move_iterator(scratch_floor_grid_overlay_draws_.end()));
            if (!std::is_sorted(out_data.floor_draws.begin(),
                                out_data.floor_draws.end(),
                                opengl_runtime_renderer_detail::draw_packet_sort_predicate_floor)) {
                std::stable_sort(out_data.floor_draws.begin(),
                                 out_data.floor_draws.end(),
                                 opengl_runtime_renderer_detail::draw_packet_sort_predicate_floor);
            }
        }
    }

    scratch_floor_marker_draws_.clear();
    const std::vector<Assets::DevFloorProjectionMarker> floor_markers = assets_->dev_floor_projection_markers();
    if (!opengl_runtime_renderer_detail::build_floor_marker_draw_packets(camera,
                                                                         floor_markers,
                                                                         target_width,
                                                                         target_height,
                                                                         scratch_floor_marker_draws_,
                                                                         out_error)) {
        return false;
    }
    if (!scratch_floor_marker_draws_.empty()) {
        out_data.floor_draws.insert(out_data.floor_draws.end(),
                                    std::make_move_iterator(scratch_floor_marker_draws_.begin()),
                                    std::make_move_iterator(scratch_floor_marker_draws_.end()));
        if (!std::is_sorted(out_data.floor_draws.begin(),
                            out_data.floor_draws.end(),
                            opengl_runtime_renderer_detail::draw_packet_sort_predicate_floor)) {
            std::stable_sort(out_data.floor_draws.begin(),
                             out_data.floor_draws.end(),
                             opengl_runtime_renderer_detail::draw_packet_sort_predicate_floor);
        }
    }

    if (!opengl_runtime_renderer_detail::build_xy_sprite_draw_packets(
            camera,
            selected_visible_assets,
            target_width,
            target_height,
            out_data.xy_sprite_draws,
            out_error,
            &frame_depth_edges)) {
        return false;
    }

    out_data.floor_draw_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        out_data.floor_draws.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.xy_sprite_draw_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        out_data.xy_sprite_draws.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));

    out_data.depth_layers.clear();
    const bool dof_requested_by_settings = dof_blur_chain::enabled(camera_settings.depth_of_field_enabled,
                                                                   camera_settings.blur_px,
                                                                   camera_settings.radial_blur_px);
    const bool dof_requested = allow_dof_depth_layers &&
        dof_requested_by_settings;
    if (dof_requested) {
        const auto bucket_depth_layer =
            [focus = out_data.focus_depth_layer](int layer) {
            constexpr int bucket_radius = render_depth::kDofFocusBucketRadius;
            const int delta = std::clamp(layer - focus, -bucket_radius, bucket_radius);
            return focus + delta;
        };
        scratch_depth_xy_sprite_packets_.clear();
        scratch_depth_xy_sprite_packets_.rehash(std::max<std::size_t>(
            scratch_depth_xy_sprite_packets_.bucket_count(), out_data.xy_sprite_draws.size()));
        for (const GpuSpriteDrawPacket& packet : out_data.xy_sprite_draws) {
            scratch_depth_xy_sprite_packets_[bucket_depth_layer(packet.depth_layer)].push_back(packet);
        }

        scratch_depth_layer_ids_.clear();
        scratch_depth_layer_ids_.reserve(std::max(scratch_depth_layer_ids_.capacity(),
                                                  scratch_depth_xy_sprite_packets_.size()));
        for (const auto& entry : scratch_depth_xy_sprite_packets_) {
            scratch_depth_layer_ids_.push_back(entry.first);
        }
        std::sort(scratch_depth_layer_ids_.begin(), scratch_depth_layer_ids_.end(), [](int lhs, int rhs) {
            return lhs > rhs;
        });

        out_data.depth_layers.reserve(scratch_depth_layer_ids_.size());
        for (int layer_id : scratch_depth_layer_ids_) {
            GpuDepthLayerDrawPackets layer{};
            layer.depth_layer = layer_id;
            layer.packets = std::move(scratch_depth_xy_sprite_packets_[layer_id]);
            if (!std::is_sorted(layer.packets.begin(), layer.packets.end(), opengl_runtime_renderer_detail::draw_packet_sort_predicate_xy)) {
                std::stable_sort(layer.packets.begin(), layer.packets.end(), opengl_runtime_renderer_detail::draw_packet_sort_predicate_xy);
            }
            layer.blur_strength_px = render_depth::dof_blur_strength_for_layer_distance(
                layer.depth_layer,
                out_data.focus_depth_layer);
            out_data.depth_layers.push_back(std::move(layer));
        }
    }

    out_data.active_depth_layer_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        out_data.depth_layers.size(),
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.debug_overlay_draw_count = 0;

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
    const bool empty_scene_submission =
        out_data.floor_draw_count == 0 &&
        out_data.xy_sprite_draw_count == 0;
    out_data.has_valid_composite_source = !out_data.suspected_incomplete_scene && !empty_scene_submission;

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
    if (size_changed) {
        destroy_render_targets();
        output_target_width_ = width;
        output_target_height_ = height;
        dof_blur_chain_.set_output_dimensions(width, height);
    }
    auto queue_missing_main = [&](const char* label) {
        const bool exists = std::any_of(deferred_creation_queue_.begin(), deferred_creation_queue_.end(),
                                        [label](const DeferredCreationJob& job) {
                                            return job.type == DeferredCreationJob::Type::MainTarget && job.label == label;
                                        });
        if (!exists) {
            deferred_creation_queue_.push_back({DeferredCreationJob::Type::MainTarget, 0, label,
                                                render_diagnostics::current_frame_stats().frame_index, 0, creation_job_sequence_++});
        }
    };
    if (!floor_target_) queue_missing_main("floor");
    if (!xy_sprite_target_) queue_missing_main("xy_sprite");
    if (!composite_target_) queue_missing_main("composite");
    if (!process_creation_queue(frame_data, out_error)) {
        return false;
    }
    if (!floor_target_ || !xy_sprite_target_ || !composite_target_) {
        out_error = "Render target creation deferred by per-frame budget.";
        return false;
    }
    return true;
}

bool OpenGLRuntimeRenderer::ensure_depth_layer_targets(const GpuSceneFrameData& frame_data, std::string& out_error) {
    out_error.clear();
    const int width = static_cast<int>(frame_data.target_width);
    const int height = static_cast<int>(frame_data.target_height);
    if (width <= 0 || height <= 0) {
        out_error = "Invalid depth-layer render target size.";
        return false;
    }

    scratch_active_layer_ids_.clear();
    scratch_active_layer_ids_.reserve(std::max(scratch_active_layer_ids_.capacity(), frame_data.depth_layers.size()));
    for (const GpuDepthLayerDrawPackets& layer : frame_data.depth_layers) {
        scratch_active_layer_ids_.push_back(layer.depth_layer);
    }
    std::sort(scratch_active_layer_ids_.begin(), scratch_active_layer_ids_.end());

    for (auto it = depth_layer_targets_.begin(); it != depth_layer_targets_.end();) {
        if (!std::binary_search(scratch_active_layer_ids_.begin(), scratch_active_layer_ids_.end(), it->first)) {
            render_diagnostics::destroy_texture(it->second);
            it = depth_layer_targets_.erase(it);
        } else {
            ++it;
        }
    }

    for (int layer_id : scratch_active_layer_ids_) {
        SDL_Texture*& target = depth_layer_targets_[layer_id];
        if (target) {
            continue;
        }
        const bool exists = std::any_of(deferred_creation_queue_.begin(), deferred_creation_queue_.end(),
                                        [layer_id](const DeferredCreationJob& job) {
                                            return job.type == DeferredCreationJob::Type::DepthLayerTarget &&
                                                   job.layer_id == layer_id;
                                        });
        if (!exists) {
            deferred_creation_queue_.push_back({DeferredCreationJob::Type::DepthLayerTarget,
                                                layer_id,
                                                "depth_layer_" + std::to_string(layer_id),
                                                render_diagnostics::current_frame_stats().frame_index,
                                                0,
                                                creation_job_sequence_++});
        }
    }
    if (!process_creation_queue(frame_data, out_error)) {
        return false;
    }
    for (int layer_id : scratch_active_layer_ids_) {
        if (!depth_layer_targets_[layer_id]) {
            out_error = "Depth layer target creation deferred by per-frame budget.";
            return false;
        }
    }

    cached_depth_layer_ids_ = scratch_active_layer_ids_;
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

    constexpr std::size_t kMaxBatchVertices = 4096;
    constexpr std::size_t kMaxBatchIndices = 8192;
    std::array<SDL_Vertex, render_sprite_geometry::kMaxClippedVertices> packet_vertices{};
    scratch_batch_vertices_.clear();
    scratch_batch_indices_.clear();
    scratch_batch_vertices_.reserve(std::max(scratch_batch_vertices_.capacity(), kMaxBatchVertices));
    scratch_batch_indices_.reserve(std::max(scratch_batch_indices_.capacity(), kMaxBatchIndices));
    SDL_Texture* batch_texture = nullptr;

    auto flush_batch = [&]() -> bool {
        if (scratch_batch_vertices_.empty() || scratch_batch_indices_.empty()) {
            scratch_batch_vertices_.clear();
            scratch_batch_indices_.clear();
            return true;
        }
        if (!render_diagnostics::render_geometry(renderer_,
                                                 batch_texture,
                                                 scratch_batch_vertices_.data(),
                                                 static_cast<int>(scratch_batch_vertices_.size()),
                                                 scratch_batch_indices_.data(),
                                                 static_cast<int>(scratch_batch_indices_.size()))) {
            out_error = "SDL_RenderGeometry failed for batched packets: " + safe_string(SDL_GetError());
            return false;
        }
        scratch_batch_vertices_.clear();
        scratch_batch_indices_.clear();
        return true;
    };

    for (const GpuSpriteDrawPacket& packet : packets) {
        const int vertex_count = std::clamp(packet.vertex_count, 0, static_cast<int>(packet_vertices.size()));
        const int index_count = std::clamp(packet.index_count, 0, static_cast<int>(packet.indices.size()));
        if (vertex_count < 3 || index_count < 3) {
            continue;
        }
        if (batch_texture != packet.source_texture && !scratch_batch_vertices_.empty()) {
            if (!flush_batch()) {
                return false;
            }
        }

        if (batch_texture != packet.source_texture) {
            batch_texture = packet.source_texture;
        }

        const std::size_t required_vertices = scratch_batch_vertices_.size() + static_cast<std::size_t>(vertex_count);
        const std::size_t required_indices = scratch_batch_indices_.size() + static_cast<std::size_t>(index_count);
        if ((required_vertices > kMaxBatchVertices || required_indices > kMaxBatchIndices) && !scratch_batch_vertices_.empty()) {
            if (!flush_batch()) {
                return false;
            }
            batch_texture = packet.source_texture;
        }

        packet_to_vertices(packet, target_width, target_height, packet_vertices);
        const int base_vertex = static_cast<int>(scratch_batch_vertices_.size());
        scratch_batch_vertices_.insert(scratch_batch_vertices_.end(), packet_vertices.begin(), packet_vertices.begin() + vertex_count);
        for (int i = 0; i < index_count; ++i) {
            scratch_batch_indices_.push_back(packet.indices[static_cast<std::size_t>(i)] + base_vertex);
        }
    }

    return flush_batch();
}

bool OpenGLRuntimeRenderer::render_frame(std::string& out_error,
                                         SDL_Texture* ui_overlay_texture,
                                         double ui_overlay_prepare_ms,
                                         bool ui_overlay_active,
                                         bool ui_overlay_redrawn) {
    out_error.clear();
    render_diagnostics::begin_frame();
    render_diagnostics::set_texture_memory_usage(render_diagnostics::tracked_texture_bytes(), false);
    render_diagnostics::set_ui_overlay_stats(ui_overlay_active, ui_overlay_redrawn, ui_overlay_prepare_ms);

    const std::uint64_t frame_submit_begin = SDL_GetPerformanceCounter();
    const std::uint64_t perf_frequency = SDL_GetPerformanceFrequency();
    auto elapsed_ms = [perf_frequency](std::uint64_t begin, std::uint64_t end) -> double {
        if (perf_frequency == 0 || end <= begin) {
            return 0.0;
        }
        return static_cast<double>(end - begin) * 1000.0 / static_cast<double>(perf_frequency);
    };
    double frame_build_ms = 0.0;
    double render_target_sync_ms = 0.0;
    double first_ensure_targets_ms = 0.0;
    double ensure_targets_ms = 0.0;
    double floor_pass_ms = 0.0;
    double xy_pass_ms = 0.0;
    double dof_pass_ms = 0.0;
    double composite_pass_ms = 0.0;
    double ui_overlay_ms = 0.0;
    double backbuffer_ms = 0.0;

    if (!renderer_ || !assets_ || screen_width_ <= 0 || screen_height_ <= 0) {
        out_error = "OpenGL runtime renderer is not ready.";
        render_diagnostics::set_submit_result(false);
        render_diagnostics::end_frame();
        return false;
    }

    std::string size_error;
    const std::uint64_t target_sync_begin = SDL_GetPerformanceCounter();
    if (!render_target_manager_.synchronize_to_output(screen_width_, screen_height_, size_error)) {
        out_error = size_error;
        render_diagnostics::set_submit_result(false);
        render_diagnostics::end_frame();
        return false;
    }
    render_target_sync_ms = elapsed_ms(target_sync_begin, SDL_GetPerformanceCounter());

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
    const std::uint64_t first_ensure_begin = SDL_GetPerformanceCounter();
    if (!ensure_render_targets(target_size_only, out_error)) {
        render_diagnostics::set_submit_result(false);
        render_diagnostics::end_frame();
        return false;
    }
    first_ensure_targets_ms = elapsed_ms(first_ensure_begin, SDL_GetPerformanceCounter());

    const bool target_size_changed_since_last_complete =
        last_complete_scene_frame_data_.has_value() &&
        (last_complete_scene_width_ != static_cast<std::uint32_t>(effective_target->x) ||
         last_complete_scene_height_ != static_cast<std::uint32_t>(effective_target->y));
    if (target_size_changed_since_last_complete) {
        hold_after_target_resize_frames_remaining_ = 1;
        last_complete_scene_frame_data_.reset();
        last_complete_scene_width_ = 0;
        last_complete_scene_height_ = 0;
        consecutive_held_incomplete_scene_frames_ = 0;
    }

    GpuSceneFrameData frame_data{};
    const std::uint64_t frame_build_begin = SDL_GetPerformanceCounter();
    if (!build_gpu_scene_frame_data(static_cast<std::uint32_t>(effective_target->x),
                                    static_cast<std::uint32_t>(effective_target->y),
                                    frame_data,
                                    out_error,
                                    true)) {
        render_diagnostics::set_submit_result(false);
        render_diagnostics::end_frame();
        return false;
    }
    frame_build_ms = elapsed_ms(frame_build_begin, SDL_GetPerformanceCounter());
    render_diagnostics::add_draw_submission_packet_build_sort_ms(
        frame_build_ms,
        frame_data.floor_draw_count + frame_data.xy_sprite_draw_count);

    if (ui_overlay_texture) {
        frame_data.ui_overlay_texture = ui_overlay_texture;
    }
    const SDL_Color floor_clear_color = resolve_runtime_floor_clear_color();

    const bool scene_motion_active = false;
    const bool empty_scene_submission =
        frame_data.floor_draw_count == 0 &&
        frame_data.xy_sprite_draw_count == 0;
    const GpuSceneFrameData* frame_to_render = &frame_data;
    render_diagnostics::set_held_scene_frame(false, std::string{});
    render_diagnostics::set_scene_hold_gating_stats(scene_motion_active,
                                                    false,
                                                    false,
                                                    false,
                                                    false);
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
    const std::uint64_t ensure_targets_begin = SDL_GetPerformanceCounter();
    if (!ensure_render_targets(*frame_to_render, out_error)) {
        render_diagnostics::end_frame();
        return false;
    }
    ensure_targets_ms = elapsed_ms(ensure_targets_begin, SDL_GetPerformanceCounter());
    render_diagnostics::add_draw_submission_resource_create_ms(
        ensure_targets_ms,
        render_diagnostics::current_frame_stats().texture_create_count +
            render_diagnostics::current_frame_stats().gpu_buffer_create_count);

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
    auto bind_target_no_clear = [&](SDL_Texture* target) -> bool {
        if (!render_diagnostics::set_render_target(renderer_, target)) {
            out_error = std::string("Failed to bind ") + (target ? "render target" : "backbuffer") +
                        ": " + safe_string(SDL_GetError());
            return false;
        }
        SDL_SetRenderViewport(renderer_, nullptr);
        SDL_SetRenderClipRect(renderer_, nullptr);
        SDL_SetRenderScale(renderer_, 1.0f, 1.0f);
        return true;
    };

    const SDL_Color transparent_clear{0, 0, 0, 0};
    const auto& camera_settings = assets_->getView().get_settings();
    const bool dof_requested_by_settings =
        dof_blur_chain::enabled(camera_settings.depth_of_field_enabled,
                                camera_settings.blur_px,
                                camera_settings.radial_blur_px);
    const float now_seconds = assets_->game_context().elapsed_seconds();
    ingest_player_damage_pulse(now_seconds);
    prune_expired_damage_pulses(now_seconds);
    const bool dof_active = dof_requested_by_settings;
    bool dof_composited = false;
    const bool draw_movement_debug = assets_->is_dev_mode() &&
                                     assets_->movement_debug_enabled() &&
                                     assets_->movement_debug_visible();
    const bool draw_anchor_debug = assets_->is_dev_mode() &&
                                   assets_->anchor_point_debug_enabled();
    const bool draw_impass_floor_debug = assets_->is_dev_mode() &&
                                         assets_->impass_floor_debug_enabled() &&
                                         assets_->impass_floor_debug_visible();
    const std::vector<Asset*>& visible_assets_for_debug = assets_->getFilteredActiveAssets();
    auto build_movement_debug_snapshots =
        [&]() -> std::unordered_map<const Asset*, render_debug::MovementDebugAssetSnapshot> {
        std::unordered_map<const Asset*, render_debug::MovementDebugAssetSnapshot> snapshots;
        if (!draw_movement_debug) {
            return snapshots;
        }
        snapshots.reserve(visible_assets_for_debug.size());
        for (const Asset* asset : visible_assets_for_debug) {
            if (!asset || !asset->anim_) {
                continue;
            }
            render_debug::MovementDebugAssetSnapshot snapshot{};
            const AnimationUpdate::ActivePlanMode mode = asset->anim_->current_plan_mode();
            if (mode == AnimationUpdate::ActivePlanMode::Plan2D) {
                const Plan* plan = asset->anim_->current_plan();
                if (!plan || plan->sanitized_checkpoints.empty()) {
                    continue;
                }
                render_debug::MovementDebugPathSnapshot path{};
                path.world_points = plan->sanitized_checkpoints;
                if (path.world_points.size() >= 2) {
                    snapshot.paths.push_back(std::move(path));
                }
            } else if (mode == AnimationUpdate::ActivePlanMode::Plan3D) {
                const Plan3D* plan3d = asset->anim_->current_plan_3d();
                if (!plan3d || plan3d->sanitized_checkpoints.empty()) {
                    continue;
                }
                render_debug::MovementDebugPathSnapshot path{};
                path.world_points.reserve(plan3d->sanitized_checkpoints.size());
                for (const axis::WorldPos& p : plan3d->sanitized_checkpoints) {
                    path.world_points.push_back(SDL_Point{p.x, p.z});
                }
                if (path.world_points.size() >= 2) {
                    snapshot.paths.push_back(std::move(path));
                }
            }
            if (!snapshot.paths.empty()) {
                snapshots.emplace(asset, std::move(snapshot));
            }
        }
        return snapshots;
    };
    auto build_impass_floor_polygons = [&]() -> std::vector<render_debug::ImpassFloorDebugPolygon> {
        std::vector<render_debug::ImpassFloorDebugPolygon> polygons;
        if (!draw_impass_floor_debug) {
            return polygons;
        }
        const auto& entries = assets_->frame_collision_entries();
        polygons.reserve(entries.size());
        for (const auto& entry : entries) {
            if (entry.canonical_type != "impassable") {
                continue;
            }
            const auto& points = entry.area.get_points();
            if (points.size() < 3) {
                continue;
            }
            render_debug::ImpassFloorDebugPolygon polygon{};
            polygon.world_points = points;
            polygons.push_back(std::move(polygon));
        }
        return polygons;
    };

    if (dof_active) {
        const std::uint64_t floor_begin = SDL_GetPerformanceCounter();
        if (!bind_target(floor_target_, floor_clear_color)) {
            render_diagnostics::end_frame();
            return false;
        }
        if (!render_far_background(assets_->getView(),
                                   frame_to_render->target_width,
                                   frame_to_render->target_height,
                                   out_error)) {
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
        if (draw_movement_debug) {
            DebugOverlayRenderer debug_overlay(renderer_);
            const auto snapshots = build_movement_debug_snapshots();
            debug_overlay.render_movement_debug(assets_->getView(),
                                                static_cast<int>(frame_to_render->target_width),
                                                static_cast<int>(frame_to_render->target_height),
                                                snapshots,
                                                visible_assets_for_debug);
        }
        if (draw_impass_floor_debug) {
            DebugOverlayRenderer debug_overlay(renderer_);
            const auto polygons = build_impass_floor_polygons();
            debug_overlay.render_impass_floor_debug(assets_->getView(),
                                                    static_cast<int>(frame_to_render->target_width),
                                                    static_cast<int>(frame_to_render->target_height),
                                                    polygons);
        }
        floor_pass_ms += elapsed_ms(floor_begin, SDL_GetPerformanceCounter());

        const std::uint64_t dof_begin = SDL_GetPerformanceCounter();
        std::vector<dof_blur_chain::LayerTexture> dof_layers;
        auto apply_damage_pulse_to_layers = [&]() {
            constexpr float kTwoPi = 6.28318530717958647692f;
            for (dof_blur_chain::LayerTexture& layer : dof_layers) {
                layer.warp_px = 0.0f;
                layer.tint_strength = 0.0f;
                layer.phase = 0.0f;
                if (!layer.texture || active_damage_pulses_.empty()) {
                    continue;
                }

                const float layer_distance = static_cast<float>(std::abs(layer.depth_layer - frame_to_render->focus_depth_layer));
                float accum_warp = 0.0f;
                float accum_tint = 0.0f;
                float accum_phase_sin = 0.0f;
                float accum_phase_cos = 0.0f;
                float accum_phase_weight = 0.0f;

                for (const ActiveDamagePulse& pulse : active_damage_pulses_) {
                    const float age = now_seconds - pulse.pulse_time_seconds;
                    if (age < 0.0f || age > dof_blur_chain::damage_pulse_tuning::kPulseLifetimeSeconds) {
                        continue;
                    }

                    float envelope = 1.0f;
                    if (age < dof_blur_chain::damage_pulse_tuning::kEnvelopeRiseSeconds) {
                        envelope = age / std::max(1.0e-4f, dof_blur_chain::damage_pulse_tuning::kEnvelopeRiseSeconds);
                    } else {
                        envelope = std::exp(-(age - dof_blur_chain::damage_pulse_tuning::kEnvelopeRiseSeconds) /
                                            std::max(1.0e-4f, dof_blur_chain::damage_pulse_tuning::kEnvelopeDecaySeconds));
                    }

                    const float health_speed_scale =
                        dof_blur_chain::damage_pulse_tuning::kLowHealthSpeedScaleMin +
                        (1.0f - dof_blur_chain::damage_pulse_tuning::kLowHealthSpeedScaleMin) *
                            std::clamp(pulse.health_ratio_after, 0.0f, 1.0f);
                    const float propagation_speed =
                        dof_blur_chain::damage_pulse_tuning::kBasePropagationLayersPerSecond * health_speed_scale;
                    const float wave_front_distance = age * propagation_speed;
                    const float wave_delta = std::abs(layer_distance - wave_front_distance);
                    const float arrival = std::clamp(
                        1.0f - (wave_delta / std::max(1.0e-4f, dof_blur_chain::damage_pulse_tuning::kWaveFrontSoftnessLayers)),
                        0.0f,
                        1.0f);
                    const float wave_strength = envelope * arrival * pulse.amplitude;
                    if (wave_strength <= 1.0e-4f) {
                        continue;
                    }

                    accum_warp += wave_strength * dof_blur_chain::damage_pulse_tuning::kMaxWarpPx;
                    accum_tint += wave_strength * dof_blur_chain::damage_pulse_tuning::kMaxTintStrength;
                    const float phase =
                        age * dof_blur_chain::damage_pulse_tuning::kPhaseFrequencyHz * kTwoPi +
                        layer_distance * 0.85f;
                    accum_phase_sin += std::sin(phase) * wave_strength;
                    accum_phase_cos += std::cos(phase) * wave_strength;
                    accum_phase_weight += wave_strength;
                }

                layer.warp_px = std::clamp(accum_warp, 0.0f, dof_blur_chain::damage_pulse_tuning::kMaxWarpPx);
                layer.tint_strength = std::clamp(accum_tint, 0.0f, dof_blur_chain::damage_pulse_tuning::kMaxTintStrength);
                if (accum_phase_weight > 1.0e-4f) {
                    layer.phase = std::atan2(accum_phase_sin, accum_phase_cos);
                }
            }
        };
        bool used_flattened_xy_dof_layer = false;
        auto rebuild_flattened_xy_dof_layer = [&]() -> bool {
            dof_layers.clear();
            if (!bind_target(xy_sprite_target_, transparent_clear)) {
                return false;
            }
            if (!frame_to_render->xy_sprite_draws.empty() &&
                !render_packet_batch(frame_to_render->xy_sprite_draws,
                                     frame_to_render->target_width,
                                     frame_to_render->target_height,
                                     out_error)) {
                return false;
            }
            dof_layers.push_back(dof_blur_chain::LayerTexture{
                frame_to_render->focus_depth_layer + 1,
                1.0f,
                xy_sprite_target_});
            used_flattened_xy_dof_layer = true;
            return true;
        };

        if (!frame_to_render->depth_layers.empty()) {
            if (!ensure_depth_layer_targets(*frame_to_render, out_error)) {
                render_diagnostics::end_frame();
                return false;
            }
            dof_layers.reserve(frame_to_render->depth_layers.size());
            for (const GpuDepthLayerDrawPackets& layer : frame_to_render->depth_layers) {
                SDL_Texture* layer_target = depth_layer_targets_[layer.depth_layer];
                if (!bind_target(layer_target, transparent_clear)) {
                    render_diagnostics::end_frame();
                    return false;
                }
                if (!render_packet_batch(layer.packets,
                                         frame_to_render->target_width,
                                         frame_to_render->target_height,
                                         out_error)) {
                    render_diagnostics::end_frame();
                    return false;
                }
                dof_layers.push_back(dof_blur_chain::LayerTexture{layer.depth_layer, layer.blur_strength_px, layer_target});
            }
        } else if (!rebuild_flattened_xy_dof_layer()) {
            render_diagnostics::end_frame();
            return false;
        }
        apply_damage_pulse_to_layers();

        const SDL_Point screen_center = assets_->getView().get_screen_center();
        const SDL_FPoint optical_center{
            std::clamp(static_cast<float>(screen_center.x), 0.0f, static_cast<float>(frame_to_render->target_width)),
            std::clamp(static_cast<float>(screen_center.y), 0.0f, static_cast<float>(frame_to_render->target_height))};
        dof_blur_chain_.set_output_dimensions(static_cast<int>(frame_to_render->target_width),
                                              static_cast<int>(frame_to_render->target_height));
        const dof_blur_chain::CompositeResult dof_result =
            dof_blur_chain_.compose(dof_layers,
                                    floor_target_,
                                    camera_settings.depth_of_field_enabled,
                                    camera_settings.blur_px,
                                    camera_settings.radial_blur_px,
                                    optical_center,
                                    frame_to_render->focus_depth_layer);

        dof_blur_chain::CompositeResult resolved_dof_result = dof_result;
        if (resolved_dof_result.valid &&
            resolved_dof_result.blur_pass_count == 0 &&
            !used_flattened_xy_dof_layer &&
            !frame_to_render->xy_sprite_draws.empty()) {
            if (!rebuild_flattened_xy_dof_layer()) {
                render_diagnostics::end_frame();
                return false;
            }
            apply_damage_pulse_to_layers();
            resolved_dof_result =
                dof_blur_chain_.compose(dof_layers,
                                        floor_target_,
                                        camera_settings.depth_of_field_enabled,
                                        camera_settings.blur_px,
                                        camera_settings.radial_blur_px,
                                        optical_center,
                                        frame_to_render->focus_depth_layer);
        }

        if (resolved_dof_result.valid) {
            if (!bind_target(composite_target_, transparent_clear)) {
                render_diagnostics::end_frame();
                return false;
            }
            if (resolved_dof_result.background_mid &&
                !render_diagnostics::render_texture(renderer_, resolved_dof_result.background_mid, nullptr, &full_rect)) {
                out_error = "Failed to composite DoF background target: " + safe_string(SDL_GetError());
                render_diagnostics::end_frame();
                return false;
            }
            if (resolved_dof_result.foreground_mid &&
                !render_diagnostics::render_texture(renderer_, resolved_dof_result.foreground_mid, nullptr, &full_rect)) {
                out_error = "Failed to composite DoF foreground target: " + safe_string(SDL_GetError());
                render_diagnostics::end_frame();
                return false;
            }
            render_diagnostics::set_blur_pass_count(resolved_dof_result.blur_pass_count);
            if (used_flattened_xy_dof_layer) {
                render_diagnostics::set_composite_layers_submitted("floor_pass->dof_flattened_xy_layer->ui_overlay");
            } else {
                render_diagnostics::set_composite_layers_submitted("floor_pass->dof_background_mid->dof_foreground_mid->ui_overlay");
            }
            dof_composited = true;
        } else {
            vibble::log::warn("[OpenGLRuntimeRenderer] DoF blur chain failed; falling back to non-DoF XY sprite composite for this frame.");
            render_diagnostics::set_blur_pass_count(0);
        }
        dof_pass_ms += elapsed_ms(dof_begin, SDL_GetPerformanceCounter());
        last_dof_path_ms_ = floor_pass_ms + dof_pass_ms;
    } else {
        if (!dof_requested_by_settings) {
            destroy_depth_layer_targets();
        }
        render_diagnostics::set_blur_pass_count(0);
    }

    if (!dof_composited) {
        if (!bind_target(composite_target_, floor_clear_color)) {
            render_diagnostics::end_frame();
            return false;
        }

        const std::uint64_t floor_begin = SDL_GetPerformanceCounter();
        if (!render_far_background(assets_->getView(),
                                   frame_to_render->target_width,
                                   frame_to_render->target_height,
                                   out_error)) {
            render_diagnostics::end_frame();
            return false;
        }
        if (!frame_to_render->floor_draws.empty() &&
            !render_packet_batch(frame_to_render->floor_draws,
                                 frame_to_render->target_width,
                                 frame_to_render->target_height,
                                 out_error)) {
            render_diagnostics::end_frame();
            return false;
        }
        if (draw_movement_debug) {
            DebugOverlayRenderer debug_overlay(renderer_);
            const auto snapshots = build_movement_debug_snapshots();
            debug_overlay.render_movement_debug(assets_->getView(),
                                                static_cast<int>(frame_to_render->target_width),
                                                static_cast<int>(frame_to_render->target_height),
                                                snapshots,
                                                visible_assets_for_debug);
        }
        if (draw_impass_floor_debug) {
            DebugOverlayRenderer debug_overlay(renderer_);
            const auto polygons = build_impass_floor_polygons();
            debug_overlay.render_impass_floor_debug(assets_->getView(),
                                                    static_cast<int>(frame_to_render->target_width),
                                                    static_cast<int>(frame_to_render->target_height),
                                                    polygons);
        }
        floor_pass_ms += elapsed_ms(floor_begin, SDL_GetPerformanceCounter());

        const std::uint64_t xy_begin = SDL_GetPerformanceCounter();
        if (!frame_to_render->xy_sprite_draws.empty() &&
            !render_packet_batch(frame_to_render->xy_sprite_draws,
                                 frame_to_render->target_width,
                                 frame_to_render->target_height,
                                 out_error)) {
            render_diagnostics::end_frame();
            return false;
        }
        xy_pass_ms += elapsed_ms(xy_begin, SDL_GetPerformanceCounter());
        render_diagnostics::set_composite_layers_submitted("direct_scene->ui_overlay");
    }

    if (draw_anchor_debug) {
        DebugOverlayRenderer debug_overlay(renderer_);
        debug_overlay.render_anchor_debug(assets_->getView(),
                                          static_cast<int>(frame_to_render->target_width),
                                          static_cast<int>(frame_to_render->target_height),
                                          visible_assets_for_debug,
                                          assets_->is_dev_mode());
    }

    if (frame_to_render->ui_overlay_texture) {
        const std::uint64_t ui_begin = SDL_GetPerformanceCounter();
        if (!render_diagnostics::render_texture(renderer_, frame_to_render->ui_overlay_texture, nullptr, &full_rect)) {
            out_error = "Failed to composite UI overlay texture: " + safe_string(SDL_GetError());
            render_diagnostics::end_frame();
            return false;
        }
        ui_overlay_ms += elapsed_ms(ui_begin, SDL_GetPerformanceCounter());
    }

    const std::uint64_t backbuffer_begin = SDL_GetPerformanceCounter();
    if (!bind_target_no_clear(nullptr)) {
        render_diagnostics::end_frame();
        return false;
    }
    if (!render_diagnostics::render_texture(renderer_, composite_target_, nullptr, &full_rect)) {
        out_error = "Failed to present composite target: " + safe_string(SDL_GetError());
        render_diagnostics::end_frame();
        return false;
    }
    backbuffer_ms = elapsed_ms(backbuffer_begin, SDL_GetPerformanceCounter());

    const double submit_ms = elapsed_ms(frame_submit_begin, SDL_GetPerformanceCounter());
    const double pipeline_bind_ms = floor_pass_ms + xy_pass_ms + dof_pass_ms + composite_pass_ms + ui_overlay_ms + backbuffer_ms;
    const double accounted_submit_ms =
        render_target_sync_ms +
        first_ensure_targets_ms +
        frame_build_ms +
        ensure_targets_ms +
        pipeline_bind_ms;
    const double submit_unaccounted_ms = std::max(0.0, submit_ms - accounted_submit_ms);
    render_diagnostics::add_draw_submission_pipeline_bind_ms(
        pipeline_bind_ms,
        render_diagnostics::current_frame_stats().render_target_switch_count +
            static_cast<std::uint32_t>(render_diagnostics::current_frame_stats().gpu_pipeline_cache_misses));
    render_diagnostics::add_draw_submission_submit_present_handoff_ms(backbuffer_ms, 1);
    render_diagnostics::add_draw_submission_ms(submit_ms);
    render_diagnostics::set_draw_submission_breakdown(submit_unaccounted_ms,
                                                      render_target_sync_ms,
                                                      first_ensure_targets_ms,
                                                      ensure_targets_ms);
    const bool empty_scene_after_startup = empty_scene_submission && startup_scene_submission_established_;
    if (empty_scene_after_startup) {
        ++post_startup_empty_scene_frame_count_;
    }

    std::ostringstream stage_summary;
    stage_summary << "target_sync=" << render_target_sync_ms
                  << " first_ensure_targets=" << first_ensure_targets_ms
                  << " frame_build=" << frame_build_ms
                  << " ensure_targets=" << ensure_targets_ms
                  << " floor=" << floor_pass_ms
                  << " xy=" << xy_pass_ms
                  << " dof=" << dof_pass_ms
                  << " dof_motion_skip=0"
                  << " scene_motion_active=0"
                  << " hold_motion_gated_suppressed=0"
                  << " empty_scene_post_startup_count=" << post_startup_empty_scene_frame_count_
                  << " empty_scene_hold_bypassed_runtime_motion=0"
                  << " empty_scene_hold_bypassed_startup_policy=0"
                  << " dof_last_ms=" << last_dof_path_ms_
                  << " composite=" << composite_pass_ms
                  << " ui_prepare=" << ui_overlay_prepare_ms
                  << " ui_copy=" << ui_overlay_ms
                  << " backbuffer=" << backbuffer_ms
                  << " submit=" << submit_ms
                  << " submit_unaccounted=" << submit_unaccounted_ms;
    render_diagnostics::set_render_stage_timings(stage_summary.str());
    render_diagnostics::set_submit_result(true);

    if (frame_data.has_valid_composite_source) {
        last_complete_scene_frame_data_ = frame_data;
        last_complete_scene_width_ = frame_data.target_width;
        last_complete_scene_height_ = frame_data.target_height;
        hold_after_target_resize_frames_remaining_ = 0;
        startup_scene_submission_established_ = true;
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

    return map_default;
}
