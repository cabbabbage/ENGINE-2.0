#pragma once

#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

/**
 * Engine-owned renderer wrapper that selects the best SDL3 backend once at startup.
 * It always exposes the same API surface regardless of whether the underlying
 * implementation is GPU, accelerated 2D, or software.
 */
enum class RenderBackendType {
    GPU,
    Render2D,
    Software
};

enum class RenderQualityTier {
    GPU,
    Accelerated,
    Software
};

struct RenderCaps {
    RenderBackendType backend_type = RenderBackendType::Render2D;
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

    // Frame control
    void begin_frame(const SDL_Color& clear_color);
    void end_frame();
    void present();

    // Basic transforms
    void set_scale(float scale_x, float scale_y);
    void set_viewport(const SDL_Rect& rect);
    void clear_viewport();

    // Texture helpers
    SDL_Texture* create_texture(SDL_PixelFormat format, SDL_TextureAccess access, int w, int h) const;
    SDL_Texture* create_texture_from_surface(SDL_Surface* surface) const;

private:
    EngineRenderer(SDL_Renderer* renderer, RenderCaps caps, RenderQualityTier tier, SDL_Window* window);

    struct AttemptResult {
        SDL_Renderer* renderer = nullptr;
        RenderCaps caps;
        std::string failure_reason;
    };

    static AttemptResult try_create_gpu(SDL_Window* window, bool prefer_vsync, const char* gpu_driver_hint);
    static AttemptResult try_create_accelerated(SDL_Window* window, bool prefer_vsync, const char* renderer_name_hint);
    static AttemptResult try_create_software(SDL_Window* window);

    static RenderCaps build_caps(SDL_Renderer* renderer, RenderBackendType backend_type);
    static RenderQualityTier choose_quality_tier(const RenderCaps& caps);
    static void log_caps(const RenderCaps& caps);

    SDL_Renderer* renderer_ = nullptr;
    SDL_Window* window_ = nullptr;
    RenderCaps caps_{};
    RenderQualityTier quality_tier_ = RenderQualityTier::Accelerated;
};
