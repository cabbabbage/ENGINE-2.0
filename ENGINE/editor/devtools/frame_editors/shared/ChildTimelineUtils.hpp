#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "assets/animation_child_data.hpp"

namespace devmode::frame_editors::child_timelines {

struct ChildFrameSample {
    int child_index = -1;
    float dx = 0.0f;
    float dy = 0.0f;
    float dz = 0.0f;
    float degree = 0.0f;
    bool visible = false;
    bool has_data = false;
};

bool timeline_entry_is_static(const nlohmann::json& entry);

ChildFrameSample child_frame_from_json(const nlohmann::json& sample, int child_index);

nlohmann::json child_frame_to_json(const ChildFrameSample& frame);

nlohmann::json build_child_timelines_payload(
    const nlohmann::json& existing_payload,
    const std::vector<std::vector<ChildFrameSample>>& static_frames_by_child,
    const std::vector<std::string>& child_assets,
    const std::vector<AnimationChildMode>& child_modes,
    const std::vector<std::vector<ChildFrameSample>>& async_timelines_by_child = {},
    const std::vector<float>& async_start_times = {},
    const std::vector<bool>& async_has_start = {});

}  // namespace devmode::frame_editors::child_timelines
