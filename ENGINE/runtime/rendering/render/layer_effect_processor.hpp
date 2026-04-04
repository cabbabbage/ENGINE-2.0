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
    static double fog_alpha_from_depth(double layer_depth,
                                       double max_cull_depth,
                                       double fog_density,
                                       double fog_depth_curve);

    bool apply_lens_blur(SDL_Texture* src,
                         SDL_Texture* dst,
                         SDL_Texture* scratch,
                         int target_w,
                         int target_h,
                         float radius_px,
                         const SDL_FPoint& optical_center,
                         float radial_radius_px,
                         float quality_scale = 1.0f) const;
    bool apply_atmospheric_fog(SDL_Texture* src,
                               SDL_Texture* dst,
                               SDL_Texture* scratch,
                               int target_w,
                               int target_h,
                               SDL_Color fog_color,
                               float fog_alpha,
                               float time_seconds,
                               float quality_scale = 1.0f) const;

private:
    bool ensure_fog_noise_tile() const;
    bool ensure_fog_sheet(int target_w, int target_h) const;
    bool update_fog_sheet(int target_w, int target_h, float time_seconds) const;
    void destroy_fog_resources();

    SDL_Renderer* renderer_ = nullptr;
    mutable SDL_Texture* fog_noise_tile_ = nullptr;
    mutable SDL_Texture* fog_sheet_tex_ = nullptr;
    mutable int fog_sheet_w_ = 0;
    mutable int fog_sheet_h_ = 0;
    mutable float fog_sheet_last_time_ = -1.0f;
    mutable std::vector<Uint8> fog_noise_pixels_;
};
