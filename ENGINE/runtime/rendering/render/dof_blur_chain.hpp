#pragma once

#include <cstdint>
#include <vector>

#include <SDL3/SDL.h>

namespace dof_blur_chain {

struct LayerTexture {
    int depth_layer = 0;
    float blur_strength = 0.0f;
    SDL_Texture* texture = nullptr;
};

struct CompositeResult {
    bool valid = false;
    SDL_Texture* background_mid = nullptr;
    SDL_Texture* foreground_mid = nullptr;
    std::uint32_t blur_pass_count = 0;
};

bool enabled(bool depth_of_field_enabled, float blur_px, float radial_blur_px);
class Renderer {
public:
    explicit Renderer(SDL_Renderer* renderer = nullptr);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void set_renderer(SDL_Renderer* renderer);
    void set_output_dimensions(int width, int height);
    void destroy_targets();

    CompositeResult compose(const std::vector<LayerTexture>& layers,
                            SDL_Texture* background_seed,
                            bool depth_of_field_enabled,
                            float blur_px,
                            float radial_blur_px,
                            SDL_FPoint optical_center,
                            int focus_depth_layer = 0);

private:
    bool ensure_targets();
    bool ensure_target(SDL_Texture*& texture, const char* label);
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
    SDL_Renderer* renderer_ = nullptr;
    int width_ = 1;
    int height_ = 1;
    SDL_Texture* background_mid_ = nullptr;
    SDL_Texture* foreground_mid_ = nullptr;
    SDL_Texture* foreground_layer_ = nullptr;
    SDL_Texture* chain_temp_ = nullptr;
    SDL_Texture* blur_work_ = nullptr;
    std::vector<LayerTexture> scratch_background_layers_{};
    std::vector<LayerTexture> scratch_foreground_layers_{};
};

} // namespace dof_blur_chain
