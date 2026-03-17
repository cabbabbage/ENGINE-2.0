#pragma once

#include <vector>

#include <nlohmann/json.hpp>

namespace devmode::room_movement_payload {

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
    bool resort_z = false;
    std::vector<ChildFrame> children;
};

MovementFrame clamp_frame(const MovementFrame& in);
std::vector<MovementFrame> parse_frames_from_payload(const std::string& payload_json);
std::vector<MovementFrame> parse_frames_from_payload(const nlohmann::json& payload);
nlohmann::json build_payload_from_frames(const std::vector<MovementFrame>& frames,
                                         const nlohmann::json& existing_payload);

}  // namespace devmode::room_movement_payload

