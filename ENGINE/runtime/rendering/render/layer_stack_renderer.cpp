#include "rendering/render/layer_stack_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

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

render_pipeline::LayerRenderResult LayerStackRenderer::render(
    const render_pipeline::LayerBuildResult& build,
    const std::vector<LayerEffectProcessor::RuntimeLight>& runtime_lights,
    bool runtime_lighting_enabled,
    float front_layer_light_strength_multiplier,
    float behind_layer_light_strength_multiplier) {
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
    frame_scratch_.clear_for_frame(static_cast<std::size_t>(build.layer_count));
    frame_scratch_.light_metadata.resize(runtime_lights.size());

    for (int layer_index : build.non_empty_layers) {
        if (layer_index < 0 || layer_index >= build.layer_count) {
            continue;
        }
        const render_pipeline::LayerSubmission& layer = build.layers[static_cast<std::size_t>(layer_index)];
        const std::size_t li = static_cast<std::size_t>(layer_index);
        const double depth_min = std::isfinite(layer.depth_min) ? layer.depth_min : layer.representative_depth;
        const double depth_max = std::isfinite(layer.depth_max) ? layer.depth_max : layer.representative_depth;
        frame_scratch_.layer_metadata[li].layer_index = layer_index;
        frame_scratch_.layer_metadata[li].depth_interval =
            render_internal::make_sorted_depth_interval(depth_min, depth_max);
        frame_scratch_.layer_metadata[li].screen_bounds = render_internal::ScreenAabb{
            layer.bounds_min_x,
            layer.bounds_min_y,
            layer.bounds_max_x,
            layer.bounds_max_y};
        frame_scratch_.layer_order_by_depth_start.push_back(li);
    }

    const std::size_t non_empty_layer_count = frame_scratch_.layer_order_by_depth_start.size();
    const std::size_t expected_lights_per_layer =
        (non_empty_layer_count > 0) ? ((runtime_lights.size() / non_empty_layer_count) + 2) : 0;
    for (std::size_t li : frame_scratch_.layer_order_by_depth_start) {
        frame_scratch_.per_layer_light_indices[li].reserve(
            std::max(frame_scratch_.per_layer_light_indices[li].capacity(), expected_lights_per_layer));
    }

    std::sort(frame_scratch_.layer_order_by_depth_start.begin(),
              frame_scratch_.layer_order_by_depth_start.end(),
              [this](std::size_t lhs, std::size_t rhs) {
                  return frame_scratch_.layer_metadata[lhs].depth_interval.min <
                         frame_scratch_.layer_metadata[rhs].depth_interval.min;
              });

    for (std::uint32_t light_index = 0; light_index < runtime_lights.size(); ++light_index) {
        const LayerEffectProcessor::RuntimeLight& light = runtime_lights[light_index];
        const std::size_t light_meta_index = static_cast<std::size_t>(light_index);
        frame_scratch_.light_metadata[light_meta_index].depth_interval = render_internal::light_depth_interval(light);
        frame_scratch_.light_metadata[light_meta_index].screen_bounds = render_internal::ScreenAabb{
            light.screen_center.x - light.radius_px,
            light.screen_center.y - light.radius_px,
            light.screen_center.x + light.radius_px,
            light.screen_center.y + light.radius_px};

        if (!std::isfinite(light.screen_center.x) || !std::isfinite(light.screen_center.y)) {
            continue;
        }

        // ????? ????? ????? ????; ????? ???? ???? ????? ????? ????/?????.
        for (const std::size_t li : frame_scratch_.layer_order_by_depth_start) {
            const FrameScratchArena::LayerMetadata& layer_meta = frame_scratch_.layer_metadata[li];
            const bool bounds_overlap =
                render_internal::screen_aabb_overlaps(frame_scratch_.light_metadata[light_meta_index].screen_bounds,
                                                      layer_meta.screen_bounds);
            const bool center_inside =
                light.screen_center.x >= layer_meta.screen_bounds.min_x &&
                light.screen_center.x <= layer_meta.screen_bounds.max_x &&
                light.screen_center.y >= layer_meta.screen_bounds.min_y &&
                light.screen_center.y <= layer_meta.screen_bounds.max_y;
            if (!bounds_overlap && !center_inside) {
                continue;
            }
            frame_scratch_.per_layer_light_indices[li].push_back(light_index);
        }
    }

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

        const std::size_t li = static_cast<std::size_t>(layer_index);
        const render_internal::DepthInterval& layer_depth = frame_scratch_.layer_metadata[li].depth_interval;

        frame_scratch_.layer_light_buffer.clear();
        frame_scratch_.owner_light_buffer.clear();
        const std::vector<std::uint32_t>& indices = frame_scratch_.per_layer_light_indices[li];
        frame_scratch_.layer_light_buffer.reserve(
            std::max(frame_scratch_.layer_light_buffer.capacity(), indices.size()));
        frame_scratch_.owner_light_buffer.reserve(
            std::max(frame_scratch_.owner_light_buffer.capacity(), indices.size()));
        for (const std::uint32_t light_index : indices) {
            if (light_index >= runtime_lights.size()) {
                continue;
            }
            LayerEffectProcessor::RuntimeLight adjusted = runtime_lights[light_index];
            const int signed_separation = render_internal::compare_depth_intervals_signed(
                frame_scratch_.light_metadata[static_cast<std::size_t>(light_index)].depth_interval,
                layer_depth);
            adjusted.intensity = render_internal::apply_layer_light_strength_bias(
                adjusted.intensity,
                signed_separation,
                front_layer_light_strength_multiplier,
                behind_layer_light_strength_multiplier);
            if (adjusted.intensity <= 0.0005f) {
                continue;
            }
            frame_scratch_.layer_light_buffer.push_back(adjusted);
            if (signed_separation == 0) {
                frame_scratch_.owner_light_buffer.push_back(adjusted);
            }
        }
        out.owning_body_lights[li] = frame_scratch_.owner_light_buffer;

        LayerEffectProcessor::LayerScratchTextures scratch{};
        scratch.dark_mask_texture = targets.dark_mask;

        LayerEffectProcessor::LayerProcessResult result = layer_effect_processor_.process_layer(
            targets.base,
            targets.lit,
            layer_depth.min,
            layer_depth.max,
            lighting_params,
            frame_scratch_.layer_light_buffer,
            scratch);

        out.final_layer_textures[static_cast<std::size_t>(layer_index)] =
            result.final_texture ? result.final_texture : targets.lit;
    }

    out.valid = true;
    return out;
}

