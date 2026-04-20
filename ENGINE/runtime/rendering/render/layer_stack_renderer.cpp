#include "rendering/render/layer_stack_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "rendering/render/render.hpp"

namespace {

void destroy_texture(SDL_Texture*& texture) {
    if (texture) {
        SDL_DestroyTexture(texture);
        texture = nullptr;
    }
}

SDL_BlendMode safe_layer_blend_mode(SDL_BlendMode blend_mode) {
    if (blend_mode == SDL_BLENDMODE_MOD || blend_mode == SDL_BLENDMODE_MUL) {
        return SDL_BLENDMODE_BLEND;
    }
    return blend_mode;
}

double choose_layer_reference_depth(double layer_depth_min, double layer_depth_max) {
    const bool finite_min = std::isfinite(layer_depth_min);
    const bool finite_max = std::isfinite(layer_depth_max);
    if (finite_min && finite_max) {
        return 0.5 * (layer_depth_min + layer_depth_max);
    }
    if (finite_min) {
        return layer_depth_min;
    }
    if (finite_max) {
        return layer_depth_max;
    }
    return 0.0;
}

} // namespace

LayerStackRenderer::LayerStackRenderer(SDL_Renderer* renderer)
    : renderer_(renderer),
      layer_effect_processor_(renderer) {}

LayerStackRenderer::~LayerStackRenderer() {
    reset_targets();
}

void LayerStackRenderer::set_output_dimensions(int screen_width, int screen_height) {
    const int safe_w = std::max(1, screen_width);
    const int safe_h = std::max(1, screen_height);
    if (screen_width_ == safe_w && screen_height_ == safe_h) {
        return;
    }
    screen_width_ = safe_w;
    screen_height_ = safe_h;
    reset_targets();
}

void LayerStackRenderer::reset_targets() {
    for (TextureSet& set : layer_targets_) {
        destroy_texture(set.base);
        destroy_texture(set.dark_mask);
        destroy_texture(set.dark_mask_merged);
        destroy_texture(set.lit);
        set.dark_mask_history_write_index = 0;
        set.valid_dark_mask_history_count = 0;
    }
    layer_targets_.clear();
    layer_light_membership_cache_.clear();
    layer_light_membership_frame_token_ = 0;
}

bool LayerStackRenderer::ensure_target(SDL_Texture*& texture) const {
    if (!renderer_ || screen_width_ <= 0 || screen_height_ <= 0) {
        return false;
    }

    int tex_w = 0;
    int tex_h = 0;
    if (texture) {
        float w = 0.0f;
        float h = 0.0f;
        if (SDL_GetTextureSize(texture, &w, &h)) {
            tex_w = static_cast<int>(std::lround(w));
            tex_h = static_cast<int>(std::lround(h));
        }
        if (tex_w != screen_width_ || tex_h != screen_height_) {
            destroy_texture(texture);
        }
    }

    if (!texture) {
        texture = SDL_CreateTexture(renderer_,
                                    SDL_PIXELFORMAT_RGBA8888,
                                    SDL_TEXTUREACCESS_TARGET,
                                    screen_width_,
                                    screen_height_);
        if (!texture) {
            return false;
        }
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    }

    return true;
}

bool LayerStackRenderer::ensure_layer_capacity(int layer_count) {
    if (layer_count < 0) {
        return false;
    }

    if (static_cast<int>(layer_targets_.size()) != layer_count) {
        reset_targets();
        layer_targets_.resize(static_cast<std::size_t>(layer_count));
    }

    for (TextureSet& set : layer_targets_) {
        if (!ensure_target(set.base) ||
            !ensure_target(set.dark_mask) ||
            !ensure_target(set.dark_mask_merged) ||
            !ensure_target(set.lit)) {
            return false;
        }
    }

    return true;
}

bool LayerStackRenderer::copy_texture(SDL_Texture* src, SDL_Texture* dst) const {
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



void LayerStackRenderer::clear_target(SDL_Texture* texture) const {
    if (!renderer_ || !texture) {
        return;
    }
    SDL_SetRenderTarget(renderer_, texture);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);
}

void LayerStackRenderer::render_layer_base(const render_pipeline::LayerSubmission& layer,
                                           SDL_Texture* target) const {
    if (!renderer_ || !target) {
        return;
    }

    clear_target(target);
    SDL_SetRenderTarget(renderer_, target);
    static constexpr int kQuadIndices[6] = {0, 1, 2, 0, 2, 3};

    for (const render_pipeline::GeometryLayerDrawItem& draw : layer.draws) {
        if (!draw.texture) {
            continue;
        }
        SDL_SetTextureBlendMode(draw.texture, safe_layer_blend_mode(draw.blend_mode));
        SDL_RenderGeometry(renderer_, draw.texture, draw.vertices.data(), 4, kQuadIndices, 6);
    }
}

std::vector<LayerEffectProcessor::RuntimeLight> LayerStackRenderer::bias_lights_for_layer(
    const std::vector<LayerEffectProcessor::RuntimeLight>& source_lights,
    double layer_reference_depth,
    float front_layer_light_strength_multiplier,
    float behind_layer_light_strength_multiplier,
    float depth_transition_world,
    std::uint32_t* depth_blended_count) const {
    std::vector<LayerEffectProcessor::RuntimeLight> result;
    result.reserve(source_lights.size());

    for (const LayerEffectProcessor::RuntimeLight& light : source_lights) {
        LayerEffectProcessor::RuntimeLight adjusted = light;
        const double relative_depth = static_cast<double>(light.world_z) - layer_reference_depth;
        adjusted.intensity = render_internal::apply_layer_light_strength_bias(
            adjusted.intensity,
            relative_depth,
            front_layer_light_strength_multiplier,
            behind_layer_light_strength_multiplier,
            depth_transition_world);
        adjusted.depth_blended = depth_transition_world > 1.0e-4f &&
                                 std::isfinite(relative_depth) &&
                                 std::abs(relative_depth) < static_cast<double>(depth_transition_world);
        if (adjusted.depth_blended && depth_blended_count) {
            ++(*depth_blended_count);
        }
        if (adjusted.intensity > 0.0005f) {
            result.push_back(adjusted);
        }
    }

    return result;
}

std::vector<LayerEffectProcessor::RuntimeLight> LayerStackRenderer::collect_layer_lights(
    int layer_index,
    const render_pipeline::LayerSubmission& layer,
    const std::vector<LayerEffectProcessor::RuntimeLight>& runtime_lights,
    float overlap_padding_px,
    float overlap_depth_padding_world,
    int overlap_hold_frames,
    std::uint32_t* strict_count,
    std::uint32_t* hysteresis_count) {
    std::vector<LayerEffectProcessor::RuntimeLight> result;
    result.reserve(runtime_lights.size());

    const double depth_min = std::isfinite(layer.slice_depth_min) ? layer.slice_depth_min : layer.slice_reference_depth;
    const double depth_max = std::isfinite(layer.slice_depth_max) ? layer.slice_depth_max : layer.slice_reference_depth;
    const float safe_padding_px = std::max(0.0f, overlap_padding_px);
    const float safe_depth_padding = std::max(0.0f, overlap_depth_padding_world);
    const int safe_hold_frames = std::max(0, overlap_hold_frames);

    for (const LayerEffectProcessor::RuntimeLight& light : runtime_lights) {
        const std::uint64_t key = membership_key(light.stable_light_id, layer_index);
        auto state_it = layer_light_membership_cache_.find(key);

        const bool strict_overlap = render_internal::light_overlaps_layer_slice(light,
                                                                                depth_min,
                                                                                depth_max,
                                                                                layer.bounds_min_x,
                                                                                layer.bounds_min_y,
                                                                                layer.bounds_max_x,
                                                                                layer.bounds_max_y,
                                                                                safe_padding_px,
                                                                                safe_depth_padding);
        const bool exit_overlap = render_internal::light_overlaps_layer_slice(light,
                                                                              depth_min,
                                                                              depth_max,
                                                                              layer.bounds_min_x,
                                                                              layer.bounds_min_y,
                                                                              layer.bounds_max_x,
                                                                              layer.bounds_max_y,
                                                                              safe_padding_px + 12.0f,
                                                                              safe_depth_padding + 10.0f);
        LayerEffectProcessor::RuntimeLight adjusted = light;
        if (strict_overlap) {
            LayerLightMembershipState& state = layer_light_membership_cache_[key];
            state.active = true;
            state.hold_frames_remaining = static_cast<std::uint8_t>(std::min(safe_hold_frames, 255));
            state.last_seen_frame = layer_light_membership_frame_token_;
            adjusted.retained_by_hysteresis = false;
            if (strict_count) {
                ++(*strict_count);
            }
            result.push_back(adjusted);
            continue;
        }

        if (state_it != layer_light_membership_cache_.end() && state_it->second.active && exit_overlap) {
            LayerLightMembershipState& state = state_it->second;
            state.hold_frames_remaining = static_cast<std::uint8_t>(std::min(safe_hold_frames, 255));
            state.last_seen_frame = layer_light_membership_frame_token_;
            adjusted.retained_by_hysteresis = true;
            if (hysteresis_count) {
                ++(*hysteresis_count);
            }
            result.push_back(adjusted);
            continue;
        }

        if (state_it != layer_light_membership_cache_.end() &&
            state_it->second.active &&
            state_it->second.hold_frames_remaining > 0) {
            LayerLightMembershipState& state = state_it->second;
            --state.hold_frames_remaining;
            state.last_seen_frame = layer_light_membership_frame_token_;
            adjusted.retained_by_hysteresis = true;
            if (hysteresis_count) {
                ++(*hysteresis_count);
            }
            result.push_back(adjusted);
            continue;
        }

        if (state_it != layer_light_membership_cache_.end()) {
            layer_light_membership_cache_.erase(state_it);
        }
    }

    return result;
}

std::vector<LayerEffectProcessor::RuntimeLight> LayerStackRenderer::collect_owner_lights(
    const render_pipeline::LayerSubmission& layer,
    const std::vector<LayerEffectProcessor::RuntimeLight>& biased_lights) const {
    std::vector<LayerEffectProcessor::RuntimeLight> result;
    result.reserve(biased_lights.size());

    const double depth_min = std::isfinite(layer.slice_depth_min) ? layer.slice_depth_min : layer.slice_reference_depth;
    const double depth_max = std::isfinite(layer.slice_depth_max) ? layer.slice_depth_max : layer.slice_reference_depth;

    for (const LayerEffectProcessor::RuntimeLight& light : biased_lights) {
        const double light_depth = static_cast<double>(light.world_z);
        if (light_depth >= depth_min && light_depth <= depth_max) {
            result.push_back(light);
        }
    }

    return result;
}

std::uint64_t LayerStackRenderer::membership_key(std::uint64_t light_id, int layer_index) {
    const std::uint64_t layer_part = static_cast<std::uint64_t>(static_cast<std::uint32_t>(std::max(0, layer_index)));
    return (light_id << 1u) ^ (layer_part * 0x9E3779B185EBCA87ULL);
}

void LayerStackRenderer::prune_membership_cache(std::uint64_t frame_token) {
    for (auto it = layer_light_membership_cache_.begin(); it != layer_light_membership_cache_.end();) {
        const std::uint64_t age = frame_token - it->second.last_seen_frame;
        if (age > 240) {
            it = layer_light_membership_cache_.erase(it);
        } else {
            ++it;
        }
    }
}

render_pipeline::LayerRenderResult LayerStackRenderer::render(
    const render_pipeline::LayerBuildResult& build,
    const std::vector<LayerEffectProcessor::RuntimeLight>& runtime_lights,
    bool runtime_lighting_enabled,
    float front_layer_light_strength_multiplier,
    float behind_layer_light_strength_multiplier,
    float overlap_padding_px,
    float overlap_depth_padding_world,
    int overlap_hold_frames,
    float depth_transition_world,
    bool dark_mask_temporal_enabled,
    float dark_mask_temporal_prev_weight) {
    render_pipeline::LayerRenderResult out{};
    out.layer_count = build.layer_count;
    out.player_layer_index = build.player_layer_index;
    out.non_empty_layers = build.non_empty_layers;

    if (!renderer_ ||
        !build.valid ||
        build.layer_count <= 0 ||
        static_cast<int>(build.layers.size()) != build.layer_count ||
        !ensure_layer_capacity(build.layer_count)) {
        return out;
    }

    out.final_layer_textures.assign(static_cast<std::size_t>(build.layer_count), nullptr);
    out.owning_body_lights.resize(static_cast<std::size_t>(build.layer_count));
    ++layer_light_membership_frame_token_;
    prune_membership_cache(layer_light_membership_frame_token_);

    LayerEffectProcessor::LayerLightingParams lighting_params{};
    lighting_params.enabled = runtime_lighting_enabled;
    lighting_params.ambient_color = SDL_Color{18, 20, 24, 255};

    for (int layer_index : build.non_empty_layers) {
        if (layer_index < 0 || layer_index >= build.layer_count) {
            continue;
        }

        const render_pipeline::LayerSubmission& layer = build.layers[static_cast<std::size_t>(layer_index)];
        TextureSet& targets = layer_targets_[static_cast<std::size_t>(layer_index)];

        render_layer_base(layer, targets.base);

        const double depth_min = std::isfinite(layer.slice_depth_min) ? layer.slice_depth_min : layer.slice_reference_depth;
        const double depth_max = std::isfinite(layer.slice_depth_max) ? layer.slice_depth_max : layer.slice_reference_depth;
        const double depth_reference = std::isfinite(layer.slice_reference_depth)
            ? layer.slice_reference_depth
            : choose_layer_reference_depth(depth_min, depth_max);

        const std::vector<LayerEffectProcessor::RuntimeLight> overlapping_lights =
            collect_layer_lights(layer_index,
                                 layer,
                                 runtime_lights,
                                 overlap_padding_px,
                                 overlap_depth_padding_world,
                                 overlap_hold_frames,
                                 &out.strict_overlap_count,
                                 &out.hysteresis_overlap_count);
        const std::vector<LayerEffectProcessor::RuntimeLight> biased_lights =
            bias_lights_for_layer(overlapping_lights,
                                  depth_reference,
                                  front_layer_light_strength_multiplier,
                                  behind_layer_light_strength_multiplier,
                                  depth_transition_world,
                                  &out.depth_blended_light_count);
        out.owning_body_lights[static_cast<std::size_t>(layer_index)] =
            collect_owner_lights(layer, biased_lights);

        LayerEffectProcessor::LayerScratchTextures scratch{};
        scratch.dark_mask_texture = targets.dark_mask;
        scratch.dark_mask_history_texture = targets.dark_mask_merged;
        const bool is_player_layer = (layer_index == build.player_layer_index);
        scratch.dark_mask_temporal_enabled = dark_mask_temporal_enabled && !is_player_layer;
        scratch.dark_mask_temporal_prev_weight = dark_mask_temporal_prev_weight;

        LayerEffectProcessor::LayerProcessResult result = layer_effect_processor_.process_layer(
            targets.base,
            targets.lit,
            depth_min,
            depth_max,
            lighting_params,
            biased_lights,
            scratch);

        if (result.lighting_applied && scratch.dark_mask_temporal_enabled) {
            ++out.temporal_merge_count;
        }


        out.final_layer_textures[static_cast<std::size_t>(layer_index)] =
            result.final_texture ? result.final_texture : targets.lit;
    }

    out.valid = true;
    return out;
}
