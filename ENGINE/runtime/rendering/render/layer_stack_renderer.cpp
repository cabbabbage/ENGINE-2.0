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

    for (int layer_index : build.non_empty_layers) {
        if (layer_index < 0 || layer_index >= build.layer_count) {
            continue;
        }
        const render_pipeline::LayerSubmission& layer = build.layers[static_cast<std::size_t>(layer_index)];
        const std::size_t li = static_cast<std::size_t>(layer_index);
        frame_scratch_.layer_depth_min[li] = std::isfinite(layer.depth_min) ? layer.depth_min : layer.representative_depth;
        frame_scratch_.layer_depth_max[li] = std::isfinite(layer.depth_max) ? layer.depth_max : layer.representative_depth;
        frame_scratch_.layer_bounds_min_x[li] = layer.bounds_min_x;
        frame_scratch_.layer_bounds_min_y[li] = layer.bounds_min_y;
        frame_scratch_.layer_bounds_max_x[li] = layer.bounds_max_x;
        frame_scratch_.layer_bounds_max_y[li] = layer.bounds_max_y;
        frame_scratch_.per_layer_light_indices[li].reserve(
            std::max(frame_scratch_.per_layer_light_indices[li].capacity(), runtime_lights.size() / 2));
    }

    for (std::uint32_t light_index = 0; light_index < runtime_lights.size(); ++light_index) {
        const LayerEffectProcessor::RuntimeLight& light = runtime_lights[light_index];
        for (int layer_index : build.non_empty_layers) {
            if (layer_index < 0 || layer_index >= build.layer_count) {
                continue;
            }
            const std::size_t li = static_cast<std::size_t>(layer_index);
            if (render_internal::light_overlaps_layer_slice(light,
                                                            frame_scratch_.layer_depth_min[li],
                                                            frame_scratch_.layer_depth_max[li],
                                                            frame_scratch_.layer_bounds_min_x[li],
                                                            frame_scratch_.layer_bounds_min_y[li],
                                                            frame_scratch_.layer_bounds_max_x[li],
                                                            frame_scratch_.layer_bounds_max_y[li])) {
                frame_scratch_.per_layer_light_indices[li].push_back(light_index);
            }
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
        const double depth_min = frame_scratch_.layer_depth_min[li];
        const double depth_max = frame_scratch_.layer_depth_max[li];
        const double layer_reference_depth = choose_layer_reference_depth(depth_min, depth_max);

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
            const double relative_depth = static_cast<double>(adjusted.world_z) - layer_reference_depth;
            adjusted.intensity = render_internal::apply_layer_light_strength_bias(
                adjusted.intensity,
                relative_depth,
                front_layer_light_strength_multiplier,
                behind_layer_light_strength_multiplier);
            if (adjusted.intensity <= 0.0005f) {
                continue;
            }
            frame_scratch_.layer_light_buffer.push_back(adjusted);
            const double light_depth = static_cast<double>(adjusted.world_z);
            if (light_depth >= depth_min && light_depth <= depth_max) {
                frame_scratch_.owner_light_buffer.push_back(adjusted);
            }
        }
        out.owning_body_lights[li] = frame_scratch_.owner_light_buffer;

        LayerEffectProcessor::LayerScratchTextures scratch{};
        scratch.dark_mask_texture = targets.dark_mask;

        LayerEffectProcessor::LayerProcessResult result = layer_effect_processor_.process_layer(
            targets.base,
            targets.lit,
            depth_min,
            depth_max,
            lighting_params,
            frame_scratch_.layer_light_buffer,
            scratch);



        out.final_layer_textures[static_cast<std::size_t>(layer_index)] =
            result.final_texture ? result.final_texture : targets.lit;
    }

    out.valid = true;
    return out;
}
