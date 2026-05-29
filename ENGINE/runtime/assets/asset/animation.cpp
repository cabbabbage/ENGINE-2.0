#include "animation.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "asset_info.hpp"
#include "asset_types.hpp"
#include "surface_utils.hpp"
#include "rendering/render/scaling_logic.hpp"
#include "utils/cache_manager.hpp"
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

struct LayerPaths {
    std::filesystem::path normal;
    std::filesystem::path mask;
};

void destroy_prepared_texture(SDL_Texture* tex) {
    if (!tex) {
        return;
    }
    CacheManager::unregister_prepared_gpu_upload(tex);
    SDL_DestroyTexture(tex);
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

    SDL_Surface* surface = CacheManager::load_surface(path.generic_string());
    if (!surface) {
        return nullptr;
    }

    CacheManager::TextureUploadOptions upload_options{};
    upload_options.semantic = CacheManager::TextureSemantic::Color;
    upload_options.enable_mipmaps = surface->w >= 128 && surface->h >= 128;
    SDL_Texture* tex = CacheManager::surface_to_texture(renderer, surface, upload_options);
    SDL_DestroySurface(surface);
    if (!tex) {
        return nullptr;
    }

    float wf = 0.0f;
    float hf = 0.0f;
    if (!SDL_GetTextureSize(tex, &wf, &hf)) {
        destroy_prepared_texture(tex);
        return nullptr;
    }

    out_w = static_cast<int>(std::lround(wf));
    out_h = static_cast<int>(std::lround(hf));
    if (out_w <= 0 || out_h <= 0) {
        destroy_prepared_texture(tex);
        return nullptr;
    }

    return tex;
}

void destroy_texture(SDL_Texture*& tex) {
    if (tex) {
        destroy_prepared_texture(tex);
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
    if (lowered == "loop") {
        return OnEndDirective::Loop;
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

    Animation::FrameCache& cache_entry = frame_cache_[idx];

    const std::string cache_root = (std::filesystem::path("cache") / info.name / "animations").lexically_normal().generic_string();
    const std::filesystem::path frame_path = std::filesystem::path(cache_root) / animation_id / (std::to_string(idx) + ".png");

    int base_w = 0, base_h = 0;
    SDL_Texture* base_tex = load_texture_from_path(renderer, frame_path, base_w, base_h);
    if (!base_tex) {
        return false;
    }
    apply_scale_mode(base_tex, info);

    destroy_texture(cache_entry.texture);
    cache_entry.texture = base_tex;
    cache_entry.width = base_w;
    cache_entry.height = base_h;
    cache_entry.source_rect = SDL_Rect{0, 0, base_w, base_h};

    // Bind the single resident texture to runtime frames
    for (auto& path : movement_paths_) {
        if (idx >= path.size()) continue;
        AnimationFrame& frame = path[idx];
        frame.texture_binding.base_texture = base_tex;
        frame.texture_binding.source_rect = SDL_Rect{0, 0, base_w, base_h};
    }

    if (idx == 0 && frame_cache_[0].texture) {
        preview_texture = frame_cache_[0].texture;
    }

    return true;
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
    for (auto& cache_entry : frame_cache_) {
        if (cache_entry.texture && destroyed.insert(cache_entry.texture).second) {
            destroy_prepared_texture(cache_entry.texture);
        }
        cache_entry.texture = nullptr;
    }
    frame_cache_.clear();
    if (audio_clip.buffer) {
        audio_clip.buffer.reset();
    }
}

void Animation::adopt_prebuilt_frames(std::vector<FrameCache> caches) {
    clear_texture_cache();
    frame_cache_ = std::move(caches);
    number_of_frames = static_cast<int>(frame_cache_.size());

    movement_paths_.clear();
    movement_paths_.emplace_back();
    if (number_of_frames > 0) {
        movement_paths_.back().resize(number_of_frames);
    }
    synchronize_runtime_frames();
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

        SDL_PixelFormat fmt = SDL_PIXELFORMAT_RGBA32;
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

    locked = source.locked;
    tags.clear();
    const std::size_t frame_count = source.frame_cache_.size();

    if (frame_count == 0) {
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

        SDL_Texture* src_tex = src_cache.texture;
        int tex_w = src_cache.width;
        int tex_h = src_cache.height;

        SDL_Texture* dst_tex = clone_texture(src_tex, tex_w, tex_h, flip_flags, &tex_w, &tex_h);
        if (dst_tex) {
            dst_cache.texture = dst_tex;
            dst_cache.width = tex_w;
            dst_cache.height = tex_h;
            dst_cache.source_rect = SDL_Rect{0, 0, tex_w, tex_h};
        }

        frame_cache_.push_back(std::move(dst_cache));
    }

    if (reverse_frames && !frame_cache_.empty()) {
        std::reverse(frame_cache_.begin(), frame_cache_.end());
    }

    synchronize_runtime_frames();
    return !frame_cache_.empty();
}

const FrameTextureBinding* Animation::get_frame(const AnimationFrame* frame, float /*requested_scale*/) const {
    if (!frame) return nullptr;
    return &frame->texture_binding;
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
    if (index < 0 || index >= static_cast<int>(frame_count())) return -1;
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

void Animation::replace_movement_paths(std::vector<std::vector<AnimationFrame>> paths) {
    movement_paths_ = std::move(paths);
    if (movement_paths_.empty()) {
        movement_paths_.emplace_back();
    }
}

std::size_t Animation::clamp_path_index(std::size_t index) const {
    if (movement_paths_.empty()) return 0;
    if (index >= movement_paths_.size()) return 0;
    return index;
}

void Animation::freeze() { frozen = true; }

bool Animation::is_frozen() const { return frozen || frame_count() <= 1; }

bool Animation::has_audio() const { return static_cast<bool>(audio_clip.buffer); }

const Animation::AudioClip* Animation::audio_data() const {
    if (!audio_clip.buffer) {
        return nullptr;
    }
    return &audio_clip;
}

bool Animation::has_frames(std::size_t path_index) const {
    return frame_count(path_index) > 0;
}

std::size_t Animation::frame_count(std::size_t path_index) const {
    return movement_path(path_index).size();
}

const std::vector<AnimationFrame>& Animation::primary_frames() const {
    return movement_path(default_movement_path_index());
}

std::vector<AnimationFrame>& Animation::primary_frames() {
    return movement_path(default_movement_path_index());
}

const AnimationFrame* Animation::primary_frame_at(std::size_t index) const {
    const auto& frames = primary_frames();
    if (index >= frames.size()) {
        return nullptr;
    }
    return &frames[index];
}

AnimationFrame* Animation::primary_frame_at(std::size_t index) {
    auto& frames = primary_frames();
    if (index >= frames.size()) {
        return nullptr;
    }
    return &frames[index];
}

void Animation::bind_textures_to_frame(AnimationFrame& frame) const {
    const int raw_index = frame.frame_index;
    if (raw_index < 0 || raw_index >= static_cast<int>(frame_cache_.size())) {
        frame.texture_binding = FrameTextureBinding{};
        return;
    }

    const std::size_t index = static_cast<std::size_t>(raw_index);
    const auto& cache = frame_cache_[index];
    frame.texture_binding.base_texture = cache.texture;
    frame.texture_binding.source_rect = cache.source_rect;
}

void Animation::update_preview_texture_from_primary_path() {
    const AnimationFrame* first = primary_frame_at(0);
    if (!first) {
        preview_texture = nullptr;
        return;
    }
    preview_texture = first->texture_binding.get_base_texture();
}

void Animation::synchronize_runtime_frames() {
    if (movement_paths_.empty()) {
        movement_paths_.emplace_back();
    }

    const std::size_t expected_frame_count = frame_cache_.size();
    for (auto& path : movement_paths_) {
        if (path.size() != expected_frame_count) {
            path.resize(expected_frame_count);
        }
        for (std::size_t idx = 0; idx < path.size(); ++idx) {
            AnimationFrame& frame = path[idx];
            frame.frame_index = static_cast<int>(idx);
            frame.is_first = (idx == 0);
            frame.is_last = (idx + 1 == path.size());
            frame.prev = (idx > 0) ? &path[idx - 1] : nullptr;
            frame.next = (idx + 1 < path.size()) ? &path[idx + 1] : nullptr;
            bind_textures_to_frame(frame);
        }
    }

    number_of_frames = static_cast<int>(expected_frame_count);
    update_preview_texture_from_primary_path();
}
