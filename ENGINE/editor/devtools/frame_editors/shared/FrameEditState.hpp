#pragma once

#include <vector>

#include <nlohmann/json.hpp>

namespace devmode::frame_editors {

struct ChildFrame {
    int child_index = -1;
    float dx = 0.0f;
    float dy = 0.0f;
    float dz = 0.0f;
    float degree = 0.0f;
    bool visible = true;
    bool has_data = false;
};

struct MovementFrame {
    float dx = 0.0f;
    float dy = 0.0f;
    float dz = 0.0f;
    float rotation_degrees = 0.0f;
    bool resort_z = false;
    std::vector<ChildFrame> children;
};

MovementFrame clamp_frame(const MovementFrame& in);
std::vector<MovementFrame> parse_frames_from_payload(const std::string& payload_json);
std::vector<MovementFrame> parse_frames_from_payload(const nlohmann::json& payload);
std::vector<std::vector<MovementFrame>> parse_movement_paths_from_payload(const nlohmann::json& payload);
nlohmann::json build_payload_from_frames(const std::vector<MovementFrame>& frames,
                                         const nlohmann::json& existing_payload);
nlohmann::json build_payload_from_movement_paths(const std::vector<std::vector<MovementFrame>>& paths,
                                                 const nlohmann::json& existing_payload);

}  // namespace devmode::frame_editors
