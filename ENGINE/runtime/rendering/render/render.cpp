#include "rendering/render/render.hpp"

#include "rendering/render/gpu_runtime_pipeline.hpp"
#include "rendering/render/render_diagnostics.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
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

} // namespace

namespace render_internal {

std::filesystem::path runtime_gpu_shader_manifest_path() {
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

    gpu_runtime_pipeline_->enqueue_frame_graph(*gpu_scene_renderer_,
                                               "startup_probe",
                                               scene_width,
                                               scene_height);

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
                           " resources=[scene.floor,scene.layers,scene.blur_background,scene.blur_foreground,scene.composite,swapchain] scene.composite.spec={w=" +
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

    if (!gpu_scene_renderer_->begin_frame(&frame_error)) {
        out_error = frame_error.empty() ? "GpuSceneRenderer::begin_frame failed." : frame_error;
        log_failure(out_error, "begin_frame");
        return false;
    }

    gpu_runtime_pipeline_->enqueue_frame_graph(*gpu_scene_renderer_,
                                               "runtime",
                                               scene_width,
                                               scene_height);

    if (!gpu_scene_renderer_->end_frame(&frame_error)) {
        out_error = frame_error.empty() ? "GpuSceneRenderer::end_frame failed." : frame_error;
        log_failure(out_error, "runtime_present_scene_composite");
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
