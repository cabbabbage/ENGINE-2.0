#include "rendering/render/dof_blur_chain.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "rendering/render/render_diagnostics.hpp"

namespace {

constexpr float kBlurEpsilonPx = 1.0e-4f;

struct TextureStateSnapshot {
    SDL_BlendMode blend_mode = SDL_BLENDMODE_BLEND;
    Uint8 alpha_mod = 255;
    Uint8 color_mod_r = 255;
    Uint8 color_mod_g = 255;
    Uint8 color_mod_b = 255;
};

float sanitized_non_negative(float value) {
    return (std::isfinite(value) && value > 0.0f) ? value : 0.0f;
}

TextureStateSnapshot capture_texture_state(SDL_Texture* texture) {
    TextureStateSnapshot state{};
    if (!texture) {
        return state;
    }
    SDL_GetTextureBlendMode(texture, &state.blend_mode);
    SDL_GetTextureAlphaMod(texture, &state.alpha_mod);
    SDL_GetTextureColorMod(texture, &state.color_mod_r, &state.color_mod_g, &state.color_mod_b);
    return state;
}

void restore_texture_state(SDL_Texture* texture, const TextureStateSnapshot& state) {
    if (!texture) {
        return;
    }
    SDL_SetTextureBlendMode(texture, state.blend_mode);
    SDL_SetTextureAlphaMod(texture, state.alpha_mod);
    SDL_SetTextureColorMod(texture, state.color_mod_r, state.color_mod_g, state.color_mod_b);
}

SDL_BlendMode make_sum_blend_mode() {
    return SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_SRC_ALPHA,
                                      SDL_BLENDFACTOR_ONE,
                                      SDL_BLENDOPERATION_ADD,
                                      SDL_BLENDFACTOR_ONE,
                                      SDL_BLENDFACTOR_ONE,
                                      SDL_BLENDOPERATION_ADD);
}

void build_gaussian_weights(std::vector<float>& weights, int kernel_radius, float sigma) {
    weights.assign(static_cast<std::size_t>(kernel_radius + 1), 0.0f);

    float sum = 0.0f;
    for (int i = 0; i <= kernel_radius; ++i) {
        const float x = static_cast<float>(i);
        const float w = std::exp(-(x * x) / (2.0f * sigma * sigma));
        weights[static_cast<std::size_t>(i)] = w;
        sum += (i == 0) ? w : (2.0f * w);
    }

    if (sum > 1.0e-6f) {
        for (float& w : weights) {
            w /= sum;
        }
    }
}

void draw_weighted_offset_sample(SDL_Renderer* renderer,
                                 SDL_Texture* texture,
                                 int draw_w,
                                 int draw_h,
                                 float offset_x,
                                 float offset_y,
                                 float weight) {
    if (!renderer || !texture || draw_w <= 0 || draw_h <= 0 || weight <= 0.0f) {
        return;
    }

    const int alpha = std::clamp(static_cast<int>(std::lround(weight * 255.0f)), 0, 255);
    if (alpha <= 0) {
        return;
    }

    SDL_SetTextureAlphaMod(texture, static_cast<Uint8>(alpha));
    const SDL_FRect src_rect{0.0f, 0.0f, static_cast<float>(draw_w), static_cast<float>(draw_h)};
    const SDL_FRect dst_rect{offset_x, offset_y, static_cast<float>(draw_w), static_cast<float>(draw_h)};
    render_diagnostics::render_texture(renderer, texture, &src_rect, &dst_rect);
}

void draw_weighted_scaled_sample(SDL_Renderer* renderer,
                                 SDL_Texture* texture,
                                 int draw_w,
                                 int draw_h,
                                 const SDL_FPoint& optical_center,
                                 float scale,
                                 float weight) {
    if (!renderer || !texture || draw_w <= 0 || draw_h <= 0 || weight <= 0.0f) {
        return;
    }

    const int alpha = std::clamp(static_cast<int>(std::lround(weight * 255.0f)), 0, 255);
    if (alpha <= 0) {
        return;
    }

    SDL_SetTextureAlphaMod(texture, static_cast<Uint8>(alpha));
    const float scaled_w = static_cast<float>(draw_w) * scale;
    const float scaled_h = static_cast<float>(draw_h) * scale;
    const SDL_FRect src_rect{0.0f, 0.0f, static_cast<float>(draw_w), static_cast<float>(draw_h)};
    const SDL_FRect dst_rect{
        optical_center.x - optical_center.x * scale,
        optical_center.y - optical_center.y * scale,
        scaled_w,
        scaled_h};
    render_diagnostics::render_texture(renderer, texture, &src_rect, &dst_rect);
}

void clear_texture_target(SDL_Renderer* renderer, SDL_Texture* target) {
    if (!renderer || !target) {
        return;
    }
    render_diagnostics::set_render_target(renderer, target);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);
}

bool copy_texture_region(SDL_Renderer* renderer,
                         SDL_Texture* src,
                         SDL_Texture* dst,
                         const SDL_FRect* src_rect,
                         const SDL_FRect* dst_rect) {
    if (!renderer || !src || !dst) {
        return false;
    }
    clear_texture_target(renderer, dst);
    render_diagnostics::set_render_target(renderer, dst);
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(src, 255);
    SDL_SetTextureColorMod(src, 255, 255, 255);
    return render_diagnostics::render_texture(renderer, src, src_rect, dst_rect);
}

bool prepare_damage_pulse_texture(SDL_Renderer* renderer,
                                  SDL_Texture* src,
                                  SDL_Texture* dst,
                                  int draw_w,
                                  int draw_h,
                                  float warp_px,
                                  float tint_strength,
                                  float phase) {
    if (!renderer || !src || !dst || draw_w <= 0 || draw_h <= 0 || src == dst) {
        return false;
    }

    const float safe_warp = std::clamp(std::abs(warp_px), 0.0f, dof_blur_chain::damage_pulse_tuning::kMaxWarpPx);
    const float safe_tint = std::clamp(tint_strength, 0.0f, dof_blur_chain::damage_pulse_tuning::kMaxTintStrength);
    if (safe_warp <= kBlurEpsilonPx && safe_tint <= kBlurEpsilonPx) {
        return copy_texture_region(renderer, src, dst, nullptr, nullptr);
    }

    const TextureStateSnapshot src_state = capture_texture_state(src);
    clear_texture_target(renderer, dst);
    render_diagnostics::set_render_target(renderer, dst);
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
    SDL_SetTextureColorMod(src, 255, 255, 255);

    const float phase_sign = std::sin(phase) >= 0.0f ? 1.0f : -1.0f;
    const float side_offset_x = safe_warp * phase_sign;
    const float side_offset_y = safe_warp * 0.22f * phase_sign;
    draw_weighted_offset_sample(renderer, src, draw_w, draw_h, side_offset_x, side_offset_y, 0.28f);
    draw_weighted_offset_sample(renderer, src, draw_w, draw_h, -side_offset_x * 0.66f, -side_offset_y * 0.66f, 0.22f);
    draw_weighted_offset_sample(renderer, src, draw_w, draw_h, 0.0f, 0.0f, 0.50f);

    if (safe_tint > kBlurEpsilonPx) {
        const Uint8 tint_alpha = static_cast<Uint8>(std::clamp(
            static_cast<int>(std::lround(safe_tint * 255.0f)),
            0,
            255));
        const float green_blue_scale = std::clamp(1.0f - safe_tint * 0.78f, 0.0f, 1.0f);
        const Uint8 green_blue = static_cast<Uint8>(std::clamp(
            static_cast<int>(std::lround(green_blue_scale * 255.0f)),
            0,
            255));
        SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
        SDL_SetTextureAlphaMod(src, tint_alpha);
        SDL_SetTextureColorMod(src, 255, green_blue, green_blue);
        (void)render_diagnostics::render_texture(renderer, src, nullptr, nullptr);
    }

    restore_texture_state(src, src_state);
    return true;
}

bool apply_axis_blur(SDL_Renderer* renderer,
                     SDL_Texture* src,
                     SDL_Texture* dst,
                     int draw_w,
                     int draw_h,
                     float radius_px,
                     float dir_x,
                     float dir_y,
                     SDL_BlendMode sum_blend) {
    if (!renderer || !src || !dst || draw_w <= 0 || draw_h <= 0 || src == dst) {
        return false;
    }

    if (radius_px <= kBlurEpsilonPx) {
        return copy_texture_region(renderer, src, dst, nullptr, nullptr);
    }

    clear_texture_target(renderer, dst);
    render_diagnostics::set_render_target(renderer, dst);

    const int kernel_radius = std::clamp(static_cast<int>(std::ceil(radius_px)), 1, 24);
    const float sigma = std::max(kBlurEpsilonPx, radius_px * 0.58f);
    std::vector<float> weights;
    build_gaussian_weights(weights, kernel_radius, sigma);

    SDL_SetTextureBlendMode(src, sum_blend);
    SDL_SetTextureColorMod(src, 255, 255, 255);
    for (int i = -kernel_radius; i <= kernel_radius; ++i) {
        draw_weighted_offset_sample(renderer,
                                    src,
                                    draw_w,
                                    draw_h,
                                    static_cast<float>(i) * dir_x,
                                    static_cast<float>(i) * dir_y,
                                    weights[static_cast<std::size_t>(std::abs(i))]);
    }
    return true;
}

bool apply_radial_zoom_blur(SDL_Renderer* renderer,
                            SDL_Texture* src,
                            SDL_Texture* dst,
                            int draw_w,
                            int draw_h,
                            float radial_radius_px,
                            const SDL_FPoint& optical_center,
                            SDL_BlendMode sum_blend) {
    if (!renderer || !src || !dst || draw_w <= 0 || draw_h <= 0 || src == dst) {
        return false;
    }

    if (radial_radius_px <= kBlurEpsilonPx) {
        return copy_texture_region(renderer, src, dst, nullptr, nullptr);
    }

    clear_texture_target(renderer, dst);
    render_diagnostics::set_render_target(renderer, dst);
    SDL_SetTextureBlendMode(src, sum_blend);
    SDL_SetTextureColorMod(src, 255, 255, 255);

    const float max_dim = static_cast<float>(std::max(draw_w, draw_h));
    const float normalized_radius = std::clamp(radial_radius_px / std::max(1.0f, max_dim), 0.0f, 1.0f);
    const int sample_count = std::clamp(static_cast<int>(std::ceil(radial_radius_px * 2.0f)), 2, 64);
    const float max_scale_delta = std::min(0.65f, normalized_radius * 3.0f);

    float total_raw_weight = 1.0f;
    std::vector<float> side_weights(static_cast<std::size_t>(sample_count), 0.0f);
    for (int i = 0; i < sample_count; ++i) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(sample_count);
        const float w = 1.0f - t;
        side_weights[static_cast<std::size_t>(i)] = w;
        total_raw_weight += 2.0f * w;
    }
    if (total_raw_weight <= 1.0e-6f) {
        return false;
    }

    draw_weighted_scaled_sample(renderer, src, draw_w, draw_h, optical_center, 1.0f, 1.0f / total_raw_weight);
    for (int i = 0; i < sample_count; ++i) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(sample_count);
        const float scale_delta = max_scale_delta * t;
        const float weight = side_weights[static_cast<std::size_t>(i)] / total_raw_weight;
        draw_weighted_scaled_sample(renderer, src, draw_w, draw_h, optical_center, std::max(0.05f, 1.0f - scale_delta), weight);
        draw_weighted_scaled_sample(renderer, src, draw_w, draw_h, optical_center, 1.0f + scale_delta, weight);
    }
    return true;
}

bool apply_lens_blur(SDL_Renderer* renderer,
                     SDL_Texture* src,
                     SDL_Texture* dst,
                     SDL_Texture* scratch,
                     int target_w,
                     int target_h,
                     float radius_px,
                     const SDL_FPoint& optical_center,
                     float radial_radius_px,
                     float quality_scale) {
    if (!renderer || !src || !dst || !scratch || target_w <= 0 || target_h <= 0 || scratch == src || scratch == dst) {
        return false;
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    const TextureStateSnapshot src_state = capture_texture_state(src);
    const TextureStateSnapshot scratch_state = capture_texture_state(scratch);
    const TextureStateSnapshot dst_state = capture_texture_state(dst);

    const SDL_BlendMode sum_blend = make_sum_blend_mode();
    if (sum_blend == SDL_BLENDMODE_INVALID) {
        const bool copied = copy_texture_region(renderer, src, dst, nullptr, nullptr);
        restore_texture_state(src, src_state);
        restore_texture_state(scratch, scratch_state);
        restore_texture_state(dst, dst_state);
        render_diagnostics::set_render_target(renderer, previous_target);
        return copied;
    }

    const float clamped_quality = std::clamp(quality_scale, 0.35f, 1.0f);
    const int process_w = std::clamp(static_cast<int>(std::lround(static_cast<float>(target_w) * clamped_quality)), 1, target_w);
    const int process_h = std::clamp(static_cast<int>(std::lround(static_cast<float>(target_h) * clamped_quality)), 1, target_h);
    const bool reduced_resolution = (process_w != target_w) || (process_h != target_h);
    const float process_blur_radius = std::max(0.0f, radius_px) * clamped_quality;
    const float process_radial_radius = std::max(0.0f, radial_radius_px) * clamped_quality;
    const SDL_FPoint scaled_optical_center{optical_center.x * clamped_quality, optical_center.y * clamped_quality};

    SDL_Texture* current = src;
    if (reduced_resolution) {
        const SDL_FRect lowres_rect{0.0f, 0.0f, static_cast<float>(process_w), static_cast<float>(process_h)};
        if (!copy_texture_region(renderer, src, scratch, nullptr, &lowres_rect)) {
            restore_texture_state(src, src_state);
            restore_texture_state(scratch, scratch_state);
            restore_texture_state(dst, dst_state);
            render_diagnostics::set_render_target(renderer, previous_target);
            return false;
        }
        current = scratch;
    }

    auto pick_other_temp = [&](SDL_Texture* texture) {
        return (texture == scratch) ? dst : scratch;
    };

    if (process_blur_radius > kBlurEpsilonPx) {
        SDL_Texture* next = pick_other_temp(current);
        if (!apply_axis_blur(renderer, current, next, process_w, process_h, process_blur_radius, 1.0f, 0.0f, sum_blend)) {
            restore_texture_state(src, src_state);
            restore_texture_state(scratch, scratch_state);
            restore_texture_state(dst, dst_state);
            render_diagnostics::set_render_target(renderer, previous_target);
            return false;
        }
        current = next;

        next = pick_other_temp(current);
        if (!apply_axis_blur(renderer, current, next, process_w, process_h, process_blur_radius, 0.0f, 1.0f, sum_blend)) {
            restore_texture_state(src, src_state);
            restore_texture_state(scratch, scratch_state);
            restore_texture_state(dst, dst_state);
            render_diagnostics::set_render_target(renderer, previous_target);
            return false;
        }
        current = next;
    }

    if (process_radial_radius > kBlurEpsilonPx) {
        SDL_Texture* next = pick_other_temp(current);
        if (!apply_radial_zoom_blur(renderer, current, next, process_w, process_h, process_radial_radius, scaled_optical_center, sum_blend)) {
            restore_texture_state(src, src_state);
            restore_texture_state(scratch, scratch_state);
            restore_texture_state(dst, dst_state);
            render_diagnostics::set_render_target(renderer, previous_target);
            return false;
        }
        current = next;
    }

    bool copied_to_dst = true;
    if (reduced_resolution) {
        const SDL_FRect lowres_rect{0.0f, 0.0f, static_cast<float>(process_w), static_cast<float>(process_h)};
        if (current == dst) {
            copied_to_dst = copy_texture_region(renderer, dst, scratch, &lowres_rect, &lowres_rect);
            current = scratch;
        }
        if (copied_to_dst) {
            copied_to_dst = copy_texture_region(renderer, current, dst, &lowres_rect, nullptr);
        }
    } else if (current != dst) {
        copied_to_dst = copy_texture_region(renderer, current, dst, nullptr, nullptr);
    }

    restore_texture_state(src, src_state);
    restore_texture_state(scratch, scratch_state);
    restore_texture_state(dst, dst_state);
    render_diagnostics::set_render_target(renderer, previous_target);
    return copied_to_dst;
}

float compute_quality_scale(int width, int height, float blur_px, float radial_blur_px) {
    const float max_radius = std::max(sanitized_non_negative(blur_px), sanitized_non_negative(radial_blur_px));
    const int min_dim = std::max(1, std::min(width, height));
    if (max_radius <= 2.0f || min_dim <= 540) {
        return 1.0f;
    }
    if (max_radius <= 6.0f) {
        return 0.9f;
    }
    if (max_radius <= 12.0f) {
        return 0.8f;
    }
    if (max_radius <= 24.0f) {
        return 0.65f;
    }
    if (min_dim <= 720) {
        return 0.6f;
    }
    return 0.5f;
}

} // namespace

namespace dof_blur_chain {

bool enabled(bool depth_of_field_enabled, float blur_px, float radial_blur_px) {
    return depth_of_field_enabled &&
           (sanitized_non_negative(blur_px) > kBlurEpsilonPx ||
            sanitized_non_negative(radial_blur_px) > kBlurEpsilonPx);
}

Renderer::Renderer(SDL_Renderer* renderer)
    : renderer_(renderer) {}

Renderer::~Renderer() {
    destroy_targets();
}

void Renderer::set_renderer(SDL_Renderer* renderer) {
    if (renderer_ == renderer) {
        return;
    }
    destroy_targets();
    renderer_ = renderer;
}

void Renderer::set_output_dimensions(int width, int height) {
    const int safe_width = std::max(1, width);
    const int safe_height = std::max(1, height);
    if (safe_width == width_ && safe_height == height_) {
        return;
    }
    width_ = safe_width;
    height_ = safe_height;
    destroy_targets();
}

void Renderer::destroy_targets() {
    render_diagnostics::destroy_texture(background_mid_);
    render_diagnostics::destroy_texture(foreground_mid_);
    render_diagnostics::destroy_texture(foreground_layer_);
    render_diagnostics::destroy_texture(chain_temp_);
    render_diagnostics::destroy_texture(blur_work_);
}

bool Renderer::ensure_target(SDL_Texture*& texture, const char* label) {
    if (!renderer_ || width_ <= 0 || height_ <= 0) {
        return false;
    }

    if (texture) {
        float w = 0.0f;
        float h = 0.0f;
        if (SDL_GetTextureSize(texture, &w, &h) &&
            static_cast<int>(std::lround(w)) == width_ &&
            static_cast<int>(std::lround(h)) == height_) {
            return true;
        }
        render_diagnostics::destroy_texture(texture);
    }

    texture = render_diagnostics::create_texture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, width_, height_);
    if (!texture) {
        return false;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    (void)label;
    return true;
}

bool Renderer::ensure_targets() {
    return ensure_target(background_mid_, "dof_background_mid") &&
           ensure_target(foreground_mid_, "dof_foreground_mid") &&
           ensure_target(foreground_layer_, "dof_foreground_layer") &&
           ensure_target(chain_temp_, "dof_chain_temp") &&
           ensure_target(blur_work_, "dof_blur_work");
}

void Renderer::clear_target(SDL_Texture* texture) const {
    clear_texture_target(renderer_, texture);
}

bool Renderer::copy_texture(SDL_Texture* src, SDL_Texture* dst) const {
    return copy_texture_region(renderer_, src, dst, nullptr, nullptr);
}

bool Renderer::composite_texture_over(SDL_Texture* src, SDL_Texture* dst) const {
    if (!renderer_ || !src || !dst) {
        return false;
    }
    render_diagnostics::set_render_target(renderer_, dst);
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(src, 255);
    SDL_SetTextureColorMod(src, 255, 255, 255);
    return render_diagnostics::render_texture(renderer_, src, nullptr, nullptr);
}

bool Renderer::blur_step(SDL_Texture* src,
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
    if (!enabled(true, safe_blur_px, safe_radial_blur_px)) {
        return copy_texture(src, dst);
    }
    return apply_lens_blur(renderer_,
                           src,
                           dst,
                           blur_work,
                           width_,
                           height_,
                           safe_blur_px,
                           optical_center,
                           safe_radial_blur_px,
                           quality_scale);
}

CompositeResult Renderer::compose(const std::vector<LayerTexture>& layers,
                                  SDL_Texture* background_seed,
                                  bool depth_of_field_enabled,
                                  float blur_px,
                                  float radial_blur_px,
                                  SDL_FPoint optical_center,
                                  int focus_depth_layer) {
    CompositeResult result{};
    if (!renderer_ || layers.empty()) {
        return result;
    }
    if (!ensure_targets()) {
        return result;
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer_);
    auto restore_and_return = [&](CompositeResult out) {
        render_diagnostics::set_render_target(renderer_, previous_target);
        return out;
    };

    const float safe_blur_px = sanitized_non_negative(blur_px);
    const float safe_radial_blur_px = sanitized_non_negative(radial_blur_px);
    const bool blur_enabled = enabled(depth_of_field_enabled, safe_blur_px, safe_radial_blur_px);
    const float quality_scale = blur_enabled
        ? compute_quality_scale(width_, height_, safe_blur_px, safe_radial_blur_px)
        : 1.0f;
    auto resolve_layer_source = [&](const LayerTexture& layer, SDL_Texture*& out_texture) -> bool {
        out_texture = layer.texture;
        if (!out_texture) {
            return false;
        }
        if (std::abs(layer.warp_px) <= kBlurEpsilonPx && layer.tint_strength <= kBlurEpsilonPx) {
            return true;
        }
        if (!prepare_damage_pulse_texture(renderer_,
                                          layer.texture,
                                          chain_temp_,
                                          width_,
                                          height_,
                                          layer.warp_px,
                                          layer.tint_strength,
                                          layer.phase)) {
            return false;
        }
        out_texture = chain_temp_;
        return true;
    };

    scratch_background_layers_.clear();
    scratch_foreground_layers_.clear();
    for (const LayerTexture& layer : layers) {
        if (!layer.texture) continue;
        if (layer.depth_layer > focus_depth_layer) scratch_background_layers_.push_back(layer);
        else if (layer.depth_layer < focus_depth_layer) scratch_foreground_layers_.push_back(layer);
    }
    std::sort(scratch_background_layers_.begin(), scratch_background_layers_.end(), [](const LayerTexture& a, const LayerTexture& b){ return a.depth_layer > b.depth_layer; });
    std::sort(scratch_foreground_layers_.begin(), scratch_foreground_layers_.end(), [](const LayerTexture& a, const LayerTexture& b){ return a.depth_layer < b.depth_layer; });

    clear_target(background_mid_);
    bool background_has_content = false;
    bool foreground_has_content = false;
    if (background_seed) {
        // Treat the far background seed as the deepest layer so sky and mountains pick up the full blur.
        if (blur_enabled && (safe_blur_px > kBlurEpsilonPx || safe_radial_blur_px > kBlurEpsilonPx)) {
            if (!blur_step(background_seed,
                           background_mid_,
                           blur_work_,
                           safe_blur_px,
                           optical_center,
                           safe_radial_blur_px,
                           quality_scale)) {
                return restore_and_return(CompositeResult{});
            }
            ++result.blur_pass_count;
        } else if (!copy_texture(background_seed, background_mid_)) {
            return restore_and_return(CompositeResult{});
        }
        background_has_content = true;
    }
    clear_target(foreground_mid_);
    for (const LayerTexture& layer : scratch_background_layers_) {
        SDL_Texture* layer_source = nullptr;
        if (!resolve_layer_source(layer, layer_source)) {
            return restore_and_return(CompositeResult{});
        }
        const float layer_blur = safe_blur_px * std::clamp(layer.blur_strength, 0.0f, 1.0f);
        const float layer_radial = safe_radial_blur_px * std::clamp(layer.blur_strength, 0.0f, 1.0f);
        if (blur_enabled && (layer_blur > kBlurEpsilonPx || layer_radial > kBlurEpsilonPx)) {
            if (!blur_step(layer_source, foreground_layer_, blur_work_, layer_blur, optical_center, layer_radial, quality_scale)) return restore_and_return(CompositeResult{});
            ++result.blur_pass_count;
            if (!composite_texture_over(foreground_layer_, background_mid_)) return restore_and_return(CompositeResult{});
        } else if (!composite_texture_over(layer_source, background_mid_)) return restore_and_return(CompositeResult{});
        background_has_content = true;
    }
    for (const LayerTexture& layer : layers) {
        if (layer.depth_layer == focus_depth_layer && layer.texture) {
            SDL_Texture* layer_source = nullptr;
            if (!resolve_layer_source(layer, layer_source)) {
                return restore_and_return(CompositeResult{});
            }
            if (!composite_texture_over(layer_source, background_mid_)) return restore_and_return(CompositeResult{});
            background_has_content = true;
        }
    }
    for (const LayerTexture& layer : scratch_foreground_layers_) {
        SDL_Texture* layer_source = nullptr;
        if (!resolve_layer_source(layer, layer_source)) {
            return restore_and_return(CompositeResult{});
        }
        const float layer_blur = safe_blur_px * std::clamp(layer.blur_strength, 0.0f, 1.0f);
        const float layer_radial = safe_radial_blur_px * std::clamp(layer.blur_strength, 0.0f, 1.0f);
        if (blur_enabled && (layer_blur > kBlurEpsilonPx || layer_radial > kBlurEpsilonPx)) {
            if (!blur_step(layer_source, foreground_layer_, blur_work_, layer_blur, optical_center, layer_radial, quality_scale)) return restore_and_return(CompositeResult{});
            ++result.blur_pass_count;
            if (!composite_texture_over(foreground_layer_, foreground_mid_)) return restore_and_return(CompositeResult{});
        } else if (!composite_texture_over(layer_source, foreground_mid_)) return restore_and_return(CompositeResult{});
        foreground_has_content = true;
    }

    result.valid = background_has_content || foreground_has_content;
    result.background_mid = background_has_content ? background_mid_ : nullptr;
    result.foreground_mid = foreground_has_content ? foreground_mid_ : nullptr;
    return restore_and_return(result);
}

} // namespace dof_blur_chain
