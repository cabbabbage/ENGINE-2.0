#pragma once

#include <string>
#include <vector>

#include "animation_frame_variant.hpp"

enum class AnimationChildMode {
    Static,
    Async,
};

struct AnimationChildData {
    std::string name;
    std::string asset_name;
    std::string animation_override;
    AnimationChildMode mode = AnimationChildMode::Static;
    bool auto_start = false;
    // Async timelines may optionally declare a start offset relative to the parent timeline.
    // start_time is expressed in seconds; start_frame is the integer frame offset at base FPS.
    float start_time = 0.0f;
    int start_frame = 0;
    bool has_start_time = false;
    std::vector<AnimationChildFrameData> frames;

    bool valid() const { return !name.empty() && !asset_name.empty(); }
    bool is_static() const { return mode == AnimationChildMode::Static; }
    bool is_async() const { return mode == AnimationChildMode::Async; }
    std::size_t frame_count() const { return frames.size(); }
};
