#pragma once

#include <optional>
#include <random>
#include <string>
#include <vector>

#include "animation/controllers/shared/controller_types.hpp"
#include "core/axis_convention.hpp"

class Asset;

namespace animation_update::custom_controllers::internal {

using MovementConfig = ::animation_update::custom_controllers::MovementConfig;

struct PatrolState {
    std::size_t next_index = 0;
};

class ControllerMovementSystem {
public:
    static bool move_by_delta_3d(Asset& self,
                                 const axis::WorldPos& delta,
                                 const std::string& animation,
                                 bool override_non_locked = true);
    static bool move_toward_point(Asset& self,
                                  const axis::WorldPos& target,
                                  int step_px,
                                  const MovementConfig& config = {});
    static bool move_away_from_point(Asset& self,
                                     const axis::WorldPos& point,
                                     int step_px,
                                     const MovementConfig& config = {});
    static bool seek_target(Asset& self,
                            const Asset& target,
                            int desired_range_px,
                            const MovementConfig& config = {});
    static bool chase_target(Asset& self,
                             const Asset& target,
                             const MovementConfig& config = {});
    static bool retreat_from_target(Asset& self,
                                    const Asset& target,
                                    int retreat_distance_px,
                                    const MovementConfig& config = {});
    static bool patrol(Asset& self,
                       const std::vector<axis::WorldPos>& points,
                       PatrolState& patrol_state,
                       const MovementConfig& config = {});
    static bool idle_wander(Asset& self,
                            std::mt19937& rng,
                            int min_delta_px,
                            int max_delta_px,
                            const MovementConfig& config = {});
    static bool return_to_origin(Asset& self,
                                 const axis::WorldPos& origin,
                                 int arrival_threshold_px,
                                 const MovementConfig& config = {});
    static bool face_target(Asset& self, const Asset& target);
    static bool face_direction(Asset& self, float dir_x, float dir_z, float pitch_radians = 0.0f);

    static long long distance_sq_3d(const Asset& a, const Asset& b);
    static long long distance_sq_3d(const Asset& a, const axis::WorldPos& b);

private:
    static axis::WorldPos world_position(const Asset& self);
    static bool auto_move_3d(Asset& self,
                             const axis::WorldPos& target,
                             const MovementConfig& config);
};

} // namespace animation_update::custom_controllers::internal
