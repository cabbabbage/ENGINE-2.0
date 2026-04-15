#include "path_sanitizer_3d.hpp"

#include <algorithm>
#include <cmath>

#include "animation_update.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_info.hpp"
#include "core/AssetsManager.hpp"
#include "gameplay/world/grid_point.hpp"
#include "utils/area.hpp"

using CollisionAreaRef3D = const Assets::FrameCollisionEntry*;

namespace {

std::vector<CollisionAreaRef3D> gather_collision_areas(const Asset& self) {
    std::vector<CollisionAreaRef3D> result;
    const Assets* assets = self.get_assets();
    if (!assets) {
        return result;
    }

    const int search_radius = (self.info && self.info->NeighborSearchRadius > 0)
        ? self.info->NeighborSearchRadius
        : 0;
    assets->query_impassable_entries(self, search_radius, result);
    return result;
}

world::GridPoint as_grid_point(const axis::WorldPos& pos, int resolution_layer) {
    return world::GridPoint::make_virtual(pos.x, pos.y, pos.z, resolution_layer);
}

bool same_world_point(const axis::WorldPos& lhs, const axis::WorldPos& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}

bool segment_hits_any(const axis::WorldPos& from,
                      const axis::WorldPos& to,
                      int                   resolution_layer,
                      const std::vector<CollisionAreaRef3D>& areas) {
    const world::GridPoint from_gp = as_grid_point(from, resolution_layer);
    const world::GridPoint to_gp   = as_grid_point(to, resolution_layer);
    for (const CollisionAreaRef3D entry : areas) {
        if (!entry) {
            continue;
        }
        if (animation_update::detail::segment_hits_area(from_gp, to_gp, entry->area)) {
            return true;
        }
    }
    return false;
}

axis::WorldPos nudge_outside_xz(axis::WorldPos point, const Area& area) {
    const SDL_Point center = area.get_center();
    int dx = point.x - center.x;
    int dz = point.z - center.y;
    if (dx == 0 && dz == 0) {
        dx = 1;
    }

    const double length = std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dz) * dz);
    const double step_x = (length == 0.0) ? 1.0 : dx / length;
    const double step_z = (length == 0.0) ? 0.0 : dz / length;

    axis::WorldPos result = point;
    int guard = 0;
    while (area.contains_point(SDL_Point{ result.x, result.z }) && guard < 512) {
        result.x += static_cast<int>(std::round(step_x));
        result.z += static_cast<int>(std::round(step_z));
        ++guard;
    }
    return result;
}

axis::WorldPos walk_back_to_perimeter(const axis::WorldPos& start,
                                      const axis::WorldPos& target,
                                      const std::vector<CollisionAreaRef3D>& areas) {
    const int steps = std::max(std::abs(target.x - start.x), std::abs(target.z - start.z));
    if (steps == 0) {
        return target;
    }

    const double step_x = (target.x - start.x) / static_cast<double>(steps);
    const double step_y = (target.y - start.y) / static_cast<double>(steps);
    const double step_z = (target.z - start.z) / static_cast<double>(steps);

    axis::WorldPos best = target;
    for (int i = steps; i >= 0; --i) {
        const axis::WorldPos candidate{
            static_cast<int>(std::round(start.x + step_x * i)),
            static_cast<int>(std::round(start.y + step_y * i)),
            static_cast<int>(std::round(start.z + step_z * i))
        };

        bool inside = false;
        for (const CollisionAreaRef3D entry : areas) {
            if (!entry) {
                continue;
            }
            if (entry->area.contains_point(SDL_Point{ candidate.x, candidate.z })) {
                inside = true;
                break;
            }
        }

        if (!inside) {
            best = candidate;
            break;
        }
    }

    return best;
}

} // namespace

std::vector<axis::WorldPos> PathSanitizer3D::sanitize(
    const Asset& self,
    const std::vector<axis::WorldPos>& absolute_checkpoints,
    int visited_thresh_px) const {
    std::vector<axis::WorldPos> sanitized;
    if (absolute_checkpoints.empty()) {
        return sanitized;
    }

    const auto collision_areas = gather_collision_areas(self);
    const axis::WorldPos origin{ self.world_x(), self.world_y(), self.world_z() };
    const long long thresh_sq = static_cast<long long>(visited_thresh_px) * visited_thresh_px;
    const Assets* assets = self.get_assets();
    const int resolution_layer = self.grid_resolution;

    for (const axis::WorldPos& checkpoint : absolute_checkpoints) {
        const axis::WorldPos anchor = sanitized.empty() ? origin : sanitized.back();
        if (thresh_sq > 0 && animation_update::detail::distance_sq_3d(anchor, checkpoint) <= thresh_sq) {
            continue;
        }

        axis::WorldPos candidate = checkpoint;
        for (const CollisionAreaRef3D entry : collision_areas) {
            if (!entry) {
                continue;
            }
            if (entry->area.contains_point(SDL_Point{ candidate.x, candidate.z })) {
                candidate = nudge_outside_xz(candidate, entry->area);
            }
        }

        if (segment_hits_any(anchor, candidate, resolution_layer, collision_areas)) {
            candidate = walk_back_to_perimeter(anchor, candidate, collision_areas);
        }

        if (same_world_point(anchor, candidate)) {
            continue;
        }

        const world::GridPoint anchor_gp = as_grid_point(anchor, resolution_layer);
        const world::GridPoint candidate_gp = as_grid_point(candidate, resolution_layer);
        const world::GridPoint anchor_bottom = animation_update::detail::bottom_middle_for(self, anchor_gp);
        const world::GridPoint candidate_bottom = animation_update::detail::bottom_middle_for(self, candidate_gp);

        if (!animation_update::detail::bottom_point_inside_playable_area(assets, candidate_bottom)) {
            continue;
        }
        if (animation_update::detail::segment_leaves_playable_area(assets, anchor_bottom, candidate_bottom)) {
            continue;
        }

        if (thresh_sq > 0 && animation_update::detail::distance_sq_3d(anchor, candidate) <= thresh_sq) {
            continue;
        }

        sanitized.push_back(candidate);
    }

    return sanitized;
}
