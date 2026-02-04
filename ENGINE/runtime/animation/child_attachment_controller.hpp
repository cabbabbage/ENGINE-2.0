#pragma once

#include <string_view>
#include <vector>

#include <SDL.h>

#include "animation/animation_update.hpp"
#include "assets/Asset.hpp"
#include "assets/animation_frame.hpp"

namespace animation_update::child_attachments {

struct ParentState {
    SDL_Point position{0, 0};
    SDL_Point base_position{0, 0};
    float scale = 1.0f;
    float world_z = 0.0f;
    float height = 0.0f;  // Parent asset height in world pixels for percentage conversion
    bool flipped = false;
    std::string_view animation_id{};
};

void update_dimensions(Asset::AnimationChildAttachment& slot);
void restart(Asset::AnimationChildAttachment& slot);
void advance_frames(std::vector<Asset::AnimationChildAttachment>& slots, const ParentState& parent_state, float dt);
void apply_frame_data(std::vector<Asset::AnimationChildAttachment>& slots, const ParentState& parent_state, const AnimationFrame* frame, const std::vector<AnimationChildFrameData>* override_children = nullptr);

}
