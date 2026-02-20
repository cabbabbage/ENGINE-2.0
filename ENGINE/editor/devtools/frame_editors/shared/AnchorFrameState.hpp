#pragma once

#include <limits>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace devmode::frame_editors {

struct FrameAnchorPoint {
    std::string name;
    int texture_x = 0;
    int texture_z = 0;
    bool in_front = true;
    bool has_pixel_coords = false;
    float normalized_x = std::numeric_limits<float>::quiet_NaN();
    float normalized_z = std::numeric_limits<float>::quiet_NaN();
    bool has_normalized_coords = false;
};

struct AnchorFrame {
    std::vector<FrameAnchorPoint> anchors;
};

std::vector<AnchorFrame> parse_anchor_frames_from_payload(const nlohmann::json& payload);
nlohmann::json build_payload_with_anchors(const std::vector<AnchorFrame>& frames, const nlohmann::json& existing_payload);

}  // namespace devmode::frame_editors
