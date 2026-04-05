#pragma once

#include <SDL3/SDL.h>
#include <vector>

class LayerEffectProcessor {
public:
    explicit LayerEffectProcessor(SDL_Renderer* renderer = nullptr) : renderer_(renderer) {}
    ~LayerEffectProcessor();
    LayerEffectProcessor(const LayerEffectProcessor&) = delete;
    LayerEffectProcessor& operator=(const LayerEffectProcessor&) = delete;

    void set_renderer(SDL_Renderer* renderer) {
        if (renderer_ == renderer) {
            return;
        }
        destroy_fog_resources();
        renderer_ = renderer;
    }

    static double radial_lens_factor_from_optics(double focal_length_mm, double f_stop);
    static double coc_blur_radius_from_depth_delta(double depth_delta,
                                                   double max_cull_depth,
                                                   double focal_length_mm,
                                                   double f_stop,
                                                   double max_blur_px);

    bool apply_lens_blur(SDL_Texture* src,
                         SDL_Texture* dst,
                         SDL_Texture* scratch,
                         int target_w,
                         int target_h,
                         float radius_px,
                         const SDL_FPoint& optical_center,
                         float radial_radius_px,
                         float quality_scale = 1.0f) const;

private:
    void destroy_fog_resources();

    SDL_Renderer* renderer_ = nullptr;
};
