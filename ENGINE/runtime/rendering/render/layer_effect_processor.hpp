#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

class LayerEffectProcessor {
public:
    struct RuntimeLight {
        std::uint64_t stable_light_id = 0;
        SDL_FPoint screen_center{0.0f, 0.0f};
        SDL_Color color{255, 255, 255, 255};
        float intensity = 0.0f;
        float opacity = 0.0f;
        float radius_px = 0.0f;
        float radius_world = 0.0f;
        float falloff = 1.0f;
        float world_z = 0.0f;
        float floor_world_x = 0.0f;
        float floor_world_z = 0.0f;
        float world_height = 0.0f;
        SDL_FPoint floor_screen_center{0.0f, 0.0f};
        bool has_floor_projection = false;
    };

    struct LayerLightingParams {
        bool enabled = false;
        SDL_Color ambient_color{0, 0, 0, 255};
    };

    struct LayerScratchTextures {
        SDL_Texture* dark_mask_texture = nullptr;
    };

    struct LayerProcessResult {
        SDL_Texture* final_texture = nullptr;
        bool lighting_applied = false;
    };

    explicit LayerEffectProcessor(SDL_Renderer* renderer = nullptr)
        : renderer_(renderer) {}
    ~LayerEffectProcessor();

    LayerEffectProcessor(const LayerEffectProcessor&) = delete;
    LayerEffectProcessor& operator=(const LayerEffectProcessor&) = delete;

    void set_renderer(SDL_Renderer* renderer);

    bool apply_lens_blur(SDL_Texture* src,
                         SDL_Texture* dst,
                         SDL_Texture* scratch,
                         int target_w,
                         int target_h,
                         float radius_px,
                         const SDL_FPoint& optical_center,
                         float radial_radius_px,
                         float quality_scale) const;

    LayerProcessResult process_layer(SDL_Texture* base_layer_texture,
                                     SDL_Texture* composited_output_texture,
                                     double layer_depth_min,
                                     double layer_depth_max,
                                     const LayerLightingParams& lighting_params,
                                     const std::vector<RuntimeLight>& lights,
                                     const LayerScratchTextures& scratch_textures);

private:
    struct BlurKernelCache {
        int kernel_radius = -1;
        float sigma = -1.0f;
        std::vector<float> offsets;
        std::vector<Uint8> alphas;
        Uint8 center_alpha = 255;
    };

    void destroy_owned_resources();
    void destroy_lighting_resources();

    SDL_Texture* ensure_light_falloff_texture(float falloff);

    SDL_BlendMode sum_blend_mode() const;
    SDL_BlendMode alpha_copy_blend_mode();
    SDL_BlendMode light_add_rgb_preserve_alpha_blend_mode();
    SDL_BlendMode alpha_masked_multiply_blend_mode();

    bool supports_strict_dark_mask_pipeline();

    void ensure_blur_kernel_cache(int kernel_radius, float sigma) const;

    SDL_Renderer* renderer_ = nullptr;
    std::unordered_map<int, SDL_Texture*> light_falloff_textures_;

    mutable BlurKernelCache blur_kernel_cache_{};

    mutable SDL_BlendMode sum_blend_mode_ = SDL_BLENDMODE_INVALID;
    mutable bool sum_blend_mode_ready_ = false;


    SDL_BlendMode alpha_copy_blend_mode_ = SDL_BLENDMODE_INVALID;
    SDL_BlendMode light_add_rgb_preserve_alpha_blend_mode_ = SDL_BLENDMODE_INVALID;
    SDL_BlendMode alpha_masked_multiply_blend_mode_ = SDL_BLENDMODE_INVALID;

    bool alpha_copy_blend_mode_ready_ = false;
    bool light_add_rgb_preserve_alpha_blend_mode_ready_ = false;
    bool alpha_masked_multiply_blend_mode_ready_ = false;

    bool warned_missing_alpha_copy_blend_mode_ = false;
    bool warned_missing_strict_dark_mask_pipeline_blend_modes_ = false;
};
