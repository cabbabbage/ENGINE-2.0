#include "rendering/render/layer_stack_renderer.hpp"

#include <algorithm>
#include <cmath>

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
        destroy_texture(set.lit);
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
        if (!ensure_target(set.base) || !ensure_target(set.lit)) {
            return false;
        }
    }

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

render_pipeline::LayerRenderResult LayerStackRenderer::render(const render_pipeline::LayerBuildResult& build) {
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

    for (int layer_index : build.non_empty_layers) {
        if (layer_index < 0 || layer_index >= build.layer_count) {
            continue;
        }

        const render_pipeline::LayerSubmission& layer = build.layers[static_cast<std::size_t>(layer_index)];
        TextureSet& targets = layer_targets_[static_cast<std::size_t>(layer_index)];
        render_layer_base(layer, targets.base);

        const LayerEffectProcessor::LayerProcessResult result =
            layer_effect_processor_.process_layer(targets.base, targets.lit);
        out.final_layer_textures[static_cast<std::size_t>(layer_index)] =
            result.final_texture ? result.final_texture : targets.lit;
    }

    out.valid = true;
    return out;
}
