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

struct SpriteBatchVertexUniformData {
    SDL_FColor vertex_uv[6]{};
    SDL_FColor modulate{1.0f, 1.0f, 1.0f, 1.0f};
};

constexpr SDL_FColor kMissingFloorDataClearColor{0.85f, 0.12f, 0.78f, 1.0f};

std::uintptr_t floor_sort_id(bool sprite_packet, std::uintptr_t sequence) {
    constexpr std::uintptr_t kSpritePacketSortOffset =
        std::uintptr_t{1} << ((sizeof(std::uintptr_t) * 8u) - 1u);
    return sprite_packet ? (kSpritePacketSortOffset + sequence) : sequence;
}

} // namespace

namespace runtime_gpu_renderer_detail {

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
            packet.modulate = SDL_FColor{1.0f, 1.0f, 1.0f, 1.0f};
            packet.sort_key = std::max(screen_br.y, screen_bl.y);
            packet.stable_sort_id = floor_sort_id(false, tile_sequence++);
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
        return std::vector<world::Chunk*>(active_chunks.begin(), active_chunks.end());
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
            runtime_gpu_renderer_detail::append_classified_sprite_draw_packet(
                true, packet, out_floor_draws, out_layer_draws);
            continue;
        }

        packet.stable_sort_id = floor_sort_id(false, layer_sequence++);
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

    const std::vector<Asset*>& visible_assets = assets_->is_dev_mode()
        ? assets_->getFilteredActiveAssets()
        : assets_->getActive();
    const WarpedScreenGrid& camera = assets_->getView();

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
            visible_assets,
            target_width,
            target_height,
            out_data.floor_draws,
            out_data.layer_draws,
            out_error)) {
        return false;
    }

    out_data.floor_draw_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        out_data.floor_draws.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.layer_sprite_draw_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        out_data.layer_draws.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.debug_overlay_draw_count = 0;
    out_data.has_valid_composite_source =
        (out_data.floor_draw_count + out_data.layer_sprite_draw_count) > 0;

    if (out_data.floor_draw_count == 0) {
        vibble::log::warn("[RuntimeGpuRenderer] No floor draw packets were built; rendering diagnostic clear color.");
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
        backend_owner_.get()->resolve_graphics_pipeline("sprite_batched", 0x2100u);
    if (!pipeline) {
        out_error = "Failed to resolve graphics pipeline 'sprite_batched' for " +
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
        std::string texture_error;
        SDL_GPUTexture* gpu_texture =
            backend_owner_.get()->resolve_gpu_texture_for_sdl_texture(draw.source_texture, texture_error);
        if (!gpu_texture) {
            out_error = "Failed to resolve draw texture for pass '" +
                        std::string(pass_label ? pass_label : "packets") + "': " +
                        (texture_error.empty() ? "unknown SDL->GPU texture import failure." : texture_error);
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

bool RuntimeGpuRenderer::render_frame(std::string& out_error) {
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

    GpuSceneFrameData frame_data{};
    if (!build_gpu_scene_frame_data(static_cast<std::uint32_t>(effective_target->x),
                                    static_cast<std::uint32_t>(effective_target->y),
                                    frame_data,
                                    out_error)) {
        (void)device->end_frame(false, frame_error);
        return false;
    }

    frame_context_.stats.floor_draw_count = frame_data.floor_draw_count;
    frame_context_.stats.layer_sprite_draw_count = frame_data.layer_sprite_draw_count;
    frame_context_.stats.debug_overlay_draw_count = frame_data.debug_overlay_draw_count;

    render_diagnostics::set_renderer_runtime_info("gpu", backend_name(), present_mode());
    std::uint64_t texture_memory_bytes = 0;
    const bool texture_memory_known = device->query_texture_memory_usage(texture_memory_bytes);
    render_diagnostics::set_texture_memory_usage(texture_memory_bytes, texture_memory_known);
    render_diagnostics::set_gpu_scene_packet_stats(frame_data.floor_draw_count,
                                                   frame_data.layer_sprite_draw_count,
                                                   frame_data.has_valid_composite_source);

    GpuSceneRenderer::SamplerResourceSpec sampler_spec{};
    if (!backend_owner_.get()->ensure_sampler_resource("linear_clamp", sampler_spec, out_error)) {
        (void)device->end_frame(false, frame_error);
        return false;
    }

    SDL_GPUColorTargetInfo target_info{};
    target_info.texture = frame_context_.swapchain_texture;
    target_info.mip_level = 0;
    target_info.layer_or_depth_plane = 0;
    target_info.clear_color = frame_data.floor_draw_count == 0
        ? kMissingFloorDataClearColor
        : SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
    target_info.load_op = SDL_GPU_LOADOP_CLEAR;
    target_info.store_op = SDL_GPU_STOREOP_STORE;
    target_info.resolve_texture = nullptr;
    target_info.resolve_mip_level = 0;
    target_info.resolve_layer = 0;
    target_info.cycle = false;
    target_info.cycle_resolve_texture = false;

    SDL_GPURenderPass* render_pass = SDL_BeginGPURenderPass(frame_context_.command_buffer, &target_info, 1, nullptr);
    if (!render_pass) {
        out_error = "SDL_BeginGPURenderPass failed: " + std::string(SDL_GetError());
        (void)device->end_frame(false, frame_error);
        return false;
    }

    frame_context_.stats.render_pass_count = 1;
    render_diagnostics::add_render_pass();

    bool render_ok = true;
    if (!render_draw_packet_batch(render_pass, frame_data.floor_draws, "floor", out_error)) {
        render_ok = false;
    } else if (!render_draw_packet_batch(render_pass,
                                         frame_data.layer_draws,
                                         "world sprites",
                                         out_error)) {
        render_ok = false;
    } else {
        frame_context_.stats.debug_overlay_draw_count = 0;
    }

    SDL_EndGPURenderPass(render_pass);

    if (!render_ok) {
        (void)device->end_frame(false, frame_error);
        return false;
    }

    if (!device->end_frame(true, frame_error)) {
        out_error = "Failed to submit GPU frame: " + frame_error;
        return false;
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

    return true;
}
