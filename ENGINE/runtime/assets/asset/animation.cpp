#include "animation.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "asset_info.hpp"
#include "asset_types.hpp"
#include "surface_utils.hpp"
#include "rendering/render/render.hpp"
#include "rendering/render/scaling_logic.hpp"
#include "utils/loading_status_notifier.hpp"
#include "utils/log.hpp"
#include <SDL3_image/SDL_image.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;

namespace {

inline void apply_scale_mode(SDL_Texture* tex, const AssetInfo& info) {
    if (!tex) {
        return;
    }
    SDL_SetTextureScaleMode(tex, info.smooth_scaling ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
}

struct VariantLayerPaths {
    std::filesystem::path normal;
    std::filesystem::path foreground;
    std::filesystem::path background;
    std::filesystem::path mask;
};

VariantLayerPaths build_variant_paths(const std::string& cache_root,
                                      const std::vector<float>& variant_steps,
                                      std::size_t variant_idx) {
    VariantLayerPaths out;
    const std::string folder = render_pipeline::ScalingLogic::VariantFolder(cache_root, variant_steps, variant_idx);
    std::filesystem::path base(folder);
    out.normal     = base / "normal";
    out.foreground = base / "foreground";
    out.background = base / "background";
    out.mask       = base / "mask";
    return out;
}

SDL_Texture* load_texture_from_path(SDL_Renderer* renderer,
                                    const std::filesystem::path& path,
                                    int& out_w,
                                    int& out_h) {
    out_w = 0;
    out_h = 0;
    if (!renderer) {
        return nullptr;
    }

    SDL_Texture* tex = IMG_LoadTexture(renderer, path.generic_string().c_str());
    if (!tex) {
        return nullptr;
    }

    float wf = 0.0f;
    float hf = 0.0f;
    if (!SDL_GetTextureSize(tex, &wf, &hf)) {
        SDL_DestroyTexture(tex);
        return nullptr;
    }

    out_w = static_cast<int>(std::lround(wf));
    out_h = static_cast<int>(std::lround(hf));
    if (out_w <= 0 || out_h <= 0) {
        SDL_DestroyTexture(tex);
        return nullptr;
    }

    return tex;
}

void destroy_texture(SDL_Texture*& tex) {
    if (tex) {
        SDL_DestroyTexture(tex);
        tex = nullptr;
    }
}

}

Animation::Animation() = default;

Animation::OnEndDirective Animation::classify_on_end(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (char ch : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    if (lowered.empty() || lowered == "default") {
        return OnEndDirective::Default;
    }
    if (lowered == "kill") {
        return OnEndDirective::Kill;
    }
    if (lowered == "lock") {
        return OnEndDirective::Lock;
    }
    if (lowered == "reverse") {
        return OnEndDirective::Reverse;
    }
    return OnEndDirective::Animation;
}

bool Animation::rebuild_frame(int frame_index,
                              SDL_Renderer* renderer,
                              const AssetInfo& info,
                              const std::string& animation_id) {
    if (!renderer || frame_index < 0 || animation_id.empty()) {
        return false;
    }

    const std::size_t idx = static_cast<std::size_t>(frame_index);
    if (idx >= frame_cache_.size()) {
        return false;
    }

    std::vector<float> variant_steps = variant_steps_;
    render_pipeline::ScalingLogic::NormalizeVariantSteps(variant_steps);

    Animation::FrameCache& cache_entry = frame_cache_[idx];
    cache_entry.resize(variant_steps.size());

    const std::string cache_root = (std::filesystem::path("cache") / info.name / "animations").lexically_normal().generic_string();

    bool success = true;

    for (std::size_t variant_idx = 0; variant_idx < variant_steps.size(); ++variant_idx) {
        const VariantLayerPaths paths = build_variant_paths(cache_root, variant_steps, variant_idx);

        const std::filesystem::path base_path = paths.normal / (std::to_string(idx) + ".png");
        int base_w = 0, base_h = 0;
        SDL_Texture* base_tex = load_texture_from_path(renderer, base_path, base_w, base_h);
        if (!base_tex) {
            success = false;
            continue;
        }
        apply_scale_mode(base_tex, info);

        int fg_w = 0, fg_h = 0;
        SDL_Texture* fg_tex = load_texture_from_path(renderer, paths.foreground / (std::to_string(idx) + ".png"), fg_w, fg_h);
        if (fg_tex) {
            apply_scale_mode(fg_tex, info);
        }

        int bg_w = 0, bg_h = 0;
        SDL_Texture* bg_tex = load_texture_from_path(renderer, paths.background / (std::to_string(idx) + ".png"), bg_w, bg_h);
        if (bg_tex) {
            apply_scale_mode(bg_tex, info);
        }

        destroy_texture(cache_entry.textures[variant_idx]);
        destroy_texture(cache_entry.foreground_textures[variant_idx]);
        destroy_texture(cache_entry.background_textures[variant_idx]);

        cache_entry.textures[variant_idx] = base_tex;
        cache_entry.widths[variant_idx] = base_w;
        cache_entry.heights[variant_idx] = base_h;
        cache_entry.source_rects[variant_idx] = SDL_Rect{0, 0, base_w, base_h};
        cache_entry.uses_atlas[variant_idx] = false;
        cache_entry.foreground_textures[variant_idx] = fg_tex;
        cache_entry.background_textures[variant_idx] = bg_tex;
    }

    for (auto& path : movement_paths_) {
        if (idx >= path.size()) {
            continue;
        }
        AnimationFrame& frame = path[idx];
        frame.variants.clear();
        frame.variants.reserve(variant_steps.size());
        for (std::size_t v = 0; v < variant_steps.size(); ++v) {
            FrameVariant variant;
            variant.varient = static_cast<int>(v);
            variant.base_texture = cache_entry.textures[v];
            variant.foreground_texture = (v < cache_entry.foreground_textures.size()) ? cache_entry.foreground_textures[v] : nullptr;
            variant.background_texture = (v < cache_entry.background_textures.size()) ? cache_entry.background_textures[v] : nullptr;
            variant.source_rect = SDL_Rect{0, 0,
                                           (v < cache_entry.widths.size() ? cache_entry.widths[v] : 0),
                                           (v < cache_entry.heights.size() ? cache_entry.heights[v] : 0)};
            variant.uses_atlas = (v < cache_entry.uses_atlas.size()) ? cache_entry.uses_atlas[v] : false;
            frame.variants.push_back(variant);
        }
    }

    if (idx == 0 && !frame_cache_.empty() && !frame_cache_[0].textures.empty()) {
        preview_texture = frame_cache_[0].textures[0];
    }

    return success;
}

bool Animation::rebuild_animation(SDL_Renderer* renderer,
                                  const AssetInfo& info,
                                  const std::string& animation_id) {
    if (!renderer || animation_id.empty()) {
        return false;
    }
    const std::size_t frame_count = frame_cache_.size();
    if (frame_count == 0) {
        return false;
    }

    bool ok = true;
    for (std::size_t i = 0; i < frame_count; ++i) {
        ok = rebuild_frame(static_cast<int>(i), renderer, info, animation_id) && ok;
    }
    return ok;
}

void Animation::clear_texture_cache() {
    std::unordered_set<SDL_Texture*> destroyed;
    auto destroy_once = [&destroyed](SDL_Texture*& tex) {
        if (tex && destroyed.insert(tex).second) {
            SDL_DestroyTexture(tex);
        }
        tex = nullptr;
    };

    for (auto& cache_entry : frame_cache_) {
        for (SDL_Texture*& tex : cache_entry.textures) {
            destroy_once(tex);
        }
        for (SDL_Texture*& tex : cache_entry.foreground_textures) {
            destroy_once(tex);
        }
        for (SDL_Texture*& tex : cache_entry.background_textures) {
            destroy_once(tex);
        }
    }
    frame_cache_.clear();
    if (audio_clip.buffer) {
        audio_clip.buffer.reset();
    }
}

void Animation::adopt_prebuilt_frames(std::vector<FrameCache> caches,
                                      std::vector<float> variant_steps) {
    clear_texture_cache();
    frame_cache_   = std::move(caches);
    variant_steps_ = std::move(variant_steps);
    render_pipeline::ScalingLogic::NormalizeVariantSteps(variant_steps_);

    const std::size_t canonical_variant_count = variant_steps_.size();
    for (auto& cache : frame_cache_) {
        cache.textures.resize(canonical_variant_count, nullptr);
        cache.widths.resize(canonical_variant_count, 0);
        cache.heights.resize(canonical_variant_count, 0);
        cache.foreground_textures.resize(canonical_variant_count, nullptr);
        cache.background_textures.resize(canonical_variant_count, nullptr);
        cache.source_rects.resize(canonical_variant_count, SDL_Rect{0, 0, 0, 0});
        cache.uses_atlas.resize(canonical_variant_count, false);
    }

    number_of_frames = static_cast<int>(frame_cache_.size());

    movement_paths_.clear();
    if (number_of_frames <= 0) {
            movement_paths_.emplace_back();
            return;
    }

    movement_paths_.emplace_back();
    auto& path = movement_paths_.back();
    path.resize(number_of_frames);
    frames.reserve(number_of_frames);

    for (std::size_t idx = 0; idx < path.size(); ++idx) {
            auto& frame = path[idx];
            frame.frame_index = static_cast<int>(idx);
            frame.is_first   = (idx == 0);
            frame.is_last    = (idx + 1 == path.size());
            frame.next       = (idx + 1 < path.size()) ? &path[idx + 1] : nullptr;
            frame.prev       = (idx > 0) ? &path[idx - 1] : nullptr;

            if (idx < frame_cache_.size()) {
                const auto& cache = frame_cache_[idx];
                for (size_t v = 0; v < cache.textures.size(); ++v) {
                    FrameVariant variant;
                    variant.varient = static_cast<int>(v);
                    variant.base_texture = cache.textures[v];
                    if (v < cache.source_rects.size()) {
                        variant.source_rect = cache.source_rects[v];
                    } else {
                        variant.source_rect = SDL_Rect{0, 0,
                                                       (v < cache.widths.size()) ? cache.widths[v] : 0,
                                                       (v < cache.heights.size()) ? cache.heights[v] : 0};
                    }
                    variant.uses_atlas = (v < cache.uses_atlas.size()) ? cache.uses_atlas[v] : false;
                    if (v < cache.foreground_textures.size()) {
                        variant.foreground_texture = cache.foreground_textures[v];
                    }
                    if (v < cache.background_textures.size()) {
                        variant.background_texture = cache.background_textures[v];
                    }
                    frame.variants.push_back(variant);
                }
            }
            frames.push_back(&frame);
    }

}

bool Animation::copy_from(const Animation& source, bool flip_horizontal, bool flip_vertical, bool reverse_frames, SDL_Renderer* renderer, AssetInfo& info) {
    if (!renderer || source.frame_cache_.empty()) {
        return false;
    }

    auto apply_scale_mode = [&info](SDL_Texture* tex) {
        if (!tex) return;
#if SDL_VERSION_ATLEAST(2, 0, 12)
        if (info.smooth_scaling) {
            SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
        } else {
            SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
        }
#endif
};

    auto clone_texture = [&](SDL_Texture* src, int width_hint, int height_hint, SDL_FlipMode flip_flags, int* out_w = nullptr, int* out_h = nullptr) -> SDL_Texture* {
        if (!src) return nullptr;

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
        apply_scale_mode(dst);

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
};

    clear_texture_cache();

    variant_steps_ = source.variant_steps_;
    render_pipeline::ScalingLogic::NormalizeVariantSteps(variant_steps_);
    locked = source.locked;
    inherit_source_movement = source.inherit_source_movement;
    const std::size_t frame_count = source.frame_cache_.size();
    const std::size_t variant_count = variant_steps_.size();

    if (variant_count == 0 || frame_count == 0) {
        return false;
    }

    SDL_FlipMode flip_flags = SDL_FLIP_NONE;
    if (flip_horizontal) {
        flip_flags = static_cast<SDL_FlipMode>(flip_flags | SDL_FLIP_HORIZONTAL);
    }
    if (flip_vertical) {
        flip_flags = static_cast<SDL_FlipMode>(flip_flags | SDL_FLIP_VERTICAL);
    }

    frame_cache_.reserve(frame_count);
    for (std::size_t frame_idx = 0; frame_idx < frame_count; ++frame_idx) {
        const FrameCache& src_cache = source.frame_cache_[frame_idx];
        FrameCache dst_cache;
        dst_cache.resize(variant_count);

        for (std::size_t variant_idx = 0; variant_idx < variant_count; ++variant_idx) {
            if (variant_idx >= src_cache.textures.size()) {
                continue;
            }

            SDL_Texture* src_tex = src_cache.textures[variant_idx];
            int tex_w = src_cache.widths[variant_idx];
            int tex_h = src_cache.heights[variant_idx];

            SDL_Texture* dst_tex = clone_texture(src_tex, tex_w, tex_h, flip_flags, &tex_w, &tex_h);
            if (!dst_tex) {
                continue;
            }
            dst_cache.textures[variant_idx] = dst_tex;
            dst_cache.widths[variant_idx] = tex_w;
            dst_cache.heights[variant_idx] = tex_h;
            dst_cache.source_rects[variant_idx] = SDL_Rect{0, 0, tex_w, tex_h};
            dst_cache.uses_atlas[variant_idx] = false;

            SDL_Texture* src_fg = (variant_idx < src_cache.foreground_textures.size()) ? src_cache.foreground_textures[variant_idx] : nullptr;
            if (src_fg) {
                SDL_Texture* dst_fg = clone_texture(src_fg, tex_w, tex_h, flip_flags);
                dst_cache.foreground_textures[variant_idx] = dst_fg;
            }

            SDL_Texture* src_bg = (variant_idx < src_cache.background_textures.size()) ? src_cache.background_textures[variant_idx] : nullptr;
            if (src_bg) {
                SDL_Texture* dst_bg = clone_texture(src_bg, tex_w, tex_h, flip_flags);
                dst_cache.background_textures[variant_idx] = dst_bg;
            }

        }

        frame_cache_.push_back(std::move(dst_cache));
    }

    if (reverse_frames && !frame_cache_.empty()) {
        std::reverse(frame_cache_.begin(), frame_cache_.end());
    }

    return !frame_cache_.empty();
}

const FrameVariant* Animation::get_frame(const AnimationFrame* frame, float requested_scale) const {
    if (!frame || frame->variants.empty()) return nullptr;

    const auto selection = render_pipeline::ScalingLogic::Choose(requested_scale, variant_steps_);
    int best_variant_idx = selection.index;

    if (best_variant_idx < 0) best_variant_idx = 0;
    if (best_variant_idx >= static_cast<int>(frame->variants.size())) best_variant_idx = static_cast<int>(frame->variants.size()) - 1;

    return &frame->variants[best_variant_idx];
}

const AnimationFrame* Animation::get_first_frame(std::size_t path_index) const {
    if (movement_paths_.empty()) return nullptr;
    path_index = clamp_path_index(path_index);
    const auto& path = movement_paths_[path_index];
    if (path.empty()) return nullptr;
    return &path[0];
}

AnimationFrame* Animation::get_first_frame(std::size_t path_index) {
    return const_cast<AnimationFrame*>(std::as_const(*this).get_first_frame(path_index));
}

int Animation::index_of(const AnimationFrame* frame) const {
    if (!frame) return -1;
    const int index = frame->frame_index;
    if (index < 0 || index >= static_cast<int>(frames.size())) return -1;
    for (const auto& path : movement_paths_) {
        if (path.empty()) continue;
        const AnimationFrame* data = path.data();
        const AnimationFrame* end  = data + path.size();
        if (frame >= data && frame < end) {
            return index;
        }
    }

    return index;
}

void Animation::change(AnimationFrame*& frame, bool& static_flag) const {
    if (frozen) return;
    auto& self = const_cast<Animation&>(*this);
    frame      = self.get_first_frame();
    static_flag = is_frozen() || locked;
}

std::size_t Animation::movement_path_count() const { return movement_paths_.size(); }

const std::vector<AnimationFrame>& Animation::movement_path(std::size_t index) const {
    static const std::vector<AnimationFrame> kEmpty{};
    if (movement_paths_.empty()) return kEmpty;
    if (index >= movement_paths_.size()) index = 0;
    return movement_paths_[index];
}

std::vector<AnimationFrame>& Animation::movement_path(std::size_t index) {
    if (movement_paths_.empty()) movement_paths_.emplace_back();
    if (index >= movement_paths_.size()) index = 0;
    return movement_paths_[index];
}

void Animation::inherit_movement_from(const Animation& source) {
    movement_paths_ = source.movement_paths_;
    if (movement_paths_.empty()) {
        return;
    }
    if (reverse_source) {
        for (auto& path : movement_paths_) {
            std::reverse(path.begin(), path.end());
        }
    }
    if (flip_movement_horizontal) {
        for (auto& path : movement_paths_) {
            for (auto& frame : path) {
                frame.dx = -frame.dx;
            }
        }
    }
    if (flip_movement_vertical) {
        for (auto& path : movement_paths_) {
            for (auto& frame : path) {
                frame.dz = -frame.dz;
            }
        }
    }
}

std::size_t Animation::clamp_path_index(std::size_t index) const {
    if (movement_paths_.empty()) return 0;
    if (index >= movement_paths_.size()) return 0;
    return index;
}

void Animation::freeze() { frozen = true; }

bool Animation::is_frozen() const { return frozen || frames.size() <= 1; }

bool Animation::has_audio() const { return static_cast<bool>(audio_clip.buffer); }

const Animation::AudioClip* Animation::audio_data() const {
    if (!audio_clip.buffer) {
        return nullptr;
    }
    return &audio_clip;
}
