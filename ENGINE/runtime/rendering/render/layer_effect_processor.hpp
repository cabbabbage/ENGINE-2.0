#pragma once

#include <SDL3/SDL.h>

#include <vector>

class LayerEffectProcessor {
public:
    struct LayerProcessResult {
        SDL_Texture* final_texture = nullptr;
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
                                     SDL_Texture* composited_output_texture);

private:
    struct BlurKernelCache {
        int kernel_radius = -1;
        float sigma = -1.0f;
        std::vector<float> offsets;
        std::vector<Uint8> alphas;
        Uint8 center_alpha = 255;
    };

    void destroy_owned_resources();

    SDL_BlendMode sum_blend_mode() const;

    void ensure_blur_kernel_cache(int kernel_radius, float sigma) const;

    SDL_Renderer* renderer_ = nullptr;

    mutable BlurKernelCache blur_kernel_cache_{};

    mutable SDL_BlendMode sum_blend_mode_ = SDL_BLENDMODE_INVALID;
    mutable bool sum_blend_mode_ready_ = false;
};
