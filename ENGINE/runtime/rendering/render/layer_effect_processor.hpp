#pragma once

#include <SDL3/SDL.h>
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

class LayerEffectProcessor {
public:
    struct RuntimeLight {
        SDL_FPoint screen_center{0.0f, 0.0f};
        SDL_Color color{255, 255, 255, 255};
        float intensity = 1.0f;
        float radius_px = 220.0f;
        float falloff = 1.8f;
        float world_z = 0.0f;
    };

    struct LayerLightingParams {
        bool enabled = true;
        SDL_Color ambient_color{18, 20, 24, 255};
    };

    struct LayerFogParams {
        bool enabled = false;
        float normalized_depth = 0.0f;
        float bottom_y_px = 0.0f;
        SDL_Color tint{222, 232, 242, 255};
    };

    struct LayerBlurParams {
        bool enabled = false;
        float radius_px = 0.0f;
        SDL_FPoint optical_center{0.0f, 0.0f};
        float radial_radius_px = 0.0f;
        float quality_scale = 1.0f;
    };

    struct LayerScratchTextures {
        SDL_Texture* dark_mask_texture = nullptr;
        SDL_Texture* blur_texture = nullptr;
        SDL_Texture* blur_scratch_texture = nullptr;
    };

    struct LayerProcessResult {
        SDL_Texture* final_texture = nullptr;
        bool lighting_applied = false;
        bool fog_applied = false;
        bool blur_applied = false;
    };

    explicit LayerEffectProcessor(SDL_Renderer* renderer = nullptr) : renderer_(renderer) {}
    ~LayerEffectProcessor();
    LayerEffectProcessor(const LayerEffectProcessor&) = delete;
    LayerEffectProcessor& operator=(const LayerEffectProcessor&) = delete;

    void set_renderer(SDL_Renderer* renderer);

    static double radial_lens_factor_from_optics(double focal_length_mm, double f_stop);
    static double coc_blur_radius_from_depth_delta(double depth_delta,
                                                   double max_cull_depth,
                                                   double focal_length_mm,
                                                   double f_stop,
                                                   double max_blur_px);

    LayerProcessResult process_layer(SDL_Texture* base_layer_texture,
                                     SDL_Texture* composited_output_texture,
                                     double layer_depth_min,
                                     double layer_depth_max,
                                     const LayerLightingParams& lighting_params,
                                     const std::vector<RuntimeLight>& lights,
                                     const LayerFogParams& fog_params,
                                     const LayerBlurParams& blur_params,
                                     const LayerScratchTextures& scratch_textures);

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
    void destroy_owned_resources();
    void destroy_fog_resources();
    void destroy_lighting_resources();
    SDL_Texture* ensure_fog_band_texture();
    bool ensure_light_accum_texture(int target_w, int target_h);
    SDL_Texture* ensure_light_falloff_texture(float falloff);

    SDL_BlendMode alpha_copy_blend_mode();

    float behind_occlusion_weight(double light_world_z,
                                  double layer_depth_min,
                                  double layer_depth_max,
                                  float light_radius_px) const;

    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* fog_band_texture_ = nullptr;
    SDL_Texture* light_accum_texture_ = nullptr;
    int light_accum_width_ = 0;
    int light_accum_height_ = 0;
    std::unordered_map<int, SDL_Texture*> light_falloff_textures_;

    SDL_BlendMode alpha_copy_blend_mode_ = SDL_BLENDMODE_INVALID;
    bool alpha_copy_blend_mode_ready_ = false;

    bool warned_missing_alpha_copy_blend_mode_ = false;
};
