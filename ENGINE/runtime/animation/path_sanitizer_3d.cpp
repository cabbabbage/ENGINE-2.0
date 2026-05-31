#include "path_sanitizer_3d.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include "animation_update.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_info.hpp"
#include "assets/asset/asset_types.hpp"
#include "collision_query_context.hpp"
#include "core/AssetsManager.hpp"
#include "gameplay/world/grid_point.hpp"
#include "utils/area.hpp"

using CollisionAreaRef3D = const Assets::FrameCollisionEntry*;

namespace {

world::GridPoint as_grid_point(const axis::WorldPos& pos, int resolution_layer) {
    return world::GridPoint::make_virtual(pos.x, pos.y, pos.z, resolution_layer);
}

std::string normalized_metadata_token(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char ch : value) {
        if (std::isalnum(ch)) {
            normalized.push_back(static_cast<char>(std::tolower(ch)));
        } else if (!normalized.empty() && normalized.back() != '_') {
            normalized.push_back('_');
        }
    }
    while (!normalized.empty() && normalized.back() == '_') {
        normalized.pop_back();
    }
    return normalized;
}

bool contains_token(std::string_view value, std::string_view token) {
    return normalized_metadata_token(value).find(token) != std::string::npos;
}

bool metadata_has_any(const AssetInfo& info, const std::vector<std::string>& tags,
                      const std::array<std::string_view, 12>& tokens) {
    const auto matches = [&tokens](std::string_view value) {
        for (const std::string_view token : tokens) {
            if (contains_token(value, token)) {
                return true;
            }
        }
        return false;
    };

    if (matches(info.name)) {
        return true;
    }
    for (const std::string& tag : tags) {
        if (matches(tag)) {
            return true;
        }
    }
    return false;
}

bool explicitly_blocks_gameplay(const AssetInfo& info) {
    static constexpr std::array<std::string_view, 12> kBlockingTokens{
        "block", "blocking", "blocker", "obstacle", "solid", "wall",
        "barrier", "collider", "collision", "impassable", "unwalkable", "gameplay_obstacle"
    };

    return metadata_has_any(info, info.tags, kBlockingTokens);
}

bool looks_like_scenery(const AssetInfo& info) {
    static constexpr std::array<std::string_view, 12> kSceneryTokens{
        "scenery", "decor", "decoration", "foliage", "grass", "flower",
        "bush", "shrub", "plant", "leaf", "leaves", "vegetation"
    };

    return metadata_has_any(info, info.tags, kSceneryTokens);
}

bool should_avoid_asset_obstacle(const Asset& self, CollisionAreaRef3D entry) {
    if (!entry || !entry->asset || entry->asset == &self || !entry->asset->info) {
        return false;
    }

    const Asset& other = *entry->asset;
    const AssetInfo& info = *other.info;
    const std::string canonical_type = asset_types::canonicalize(info.type);
    if (canonical_type == asset_types::boundary) {
        return true;
    }

    if (explicitly_blocks_gameplay(info)) {
        return true;
    }

    if (looks_like_scenery(info)) {
        return false;
    }

    if (other.isMovementEnabled()) {
        return true;
    }

    return canonical_type == asset_types::enemy || canonical_type == asset_types::npc;
}

std::vector<CollisionAreaRef3D> filtered_asset_obstacles(
    const Asset& self,
    const std::vector<CollisionAreaRef3D>& collision_areas) {
    std::vector<CollisionAreaRef3D> filtered;
    filtered.reserve(collision_areas.size());
    for (CollisionAreaRef3D entry : collision_areas) {
        if (should_avoid_asset_obstacle(self, entry)) {
            filtered.push_back(entry);
        }
    }
    return filtered;
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
    int visited_thresh_px,
    CollisionQueryContext* collision_context) const {
    std::vector<axis::WorldPos> sanitized;
    if (absolute_checkpoints.empty()) {
        return sanitized;
    }

    const Assets* assets = self.get_assets();
    const axis::WorldPos origin{ self.world_x(), self.world_y(), self.world_z() };
    int furthest_checkpoint_distance_px = 0;
    for (const axis::WorldPos& checkpoint : absolute_checkpoints) {
        const long long dx = static_cast<long long>(checkpoint.x) - static_cast<long long>(origin.x);
        const long long dz = static_cast<long long>(checkpoint.z) - static_cast<long long>(origin.z);
        const int distance = static_cast<int>(std::lround(std::sqrt(static_cast<double>(dx * dx + dz * dz))));
        furthest_checkpoint_distance_px = std::max(furthest_checkpoint_distance_px, distance);
    }
    CollisionQueryContext local_collision_context;
    CollisionQueryContext& context = collision_context ? *collision_context : local_collision_context;
    if (!collision_context) {
        context.set_furthest_checkpoint_distance_px(furthest_checkpoint_distance_px);
    }
    const auto collision_areas = filtered_asset_obstacles(self, context.collisions_for(self));
    const long long thresh_sq = static_cast<long long>(visited_thresh_px) * visited_thresh_px;
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
