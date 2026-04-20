#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "rendering/render/layer_effect_processor.hpp"
#include "rendering/render/render_pipeline_types.hpp"

class LayerStackRenderer {
public:
    explicit LayerStackRenderer(SDL_Renderer* renderer);
    ~LayerStackRenderer();

    LayerStackRenderer(const LayerStackRenderer&) = delete;
    LayerStackRenderer& operator=(const LayerStackRenderer&) = delete;

    void set_output_dimensions(int screen_width, int screen_height);

    render_pipeline::LayerRenderResult render(
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
        float dark_mask_temporal_prev_weight);

private:


    struct TextureSet {
        SDL_Texture* base = nullptr;
        SDL_Texture* dark_mask = nullptr;
        SDL_Texture* dark_mask_merged = nullptr;
        SDL_Texture* lit = nullptr;
        std::uint8_t dark_mask_history_write_index = 0;
        std::uint8_t valid_dark_mask_history_count = 0;
    };
    struct LayerLightMembershipState {
        bool active = false;
        std::uint8_t hold_frames_remaining = 0;
        std::uint64_t last_seen_frame = 0;
    };

    bool ensure_target(SDL_Texture*& texture) const;
    bool ensure_layer_capacity(int layer_count);
    void reset_targets();
    void clear_target(SDL_Texture* texture) const;
    bool copy_texture(SDL_Texture* src, SDL_Texture* dst) const;
  

    void render_layer_base(const render_pipeline::LayerSubmission& layer,
                           SDL_Texture* target) const;

    std::vector<LayerEffectProcessor::RuntimeLight> bias_lights_for_layer(
        const std::vector<LayerEffectProcessor::RuntimeLight>& source_lights,
        double layer_reference_depth,
        float front_layer_light_strength_multiplier,
        float behind_layer_light_strength_multiplier,
        float depth_transition_world,
        std::uint32_t* depth_blended_count) const;

    std::vector<LayerEffectProcessor::RuntimeLight> collect_layer_lights(
        int layer_index,
        const render_pipeline::LayerSubmission& layer,
        const std::vector<LayerEffectProcessor::RuntimeLight>& runtime_lights,
        float overlap_padding_px,
        float overlap_depth_padding_world,
        int overlap_hold_frames,
        std::uint32_t* strict_count,
        std::uint32_t* hysteresis_count);

    std::vector<LayerEffectProcessor::RuntimeLight> collect_owner_lights(
        const render_pipeline::LayerSubmission& layer,
        const std::vector<LayerEffectProcessor::RuntimeLight>& biased_lights) const;

    static std::uint64_t membership_key(std::uint64_t light_id, int layer_index);
    void prune_membership_cache(std::uint64_t frame_token);

    SDL_Renderer* renderer_ = nullptr;
    int screen_width_ = 0;
    int screen_height_ = 0;
    LayerEffectProcessor layer_effect_processor_;
    std::vector<TextureSet> layer_targets_;
    std::unordered_map<std::uint64_t, LayerLightMembershipState> layer_light_membership_cache_;
    std::uint64_t layer_light_membership_frame_token_ = 0;
};
