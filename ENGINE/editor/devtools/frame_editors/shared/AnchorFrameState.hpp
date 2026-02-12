#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace devmode::frame_editors {

struct FrameAnchorPoint {
    std::string name;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float rotation = 0.0f;
};

struct AnchorFrame {
    std::vector<FrameAnchorPoint> anchors;
};

std::vector<AnchorFrame> parse_anchor_frames_from_payload(const nlohmann::json& payload);
nlohmann::json build_payload_with_anchors(const std::vector<AnchorFrame>& frames, const nlohmann::json& existing_payload);

}  // namespace devmode::frame_editors

