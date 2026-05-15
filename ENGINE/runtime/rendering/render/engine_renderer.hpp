#pragma once

#include <memory>
#include <string>
#include <cstdint>

#include <SDL3/SDL.h>


class EngineRenderer {
public:
    static std::unique_ptr<EngineRenderer> Create(SDL_Window* window, bool prefer_vsync = true);

    EngineRenderer(const EngineRenderer&) = delete;
    EngineRenderer& operator=(const EngineRenderer&) = delete;

    ~EngineRenderer();

    SDL_Renderer* raw() const { return renderer_; }
    SDL_Window* window() const { return window_; }
    const std::string& renderer_name() const { return renderer_name_; }
    const std::string& present_mode_name() const { return present_mode_name_; }

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
    EngineRenderer(SDL_Renderer* renderer, SDL_Window* window);

    struct AttemptResult {
        SDL_Renderer* renderer = nullptr;
        std::string failure_reason;
    };

    static AttemptResult try_create_opengl(SDL_Window* window, bool prefer_vsync);
    static void log_opengl_renderer(SDL_Renderer* renderer);

    SDL_Renderer* renderer_ = nullptr;
    SDL_Window* window_ = nullptr;
    std::string renderer_name_ = "unknown";
    std::string present_mode_name_ = "vsync";
    std::uint64_t last_present_counter_ = 0;
};
