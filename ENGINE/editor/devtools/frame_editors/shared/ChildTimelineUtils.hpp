#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "assets/animation_child_data.hpp"

namespace devmode::frame_editors::child_timelines {

struct ChildFrameSample {
    int child_index = -1;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float degree = 0.0f;
    bool visible = false;
    bool has_data = false;
};

ChildFrameSample child_frame_from_json(const nlohmann::json& sample, int child_index);

nlohmann::json child_frame_to_json(const ChildFrameSample& frame);

nlohmann::json build_child_timelines_payload(
    const nlohmann::json& existing_payload,
    const std::vector<std::vector<ChildFrameSample>>& static_frames_by_child,
    const std::vector<std::string>& child_assets);

}  // namespace devmode::frame_editors::child_timelines
