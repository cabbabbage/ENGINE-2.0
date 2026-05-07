#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include "rendering/render/runtime_gpu_renderer.hpp"

class Assets;

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
    SDL_Renderer* renderer_ = nullptr;
    Assets* assets_ = nullptr;
    int screen_width_ = 1;
    int screen_height_ = 1;

    std::unique_ptr<RuntimeGpuRenderer> runtime_gpu_renderer_;

    bool gpu_runtime_path_enabled_ = false;
};
