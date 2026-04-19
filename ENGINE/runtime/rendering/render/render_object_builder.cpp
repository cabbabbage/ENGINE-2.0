#include "rendering/render/render_object_builder.hpp"

#include <algorithm>
#include <cmath>

#include "assets/asset/Asset.hpp"
#include "assets/asset/animation_frame.hpp"
#include "assets/asset/animation_frame_variant.hpp"
#include "rendering/render/render_object.hpp"

namespace render_build {
namespace {

constexpr SDL_FPoint kDefaultProjectionAnchorUv{0.5f, 1.0f};

bool query_texture_size_direct(SDL_Texture* texture, int* out_w, int* out_h) {
    if (!texture || !out_w || !out_h) {
        return false;
    }

    float wf = 0.0f;
    float hf = 0.0f;
    if (!SDL_GetTextureSize(texture, &wf, &hf)) {
        return false;
    }

    *out_w = std::max(1, static_cast<int>(std::lround(wf)));
    *out_h = std::max(1, static_cast<int>(std::lround(hf)));
    return true;
}

} // namespace

bool build_direct_asset_render_object(Asset* asset, RenderObject& out_object) {
    out_object = RenderObject{};
    if (!asset) {
        return false;
    }

    SDL_Texture* base_tex = nullptr;
    const FrameVariant* selected_variant = nullptr;

    if (asset->info) {
        auto anim_it = asset->info->animations.find(asset->current_animation);
        if (anim_it != asset->info->animations.end() && asset->current_frame) {
            const auto& variants = asset->current_frame->variants;
            if (!variants.empty()) {
                int variant_idx = std::clamp(asset->current_variant_index, 0, static_cast<int>(variants.size()) - 1);
                const FrameVariant& variant = variants[static_cast<std::size_t>(variant_idx)];
                selected_variant = &variant;
                base_tex = variant.get_base_texture();
            }
        }
    }

    if (!base_tex) {
        base_tex = asset->get_current_frame();
    }
    if (!base_tex) {
        return false;
    }

    int texture_w = 0;
    int texture_h = 0;
    if (!query_texture_size_direct(base_tex, &texture_w, &texture_h)) {
        return false;
    }

    SDL_Rect src_rect{0, 0, texture_w, texture_h};
    bool has_src_rect = false;
    int frame_w = texture_w;
    int frame_h = texture_h;
    if (selected_variant) {
        if (selected_variant->source_rect.w > 0 && selected_variant->source_rect.h > 0) {
            frame_w = selected_variant->source_rect.w;
            frame_h = selected_variant->source_rect.h;
            src_rect = selected_variant->source_rect;
        }
        has_src_rect = selected_variant->uses_atlas;
    }

    float remainder = asset->current_remaining_scale_adjustment;
    if (!std::isfinite(remainder) || remainder <= 0.0f) {
        remainder = 1.0f;
    }

    int final_w = static_cast<int>(std::lround(static_cast<float>(frame_w) * remainder));
    int final_h = static_cast<int>(std::lround(static_cast<float>(frame_h) * remainder));
    final_w = std::max(1, final_w);
    final_h = std::max(1, final_h);

    const float world_anchor_x = asset->smoothed_translation_x() + asset->render_anchor_offset_x();
    const float world_anchor_y = asset->smoothed_translation_y() + asset->render_anchor_offset_y();
    const float world_anchor_z_offset = asset->world_z_offset() + asset->render_anchor_offset_z();
    const SDL_FlipMode base_flip = asset->effective_render_flip();
    const double base_angle = asset->effective_render_angle();
    const Uint8 asset_alpha = static_cast<Uint8>(std::lround(
        std::clamp(asset->smoothed_alpha(), 0.0f, 1.0f) * 255.0f));

    out_object.texture = base_tex;
    out_object.screen_rect = SDL_Rect{
        static_cast<int>(std::lround(world_anchor_x)),
        static_cast<int>(std::lround(world_anchor_y)),
        final_w,
        final_h
    };
    out_object.world_anchor_x = world_anchor_x;
    out_object.world_anchor_y = world_anchor_y;
    out_object.color_mod = SDL_Color{255, 255, 255, asset_alpha};
    out_object.blend_mode = SDL_BLENDMODE_BLEND;
    out_object.angle = base_angle;
    out_object.center = SDL_Point{0, 0};
    out_object.use_custom_center = false;
    out_object.flip = base_flip;
    out_object.texture_w = frame_w;
    out_object.texture_h = frame_h;
    out_object.has_texture_size = (frame_w > 0 && frame_h > 0);
    out_object.atlas_w = texture_w;
    out_object.atlas_h = texture_h;
    out_object.has_atlas_size = (texture_w > 0 && texture_h > 0);
    out_object.dimension_cache_texture = base_tex;
    out_object.world_z_offset = world_anchor_z_offset;
    out_object.projection_anchor_uv = kDefaultProjectionAnchorUv;
    if (has_src_rect) {
        out_object.has_src_rect = true;
        out_object.src_rect = src_rect;
    }

    return true;
}

} // namespace render_build
