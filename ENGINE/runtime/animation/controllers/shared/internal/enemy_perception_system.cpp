#include "animation/controllers/shared/internal/enemy_perception_system.hpp"

#include <cmath>

#include "animation/animation_update.hpp"
#include "assets/asset/Asset.hpp"

namespace animation_update::custom_controllers::internal {

EnemyPerceptionSnapshot EnemyPerceptionSystem::build(const Asset& self,
                                                     const Asset* target,
                                                     bool target_in_same_room,
                                                     int recent_blocked_frames) {
    EnemyPerceptionSnapshot snapshot{};
    snapshot.self_id = animation_update::detail::stable_asset_id(self);
    snapshot.self_position = axis::WorldPos{self.world_x(), self.world_y(), self.world_z()};
    snapshot.target_in_same_room = target_in_same_room;
    snapshot.recent_blocked_frames = recent_blocked_frames;
    snapshot.grounded = true;

    if (!target || target->dead || !target->active) {
        return snapshot;
    }

    snapshot.target_id = animation_update::detail::stable_asset_id(*target);
    snapshot.target_position = axis::WorldPos{target->world_x(), target->world_y(), target->world_z()};
    const long long dx = static_cast<long long>(target->world_x()) - static_cast<long long>(self.world_x());
    const long long dz = static_cast<long long>(target->world_z()) - static_cast<long long>(self.world_z());
    snapshot.horizontal_distance_px = static_cast<int>(
        std::lround(std::sqrt(static_cast<double>((dx * dx) + (dz * dz)))));
    snapshot.vertical_delta_px = target->world_y() - self.world_y();
    snapshot.target_valid = target_in_same_room;
    snapshot.target_hittable = target->isHitboxEnabled();
    snapshot.has_line_of_approach = target_in_same_room && recent_blocked_frames == 0;
    return snapshot;
}

} // namespace animation_update::custom_controllers::internal
