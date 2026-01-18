#pragma once

#include <array>
#include <vector>

#include <nlohmann/json.hpp>

#include "animation_update/combat_geometry.hpp"

namespace devmode::frame_editors {

struct ChildFrame {
    int child_index = -1;
    float dx = 0.0f;
    float dy = 0.0f;
    float degree = 0.0f;
    bool visible = true;
    bool render_in_front = true;
    bool has_data = false;
};

struct MovementFrame {
    float dx = 0.0f;
    float dy = 0.0f;
    bool resort_z = false;
    std::vector<ChildFrame> children;

    animation_update::FrameHitGeometry hit;
    animation_update::FrameAttackGeometry attack;
};

inline constexpr std::array<const char*, 3> kDamageTypeNames = {"projectile", "melee", "explosion"};

MovementFrame clamp_frame(const MovementFrame& in);
std::vector<MovementFrame> parse_frames_from_payload(const std::string& payload_json);
std::vector<MovementFrame> parse_frames_from_payload(const nlohmann::json& payload);
nlohmann::json build_payload_from_frames(const std::vector<MovementFrame>& frames,
                                         const nlohmann::json& existing_payload);

}  // namespace devmode::frame_editors
