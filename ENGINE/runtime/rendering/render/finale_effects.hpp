#pragma once

#include <string>

#include <SDL3/SDL.h>

namespace finale_effects {

struct FinalEffectSettings {
    // Effect toggles. Only motion blur is enabled.
    static constexpr bool brightness_enabled = false;
    static constexpr bool saturation_enabled = false;
    static constexpr bool contrast_enabled = false;
    static constexpr bool sharpness_enabled = false;
    static constexpr bool vignette_enabled = false;
    static constexpr bool film_grain_enabled = false;
    static constexpr bool chromatic_aberration_enabled = false;
    static constexpr bool motion_blur_enabled = true;

    // Disabled effect settings.
    static constexpr float brightness = 1.0f;
    static constexpr float saturation = 1.0f;
    static constexpr float contrast = 1.0f;
    static constexpr float sharpness = 0.0f;
    static constexpr float vignette_strength = 0.0f;
    static constexpr float film_grain_strength = 0.0f;
    static constexpr float chromatic_aberration_px = 0.0f;

    // Motion blur.
    // This is 0 to 100 percent.
    // 33 means the new frame is drawn over the previous accumulated frame at 33% opacity.
    // Lower = longer trails.
    // Higher = faster fade / less blur.
    static constexpr float motion_blur_new_frame_opacity_percent = 73.0f;
};

class FinalEffects {
public:
    FinalEffects() = delete;

    static SDL_Texture* apply(
        SDL_Renderer* renderer,
        SDL_Texture* source,
        SDL_Texture* destination,
        int width,
        int height,
        std::string& out_error);

    static void destroy_cached_textures();
    static void reset_motion_blur_history();

private:
    static bool copy_texture(
        SDL_Renderer* renderer,
        SDL_Texture* source,
        SDL_Texture* destination,
        int width,
        int height,
        std::string& out_error);

    static SDL_Texture* apply_brightness(
        SDL_Renderer* renderer,
        SDL_Texture* source,
        SDL_Texture* destination,
        int width,
        int height,
        std::string& out_error);

    static SDL_Texture* apply_saturation(
        SDL_Renderer* renderer,
        SDL_Texture* source,
        SDL_Texture* destination,
        int width,
        int height,
        std::string& out_error);

    static SDL_Texture* apply_contrast(
        SDL_Renderer* renderer,
        SDL_Texture* source,
        SDL_Texture* destination,
        int width,
        int height,
        std::string& out_error);

    static SDL_Texture* apply_sharpness(
        SDL_Renderer* renderer,
        SDL_Texture* source,
        SDL_Texture* destination,
        int width,
        int height,
        std::string& out_error);

    static SDL_Texture* apply_vignette(
        SDL_Renderer* renderer,
        SDL_Texture* source,
        SDL_Texture* destination,
        int width,
        int height,
        std::string& out_error);

    static SDL_Texture* apply_film_grain(
        SDL_Renderer* renderer,
        SDL_Texture* source,
        SDL_Texture* destination,
        int width,
        int height,
        std::string& out_error);

    static SDL_Texture* apply_chromatic_aberration(
        SDL_Renderer* renderer,
        SDL_Texture* source,
        SDL_Texture* destination,
        int width,
        int height,
        std::string& out_error);

    static SDL_Texture* apply_motion_blur(
        SDL_Renderer* renderer,
        SDL_Texture* source,
        SDL_Texture* destination,
        int width,
        int height,
        std::string& out_error);
};

} // namespace finale_effects

SDL_Texture* helperpereffects(
    SDL_Renderer* renderer,
    SDL_Texture* source,
    SDL_Texture* destination,
    int width,
    int height,
    std::string& out_error);