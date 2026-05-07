#include "rendering/render/render.hpp"

#include "core/AssetsManager.hpp"
#include "rendering/render/render_diagnostics.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>

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
      screen_height_(std::max(1, screen_height)) {
    (void)map_manifest;
    (void)map_id;

    std::string reason;
    if (!prerequisites_ready(renderer_, assets_, &reason)) {
        throw std::invalid_argument(reason.empty() ? "SceneRenderer prerequisites not met." : reason);
    }

    std::string runtime_error;
    runtime_gpu_renderer_ =
        RuntimeGpuRenderer::Create(renderer_, assets_, screen_width_, screen_height_, runtime_error);
    if (!runtime_gpu_renderer_) {
        throw std::runtime_error("[SceneRenderer] GPU runtime renderer initialization failed: " + runtime_error);
    }

    gpu_runtime_path_enabled_ = true;
    vibble::log::info("[SceneRenderer] GPU runtime renderer active.");
}

SceneRenderer::~SceneRenderer() = default;

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
    if (runtime_gpu_renderer_) {
        runtime_gpu_renderer_->set_output_dimensions(screen_width_, screen_height_);
    }
}

std::optional<SDL_Point> SceneRenderer::postprocess_target_size() const {
    if (!runtime_gpu_renderer_) {
        return std::nullopt;
    }
    return runtime_gpu_renderer_->scene_target_size();
}

void SceneRenderer::render(SDL_Texture* ui_overlay_texture) {
    if (!renderer_ || !assets_ || !runtime_gpu_renderer_ || screen_width_ <= 0 || screen_height_ <= 0) {
        return;
    }

    render_diagnostics::begin_frame();
    render_diagnostics::set_texture_memory_usage(render_diagnostics::tracked_texture_bytes(), false);

    auto fail_gpu_frame = [&](const std::string& error_message) {
        render_diagnostics::set_renderer_runtime_info("gpu", "failed", "fatal");
        render_diagnostics::set_submit_result(false);
        render_diagnostics::end_frame();
        const RenderFrameStats& stats = render_diagnostics::current_frame_stats();
        vibble::log::error("[SceneRenderer] GPU runtime frame failed: reason='" + error_message +
                           "' command_buffer_acquired=" + (stats.command_buffer_acquired ? std::string("true") : "false") +
                           " swapchain_acquired=" + (stats.swapchain_acquired ? std::string("true") : "false") +
                           " swapchain_dimensions=" + std::to_string(stats.swapchain_width) + "x" +
                           std::to_string(stats.swapchain_height) +
                           " floor_packet_count=" + std::to_string(stats.floor_packet_count) +
                           " sprite_packet_count=" + std::to_string(stats.sprite_packet_count) +
                           " draw_call_count=" + std::to_string(stats.draw_call_count) +
                           " skipped_textures=" + std::to_string(stats.skipped_texture_count) +
                           " failed_texture_names='" + stats.failed_texture_names + "'" +
                           " submit_succeeded=" + (stats.submit_succeeded ? std::string("true") : "false"));
        throw std::runtime_error("[SceneRenderer] Fatal GPU runtime failure: " + error_message);
    };

    std::string frame_error;
    if (!runtime_gpu_renderer_->render_frame(frame_error, ui_overlay_texture)) {
        fail_gpu_frame(frame_error.empty() ? "Unknown GPU frame failure." : frame_error);
        return;
    }
    render_diagnostics::end_frame();
}
