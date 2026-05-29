#include "animation_cloner.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/cache_manager.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "asset_info.hpp"

namespace {

void apply_scale_mode(SDL_Texture* tex, const AssetInfo& info) {
    if (!tex) return;
#if SDL_VERSION_ATLEAST(2, 0, 12)
    SDL_SetTextureScaleMode(tex, info.smooth_scaling ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
#endif
}

bool renderer_has_gpu_runtime_bridge(SDL_Renderer* renderer) {
    if (!renderer) {
        return false;
    }
    SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
    if (!props) {
        return false;
    }
    return SDL_GetPointerProperty(props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr) != nullptr;
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

    const bool flip_horizontal = (static_cast<int>(flip_flags) & static_cast<int>(SDL_FLIP_HORIZONTAL)) != 0;
    const bool flip_vertical = (static_cast<int>(flip_flags) & static_cast<int>(SDL_FLIP_VERTICAL)) != 0;
    if (const CacheManager::PreparedGpuTextureUpload* prepared = CacheManager::prepared_gpu_upload_for_texture(src)) {
        SDL_Texture* dst = CacheManager::create_texture_from_prepared_upload(renderer,
                                                                              *prepared,
                                                                              flip_horizontal,
                                                                              flip_vertical);
        if (!dst) {
            vibble::log::error("[AnimationCloner] Failed to clone prepared GPU texture upload payload.");
            return nullptr;
        }
        SDL_SetTextureBlendMode(dst, SDL_BLENDMODE_BLEND);
        apply_scale_mode(dst, info);
        if (out_w) *out_w = prepared->width;
        if (out_h) *out_h = prepared->height;
        return dst;
    }

    if (renderer_has_gpu_runtime_bridge(renderer)) {
        vibble::log::error("[AnimationCloner] Source texture has no prepared GPU upload payload; refusing render-target clone on GPU runtime renderer.");
        return nullptr;
    }

    SDL_PixelFormat fmt = SDL_PIXELFORMAT_RGBA32;
    int tex_w = width_hint;
    int tex_h = height_hint;

    const bool need_dims = tex_w <= 0 || tex_h <= 0;
    if (SDL_PropertiesID props = SDL_GetTextureProperties(src)) {
        fmt    = static_cast<SDL_PixelFormat>(SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_FORMAT_NUMBER, fmt));
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

void apply_movement_transforms(std::vector<std::vector<AnimationFrame>>& movement_paths,
                               bool                                      reverse_frames,
                               bool                                      invert_x,
                               bool                                      invert_y,
                               bool                                      invert_z) {
    if (reverse_frames) {
        for (auto& path : movement_paths) {
            std::reverse(path.begin(), path.end());
        }
    }
    for (auto& path : movement_paths) {
        for (auto& frame : path) {
            if (invert_x) frame.dx = -frame.dx;
            if (invert_y) frame.dy = -frame.dy;
            if (invert_z) frame.dz = -frame.dz;
        }
    }
}

void recompute_movement_totals(Animation& animation) {
    animation.total_dx = 0;
    animation.total_dy = 0;
    animation.total_dz = 0;
    animation.total_dr = 0.0f;
    animation.movment = false;

    if (animation.movement_path_count() == 0) {
        return;
    }

    const auto& primary_path = animation.movement_path(0);
    for (const auto& frame : primary_path) {
        animation.total_dx += frame.dx;
        animation.total_dy += frame.dy;
        animation.total_dz += frame.dz;
        animation.total_dr += frame.rotation_degrees;
        if (frame.dx != 0 || frame.dy != 0 || frame.dz != 0) {
            animation.movment = true;
        }
    }
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

    dest.locked           = source.locked;
    if (opts.inherit_on_end_from_source) {
        dest.on_end_animation = source.on_end_animation;
        dest.on_end_behavior = source.on_end_behavior;
    }
    dest.randomize        = source.randomize;
    dest.rnd_start        = source.rnd_start;
    dest.frozen           = source.frozen;
    dest.audio_clip       = source.audio_clip;

    const std::size_t frame_count = source.frame_cache_.size();
    if (frame_count == 0) {
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
        SDL_Texture* src_tex = src_cache.texture;
        int base_tex_w = src_cache.width;
        int base_tex_h = src_cache.height;
        SDL_Texture* cloned_tex = clone_texture(src_tex,
                                                base_tex_w,
                                                base_tex_h,
                                                flip_flags,
                                                renderer,
                                                info,
                                                &base_tex_w,
                                                &base_tex_h);
        if (src_tex && !cloned_tex) {
            dest.clear_texture_cache();
            vibble::log::error("[AnimationCloner] Failed to clone animation frame texture during loading.");
            return false;
        }
        dst_cache.texture = cloned_tex;
        dst_cache.width = base_tex_w;
        dst_cache.height = base_tex_h;
        dst_cache.source_rect = SDL_Rect{0, 0, base_tex_w, base_tex_h};

        dest.frame_cache_.push_back(std::move(dst_cache));
    }

    dest.number_of_frames = static_cast<int>(frame_count);
    std::vector<std::vector<AnimationFrame>> cloned_movement_paths;
    cloned_movement_paths.reserve(source.movement_path_count());
    for (std::size_t path_index = 0; path_index < source.movement_path_count(); ++path_index) {
        cloned_movement_paths.push_back(source.movement_path(path_index));
    }
    if (cloned_movement_paths.empty()) {
        cloned_movement_paths.emplace_back();
    }

    apply_movement_transforms(cloned_movement_paths,
                              opts.reverse_frames,
                              opts.invert_movement_x,
                              opts.invert_movement_y,
                              opts.invert_movement_z);
    dest.replace_movement_paths(std::move(cloned_movement_paths));
    recompute_movement_totals(dest);

    dest.synchronize_runtime_frames();

    return !dest.frame_cache_.empty();
}



