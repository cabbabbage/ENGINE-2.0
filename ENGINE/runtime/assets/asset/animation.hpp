#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>
#include "animation_frame.hpp"
#include "rendering/render/render.hpp"
#include "animation_frame_variant.hpp"
#include "PointPercentage.hpp"

inline constexpr int kBaseAnimationFps = 24;

class AssetInfo;

class Animation {

    friend class AnimationLoader;
    friend class AnimationCloner;
public:
    enum class OnEndDirective {
        Default,
        Kill,
        Lock,
        Reverse,
        Animation,
};

    struct FrameCache {
        std::vector<SDL_Texture*> textures;
        std::vector<int> widths;
        std::vector<int> heights;
        std::vector<SDL_Texture*> foreground_textures;
        std::vector<SDL_Texture*> background_textures;
        std::vector<SDL_Rect> source_rects;
        std::vector<bool> uses_atlas;

        void resize(std::size_t variant_count) {
            textures.assign(variant_count, nullptr);
            widths.assign(variant_count, 0);
            heights.assign(variant_count, 0);
            foreground_textures.assign(variant_count, nullptr);
            background_textures.assign(variant_count, nullptr);
            source_rects.assign(variant_count, SDL_Rect{0, 0, 0, 0});
            uses_atlas.assign(variant_count, false);
        }
};

    struct AudioClip {
        std::string name;
        std::string path;
        int volume = 100;
        bool effects = false;
        struct AudioBuffer {
            SDL_AudioSpec spec{};
            std::vector<Uint8> samples;
        };
        std::shared_ptr<AudioBuffer> buffer;
};

    Animation();
    const FrameVariant* get_frame(const AnimationFrame* frame, float requested_scale) const;
    const AnimationFrame* get_first_frame(std::size_t path_index = 0) const;
    AnimationFrame* get_first_frame(std::size_t path_index = 0);
    int index_of(const AnimationFrame* frame) const;
    void change(AnimationFrame*& frame, bool& static_flag) const;
    void freeze();
    bool is_frozen() const;
    bool has_audio() const;
    const AudioClip* audio_data() const;
    void clear_texture_cache();
    void adopt_prebuilt_frames(std::vector<FrameCache> caches, std::vector<float> variant_steps);

    bool rebuild_frame(int frame_index, SDL_Renderer* renderer, const AssetInfo& info, const std::string& animation_id);

    bool rebuild_animation(SDL_Renderer* renderer, const AssetInfo& info, const std::string& animation_id);
    bool copy_from(const Animation& source, bool flip_horizontal, bool flip_vertical, bool reverse_frames, SDL_Renderer* renderer, class AssetInfo& info);
    static OnEndDirective classify_on_end(std::string_view value);
    struct Source {
        std::string kind;
        std::string path;
        std::string name;
    } source{};
    bool flipped_source = false;
    bool flip_vertical_source = false;
    bool flip_movement_horizontal = false;
    bool flip_movement_vertical = false;
    bool reverse_source = false;
    bool inherit_source_movement = false;
    bool locked = false;
    int number_of_frames = 0;
    int total_dx = 0;
    int total_dy = 0;
    int total_dz = 0;
    bool movment = false;
    bool rnd_start = false;
    std::string on_end_animation;
    OnEndDirective on_end_behavior = OnEndDirective::Default;
    std::vector<AnimationFrame*> frames;
    bool randomize = false;
    bool loop = true;
    bool frozen = false;
    SDL_Texture* preview_texture = nullptr;
    std::size_t movement_path_count() const;
    const std::vector<AnimationFrame>& movement_path(std::size_t index) const;
    std::vector<AnimationFrame>& movement_path(std::size_t index);
    void inherit_movement_from(const Animation& source);
    std::size_t default_movement_path_index() const { return 0; }
    std::size_t clamp_path_index(std::size_t index) const;
    std::size_t variant_count() const { return variant_steps_.size(); }
    const std::vector<float>& variant_steps() const { return variant_steps_; }
private:
    std::vector<FrameCache> frame_cache_;
    AudioClip audio_clip;
    std::vector<std::vector<AnimationFrame>> movement_paths_;
    std::vector<float> variant_steps_;
    void bind_textures_to_frame(AnimationFrame& frame) const;
    void refresh_frame_texture_bindings();
};

struct PrebuiltAnimationFrames {
    std::vector<Animation::FrameCache> frames;
    std::vector<float> variant_steps;
    int canvas_width = 0;
    int canvas_height = 0;
    bool uses_atlas = false;
};
