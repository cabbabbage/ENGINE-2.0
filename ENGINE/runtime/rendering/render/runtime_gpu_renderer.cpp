#include "rendering/render/runtime_gpu_renderer.hpp"

#include "core/AssetsManager.hpp"
#include "assets/asset/Asset.hpp"
#include "gameplay/world/chunk.hpp"
#include "rendering/render/render.hpp"
#include "rendering/render/render_diagnostics.hpp"
#include "rendering/render/layer_depth_bins.hpp"
#include "rendering/render/render_depth_policy.hpp"
#include "rendering/render/render_object.hpp"
#include "rendering/render/render_object_builder.hpp"
#include "rendering/render/render_object_projection.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace {

int renderer_max_texture_size(SDL_Renderer* renderer) {
    if (!renderer) {
        return 0;
    }
    const SDL_PropertiesID renderer_props = SDL_GetRendererProperties(renderer);
    if (!renderer_props) {
        return 0;
    }
    return static_cast<int>(
        SDL_GetNumberProperty(renderer_props, SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 0));
}

int clamp_dimension_to_renderer_limit(int value, int renderer_limit, const char* axis_label) {
    const int safe_value = std::max(1, value);
    if (renderer_limit <= 0 || safe_value <= renderer_limit) {
        return safe_value;
    }
    vibble::log::warn(std::string{"[RuntimeGpuRenderer] Clamping "} + axis_label +
                      " dimension from " + std::to_string(safe_value) +
                      " to renderer max texture size " + std::to_string(renderer_limit) + ".");
    return renderer_limit;
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

struct SpriteBatchVertexUniformData {
    SDL_FColor vertex_uv[6]{};
    SDL_FColor modulate{1.0f, 1.0f, 1.0f, 1.0f};
};

std::uintptr_t floor_sort_id(bool sprite_packet, std::uintptr_t sequence) {
    constexpr std::uintptr_t kSpritePacketSortOffset =
        std::uintptr_t{1} << ((sizeof(std::uintptr_t) * 8u) - 1u);
    return sprite_packet ? (kSpritePacketSortOffset + sequence) : sequence;
}

int classify_depth_layer_for_asset(const WarpedScreenGrid& camera, const Asset& asset) {
    const auto& settings = camera.get_settings();
    const double focus_world_z = camera.current_focus_plane_world_z();
    const double asset_world_z =
        static_cast<double>(asset.world_z()) +
        static_cast<double>(asset.world_z_offset()) +
        static_cast<double>(asset.render_anchor_offset_z());
    const double signed_distance = asset_world_z - focus_world_z;
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

} // namespace

namespace runtime_gpu_renderer_detail {

int classify_depth_layer_for_asset(const WarpedScreenGrid& camera, const Asset& asset) {
    return ::classify_depth_layer_for_asset(camera, asset);
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
            packet.sort_key = std::max(screen_br.y, screen_bl.y);
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

    const auto draw_sort_predicate = [](const GpuSpriteDrawPacket& lhs,
                                        const GpuSpriteDrawPacket& rhs) {
        if (lhs.sort_key == rhs.sort_key) {
            return lhs.stable_sort_id < rhs.stable_sort_id;
        }
        return lhs.sort_key < rhs.sort_key;
    };
    std::sort(out_packets.begin(), out_packets.end(), draw_sort_predicate);
    return true;
}

} // namespace runtime_gpu_renderer_detail

void RuntimeGpuRenderer::RenderTargetLifecycleManager::set_requested_size(int screen_width,
                                                                          int screen_height) {
    requested_width = std::max(1, screen_width);
    requested_height = std::max(1, screen_height);
    active_width = requested_width;
    active_height = requested_height;
}

bool RuntimeGpuRenderer::RenderTargetLifecycleManager::synchronize_to_swapchain(Uint32 swapchain_width,
                                                                               Uint32 swapchain_height,
                                                                               std::string& out_error) {
    out_error.clear();
    if (swapchain_width == 0 || swapchain_height == 0) {
        out_error = "Swapchain dimensions are invalid.";
        return false;
    }
    active_width = static_cast<int>(swapchain_width);
    active_height = static_cast<int>(swapchain_height);
    return true;
}

std::optional<SDL_Point> RuntimeGpuRenderer::RenderTargetLifecycleManager::current_size() const {
    const int width = active_width > 0 ? active_width : requested_width;
    const int height = active_height > 0 ? active_height : requested_height;
    if (width <= 0 || height <= 0) {
        return std::nullopt;
    }
    return SDL_Point{width, height};
}

RuntimeGpuRenderer::RuntimeGpuRenderer(SDL_Renderer* renderer,
                                       Assets* assets,
                                       int screen_width,
                                       int screen_height)
    : renderer_(renderer),
      assets_(assets),
      screen_width_(std::max(1, screen_width)),
      screen_height_(std::max(1, screen_height)) {
    render_target_manager_.set_requested_size(screen_width_, screen_height_);
}

RuntimeGpuRenderer::~RuntimeGpuRenderer() = default;

std::unique_ptr<RuntimeGpuRenderer> RuntimeGpuRenderer::Create(SDL_Renderer* renderer,
                                                               Assets* assets,
                                                               int screen_width,
                                                               int screen_height,
                                                               std::string& out_error) {
    auto runtime_renderer = std::unique_ptr<RuntimeGpuRenderer>(
        new RuntimeGpuRenderer(renderer, assets, screen_width, screen_height));
    if (!runtime_renderer->initialize(out_error)) {
        return nullptr;
    }
    return runtime_renderer;
}

bool RuntimeGpuRenderer::initialize(std::string& out_error) {
    out_error.clear();
    std::string gpu_error;
    backend_owner_.scene_renderer = GpuSceneRenderer::Create(renderer_, false, gpu_error);
    if (!backend_owner_.ready()) {
        out_error = "[RuntimeGpuRenderer] GPU runtime renderer initialization failed: " + gpu_error;
        return false;
    }

    const std::filesystem::path shader_manifest_path = render_internal::runtime_gpu_shader_manifest_path();
    if (!std::filesystem::exists(shader_manifest_path)) {
        out_error = "[RuntimeGpuRenderer] GPU runtime manifest missing: " + shader_manifest_path.string();
        return false;
    }
    if (!backend_owner_.get()->load_shader_packages(shader_manifest_path.string(), gpu_error)) {
        out_error = "[RuntimeGpuRenderer] GPU shader package load failed: " + gpu_error +
                    " manifest=" + shader_manifest_path.string();
        return false;
    }

    if (!ensure_scene_target(out_error)) {
        return false;
    }

    if (!validate_preloaded_sprite_textures(out_error)) {
        return false;
    }

    return true;
}

void RuntimeGpuRenderer::set_output_dimensions(int screen_width, int screen_height) {
    screen_width_ = std::max(1, screen_width);
    screen_height_ = std::max(1, screen_height);
    if (renderer_) {
        const int max_texture_size = renderer_max_texture_size(renderer_);
        screen_width_ = clamp_dimension_to_renderer_limit(screen_width_, max_texture_size, "scene width");
        screen_height_ = clamp_dimension_to_renderer_limit(screen_height_, max_texture_size, "scene height");
    }
    render_target_manager_.set_requested_size(screen_width_, screen_height_);
}

std::optional<SDL_Point> RuntimeGpuRenderer::scene_target_size() const {
    return render_target_manager_.current_size();
}

bool RuntimeGpuRenderer::validate_preloaded_sprite_textures(std::string& out_error) const {
    out_error.clear();
    if (!assets_) {
        out_error = "[RuntimeGpuRenderer] GPU preload validation failed: assets context unavailable.";
        return false;
    }
    if (!backend_owner_.ready() || !backend_owner_.get()) {
        out_error = "[RuntimeGpuRenderer] GPU preload validation failed: backend renderer unavailable.";
        return false;
    }

    std::vector<std::string> failures;
    failures.reserve(8);
    constexpr std::size_t kMaxLoggedFailures = 8;

    const auto record_failure = [&](std::string failure) {
        if (failures.size() < kMaxLoggedFailures) {
            failures.push_back(std::move(failure));
        }
    };

    for (Asset* asset : assets_->all) {
        if (!asset || !asset->info || asset->dead) {
            continue;
        }

        render_build::DirectAssetRenderCacheRecord cache_record{};
        if (!render_build::refresh_direct_asset_render_cache(asset, cache_record)) {
            record_failure(describe_asset_sprite_source(asset) +
                           " detail='sprite texture was not resolved during preload validation.'");
            continue;
        }

        std::string texture_error;
        if (!backend_owner_.get()->resolve_gpu_texture_for_sdl_texture(cache_record.texture, texture_error)) {
            const std::string texture_id =
                "sdl_texture_ptr=" + std::to_string(reinterpret_cast<std::uintptr_t>(cache_record.texture));
            record_failure(describe_asset_sprite_source(asset,
                                                       texture_id.c_str(),
                                                       cache_record.frame_identity,
                                                       cache_record.variant_identity) +
                           " detail='" + (texture_error.empty()
                                              ? "unknown SDL->GPU texture import failure."
                                              : texture_error) + "'");
        }
    }

    if (failures.empty()) {
        return true;
    }

    std::string message = "[RuntimeGpuRenderer] GPU preload validation failed: ";
    for (std::size_t i = 0; i < failures.size(); ++i) {
        if (i > 0) {
            message += " | ";
        }
        message += failures[i];
    }
    if (failures.size() == kMaxLoggedFailures) {
        message += " | additional failures omitted";
    }

    vibble::log::error(message);
    out_error = message;
    return false;
}

const std::string& RuntimeGpuRenderer::present_mode() const {
    static const std::string kUnknown = "unknown";
    return (backend_owner_.ready() && backend_owner_.get()->device())
        ? backend_owner_.get()->device()->present_mode()
        : kUnknown;
}

const std::string& RuntimeGpuRenderer::backend_name() const {
    static const std::string kUnknown = "unknown";
    return (backend_owner_.ready() && backend_owner_.get()->device())
        ? backend_owner_.get()->device()->backend_name()
        : kUnknown;
}

bool RuntimeGpuRenderer::ensure_scene_target(std::string& out_error) {
    out_error.clear();
    if (!renderer_ || !backend_owner_.ready() || screen_width_ <= 0 || screen_height_ <= 0) {
        out_error = "RuntimeGpuRenderer not ready for scene target initialization.";
        return false;
    }

    const int max_texture_size = renderer_max_texture_size(renderer_);
    screen_width_ = clamp_dimension_to_renderer_limit(screen_width_, max_texture_size, "scene width");
    screen_height_ = clamp_dimension_to_renderer_limit(screen_height_, max_texture_size, "scene height");
    render_target_manager_.set_requested_size(screen_width_, screen_height_);
    GpuSceneRenderer::SamplerResourceSpec sampler_spec{};
    return backend_owner_.get()->ensure_sampler_resource("linear_clamp", sampler_spec, out_error);
}

std::vector<world::Chunk*> RuntimeGpuRenderer::runtime_floor_chunks() const {
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
        vibble::log::warn("[RuntimeGpuRenderer] Active chunk set had no floor tiles; falling back to all world chunks.");
    }
    return assets_->world_grid().all_chunks();
}

bool runtime_gpu_renderer_detail::build_floor_sprite_draw_packets(
    const WarpedScreenGrid& camera,
    const std::vector<Asset*>& visible_assets,
    std::uint32_t target_width,
    std::uint32_t target_height,
    std::vector<GpuSpriteDrawPacket>& out_floor_draws,
    std::vector<GpuSpriteDrawPacket>& out_layer_draws,
    std::string& out_error) {
    out_layer_draws.clear();
    out_floor_draws.reserve(out_floor_draws.size() + visible_assets.size());
    out_layer_draws.reserve(visible_assets.size());
    out_error.clear();

    const float output_w = static_cast<float>(std::max<std::uint32_t>(1u, target_width));
    const float output_h = static_cast<float>(std::max<std::uint32_t>(1u, target_height));
    std::uintptr_t floor_sequence = 0u;
    std::uintptr_t layer_sequence = 0u;

    for (Asset* asset : visible_assets) {
        if (!asset || asset->dead || !asset->info) {
            continue;
        }

        RenderObject object{};
        if (!render_build::build_direct_asset_render_object(asset, object) || !object.texture) {
            continue;
        }

        const Asset::PerspectiveSample perspective = asset->runtime_perspective_sample();
        const float perspective_scale = perspective.scale;
        const float world_z = static_cast<float>(asset->world_z()) + object.world_z_offset;

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
        packet.sort_key = std::max(projected.screen_bl.y, projected.screen_br.y);
        fill_sprite_packet_vertices(projected, u0, v0, u1, v1, output_w, output_h, packet);

        if (asset->isFloorBoxesEnabled()) {
            packet.stable_sort_id = floor_sort_id(true, floor_sequence++);
            packet.is_floor_packet = true;
            packet.depth_layer = 0;
            runtime_gpu_renderer_detail::append_classified_sprite_draw_packet(
                true, packet, out_floor_draws, out_layer_draws);
            continue;
        }

        packet.stable_sort_id = floor_sort_id(false, layer_sequence++);
        packet.is_floor_packet = false;
        packet.depth_layer = runtime_gpu_renderer_detail::classify_depth_layer_for_asset(camera, *asset);
        runtime_gpu_renderer_detail::append_classified_sprite_draw_packet(
            false, packet, out_floor_draws, out_layer_draws);
    }

    const auto draw_sort_predicate = [](const GpuSpriteDrawPacket& lhs,
                                        const GpuSpriteDrawPacket& rhs) {
        if (lhs.sort_key == rhs.sort_key) {
            return lhs.stable_sort_id < rhs.stable_sort_id;
        }
        return lhs.sort_key < rhs.sort_key;
    };
    std::sort(out_floor_draws.begin(), out_floor_draws.end(), draw_sort_predicate);
    std::stable_sort(out_layer_draws.begin(), out_layer_draws.end(), draw_sort_predicate);
    return true;
}

void runtime_gpu_renderer_detail::append_classified_sprite_draw_packet(bool floor_tagged,
                                                                       const GpuSpriteDrawPacket& packet,
                                                                       std::vector<GpuSpriteDrawPacket>& out_floor_draws,
                                                                       std::vector<GpuSpriteDrawPacket>& out_layer_draws) {
    if (floor_tagged) {
        out_floor_draws.push_back(packet);
        return;
    }
    out_layer_draws.push_back(packet);
}

const std::vector<Asset*>& runtime_gpu_renderer_detail::select_visible_assets_for_gpu_frame(
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

bool RuntimeGpuRenderer::build_gpu_scene_frame_data(std::uint32_t target_width,
                                                    std::uint32_t target_height,
                                                    GpuSceneFrameData& out_data,
                                                    std::string& out_error) const {
    out_data = GpuSceneFrameData{};
    out_error.clear();
    if (!assets_) {
        out_error = "Assets context unavailable for GPU scene frame packet build.";
        return false;
    }

    const std::vector<Asset*>& active_assets = assets_->getActive();
    const std::vector<Asset*>& filtered_active_assets = assets_->getFilteredActiveAssets();
    const std::vector<Asset*>& all_assets = assets_->all;
    bool used_active_fallback = false;
    const std::vector<Asset*>& selected_visible_assets =
        runtime_gpu_renderer_detail::select_visible_assets_for_gpu_frame(
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

    out_data.target_width = target_width;
    out_data.target_height = target_height;
    out_data.active_asset_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        active_assets.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.filtered_active_asset_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        filtered_active_assets.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.selected_asset_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        visible_assets->size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.visible_traversal_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        traversal_count, static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.dev_mode = assets_->is_dev_mode();
    out_data.focus_filter_active = assets_->focus_filter_active();
    out_data.used_active_asset_fallback = used_active_fallback;

    if (used_active_fallback) {
        vibble::log::warn("[RuntimeGpuRenderer] Dev filtered render list was empty; using active assets for this GPU frame. "
                          "active_count=" + std::to_string(active_assets.size()) +
                          " filtered_count=" + std::to_string(filtered_active_assets.size()) +
                          " focus_filter_active=" + std::string(assets_->focus_filter_active() ? "true" : "false") +
                          " traversal_count=" + std::to_string(traversal_count));
    }

    if (!runtime_gpu_renderer_detail::build_floor_tile_draw_packets(
            camera,
            runtime_floor_chunks(),
            target_width,
            target_height,
            out_data.floor_draws)) {
        out_error = "Failed to build map-floor tile packets.";
        return false;
    }

    if (!runtime_gpu_renderer_detail::build_floor_sprite_draw_packets(
            camera,
            *visible_assets,
            target_width,
            target_height,
            out_data.floor_draws,
            out_data.layer_draws,
            out_error)) {
        return false;
    }

    // Emergency fallback for movement-time visibility/culling gaps: retry sprite packet build
    // against all tracked assets before presenting a floor-only frame.
    if (out_data.layer_draws.empty() && !all_assets.empty() && !used_all_assets_visibility_fallback) {
        std::vector<GpuSpriteDrawPacket> fallback_floor_draws = out_data.floor_draws;
        std::vector<GpuSpriteDrawPacket> fallback_layer_draws{};
        std::string fallback_error;
        if (!runtime_gpu_renderer_detail::build_floor_sprite_draw_packets(
                camera,
                all_assets,
                target_width,
                target_height,
                fallback_floor_draws,
                fallback_layer_draws,
                fallback_error)) {
            out_error = fallback_error;
            return false;
        }
        if (!fallback_layer_draws.empty()) {
            out_data.floor_draws = std::move(fallback_floor_draws);
            out_data.layer_draws = std::move(fallback_layer_draws);
            out_data.selected_asset_count = static_cast<std::uint32_t>(std::min<std::size_t>(
                all_assets.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
            vibble::log::warn("[RuntimeGpuRenderer] Rebuilt sprite packets from all assets after primary visibility pass produced no layer packets.");
        }
    }

    for (std::vector<GpuSpriteDrawPacket>* draws : {&out_data.floor_draws, &out_data.layer_draws}) {
        for (GpuSpriteDrawPacket& draw : *draws) {
            if (!draw.source_texture) {
                out_error = "FATAL missing source SDL texture; " + describe_draw_packet_source(draw) +
                            " renderer='RuntimeGpuRenderer' frame_pass='build_gpu_scene_frame_data'";
                return false;
            }
            std::string texture_error;
            draw.source_gpu_texture = backend_owner_.get()->resolve_gpu_texture_for_sdl_texture(draw.source_texture, texture_error);
            if (!draw.source_gpu_texture) {
                out_error = "FATAL missing GPU-backed texture; " + describe_draw_packet_source(draw) +
                            " renderer='RuntimeGpuRenderer' frame_pass='build_gpu_scene_frame_data' detail='" +
                            texture_error + "'";
                return false;
            }
        }
    }

    out_data.floor_draw_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        out_data.floor_draws.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.layer_sprite_draw_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        out_data.layer_draws.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    std::unordered_map<int, std::vector<GpuSpriteDrawPacket>> depth_layer_packets{};
    depth_layer_packets.reserve(out_data.layer_draws.size());
    for (const GpuSpriteDrawPacket& packet : out_data.layer_draws) {
        depth_layer_packets[packet.depth_layer].push_back(packet);
    }
    std::vector<int> depth_layer_ids{};
    depth_layer_ids.reserve(depth_layer_packets.size());
    for (const auto& entry : depth_layer_packets) {
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
        layer.packets = std::move(depth_layer_packets[layer_id]);
        const auto draw_sort_predicate = [](const GpuSpriteDrawPacket& lhs,
                                            const GpuSpriteDrawPacket& rhs) {
            if (lhs.sort_key == rhs.sort_key) {
                return lhs.stable_sort_id < rhs.stable_sort_id;
            }
            return lhs.sort_key < rhs.sort_key;
        };
        std::stable_sort(layer.packets.begin(), layer.packets.end(), draw_sort_predicate);
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
    out_data.active_depth_layer_count = static_cast<std::uint32_t>(
        std::min<std::size_t>(out_data.depth_layers.size(),
                              static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.debug_overlay_draw_count = 0;
    out_data.has_valid_composite_source = true;

    render_diagnostics::set_packet_counts(out_data.floor_draw_count, out_data.layer_sprite_draw_count);
    render_diagnostics::set_active_depth_layer_count(out_data.active_depth_layer_count);
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
    }

    if (out_data.floor_draw_count == 0) {
        vibble::log::warn("[RuntimeGpuRenderer] No floor draw packets were built; floor target will clear transparently.");
    }
    out_data.suspected_incomplete_scene =
        out_data.layer_sprite_draw_count == 0 &&
        out_data.active_asset_count > 0 &&
        (out_data.selected_asset_count > 0 || out_data.visible_traversal_count > 0);
    if (out_data.suspected_incomplete_scene) {
        vibble::log::warn("[RuntimeGpuRenderer] GPU frame has floor packets but no layer packets. "
                          "active_count=" + std::to_string(active_assets.size()) +
                          " filtered_count=" + std::to_string(filtered_active_assets.size()) +
                          " selected_count=" + std::to_string(visible_assets->size()) +
                          " focus_filter_active=" + std::string(assets_->focus_filter_active() ? "true" : "false") +
                          " traversal_count=" + std::to_string(traversal_count));
    }

    return true;
}

bool RuntimeGpuRenderer::render_draw_packet_batch(SDL_GPURenderPass* render_pass,
                                                  const std::vector<GpuSpriteDrawPacket>& packets,
                                                  const char* pass_label,
                                                  std::string& out_error) {
    out_error.clear();
    if (packets.empty()) {
        return true;
    }
    if (!render_pass) {
        out_error = "Render pass handle was null.";
        return false;
    }
    if (!backend_owner_.ready() || !backend_owner_.get()->device()) {
        out_error = "GPU backend owner is unavailable while drawing " +
                    std::string(pass_label ? pass_label : "packets");
        return false;
    }
    if (frame_context_.command_buffer == nullptr) {
        out_error = "Command buffer was null while drawing " +
                    std::string(pass_label ? pass_label : "packets");
        return false;
    }

    SDL_GPUGraphicsPipeline* pipeline =
        backend_owner_.get()->resolve_graphics_pipeline("sprite_textured", 0x2100u);
    if (!pipeline) {
        out_error = "Failed to resolve graphics pipeline 'sprite_textured' for " +
                    std::string(pass_label ? pass_label : "packets");
        return false;
    }

    SDL_GPUSampler* linear_sampler = backend_owner_.get()->find_sampler_resource("linear_clamp");
    if (!linear_sampler) {
        out_error = "Sampler 'linear_clamp' not available while drawing " +
                    std::string(pass_label ? pass_label : "packets");
        return false;
    }

    SDL_BindGPUGraphicsPipeline(render_pass, pipeline);

    for (const GpuSpriteDrawPacket& draw : packets) {
        SDL_GPUTexture* gpu_texture = draw.source_gpu_texture
            ? draw.source_gpu_texture
            : backend_owner_.get()->find_gpu_texture_for_sdl_texture(draw.source_texture);
        if (!gpu_texture) {
            out_error = "FATAL missing GPU-backed texture; " + describe_draw_packet_source(draw) +
                        " renderer='RuntimeGpuRenderer' frame_pass='" +
                        std::string(pass_label ? pass_label : "packets") + "'";
            render_diagnostics::add_skipped_texture_count();
            render_diagnostics::set_failed_texture_names(describe_draw_packet_source(draw));
            return false;
        }

        SDL_GPUTextureSamplerBinding sampler_binding{};
        sampler_binding.texture = gpu_texture;
        sampler_binding.sampler = linear_sampler;
        SDL_BindGPUFragmentSamplers(render_pass, 0u, &sampler_binding, 1u);

        SpriteBatchVertexUniformData uniform_data{};
        for (std::size_t i = 0; i < draw.vertices.size(); ++i) {
            const GpuSpriteVertex& vertex = draw.vertices[i];
            uniform_data.vertex_uv[i] = SDL_FColor{vertex.clip_x, vertex.clip_y, vertex.uv_x, vertex.uv_y};
        }
        uniform_data.modulate = draw.modulate;
        SDL_PushGPUVertexUniformData(frame_context_.command_buffer,
                                     0u,
                                     &uniform_data,
                                     static_cast<Uint32>(sizeof(uniform_data)));

        SDL_DrawGPUPrimitives(render_pass, 6u, 1u, 0u, 0u);
        render_diagnostics::add_draw_call_count();
        ++frame_context_.stats.draw_call_count;
    }

    return true;
}

bool RuntimeGpuRenderer::render_frame(std::string& out_error, SDL_Texture* ui_overlay_texture) {
    out_error.clear();
    frame_context_ = FrameContext{};

    if (!backend_owner_.ready() || !backend_owner_.get()->device()) {
        out_error = "GPU backend owner is unavailable.";
        return false;
    }

    GpuRenderDevice* device = backend_owner_.get()->device();
    if (!device) {
        out_error = "GPU render device is unavailable.";
        return false;
    }

    const std::uint64_t pipeline_hits_before = backend_owner_.get()->pipeline_cache().total_hits();
    const std::uint64_t pipeline_misses_before = backend_owner_.get()->pipeline_cache().total_misses();

    std::string frame_error;
    if (!device->begin_frame(frame_error)) {
        out_error = "Failed to acquire GPU frame: " + frame_error;
        return false;
    }

    const GpuRenderDevice::FrameState& frame_state = device->frame_state();
    frame_context_.command_buffer = frame_state.command_buffer;
    frame_context_.swapchain_texture = frame_state.swapchain_texture;
    frame_context_.swapchain_width = frame_state.swapchain_width;
    frame_context_.swapchain_height = frame_state.swapchain_height;
    render_diagnostics::set_command_buffer_acquired(frame_context_.command_buffer != nullptr);
    render_diagnostics::set_swapchain_acquired(frame_context_.swapchain_texture != nullptr);
    render_diagnostics::set_swapchain_dimensions(frame_context_.swapchain_width, frame_context_.swapchain_height);

    if (frame_context_.command_buffer == nullptr) {
        out_error = "Failed to acquire GPU command buffer.";
        (void)device->end_frame(false, frame_error);
        return false;
    }
    if (frame_context_.swapchain_texture == nullptr) {
        out_error = "Failed to acquire GPU swapchain texture.";
        (void)device->end_frame(false, frame_error);
        return false;
    }
    if (!render_target_manager_.synchronize_to_swapchain(frame_context_.swapchain_width,
                                                         frame_context_.swapchain_height,
                                                         out_error)) {
        (void)device->end_frame(false, frame_error);
        return false;
    }

    const std::optional<SDL_Point> effective_target = render_target_manager_.current_size();
    if (!effective_target.has_value() || effective_target->x <= 0 || effective_target->y <= 0) {
        out_error = "Invalid render target dimensions.";
        (void)device->end_frame(false, frame_error);
        return false;
    }

    screen_width_ = effective_target->x;
    screen_height_ = effective_target->y;
    render_target_manager_.set_requested_size(screen_width_, screen_height_);
    if (last_complete_scene_frame_data_.has_value() &&
        (last_complete_scene_width_ != static_cast<std::uint32_t>(screen_width_) ||
         last_complete_scene_height_ != static_cast<std::uint32_t>(screen_height_))) {
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
        (void)device->end_frame(false, frame_error);
        return false;
    }
    if (ui_overlay_texture) {
        frame_data.ui_overlay_texture = ui_overlay_texture;
        frame_data.ui_overlay_gpu_texture =
            backend_owner_.get()->resolve_gpu_texture_for_sdl_texture(ui_overlay_texture, out_error);
        if (!frame_data.ui_overlay_gpu_texture) {
            out_error = "[RuntimeGpuRenderer] Failed to resolve UI overlay texture for GPU composite: " +
                        (out_error.empty() ? std::string(SDL_GetError()) : out_error);
            (void)device->end_frame(false, frame_error);
            render_diagnostics::set_submit_result(false);
            return false;
        }
    }

    const bool hold_incomplete_scene_frame =
        frame_data.suspected_incomplete_scene &&
        last_complete_scene_frame_data_.has_value() &&
        last_complete_scene_width_ == frame_data.target_width &&
        last_complete_scene_height_ == frame_data.target_height;
    const bool hold_zero_sprite_scene_frame =
        frame_data.layer_sprite_draw_count == 0 &&
        last_complete_scene_frame_data_.has_value() &&
        last_complete_scene_frame_data_->layer_sprite_draw_count > 0 &&
        last_complete_scene_width_ == frame_data.target_width &&
        last_complete_scene_height_ == frame_data.target_height;

    GpuSceneFrameData held_frame_data{};
    const GpuSceneFrameData* frame_to_render = &frame_data;
    if (hold_incomplete_scene_frame || hold_zero_sprite_scene_frame) {
        held_frame_data = *last_complete_scene_frame_data_;
        held_frame_data.ui_overlay_texture = frame_data.ui_overlay_texture;
        held_frame_data.ui_overlay_gpu_texture = frame_data.ui_overlay_gpu_texture;
        held_frame_data.debug_overlay_draw_count = frame_data.debug_overlay_draw_count;
        frame_to_render = &held_frame_data;

        ++consecutive_held_incomplete_scene_frames_;
        if (consecutive_held_incomplete_scene_frames_ == 1 ||
            (consecutive_held_incomplete_scene_frames_ % 60u) == 0u) {
            vibble::log::warn("[RuntimeGpuRenderer] Holding last complete GPU scene instead of presenting an incomplete floor-only frame. "
                              "hold_reason=" + std::string(hold_zero_sprite_scene_frame ? "zero_sprite" : "incomplete_scene") +
                              " " +
                              "held_frames=" + std::to_string(consecutive_held_incomplete_scene_frames_) +
                              " current_floor_packets=" + std::to_string(frame_data.floor_draw_count) +
                              " current_normal_packets=" + std::to_string(frame_data.layer_sprite_draw_count) +
                              " cached_floor_packets=" + std::to_string(held_frame_data.floor_draw_count) +
                              " cached_normal_packets=" + std::to_string(held_frame_data.layer_sprite_draw_count) +
                              " active_count=" + std::to_string(frame_data.active_asset_count) +
                              " filtered_count=" + std::to_string(frame_data.filtered_active_asset_count) +
                              " selected_count=" + std::to_string(frame_data.selected_asset_count) +
                              " traversal_count=" + std::to_string(frame_data.visible_traversal_count) +
                              " focus_filter_active=" + std::string(frame_data.focus_filter_active ? "true" : "false"));
        }
    } else {
        consecutive_held_incomplete_scene_frames_ = 0;
    }

    frame_context_.stats.floor_draw_count = frame_to_render->floor_draw_count;
    frame_context_.stats.layer_sprite_draw_count = frame_to_render->layer_sprite_draw_count;
    frame_context_.stats.debug_overlay_draw_count = frame_to_render->debug_overlay_draw_count;

    render_diagnostics::set_renderer_runtime_info("gpu", backend_name(), present_mode());

    if (!backend_owner_.get()->render_active_frame(*frame_to_render, out_error)) {
        (void)device->end_frame(false, frame_error);
        render_diagnostics::set_submit_result(false);
        return false;
    }

    if (!device->end_frame(true, frame_error)) {
        render_diagnostics::set_submit_result(false);
        out_error = "Failed to submit GPU frame: " + frame_error;
        return false;
    }
    render_diagnostics::set_submit_result(true);

    if (!hold_incomplete_scene_frame && !hold_zero_sprite_scene_frame && frame_data.layer_sprite_draw_count > 0) {
        last_complete_scene_frame_data_ = frame_data;
        last_complete_scene_width_ = frame_data.target_width;
        last_complete_scene_height_ = frame_data.target_height;
    }

    const std::uint64_t pipeline_hits_after = backend_owner_.get()->pipeline_cache().total_hits();
    const std::uint64_t pipeline_misses_after = backend_owner_.get()->pipeline_cache().total_misses();
    const std::uint64_t frame_hits = (pipeline_hits_after >= pipeline_hits_before)
        ? (pipeline_hits_after - pipeline_hits_before)
        : pipeline_hits_after;
    const std::uint64_t frame_misses = (pipeline_misses_after >= pipeline_misses_before)
        ? (pipeline_misses_after - pipeline_misses_before)
        : pipeline_misses_after;
    const double frame_hit_rate = (frame_hits + frame_misses) == 0
        ? 1.0
        : static_cast<double>(frame_hits) / static_cast<double>(frame_hits + frame_misses);
    render_diagnostics::set_gpu_pipeline_cache_stats(frame_hits, frame_misses, frame_hit_rate);

    const RenderFrameStats& stats = render_diagnostics::current_frame_stats();
    vibble::log::info("[RuntimeGpuRenderer] render_summary backend=" + backend_name() +
                     " present_mode=" + present_mode() +
                     " command_buffer_acquired=" + std::string(stats.command_buffer_acquired ? "true" : "false") +
                     " swapchain_acquired=" + std::string(stats.swapchain_acquired ? "true" : "false") +
                     " swapchain_dimensions=" + std::to_string(stats.swapchain_width) + "x" +
                      std::to_string(stats.swapchain_height) +
                     " floor_target=" + std::to_string(stats.floor_target_width) + "x" +
                     std::to_string(stats.floor_target_height) +
                     " floor_packets=" + std::to_string(stats.floor_packet_count) +
                     " normal_packets=" + std::to_string(stats.sprite_packet_count) +
                     " depth_layers=" + std::to_string(stats.active_depth_layer_count) +
                     " blur_passes=" + std::to_string(stats.blur_pass_count) +
                     " clear_executed=" + std::string(stats.clear_executed ? "true" : "false") +
                     " floor_packet_count=" + std::to_string(stats.floor_packet_count) +
                     " sprite_packet_count=" + std::to_string(stats.sprite_packet_count) +
                     " packets_per_depth_layer='" + stats.packets_per_depth_layer + "'" +
                     " blur_strength_per_layer='" + stats.blur_strength_per_layer + "'" +
                     " composite_layers='" + stats.composite_layers_submitted + "'" +
                     " draw_call_count=" + std::to_string(stats.draw_call_count) +
                     " skipped_textures=" + std::to_string(stats.skipped_texture_count) +
                     " failed_texture_names='" + stats.failed_texture_names + "'" +
                     " submit_succeeded=" + std::string(stats.submit_succeeded ? "true" : "false"));

    return true;
}
