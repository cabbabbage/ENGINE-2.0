#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <vector>

#include "rendering/render/layer_effect_processor.hpp"
#include "rendering/render/render_pipeline_types.hpp"

class BlurChainRenderer {
public:
    explicit BlurChainRenderer(SDL_Renderer* renderer);
    ~BlurChainRenderer();

    BlurChainRenderer(const BlurChainRenderer&) = delete;
    BlurChainRenderer& operator=(const BlurChainRenderer&) = delete;

    void set_output_dimensions(int screen_width, int screen_height);

    render_pipeline::BlurCompositeResult compose(const render_pipeline::LayerRenderResult& layer_render,
                                                 SDL_Texture* background_seed,
                                                 bool depth_of_field_enabled,
                                                 float blur_px,
                                                 float radial_blur_px,
                                                 SDL_FPoint optical_center);

private:
    bool ensure_targets();
    bool ensure_target(SDL_Texture*& texture);
    void destroy_targets();
    void clear_target(SDL_Texture* texture) const;
    bool copy_texture(SDL_Texture* src, SDL_Texture* dst) const;
    bool composite_texture_over(SDL_Texture* src, SDL_Texture* dst) const;

    bool blur_step(SDL_Texture* src,
                   SDL_Texture* dst,
                   SDL_Texture* blur_work,
                   float blur_px,
                   SDL_FPoint optical_center,
                   float radial_blur_px,
                   float quality_scale) const;

    static std::vector<int> build_repeat_schedule(std::size_t chain_size, int total_pass_budget);
    static int compute_total_pass_budget(std::size_t chain_size,
                                         float blur_px,
                                         float radial_blur_px);
    static float compute_quality_scale(int screen_width,
                                       int screen_height,
                                       float blur_px,
                                       float radial_blur_px);

    bool compose_chain(const std::vector<int>& chain,
                       const std::vector<SDL_Texture*>& layer_textures,
                       SDL_Texture* seed_texture,
                       SDL_Texture* output_texture,
                       SDL_Texture* temp_texture,
                       bool blur_enabled,
                       float blur_px,
                       float radial_blur_px,
                       SDL_FPoint optical_center,
                       float blur_quality_scale,
                       bool& out_has_content) const;

    SDL_Renderer* renderer_ = nullptr;
    int screen_width_ = 0;
    int screen_height_ = 0;

    mutable LayerEffectProcessor blur_processor_;
    SDL_Texture* background_mid_tex_ = nullptr;
    SDL_Texture* foreground_mid_tex_ = nullptr;
    SDL_Texture* chain_temp_tex_ = nullptr;
    SDL_Texture* blur_work_tex_ = nullptr;
};
