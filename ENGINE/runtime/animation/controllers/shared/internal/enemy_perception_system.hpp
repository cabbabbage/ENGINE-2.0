#pragma once

#include <optional>
#include <string>

#include "core/axis_convention.hpp"

class Asset;

namespace animation_update::custom_controllers::internal {

struct EnemyPerceptionSnapshot {
    std::string self_id;
    std::optional<std::string> target_id = std::nullopt;
    axis::WorldPos self_position{0, 0, 0};
    axis::WorldPos target_position{0, 0, 0};
    axis::WorldPos target_velocity_smoothed{0, 0, 0};
    int horizontal_distance_px = 0;
    int vertical_delta_px = 0;
    bool target_valid = false;
    bool target_in_same_room = false;
    bool target_hittable = false;
    bool has_line_of_approach = false;
    bool grounded = true;
    int recent_blocked_frames = 0;
};

class EnemyPerceptionSystem {
public:
    static EnemyPerceptionSnapshot build(const Asset& self,
                                         const Asset* target,
                                         bool target_in_same_room,
                                         int recent_blocked_frames);
};

} // namespace animation_update::custom_controllers::internal
