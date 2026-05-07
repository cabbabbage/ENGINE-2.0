#include "rendering/render/runtime_gpu_renderer.hpp"

#include "core/AssetsManager.hpp"
#include "assets/asset/Asset.hpp"
#include "gameplay/world/chunk.hpp"
#include "rendering/render/render.hpp"
#include "rendering/render/render_diagnostics.hpp"
#include "rendering/render/render_object.hpp"
#include "rendering/render/render_object_builder.hpp"
#include "rendering/render/render_object_projection.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>

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

} // namespace

namespace runtime_gpu_renderer_detail {

bool build_map_floor_tile_draw_packets(const WarpedScreenGrid& camera,
                                       const std::vector<world::Chunk*>& chunks,
                                       std::uint32_t target_width,
                                       std::uint32_t target_height,
                                       std::vector<GpuSpriteDrawPacket>& out_packets) {
    out_packets.clear();
    const float output_w = static_cast<float>(std::max<std::uint32_t>(1u, target_width));
    const float output_h = static_cast<float>(std::max<std::uint32_t>(1u, target_height));

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
            packet.modulate = SDL_FColor{1.0f, 1.0f, 1.0f, 1.0f};
            packet.sort_key = std::max(screen_br.y, screen_bl.y);
            const std::uintptr_t texture_id = reinterpret_cast<std::uintptr_t>(tile.texture);
            const std::uintptr_t coord_hash =
                (static_cast<std::uintptr_t>(static_cast<std::uint32_t>(tile.world_rect.x)) << 1u) ^
                (static_cast<std::uintptr_t>(static_cast<std::uint32_t>(tile.world_rect.y)) << 17u) ^
                (static_cast<std::uintptr_t>(static_cast<std::uint32_t>(tile.world_rect.w)) << 33u) ^
                (static_cast<std::uintptr_t>(static_cast<std::uint32_t>(tile.world_rect.h)) << 49u);
            packet.stable_sort_id = texture_id ^ coord_hash;
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
    std::stable_sort(out_packets.begin(), out_packets.end(), draw_sort_predicate);
    return true;
}

void append_classified_sprite_draw_packet(bool floor_tagged,
                                          const GpuSpriteDrawPacket& packet,
                                          GpuSceneFrameData& in_out_frame_data) {
    if (floor_tagged) {
        in_out_frame_data.floor_sprite_draws.push_back(packet);
        return;
    }
    in_out_frame_data.layer_draws.push_back(packet);
}

} // namespace runtime_gpu_renderer_detail

RuntimeGpuRenderer::RuntimeGpuRenderer(SDL_Renderer* renderer,
                                       Assets* assets,
                                       int screen_width,
                                       int screen_height)
    : renderer_(renderer),
      assets_(assets),
      screen_width_(std::max(1, screen_width)),
      screen_height_(std::max(1, screen_height)),
      gpu_runtime_pipeline_(std::make_unique<GpuRuntimePipeline>()) {}

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
    gpu_scene_renderer_ = GpuSceneRenderer::Create(renderer_, false, gpu_error);
    if (!gpu_scene_renderer_) {
        out_error = "[RuntimeGpuRenderer] GPU runtime renderer initialization failed: " + gpu_error;
        return false;
    }

    const std::filesystem::path shader_manifest_path = render_internal::runtime_gpu_shader_manifest_path();
    if (!std::filesystem::exists(shader_manifest_path)) {
        out_error = "[RuntimeGpuRenderer] GPU runtime manifest missing: " + shader_manifest_path.string();
        return false;
    }
    if (!gpu_scene_renderer_->load_shader_packages(shader_manifest_path.string(), gpu_error)) {
        out_error = "[RuntimeGpuRenderer] GPU shader package load failed: " + gpu_error +
                    " manifest=" + shader_manifest_path.string();
        return false;
    }

    if (!ensure_scene_target(out_error)) {
        return false;
    }

    return true;
}

void RuntimeGpuRenderer::set_output_dimensions(int screen_width, int screen_height) {
    screen_width_ = std::max(1, screen_width);
    screen_height_ = std::max(1, screen_height);
}


const std::string& RuntimeGpuRenderer::present_mode() const {
    static const std::string kUnknown = "unknown";
    return (gpu_scene_renderer_ && gpu_scene_renderer_->device())
        ? gpu_scene_renderer_->device()->present_mode()
        : kUnknown;
}

const std::string& RuntimeGpuRenderer::backend_name() const {
    static const std::string kUnknown = "unknown";
    return (gpu_scene_renderer_ && gpu_scene_renderer_->device())
        ? gpu_scene_renderer_->device()->backend_name()
        : kUnknown;
}

bool RuntimeGpuRenderer::ensure_scene_target(std::string& out_error) {
    out_error.clear();
    if (!renderer_ || !gpu_scene_renderer_ || !gpu_runtime_pipeline_ || screen_width_ <= 0 || screen_height_ <= 0) {
        out_error = "RuntimeGpuRenderer not ready for scene target initialization.";
        return false;
    }

    const int max_texture_size = renderer_max_texture_size(renderer_);
    screen_width_ = clamp_dimension_to_renderer_limit(screen_width_, max_texture_size, "scene width");
    screen_height_ = clamp_dimension_to_renderer_limit(screen_height_, max_texture_size, "scene height");
    return gpu_runtime_pipeline_->ensure_shared_resources(*gpu_scene_renderer_, out_error);
}

std::vector<world::Chunk*> RuntimeGpuRenderer::runtime_floor_chunks() const {
    if (!assets_) {
        return {};
    }
    const std::vector<world::Chunk*>& active_chunks = assets_->active_chunks();
    if (!active_chunks.empty()) {
        return std::vector<world::Chunk*>(active_chunks.begin(), active_chunks.end());
    }
    return assets_->world_grid().all_chunks();
}

bool RuntimeGpuRenderer::build_gpu_scene_frame_data(GpuSceneFrameData& out_data, std::string& out_error) const {
    out_data = GpuSceneFrameData{};
    out_error.clear();
    if (!assets_) {
        out_error = "Assets context unavailable for GPU scene frame packet build.";
        return false;
    }

    const std::vector<Asset*>& visible_assets = assets_->is_dev_mode()
        ? assets_->getFilteredActiveAssets()
        : assets_->getActive();
    const WarpedScreenGrid& camera = assets_->getView();

    const float target_width = static_cast<float>(std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(screen_width_)));
    const float target_height = static_cast<float>(std::max<std::uint32_t>(1u, static_cast<std::uint32_t>(screen_height_)));

    std::size_t visible_count = 0;
    std::size_t drawable_count = 0;

    if (!runtime_gpu_renderer_detail::build_map_floor_tile_draw_packets(
            camera,
            runtime_floor_chunks(),
            static_cast<std::uint32_t>(target_width),
            static_cast<std::uint32_t>(target_height),
            out_data.map_floor_draws)) {
        out_error = "Failed to build map-floor tile packets.";
        return false;
    }

    out_data.floor_sprite_draws.reserve(visible_assets.size());
    out_data.layer_draws.reserve(visible_assets.size());

    for (Asset* asset : visible_assets) {
        if (!asset || asset->dead || !asset->info) {
            continue;
        }
        ++visible_count;

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
        packet.modulate = SDL_FColor{
            static_cast<float>(object.color_mod.r) / 255.0f,
            static_cast<float>(object.color_mod.g) / 255.0f,
            static_cast<float>(object.color_mod.b) / 255.0f,
            static_cast<float>(object.color_mod.a) / 255.0f,
        };
        packet.sort_key = std::max(projected.screen_bl.y, projected.screen_br.y);
        packet.stable_sort_id = reinterpret_cast<std::uintptr_t>(asset);
        fill_sprite_packet_vertices(projected, u0, v0, u1, v1, target_width, target_height, packet);

        runtime_gpu_renderer_detail::append_classified_sprite_draw_packet(
            asset->isFloorBoxesEnabled(),
            packet,
            out_data);
        ++drawable_count;
    }

    const auto draw_sort_predicate = [](const GpuSpriteDrawPacket& lhs,
                                        const GpuSpriteDrawPacket& rhs) {
        if (lhs.sort_key == rhs.sort_key) {
            return lhs.stable_sort_id < rhs.stable_sort_id;
        }
        return lhs.sort_key < rhs.sort_key;
    };
    std::stable_sort(out_data.floor_sprite_draws.begin(), out_data.floor_sprite_draws.end(), draw_sort_predicate);
    std::stable_sort(out_data.layer_draws.begin(), out_data.layer_draws.end(), draw_sort_predicate);

    out_data.map_floor_draw_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        out_data.map_floor_draws.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.floor_sprite_draw_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        out_data.floor_sprite_draws.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.floor_draw_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        out_data.map_floor_draws.size() + out_data.floor_sprite_draws.size(),
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.layer_sprite_draw_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        out_data.layer_draws.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.debug_overlay_draw_count = 0;
    out_data.has_valid_composite_source =
        (out_data.floor_draw_count + out_data.layer_sprite_draw_count) > 0;

    if (visible_count > 0 && drawable_count == 0 && out_data.map_floor_draw_count == 0) {
        out_error = "GPU scene packet invalid: visible assets exist but zero drawable sprite packets were built.";
        vibble::log::error("[RuntimeGpuRenderer] " + out_error);
        return false;
    }

    return true;
}

bool RuntimeGpuRenderer::execute_gpu_frame_graph(std::string& out_error) {
    out_error.clear();

    const char* renderer_name = renderer_ ? SDL_GetRendererName(renderer_) : nullptr;
    const auto backend_name = renderer_name ? std::string{renderer_name} : std::string{"unknown"};
    const auto log_failure = [&](const std::string& reason, const std::string& pass_name) {
        vibble::log::error("[RuntimeGpuRenderer] GPU frame-graph failure backend=" + backend_name +
                           " pass=" + pass_name +
                           " reason=" + reason);
    };

    if (!gpu_scene_renderer_ || !gpu_runtime_pipeline_) {
        out_error = "GpuSceneRenderer is not initialized.";
        log_failure(out_error, "preflight");
        return false;
    }
    const std::uint32_t scene_width = static_cast<std::uint32_t>(std::max(1, screen_width_));
    const std::uint32_t scene_height = static_cast<std::uint32_t>(std::max(1, screen_height_));

    std::string frame_error;

    GpuSceneFrameData frame_data{};
    if (!build_gpu_scene_frame_data(frame_data, out_error)) {
        log_failure(out_error, "scene_packet_build");
        return false;
    }
    render_diagnostics::set_gpu_scene_packet_stats(frame_data.floor_draw_count, frame_data.layer_sprite_draw_count, frame_data.has_valid_composite_source, false);

    gpu_scene_renderer_->reset_frame_graph();
    if (!gpu_runtime_pipeline_->enqueue_frame_graph(*gpu_scene_renderer_,
                                                    frame_data,
                                                    "runtime",
                                                    scene_width,
                                                    scene_height,
                                                    frame_error)) {
        out_error = frame_error.empty()
            ? "Failed to enqueue runtime GPU frame graph."
            : frame_error;
        log_failure(out_error, "enqueue_frame_graph");
        return false;
    }

    if (!gpu_scene_renderer_->begin_frame(&frame_error, false)) {
        out_error = frame_error.empty() ? "GpuSceneRenderer::begin_frame failed." : frame_error;
        log_failure(out_error, "begin_frame");
        return false;
    }

    if (!gpu_scene_renderer_->end_frame(&frame_error)) {
        out_error = frame_error.empty() ? "GpuSceneRenderer::end_frame failed." : frame_error;
        log_failure(out_error, "runtime_present_main_scene");
        return false;
    }

    render_diagnostics::set_gpu_scene_packet_stats(frame_data.floor_draw_count,
                                                   frame_data.layer_sprite_draw_count,
                                                   frame_data.has_valid_composite_source,
                                                   true);
    return true;
}

bool RuntimeGpuRenderer::render_frame(std::string& out_error) {
    out_error.clear();

    if (!ensure_scene_target(out_error)) {
        return false;
    }
    if (!execute_gpu_frame_graph(out_error)) {
        return false;
    }
    return true;
}
