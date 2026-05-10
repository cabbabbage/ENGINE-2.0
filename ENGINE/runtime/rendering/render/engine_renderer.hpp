#pragma once

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

#include <SDL3/SDL.h>

#include "rendering/render/gpu_format_policy.hpp"

enum class RenderBackendType {
    OpenGL,
    GPU
};

enum class RenderQualityTier {
    OpenGL,
    GPU
};

struct RenderCaps {
    RenderBackendType backend_type = RenderBackendType::OpenGL;
    bool supports_render_targets = false;
    bool supports_texture_scale_modes = true;
    int max_texture_size = 0;
    bool supports_blend_modes = true;
    bool is_software = false;
    bool vsync_enabled = false;
    std::string renderer_name;
};

class EngineRenderer {
public:
    static std::unique_ptr<EngineRenderer> Create(SDL_Window* window, bool prefer_vsync = true);

    EngineRenderer(const EngineRenderer&) = delete;
    EngineRenderer& operator=(const EngineRenderer&) = delete;

    ~EngineRenderer();

    SDL_Renderer* raw() const { return renderer_; }
    SDL_Window* window() const { return window_; }
    const RenderCaps& caps() const { return caps_; }
    RenderQualityTier quality_tier() const { return quality_tier_; }
    const std::string& present_mode_name() const { return present_mode_name_; }
    const RuntimeGpuFormatPolicy* gpu_format_policy() const {
        return has_gpu_format_policy_ ? &gpu_format_policy_ : nullptr;
    }
    bool runtime_gpu_supported() const { return renderer_ && !caps_.is_software; }

    // בקרת פריים
    void begin_frame(const SDL_Color& clear_color);
    void end_frame();
    void present();

    // טרנספורמציות בסיסיות
    void set_scale(float scale_x, float scale_y);
    void set_viewport(const SDL_Rect& rect);
    void clear_viewport();

    // כלי עזר לטקסטורות
    SDL_Texture* create_texture(SDL_PixelFormat format, SDL_TextureAccess access, int w, int h) const;
    SDL_Texture* create_texture_from_surface(SDL_Surface* surface) const;

private:
    EngineRenderer(SDL_Renderer* renderer, RenderCaps caps, RenderQualityTier tier, SDL_Window* window);

    struct AttemptResult {
        SDL_Renderer* renderer = nullptr;
        RenderCaps caps;
        std::string failure_reason;
    };

    static AttemptResult try_create_opengl(SDL_Window* window, bool prefer_vsync);

    static RenderCaps build_caps(SDL_Renderer* renderer, RenderBackendType backend_type);
    static void log_caps(const RenderCaps& caps);

    SDL_Renderer* renderer_ = nullptr;
    SDL_Window* window_ = nullptr;
    RenderCaps caps_{};
    RenderQualityTier quality_tier_ = RenderQualityTier::OpenGL;
    std::string present_mode_name_ = "vsync";
    RuntimeGpuFormatPolicy gpu_format_policy_{};
    bool has_gpu_format_policy_ = false;
    std::uint64_t last_present_counter_ = 0;
};
