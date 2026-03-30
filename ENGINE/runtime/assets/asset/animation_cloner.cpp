#include "animation_cloner.hpp"
#include "utils/sdl_render_conversions.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "asset_info.hpp"
#include "rendering/render/scaling_logic.hpp"

namespace {

void apply_scale_mode(SDL_Texture* tex, const AssetInfo& info) {
    if (!tex) return;
#if SDL_VERSION_ATLEAST(2, 0, 12)
    SDL_SetTextureScaleMode(tex, info.smooth_scaling ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
#endif
}

SDL_Texture* clone_texture(SDL_Texture* src,
                           int width_hint,
                           int height_hint,
                           SDL_FlipMode flip_flags,
                           SDL_Renderer* renderer,
                           const AssetInfo& info,
                           int* out_w = nullptr,
                           int* out_h = nullptr) {
    if (!src || !renderer) {
        return nullptr;
    }

    SDL_PixelFormat fmt = SDL_PIXELFORMAT_RGBA8888;
    int access = 0;
    int tex_w = width_hint;
    int tex_h = height_hint;

    const bool need_dims = tex_w <= 0 || tex_h <= 0;
    if (SDL_PropertiesID props = SDL_GetTextureProperties(src)) {
        fmt    = static_cast<SDL_PixelFormat>(SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_FORMAT_NUMBER, fmt));
        access = static_cast<int>(SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_ACCESS_NUMBER, access));
    }
    if (need_dims) {
        float fw = 0.0f;
        float fh = 0.0f;
        if (SDL_GetTextureSize(src, &fw, &fh)) {
            tex_w = static_cast<int>(std::lround(fw));
            tex_h = static_cast<int>(std::lround(fh));
        }
    }
    tex_w = std::max(1, tex_w);
    tex_h = std::max(1, tex_h);

    SDL_Texture* dst = SDL_CreateTexture(renderer, fmt, SDL_TEXTUREACCESS_TARGET, tex_w, tex_h);
    if (!dst) {
        return nullptr;
    }

    SDL_SetTextureBlendMode(dst, SDL_BLENDMODE_BLEND);
    apply_scale_mode(dst, info);

    SDL_Texture* prev_target = SDL_GetRenderTarget(renderer);
    SDL_SetRenderTarget(renderer, dst);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);

    SDL_Rect rect{ 0, 0, tex_w, tex_h };
    if (flip_flags != SDL_FLIP_NONE) {
        sdl_render::TextureRotated(renderer, src, nullptr, &rect, 0.0, nullptr, flip_flags);
    } else {
        sdl_render::Texture(renderer, src, nullptr, &rect);
    }

    SDL_SetRenderTarget(renderer, prev_target);
    if (out_w) *out_w = tex_w;
    if (out_h) *out_h = tex_h;
    return dst;
}

template <typename TBox>
TBox transform_box_corners(TBox box,
                           bool flip_horizontal,
                           bool flip_vertical,
                           int frame_w,
                           int frame_h) {
    animation_update::FrameBoxRect flipped = box.rect;
    if (flip_horizontal && frame_w > 0) {
        const int next_left = frame_w - 1 - box.rect.right;
        const int next_right = frame_w - 1 - box.rect.left;
        flipped.left = next_left;
        flipped.right = next_right;
    }
    if (flip_vertical && frame_h > 0) {
        const int next_top = frame_h - 1 - box.rect.bottom;
        const int next_bottom = frame_h - 1 - box.rect.top;
        flipped.top = next_top;
        flipped.bottom = next_bottom;
    }
    box.set_rect(flipped);
    return box;
}

}

bool AnimationCloner::Clone(const Animation& source,
                            Animation&       dest,
                            const Options&   opts,
                            SDL_Renderer*    renderer,
                            AssetInfo&       info) {
    if (!renderer || source.frame_cache_.empty()) {
        return false;
    }

    dest.clear_texture_cache();

    dest.variant_steps_   = source.variant_steps_;
    render_pipeline::ScalingLogic::NormalizeVariantSteps(dest.variant_steps_);
    dest.locked           = source.locked;
    if (opts.inherit_on_end_from_source) {
        dest.on_end_animation = source.on_end_animation;
        dest.on_end_behavior = source.on_end_behavior;
    }
    dest.randomize        = source.randomize;
    dest.rnd_start        = source.rnd_start;
    dest.frozen           = source.frozen;
    dest.movment          = source.movment;
    dest.total_dx         = source.total_dx;
    dest.total_dy         = source.total_dy;
    dest.total_dz         = source.total_dz;
    dest.audio_clip       = source.audio_clip;

    const std::size_t frame_count   = source.frame_cache_.size();
    const std::size_t variant_count = dest.variant_steps_.size();
    if (frame_count == 0 || variant_count == 0) {
        return false;
    }

    auto src_index_for = [&](std::size_t dst_idx) -> std::size_t {
        return opts.reverse_frames ? (frame_count - 1 - dst_idx) : dst_idx;
};

    SDL_FlipMode flip_flags = SDL_FLIP_NONE;
    if (opts.flip_horizontal) flip_flags = static_cast<SDL_FlipMode>(flip_flags | SDL_FLIP_HORIZONTAL);
    if (opts.flip_vertical)   flip_flags = static_cast<SDL_FlipMode>(flip_flags | SDL_FLIP_VERTICAL);

    dest.frame_cache_.reserve(frame_count);
    for (std::size_t dst_idx = 0; dst_idx < frame_count; ++dst_idx) {
        const std::size_t src_idx = src_index_for(dst_idx);
        if (src_idx >= source.frame_cache_.size()) {
            continue;
        }
        const Animation::FrameCache& src_cache = source.frame_cache_[src_idx];
        Animation::FrameCache dst_cache;
        dst_cache.resize(variant_count);

        for (std::size_t v = 0; v < variant_count; ++v) {
            SDL_Texture* src_tex = (v < src_cache.textures.size()) ? src_cache.textures[v] : nullptr;
            int base_tex_w = (v < src_cache.widths.size()) ? src_cache.widths[v] : 0;
            int base_tex_h = (v < src_cache.heights.size()) ? src_cache.heights[v] : 0;
            dst_cache.textures[v] = clone_texture(src_tex,
                                                  base_tex_w,
                                                  base_tex_h,
                                                  flip_flags,
                                                  renderer,
                                                  info,
                                                  &base_tex_w,
                                                  &base_tex_h);
            dst_cache.widths[v]   = base_tex_w;
            dst_cache.heights[v]  = base_tex_h;

            SDL_Texture* src_fg = (v < src_cache.foreground_textures.size()) ? src_cache.foreground_textures[v] : nullptr;
            if (src_fg) {
                dst_cache.foreground_textures[v] = clone_texture(src_fg,
                                                                 0,
                                                                 0,
                                                                 flip_flags,
                                                                 renderer,
                                                                 info);
            }
            SDL_Texture* src_bg = (v < src_cache.background_textures.size()) ? src_cache.background_textures[v] : nullptr;
            if (src_bg) {
                dst_cache.background_textures[v] = clone_texture(src_bg,
                                                                 0,
                                                                 0,
                                                                 flip_flags,
                                                                 renderer,
                                                                 info);
            }
        }

        dest.frame_cache_.push_back(std::move(dst_cache));
    }

    dest.number_of_frames = static_cast<int>(frame_count);

    dest.synchronize_runtime_frames();

    return !dest.frame_cache_.empty();
}



