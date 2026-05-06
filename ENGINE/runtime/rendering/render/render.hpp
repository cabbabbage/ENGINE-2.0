#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include "rendering/render/gpu_scene_renderer.hpp"
#include "rendering/render/gpu_runtime_pipeline.hpp"

class Assets;
class GpuRuntimePipeline;

namespace render_internal {
std::filesystem::path runtime_gpu_shader_manifest_path();
}

class SceneRenderer {
public:
    SceneRenderer(SDL_Renderer* renderer,
                  Assets* assets,
                  int screen_width,
                  int screen_height,
                  const nlohmann::json& map_manifest,
                  const std::string& map_id);
    ~SceneRenderer();

    static inline bool prerequisites_ready(SDL_Renderer* renderer,
                                           Assets* assets,
                                           std::string* reason = nullptr) {
        if (!renderer) {
            if (reason) {
                *reason = "SDL_Renderer pointer is null.";
            }
            return false;
        }
        if (!assets) {
            if (reason) {
                *reason = "Assets pointer is null.";
            }
            return false;
        }
        if (reason) {
            reason->clear();
        }
        return true;
    }

    void render();
    SDL_Renderer* get_renderer() const;
    void set_output_dimensions(int screen_width, int screen_height);
    int output_width() const { return screen_width_; }
    int output_height() const { return screen_height_; }
    std::optional<SDL_Point> postprocess_target_size() const;
    bool gpu_runtime_path_enabled() const { return gpu_runtime_path_enabled_; }

private:
    bool ensure_scene_target();
    bool ensure_authoritative_graph_resources(std::uint32_t scene_width,
                                              std::uint32_t scene_height,
                                              std::string& out_error);
    bool probe_runtime_pipeline_startup(std::string& out_error);
    bool execute_gpu_frame_graph(std::string& out_error);
    bool build_gpu_scene_frame_data(GpuSceneFrameData& out_data, std::string& out_error) const;

    SDL_Renderer* renderer_ = nullptr;
    Assets* assets_ = nullptr;
    int screen_width_ = 1;
    int screen_height_ = 1;

    GpuSceneRenderer::TextureResourceSpec scene_composite_resource_spec_{};

    std::unique_ptr<GpuSceneRenderer> gpu_scene_renderer_;
    std::unique_ptr<GpuRuntimePipeline> gpu_runtime_pipeline_;

    bool gpu_runtime_path_enabled_ = false;
    bool render_path_status_logged_ = false;
};
