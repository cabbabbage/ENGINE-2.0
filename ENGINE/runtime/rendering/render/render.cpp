#include "rendering/render/render.hpp"

#include "rendering/render/gpu_runtime_pipeline.hpp"
#include "rendering/render/render_object.hpp"
#include "rendering/render/render_object_builder.hpp"
#include "rendering/render/render_object_projection.hpp"
#include "rendering/render/render_diagnostics.hpp"
#include "core/AssetsManager.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string texture_usage_flags_to_string(SDL_GPUTextureUsageFlags usage) {
    std::vector<std::string> flags;
    if ((usage & SDL_GPU_TEXTUREUSAGE_SAMPLER) != 0) {
        flags.emplace_back("sampler");
    }
    if ((usage & SDL_GPU_TEXTUREUSAGE_COLOR_TARGET) != 0) {
        flags.emplace_back("color_target");
    }
    if ((usage & SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET) != 0) {
        flags.emplace_back("depth_stencil");
    }
    if ((usage & SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ) != 0) {
        flags.emplace_back("graphics_storage_read");
    }
    if ((usage & SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ) != 0) {
        flags.emplace_back("compute_storage_read");
    }
    if ((usage & SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE) != 0) {
        flags.emplace_back("compute_storage_write");
    }
    if ((usage & SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE) != 0) {
        flags.emplace_back("compute_storage_rw");
    }

    std::string result;
    for (std::size_t i = 0; i < flags.size(); ++i) {
        if (i > 0) {
            result += "|";
        }
        result += flags[i];
    }
    return result.empty() ? std::string{"none"} : result;
}

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
    vibble::log::warn(std::string{"[SceneRenderer] Clamping "} + axis_label +
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

void fill_sprite_packet_vertices(const render_projection::ProjectedSpriteFrame& projected,
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

    out_packet.vertices[0] = make_vertex(projected.screen_tl, u0, v0);
    out_packet.vertices[1] = make_vertex(projected.screen_tr, u1, v0);
    out_packet.vertices[2] = make_vertex(projected.screen_br, u1, v1);
    out_packet.vertices[3] = make_vertex(projected.screen_tl, u0, v0);
    out_packet.vertices[4] = make_vertex(projected.screen_br, u1, v1);
    out_packet.vertices[5] = make_vertex(projected.screen_bl, u0, v1);
}

} // namespace

namespace render_internal {

std::filesystem::path runtime_gpu_shader_manifest_path() {
    if (SDL_Environment* env = SDL_GetEnvironment()) {
        if (const char* override_manifest_path =
                SDL_GetEnvironmentVariable(env, "VIBBLE_GPU_SHADER_MANIFEST");
            override_manifest_path && *override_manifest_path) {
            return std::filesystem::path(override_manifest_path);
        }
    }
    if (const char* override_manifest_path = std::getenv("VIBBLE_GPU_SHADER_MANIFEST");
        override_manifest_path && *override_manifest_path) {
        return std::filesystem::path(override_manifest_path);
    }
    return std::filesystem::path("ENGINE/runtime/rendering/shaders/runtime_shaders.json");
}

} // namespace render_internal

SceneRenderer::SceneRenderer(SDL_Renderer* renderer,
                             Assets* assets,
                             int screen_width,
                             int screen_height,
                             const nlohmann::json& map_manifest,
                             const std::string& map_id)
    : renderer_(renderer),
      assets_(assets),
      screen_width_(std::max(1, screen_width)),
      screen_height_(std::max(1, screen_height)),
      gpu_runtime_pipeline_(std::make_unique<GpuRuntimePipeline>()) {
    (void)map_manifest;
    (void)map_id;

    std::string reason;
    if (!prerequisites_ready(renderer_, assets_, &reason)) {
        throw std::invalid_argument(reason.empty() ? "SceneRenderer prerequisites not met." : reason);
    }

    std::string gpu_error;
    gpu_scene_renderer_ = GpuSceneRenderer::Create(renderer_, false, gpu_error);
    if (!gpu_scene_renderer_) {
        throw std::runtime_error("[SceneRenderer] GPU runtime renderer initialization failed: " + gpu_error);
    }

    const std::filesystem::path shader_manifest_path = render_internal::runtime_gpu_shader_manifest_path();
    if (!std::filesystem::exists(shader_manifest_path)) {
        throw std::runtime_error("[SceneRenderer] GPU runtime manifest missing: " + shader_manifest_path.string());
    }
    if (!gpu_scene_renderer_->load_shader_packages(shader_manifest_path.string(), gpu_error)) {
        throw std::runtime_error("[SceneRenderer] GPU shader package load failed: " + gpu_error +
                                 " manifest=" + shader_manifest_path.string());
    }

    if (!ensure_scene_target()) {
        throw std::runtime_error("[SceneRenderer] GPU-only initialization failed: could not initialize scene resources.");
    }

    std::string startup_probe_error;
    if (!probe_runtime_pipeline_startup(startup_probe_error)) {
        throw std::runtime_error("[SceneRenderer] GPU frame-graph startup probe failed: " + startup_probe_error);
    }

    gpu_runtime_path_enabled_ = true;
    vibble::log::info("[SceneRenderer] GPU runtime renderer active.");
}

SceneRenderer::~SceneRenderer() = default;

bool SceneRenderer::ensure_scene_target() {
    if (!renderer_ || !gpu_scene_renderer_ || !gpu_runtime_pipeline_ || screen_width_ <= 0 || screen_height_ <= 0) {
        return false;
    }

    const int max_texture_size = renderer_max_texture_size(renderer_);
    screen_width_ = clamp_dimension_to_renderer_limit(screen_width_, max_texture_size, "scene target width");
    screen_height_ = clamp_dimension_to_renderer_limit(screen_height_, max_texture_size, "scene target height");

    scene_composite_resource_spec_.width = static_cast<Uint32>(screen_width_);
    scene_composite_resource_spec_.height = static_cast<Uint32>(screen_height_);
    scene_composite_resource_spec_.format = gpu_scene_renderer_->device()->format_policy().albedo_format;
    scene_composite_resource_spec_.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    scene_composite_resource_spec_.layer_count_or_depth = 1;
    scene_composite_resource_spec_.num_levels = 1;
    scene_composite_resource_spec_.sample_count = SDL_GPU_SAMPLECOUNT_1;

    std::string ensure_error;
    if (!ensure_authoritative_graph_resources(scene_composite_resource_spec_.width,
                                              scene_composite_resource_spec_.height,
                                              ensure_error)) {
        vibble::log::error("[SceneRenderer] Failed to allocate frame-graph resources: " + ensure_error);
        return false;
    }
    return gpu_runtime_pipeline_->ensure_shared_resources(*gpu_scene_renderer_, ensure_error);
}

bool SceneRenderer::ensure_authoritative_graph_resources(std::uint32_t scene_width,
                                                         std::uint32_t scene_height,
                                                         std::string& out_error) {
    out_error.clear();
    if (!gpu_scene_renderer_ || !gpu_scene_renderer_->device()) {
        out_error = "GpuSceneRenderer is not initialized.";
        return false;
    }
    if (!gpu_runtime_pipeline_) {
        out_error = "GpuRuntimePipeline is not initialized.";
        return false;
    }
    if (!gpu_runtime_pipeline_->ensure_resources(*gpu_scene_renderer_, scene_width, scene_height, out_error)) {
        return false;
    }
    if (!gpu_runtime_pipeline_->ensure_shared_resources(*gpu_scene_renderer_, out_error)) {
        return false;
    }
    out_error.clear();
    return true;
}

bool SceneRenderer::probe_runtime_pipeline_startup(std::string& out_error) {
    out_error.clear();

    if (!gpu_scene_renderer_ || !gpu_runtime_pipeline_) {
        out_error = "GPU runtime pipeline is not initialized.";
        return false;
    }
    if (!gpu_scene_renderer_->device() || !gpu_scene_renderer_->ready()) {
        out_error = "GpuSceneRenderer device is not initialized.";
        return false;
    }

    const std::uint32_t scene_width = static_cast<std::uint32_t>(std::max(1, screen_width_));
    const std::uint32_t scene_height = static_cast<std::uint32_t>(std::max(1, screen_height_));

    std::string frame_error;
    if (!ensure_authoritative_graph_resources(scene_width, scene_height, frame_error)) {
        out_error = frame_error.empty()
            ? "Failed to allocate frame-graph resources during startup probe."
            : frame_error;
        return false;
    }
    if (!gpu_scene_renderer_->begin_frame(&frame_error)) {
        out_error = frame_error.empty() ? "GpuSceneRenderer::begin_frame failed during startup probe." : frame_error;
        return false;
    }

    GpuSceneFrameData startup_frame_data{};
    startup_frame_data.floor_draw_count = 0;
    startup_frame_data.layer_sprite_draw_count = 0;
    startup_frame_data.has_valid_composite_source = false;
    startup_frame_data.debug_overlay_draw_count = 0;
    if (!gpu_runtime_pipeline_->enqueue_frame_graph(*gpu_scene_renderer_,
                                                    startup_frame_data,
                                                    "startup_probe",
                                                    scene_width,
                                                    scene_height,
                                                    frame_error)) {
        out_error = frame_error.empty()
            ? "Failed to enqueue startup GPU runtime frame graph."
            : frame_error;
        gpu_scene_renderer_->abort_frame();
        return false;
    }

    if (!gpu_scene_renderer_->end_frame(&frame_error)) {
        out_error = frame_error.empty() ? "GpuSceneRenderer::end_frame failed during startup probe." : frame_error;
        return false;
    }

    return true;
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
    scene_composite_resource_spec_.width = static_cast<std::uint32_t>(screen_width_);
    scene_composite_resource_spec_.height = static_cast<std::uint32_t>(screen_height_);
}

bool SceneRenderer::execute_gpu_frame_graph(std::string& out_error) {
    out_error.clear();

    const char* renderer_name = renderer_ ? SDL_GetRendererName(renderer_) : nullptr;
    const auto backend_name = renderer_name ? std::string{renderer_name} : std::string{"unknown"};
    const auto log_failure = [&](const std::string& reason, const std::string& pass_name) {
        vibble::log::error("[SceneRenderer] GPU frame-graph failure backend=" + backend_name +
                           " pass=" + pass_name +
                           " resources=[scene.floor,scene.layers,scene.composite,swapchain] scene.composite.spec={w=" +
                           std::to_string(scene_composite_resource_spec_.width) + ",h=" +
                           std::to_string(scene_composite_resource_spec_.height) + ",usage=" +
                           texture_usage_flags_to_string(scene_composite_resource_spec_.usage) + "} reason=" + reason);
    };

    if (!gpu_scene_renderer_ || !gpu_runtime_pipeline_) {
        out_error = "GpuSceneRenderer is not initialized.";
        log_failure(out_error, "preflight");
        return false;
    }
    if (scene_composite_resource_spec_.width == 0 || scene_composite_resource_spec_.height == 0) {
        out_error = "scene.composite resource spec has invalid dimensions.";
        log_failure(out_error, "preflight");
        return false;
    }

    const std::uint32_t scene_width = scene_composite_resource_spec_.width;
    const std::uint32_t scene_height = scene_composite_resource_spec_.height;

    std::string frame_error;
    if (!ensure_authoritative_graph_resources(scene_width, scene_height, frame_error)) {
        out_error = frame_error.empty() ? "Failed to allocate frame-graph resources." : frame_error;
        log_failure(out_error, "resource_setup");
        return false;
    }

    GpuSceneFrameData frame_data{};
    if (!build_gpu_scene_frame_data(frame_data, out_error)) {
        log_failure(out_error, "scene_packet_build");
        return false;
    }
    render_diagnostics::set_gpu_scene_packet_stats(frame_data.floor_draw_count,
                                                   frame_data.layer_sprite_draw_count,
                                                   frame_data.has_valid_composite_source,
                                                   true);

    if (!gpu_scene_renderer_->begin_frame(&frame_error)) {
        out_error = frame_error.empty() ? "GpuSceneRenderer::begin_frame failed." : frame_error;
        log_failure(out_error, "begin_frame");
        return false;
    }

    if (!gpu_runtime_pipeline_->enqueue_frame_graph(*gpu_scene_renderer_,
                                                    frame_data,
                                                    "runtime",
                                                    scene_width,
                                                    scene_height,
                                                    frame_error)) {
        out_error = frame_error.empty()
            ? "Failed to enqueue runtime GPU frame graph."
            : frame_error;
        gpu_scene_renderer_->abort_frame();
        log_failure(out_error, "enqueue_frame_graph");
        return false;
    }

    if (!gpu_scene_renderer_->end_frame(&frame_error)) {
        out_error = frame_error.empty() ? "GpuSceneRenderer::end_frame failed." : frame_error;
        log_failure(out_error, "runtime_present_scene_composite");
        return false;
    }

    return true;
}

bool SceneRenderer::build_gpu_scene_frame_data(GpuSceneFrameData& out_data, std::string& out_error) const {
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

    const float target_width = static_cast<float>(std::max<std::uint32_t>(1u, scene_composite_resource_spec_.width));
    const float target_height = static_cast<float>(std::max<std::uint32_t>(1u, scene_composite_resource_spec_.height));

    std::size_t visible_count = 0;
    std::size_t drawable_count = 0;

    out_data.floor_draws.reserve(visible_assets.size());
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

        if (asset->isFloorBoxesEnabled()) {
            out_data.floor_draws.push_back(packet);
        } else {
            out_data.layer_draws.push_back(packet);
        }
        ++drawable_count;
    }

    const auto draw_sort_predicate = [](const GpuSpriteDrawPacket& lhs,
                                        const GpuSpriteDrawPacket& rhs) {
        if (lhs.sort_key == rhs.sort_key) {
            return lhs.stable_sort_id < rhs.stable_sort_id;
        }
        return lhs.sort_key < rhs.sort_key;
    };
    std::stable_sort(out_data.floor_draws.begin(), out_data.floor_draws.end(), draw_sort_predicate);
    std::stable_sort(out_data.layer_draws.begin(), out_data.layer_draws.end(), draw_sort_predicate);

    out_data.floor_draw_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        out_data.floor_draws.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.layer_sprite_draw_count = static_cast<std::uint32_t>(std::min<std::size_t>(
        out_data.layer_draws.size(), static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
    out_data.debug_overlay_draw_count = 0;
    out_data.has_valid_composite_source = (out_data.floor_draw_count + out_data.layer_sprite_draw_count) > 0;

    if (visible_count > 0 && drawable_count == 0) {
        out_error = "GPU scene packet invalid: visible assets exist but zero drawable sprite packets were built.";
        vibble::log::error("[SceneRenderer] " + out_error);
        return false;
    }

    return true;
}

std::optional<SDL_Point> SceneRenderer::postprocess_target_size() const {
    if (scene_composite_resource_spec_.width == 0 || scene_composite_resource_spec_.height == 0) {
        return std::nullopt;
    }
    return SDL_Point{static_cast<int>(scene_composite_resource_spec_.width),
                     static_cast<int>(scene_composite_resource_spec_.height)};
}

void SceneRenderer::render() {
    if (!renderer_ || !assets_ || screen_width_ <= 0 || screen_height_ <= 0) {
        return;
    }

    render_diagnostics::begin_frame();
    render_diagnostics::set_texture_memory_usage(render_diagnostics::tracked_texture_bytes(), false);

    auto fail_gpu_frame = [&](const std::string& error_message, bool abort_open_gpu_frame) {
        if (abort_open_gpu_frame && gpu_scene_renderer_) {
            gpu_scene_renderer_->abort_frame();
        }
        render_diagnostics::note_gpu_frame_skipped_due_to_failure();
        render_diagnostics::set_renderer_runtime_info("gpu", "failed", "fatal");
        render_diagnostics::end_frame();
        vibble::log::error("[SceneRenderer] GPU runtime frame failed: " + error_message);
        throw std::runtime_error("[SceneRenderer] Fatal GPU runtime failure: " + error_message);
    };

    if (!ensure_scene_target()) {
        fail_gpu_frame("Failed to allocate authoritative GPU runtime resources.", false);
        return;
    }

    std::string frame_graph_error;
    if (!execute_gpu_frame_graph(frame_graph_error)) {
        fail_gpu_frame("Frame-graph execution failed: " + frame_graph_error, false);
        return;
    }

    constexpr const char* kCanonicalRuntimePath = "scene_composite_present_graph";
    if (!render_path_status_logged_) {
        vibble::log::info(std::string{"[SceneRenderer] runtime_path="} + kCanonicalRuntimePath);
        render_path_status_logged_ = true;
    }
    render_diagnostics::set_renderer_runtime_info("gpu", kCanonicalRuntimePath, "vsync");
    render_diagnostics::end_frame();
}
