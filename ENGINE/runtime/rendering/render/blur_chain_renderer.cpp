#include "rendering/render/blur_chain_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "rendering/render/render.hpp"

namespace {

constexpr float kBlurEpsilon = 1.0e-4f;

/*
    CLEARLY MARKED ATMOSPHERE TUNING

    Increase this to make distant layers hazier and a little dustier.
    Good starting range: 0.0f to 1.5f
*/
constexpr float kAtmosphereDustAmount = 0.95f;

constexpr Uint8 kClearNightTintR = 168;
constexpr Uint8 kClearNightTintG = 188;
constexpr Uint8 kClearNightTintB = 232;

void destroy_texture(SDL_Texture*& texture) {
    if (texture) {
        SDL_DestroyTexture(texture);
        texture = nullptr;
    }
}

float sanitized_non_negative(float value) {
    return (std::isfinite(value) && value > 0.0f) ? value : 0.0f;
}

float clamp01(float value) {
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return std::clamp(value, 0.0f, 1.0f);
}

Uint8 lerp_u8(Uint8 a, Uint8 b, float t) {
    const float clamped_t = clamp01(t);
    const float value = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * clamped_t;
    return static_cast<Uint8>(std::clamp(std::lround(value), 0l, 255l));
}

SDL_Texture* find_player_layer_texture(const render_pipeline::LayerRenderResult& layer_render) {
    const int player_layer_index = layer_render.player_layer_index;
    if (player_layer_index < 0) {
        return nullptr;
    }
    if (static_cast<std::size_t>(player_layer_index) >= layer_render.final_layer_textures.size()) {
        return nullptr;
    }
    return layer_render.final_layer_textures[static_cast<std::size_t>(player_layer_index)];
}

} // namespace

BlurChainRenderer::BlurChainRenderer(SDL_Renderer* renderer)
    : renderer_(renderer),
      blur_processor_(renderer) {}

BlurChainRenderer::~BlurChainRenderer() {
    destroy_targets();
}

void BlurChainRenderer::destroy_targets() {
    destroy_texture(background_mid_tex_);
    destroy_texture(foreground_mid_tex_);
    destroy_texture(chain_temp_tex_);
    destroy_texture(blur_work_tex_);
}

void BlurChainRenderer::set_output_dimensions(int screen_width, int screen_height) {
    const int safe_w = std::max(1, screen_width);
    const int safe_h = std::max(1, screen_height);
    if (safe_w == screen_width_ && safe_h == screen_height_) {
        return;
    }

    screen_width_ = safe_w;
    screen_height_ = safe_h;
    destroy_targets();
}

bool BlurChainRenderer::ensure_target(SDL_Texture*& texture) {
    if (!renderer_ || screen_width_ <= 0 || screen_height_ <= 0) {
        return false;
    }

    if (texture) {
        float w = 0.0f;
        float h = 0.0f;
        if (SDL_GetTextureSize(texture, &w, &h) &&
            static_cast<int>(std::lround(w)) == screen_width_ &&
            static_cast<int>(std::lround(h)) == screen_height_) {
            return true;
        }
        destroy_texture(texture);
    }

    texture = SDL_CreateTexture(renderer_,
                                SDL_PIXELFORMAT_RGBA8888,
                                SDL_TEXTUREACCESS_TARGET,
                                screen_width_,
                                screen_height_);
    if (!texture) {
        return false;
    }

    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return true;
}

bool BlurChainRenderer::ensure_targets() {
    return ensure_target(background_mid_tex_) &&
           ensure_target(foreground_mid_tex_) &&
           ensure_target(chain_temp_tex_) &&
           ensure_target(blur_work_tex_);
}

void BlurChainRenderer::clear_target(SDL_Texture* texture) const {
    if (!renderer_ || !texture) {
        return;
    }

    SDL_SetRenderTarget(renderer_, texture);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);
}

bool BlurChainRenderer::copy_texture(SDL_Texture* src, SDL_Texture* dst) const {
    if (!renderer_ || !src || !dst) {
        return false;
    }

    clear_target(dst);
    SDL_SetRenderTarget(renderer_, dst);
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(src, 255);
    SDL_SetTextureColorMod(src, 255, 255, 255);
    SDL_RenderTexture(renderer_, src, nullptr, nullptr);
    return true;
}

bool BlurChainRenderer::composite_texture_over(SDL_Texture* src, SDL_Texture* dst) const {
    if (!renderer_ || !src || !dst) {
        return false;
    }

    SDL_SetRenderTarget(renderer_, dst);
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(src, 255);
    SDL_SetTextureColorMod(src, 255, 255, 255);
    SDL_RenderTexture(renderer_, src, nullptr, nullptr);
    return true;
}

bool BlurChainRenderer::blur_step(SDL_Texture* src,
                                  SDL_Texture* dst,
                                  SDL_Texture* blur_work,
                                  float blur_px,
                                  SDL_FPoint optical_center,
                                  float radial_blur_px,
                                  float quality_scale) const {
    if (!src || !dst || !blur_work) {
        return false;
    }

    const float safe_blur_px = sanitized_non_negative(blur_px);
    const float safe_radial_blur_px = sanitized_non_negative(radial_blur_px);
    if (!render_internal::dof_blur_chain_enabled(true, safe_blur_px, safe_radial_blur_px)) {
        return copy_texture(src, dst);
    }

    return blur_processor_.apply_lens_blur(src,
                                           dst,
                                           blur_work,
                                           screen_width_,
                                           screen_height_,
                                           safe_blur_px,
                                           optical_center,
                                           safe_radial_blur_px,
                                           quality_scale);
}

bool BlurChainRenderer::apply_atmosphere_to_texture(SDL_Texture* src,
                                                    SDL_Texture* dst,
                                                    const AtmosphereParams& params) const {
    if (!renderer_ || !src || !dst) {
        return false;
    }

    const float amount = clamp01(params.amount);
    if (amount <= 0.0001f) {
        return copy_texture(src, dst);
    }

    clear_target(dst);
    SDL_SetRenderTarget(renderer_, dst);

    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(src, 255);

    const float desaturation = clamp01(params.desaturation);
    const float tint_mix = amount * (0.50f + 0.50f * desaturation);

    const Uint8 mod_r = lerp_u8(255, params.tint_r, tint_mix);
    const Uint8 mod_g = lerp_u8(255, params.tint_g, tint_mix);
    const Uint8 mod_b = lerp_u8(255, params.tint_b, tint_mix);

    SDL_SetTextureColorMod(src, mod_r, mod_g, mod_b);
    SDL_RenderTexture(renderer_, src, nullptr, nullptr);

    const float lift = clamp01(params.lift);
    if (lift > 0.0001f) {
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_ADD);
        SDL_SetRenderDrawColor(renderer_,
                               params.tint_r,
                               params.tint_g,
                               params.tint_b,
                               static_cast<Uint8>(std::clamp(std::lround(lift * 255.0f), 0l, 255l)));
        SDL_RenderFillRect(renderer_, nullptr);
    }

    SDL_SetTextureColorMod(src, 255, 255, 255);
    SDL_SetTextureAlphaMod(src, 255);
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
    return true;
}

std::vector<int> BlurChainRenderer::build_repeat_schedule(std::size_t chain_size, int total_pass_budget) {
    if (chain_size == 0 || total_pass_budget <= 0) {
        return {};
    }

    std::vector<int> schedule(chain_size, 0);
    const std::size_t pass_count = static_cast<std::size_t>(std::max(0, total_pass_budget));

    for (std::size_t pass_index = 0; pass_index < pass_count; ++pass_index) {
        std::size_t slot =
            std::min(chain_size - 1,
                     ((pass_index + 1) * chain_size) / (pass_count + 1));

        if (chain_size > 1) {
            slot = std::max<std::size_t>(1, slot);
        }

        ++schedule[slot];
    }

    if (chain_size == 1 && schedule[0] == 0 && total_pass_budget > 0) {
        schedule[0] = 1;
    }

    if (chain_size > 1 && schedule.back() == 0 && total_pass_budget > 0) {
        ++schedule.back();
    }

    return schedule;
}

int BlurChainRenderer::compute_total_pass_budget(std::size_t chain_size,
                                                 float blur_px,
                                                 float radial_blur_px) {
    if (chain_size == 0) {
        return 0;
    }

    const float safe_blur_px = sanitized_non_negative(blur_px);
    const float safe_radial_blur_px = sanitized_non_negative(radial_blur_px);
    const float max_radius = std::max(safe_blur_px, safe_radial_blur_px);

    if (max_radius <= kBlurEpsilon) {
        return 0;
    }

    int budget = 1;
    if (max_radius >= 6.0f) {
        budget = 2;
    }
    if (max_radius >= 16.0f) {
        budget = 3;
    }
    if (max_radius >= 36.0f) {
        budget = 4;
    }

    if (chain_size <= 1) {
        budget = 1;
    } else if (chain_size == 2) {
        budget = std::min(budget, 2);
    } else {
        budget = std::min(budget, static_cast<int>(chain_size));
    }

    return std::max(0, budget);
}

float BlurChainRenderer::compute_quality_scale(int screen_width,
                                               int screen_height,
                                               float blur_px,
                                               float radial_blur_px) {
    const float safe_blur_px = sanitized_non_negative(blur_px);
    const float safe_radial_blur_px = sanitized_non_negative(radial_blur_px);
    const float max_radius = std::max(safe_blur_px, safe_radial_blur_px);
    const int min_dim = std::max(1, std::min(screen_width, screen_height));

    if (max_radius <= 2.0f || min_dim <= 540) {
        return 1.0f;
    }
    if (max_radius <= 6.0f) {
        return 0.92f;
    }
    if (max_radius <= 12.0f) {
        return 0.84f;
    }
    if (max_radius <= 24.0f) {
        return 0.72f;
    }
    if (min_dim <= 720) {
        return 0.62f;
    }
    return 0.54f;
}

BlurChainRenderer::AtmosphereParams BlurChainRenderer::compute_atmosphere_params(int chain_step_index,
                                                                                 int chain_size,
                                                                                 AtmosphereSide side) {
    AtmosphereParams params{};
    if (chain_size <= 0 || chain_step_index < 0) {
        return params;
    }

    const float dust = std::max(0.0f, kAtmosphereDustAmount);
    if (dust <= 0.0001f) {
        return params;
    }

    const float t =
        (chain_size <= 1)
            ? 1.0f
            : clamp01(static_cast<float>(chain_step_index) / static_cast<float>(chain_size - 1));

    if (side == AtmosphereSide::Background) {
        params.amount = clamp01((0.10f + 0.40f * t) * dust);
        params.desaturation = clamp01((0.18f + 0.34f * t) * dust);
        params.lift = clamp01((0.025f + 0.065f * t) * dust);
    } else {
        params.amount = clamp01((0.02f + 0.10f * t) * dust);
        params.desaturation = clamp01((0.04f + 0.10f * t) * dust);
        params.lift = clamp01((0.006f + 0.022f * t) * dust);
    }

    params.tint_r = kClearNightTintR;
    params.tint_g = kClearNightTintG;
    params.tint_b = kClearNightTintB;
    return params;
}

bool BlurChainRenderer::compose_chain(const std::vector<int>& chain,
                                      const std::vector<SDL_Texture*>& layer_textures,
                                      SDL_Texture* seed_texture,
                                      SDL_Texture* output_texture,
                                      SDL_Texture* temp_texture,
                                      bool blur_enabled,
                                      float blur_px,
                                      float radial_blur_px,
                                      SDL_FPoint optical_center,
                                      float blur_quality_scale,
                                      AtmosphereSide atmosphere_side,
                                      bool& out_has_content) const {
    out_has_content = false;
    if (!renderer_ || !output_texture || !temp_texture) {
        return false;
    }

    clear_target(output_texture);

    SDL_Texture* accum = output_texture;
    SDL_Texture* temp = temp_texture;
    bool initialized = false;

    if (seed_texture) {
        if (!copy_texture(seed_texture, accum)) {
            return false;
        }
        initialized = true;
        out_has_content = true;
    }

    if (chain.empty()) {
        return true;
    }

    const int total_pass_budget =
        blur_enabled ? compute_total_pass_budget(chain.size(), blur_px, radial_blur_px) : 0;
    const std::vector<int> repeat_schedule =
        blur_enabled ? build_repeat_schedule(chain.size(), total_pass_budget) : std::vector<int>{};

    for (std::size_t step = 0; step < chain.size(); ++step) {
        const int layer_index = chain[step];
        if (layer_index < 0 || static_cast<std::size_t>(layer_index) >= layer_textures.size()) {
            continue;
        }

        SDL_Texture* layer_texture = layer_textures[static_cast<std::size_t>(layer_index)];
        if (!layer_texture) {
            continue;
        }

        const AtmosphereParams atmosphere =
            compute_atmosphere_params(static_cast<int>(step),
                                      static_cast<int>(chain.size()),
                                      atmosphere_side);

        //if (!apply_atmosphere_to_texture(layer_texture, temp, atmosphere)) {
         //   return false;
        //}

        if (!initialized) {
            if (!copy_texture(temp, accum)) {
                return false;
            }
            initialized = true;
        } else {
            if (!composite_texture_over(temp, accum)) {
                return false;
            }
        }
        out_has_content = true;

        const int repeat_count =
            (step < repeat_schedule.size()) ? std::max(0, repeat_schedule[step]) : 0;
        for (int pass = 0; pass < repeat_count; ++pass) {
            if (!blur_step(accum,
                           temp,
                           blur_work_tex_,
                           blur_px,
                           optical_center,
                           radial_blur_px,
                           blur_quality_scale)) {
                return false;
            }
            std::swap(accum, temp);
        }
    }

    if (out_has_content && accum != output_texture) {
        return copy_texture(accum, output_texture);
    }

    return true;
}

render_pipeline::BlurCompositeResult BlurChainRenderer::compose(
    const render_pipeline::LayerRenderResult& layer_render,
    SDL_Texture* background_seed,
    bool depth_of_field_enabled,
    float blur_px,
    float radial_blur_px,
    SDL_FPoint optical_center) {
    render_pipeline::BlurCompositeResult result{};
    if (!renderer_ || !layer_render.valid || layer_render.non_empty_layers.empty()) {
        return result;
    }
    if (!ensure_targets()) {
        return result;
    }

    const float safe_blur_px = sanitized_non_negative(blur_px);
    const float safe_radial_blur_px = sanitized_non_negative(radial_blur_px);
    const bool blur_enabled =
        render_internal::dof_blur_chain_enabled(depth_of_field_enabled,
                                                safe_blur_px,
                                                safe_radial_blur_px);

    const float blur_quality_scale =
        blur_enabled
            ? compute_quality_scale(screen_width_, screen_height_, safe_blur_px, safe_radial_blur_px)
            : 1.0f;

    std::vector<int> background_chain =
        render_internal::background_chain_layers(layer_render.non_empty_layers,
                                                 layer_render.player_layer_index);
    const std::vector<int> foreground_chain =
        render_internal::foreground_chain_layers(layer_render.non_empty_layers,
                                                 layer_render.player_layer_index);

    background_chain.erase(
        std::remove(background_chain.begin(),
                    background_chain.end(),
                    layer_render.player_layer_index),
        background_chain.end());

    bool background_has_content = false;
    bool foreground_has_content = false;

    if (!compose_chain(background_chain,
                       layer_render.final_layer_textures,
                       background_seed,
                       background_mid_tex_,
                       chain_temp_tex_,
                       blur_enabled,
                       safe_blur_px,
                       safe_radial_blur_px,
                       optical_center,
                       blur_quality_scale,
                       AtmosphereSide::Background,
                       background_has_content)) {
        return result;
    }

    if (SDL_Texture* player_layer_texture = find_player_layer_texture(layer_render)) {
        if (!background_has_content) {
            if (!copy_texture(player_layer_texture, background_mid_tex_)) {
                return result;
            }
            background_has_content = true;
        } else {
            if (!composite_texture_over(player_layer_texture, background_mid_tex_)) {
                return result;
            }
        }
    }

    if (!compose_chain(foreground_chain,
                       layer_render.final_layer_textures,
                       nullptr,
                       foreground_mid_tex_,
                       chain_temp_tex_,
                       blur_enabled,
                       safe_blur_px,
                       safe_radial_blur_px,
                       optical_center,
                       blur_quality_scale,
                       AtmosphereSide::Foreground,
                       foreground_has_content)) {
        return result;
    }

    result.valid = background_has_content || foreground_has_content;
    result.background_mid = background_has_content ? background_mid_tex_ : nullptr;
    result.foreground_mid = foreground_has_content ? foreground_mid_tex_ : nullptr;
    return result;
}