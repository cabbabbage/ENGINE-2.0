#pragma once

#include <string>
#include <vector>

#include "animation_frame_variant.hpp"

struct AnimationChildData {
    std::string name;
    std::string asset_name;
    std::string animation_override;
    std::vector<AnimationChildFrameData> frames;

    bool valid() const { return !name.empty() && !asset_name.empty(); }
    std::size_t frame_count() const { return frames.size(); }
};
