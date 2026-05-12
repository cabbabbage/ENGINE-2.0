#include "rendering/render/render.hpp"

#include "core/AssetsManager.hpp"
#include "rendering/render/render_diagnostics.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>

namespace render_internal {

std::filesystem::path opengl_runtime_shader_manifest_path() {
    if (SDL_Environment* env = SDL_GetEnvironment()) {
        if (const char* override_manifest_path =
                SDL_GetEnvironmentVariable(env, "VIBBLE_OPENGL_SHADER_MANIFEST");
            override_manifest_path && *override_manifest_path) {
            return std::filesystem::path(override_manifest_path);
        }
    }
    if (const char* override_manifest_path = std::getenv("VIBBLE_OPENGL_SHADER_MANIFEST");
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
    opengl_runtime_renderer_ =
        OpenGLRuntimeRenderer::Create(renderer_, assets_, screen_width_, screen_height_, runtime_error);
    if (!opengl_runtime_renderer_) {
        throw std::runtime_error("[SceneRenderer] OpenGL runtime renderer initialization failed: " + runtime_error);
    }

    gpu_runtime_path_enabled_ = true;
    vibble::log::info("[SceneRenderer] OpenGL runtime renderer active.");
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
    if (opengl_runtime_renderer_) {
        opengl_runtime_renderer_->set_output_dimensions(screen_width_, screen_height_);
    }
}

std::optional<SDL_Point> SceneRenderer::postprocess_target_size() const {
    if (!opengl_runtime_renderer_) {
        return std::nullopt;
    }
    return opengl_runtime_renderer_->scene_target_size();
}

void SceneRenderer::render(SDL_Texture* ui_overlay_texture) {
    if (!renderer_ || !assets_ || !opengl_runtime_renderer_ || screen_width_ <= 0 || screen_height_ <= 0) {
        return;
    }

    auto fail_gpu_frame = [&](const std::string& error_message) {
        render_diagnostics::set_renderer_runtime_info("opengl", "failed", "fatal");
        render_diagnostics::set_submit_result(false);
        const RenderFrameStats& stats = render_diagnostics::current_frame_stats();
        vibble::log::error("[SceneRenderer] OpenGL runtime frame failed: reason='" + error_message +
                           "' floor_packet_count=" + std::to_string(stats.floor_packet_count) +
                           " xy_sprite_packet_count=" + std::to_string(stats.xy_sprite_packet_count) +
                           " draw_call_count=" + std::to_string(stats.draw_call_count) +
                           " skipped_textures=" + std::to_string(stats.skipped_texture_count) +
                           " failed_texture_names='" + stats.failed_texture_names + "'" +
                           " submit_succeeded=" + (stats.submit_succeeded ? std::string("true") : "false"));
        throw std::runtime_error("[SceneRenderer] Fatal OpenGL runtime failure: " + error_message);
    };

    std::string frame_error;
    if (!opengl_runtime_renderer_->render_frame(frame_error, ui_overlay_texture)) {
        fail_gpu_frame(frame_error.empty() ? "Unknown OpenGL frame failure." : frame_error);
        return;
    }
}
