#include "get_best_path.hpp"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <limits>
#include <string>

#include "asset/Asset.hpp"
#include "asset/animation.hpp"
#include "asset/animation_frame.hpp"
#include "asset/asset_info.hpp"
#include "asset/asset_types.hpp"
#include "animation_update.hpp"
#include "core/AssetsManager.hpp"
#include "core/asset_list.hpp"
#include "utils/area.hpp"
#include "world/grid_point.hpp"

namespace {
struct CollisionEntry {
    const Asset* asset = nullptr;
    Area         area{ "impassable" };
};

std::vector<CollisionEntry> gather_collision_entries(const Asset& self) {
    std::vector<CollisionEntry> entries;
    const AssetList* list = self.get_impassable_naighbors();
    if (!list) {
        return entries;
    }

    std::vector<Asset*> neighbors;
    list->full_list(neighbors);
    entries.reserve(neighbors.size());

    for (Asset* neighbor : neighbors) {
        if (!neighbor || neighbor == &self || !neighbor->info) {
            continue;
        }

        Area area = neighbor->get_area("impassable");
        if (area.get_points().empty()) {
            area = neighbor->get_area("collision_area");
        }
        if (area.get_points().empty()) {
            continue;
        }

        entries.push_back(CollisionEntry{ neighbor, std::move(area) });
    }

    return entries;
}

bool blocked_step(const world::GridPoint& from,
                  const world::GridPoint& to,
                  const std::vector<CollisionEntry>& collisions,
                  const Asset& self,
                  const Assets* assets_owner) {
    const world::GridPoint start_bottom = animation_update::detail::bottom_middle_for(self, from);
    const world::GridPoint dest_bottom  = animation_update::detail::bottom_middle_for(self, to);

    if (animation_update::detail::segment_leaves_playable_area(assets_owner, start_bottom, dest_bottom)) {
        return true;
    }

    for (const CollisionEntry& entry : collisions) {
        const Asset* other = entry.asset;
        if (!other || other == &self || !other->info) {
            continue;
        }

        if (animation_update::detail::segment_hits_area(from, to, entry.area)) {
            return true;
        }

        bool overlap_check = animation_update::detail::should_consider_overlap(self, *other);

        if (overlap_check) {
            const world::GridPoint other_bottom = animation_update::detail::bottom_middle_for(*other, world::grid_math::from_sdl(other->world_point(), other->world_z(), other->grid_resolution));
            if (animation_update::detail::distance_sq(dest_bottom, other_bottom) <
                animation_update::detail::kOverlapDistanceSq) {
                return true;
            }
        }
    }

    return false;
}

struct AnimationDescriptor {
    std::string                     id;
    const Animation*                animation   = nullptr;
    std::size_t                     path_index  = 0;
    const std::vector<AnimationFrame>* frames    = nullptr;
    bool                            locked      = false;
    int                             frame_count = 0;
};

std::vector<AnimationDescriptor> gather_movement_animations(const Asset& self) {
    std::vector<AnimationDescriptor> result;
    if (!self.info) {
        return result;
    }

    for (const auto& [id, anim] : self.info->animations) {
        const std::size_t path_count = anim.movement_path_count();
        for (std::size_t path_index = 0; path_index < path_count; ++path_index) {
            const auto& frames = anim.movement_path(path_index);
            const int   frame_count = static_cast<int>(frames.size());
            if (frame_count <= 0) {
                continue;
            }

            bool has_motion = false;
            for (const auto& frame : frames) {
                if (frame.dx != 0 || frame.dy != 0) {
                    has_motion = true;
                    break;
                }
            }
            if (!has_motion) {
                continue;
            }

            result.push_back(AnimationDescriptor{ id, &anim, path_index, &frames, anim.locked, frame_count });
        }
    }

    return result;
}

struct CandidateStride {
    std::string       animation_id;
    world::GridPoint  end_position = world::GridPoint::make_virtual(0, 0, 0, 0);
    int               frames   = 0;
    int               dist_sq  = std::numeric_limits<int>::max();
    bool              reaches  = false;
    bool              valid    = false;
    std::size_t       path_index = 0;
};

struct SmallestStride {
    std::string      anim_id;
    std::size_t      path_index = 0;
    world::GridPoint delta      = world::GridPoint::make_virtual(0, 0, 0, 0);
};

}

Plan GetBestPath::operator()(const Asset& self,
                             const std::vector<SDL_Point>& sanitized_checkpoints,
                             int visited_thresh_px,
                             const vibble::grid::Grid& grid) const {
    Plan plan;
    plan.sanitized_checkpoints = sanitized_checkpoints;

    const int world_z = self.world_z();
    const int layer   = self.grid_resolution;
    std::vector<world::GridPoint> checkpoints;
    checkpoints.reserve(sanitized_checkpoints.size());
    for (const auto& cp : sanitized_checkpoints) {
        checkpoints.emplace_back(world::grid_math::from_sdl(cp, world_z, layer));
    }

    world::GridPoint cursor = world::grid_math::from_sdl(self.world_point(), world_z, layer);
    plan.final_dest  = cursor.to_sdl_point();
    plan.world_start = cursor.to_sdl_point();

    if (!self.info) {
        return plan;
    }

    const auto collisions  = gather_collision_entries(self);
    const Assets* assets   = self.get_assets();
    const int visited_sq   = visited_thresh_px * visited_thresh_px;
    auto movement_anims    = gather_movement_animations(self);

    SmallestStride min_stride;
    int min_sum = std::numeric_limits<int>::max();
    for (const auto& descriptor : movement_anims) {
        const auto* frames_path = descriptor.frames;
        if (!frames_path) {
            continue;
        }
        for (const auto& frame : *frames_path) {
            SDL_Point delta = animation_update::detail::frame_world_delta(frame, self, grid);
            int sum = std::abs(delta.x) + std::abs(delta.y);
            if (sum > 0 && sum < min_sum) {
                min_sum = sum;
                min_stride = { descriptor.id, descriptor.path_index, world::GridPoint::make_virtual(delta.x, delta.y, 0, layer) };
            }
        }
    }

    bool aborted = false;
    for (const world::GridPoint& checkpoint : checkpoints) {
        if (visited_sq > 0 && animation_update::detail::distance_sq(cursor, checkpoint) <= visited_sq) {
            continue;
        }

        int safeguard = 0;
        while (animation_update::detail::distance_sq(cursor, checkpoint) > visited_sq) {
            CandidateStride best;
            const int       current_dist_sq = animation_update::detail::distance_sq(cursor, checkpoint);

            for (const auto& descriptor : movement_anims) {
                if (!descriptor.animation) {
                    continue;
                }

                const auto* frames_path = descriptor.frames;
                if (!frames_path) {
                    continue;
                }

                const int max_frames = descriptor.frame_count;
                if (max_frames <= 0) {
                    continue;
                }

                const int min_frames = descriptor.locked ? max_frames : 1;
                for (int frames = min_frames; frames <= max_frames; ++frames) {
                    world::GridPoint simulated = cursor;
                    bool             blocked   = false;

                    for (int i = 0; i < frames; ++i) {
                        const AnimationFrame& frame = (*frames_path)[i];
                        SDL_Point delta = animation_update::detail::frame_world_delta(frame, self, grid);
                        world::GridPoint next = world::grid_math::offset(simulated, delta);
                        if (blocked_step(simulated, next, collisions, self, assets)) {
                            blocked = true;
                            break;
                        }
                        simulated = std::move(next);
                    }

                    if (blocked) {
                        continue;
                    }

                    const int dist_sq = animation_update::detail::distance_sq(simulated, checkpoint);
                    const bool reaches = (visited_sq == 0) ? (dist_sq == 0) : (dist_sq <= visited_sq);
                    const bool progress = dist_sq < current_dist_sq;
                    if (!reaches && !progress) {
                        continue;
                    }

                    bool use_candidate = false;
                    if (!best.valid) {
                        use_candidate = true;
                    } else if (reaches != best.reaches) {
                        use_candidate = reaches;
                    } else if (reaches && frames < best.frames) {
                        use_candidate = true;
                    } else if (!reaches && dist_sq < best.dist_sq) {
                        use_candidate = true;
                    } else if (!reaches && dist_sq == best.dist_sq && frames < best.frames) {
                        use_candidate = true;
                    }

                    if (use_candidate) {
                        best.valid        = true;
                        best.reaches      = reaches;
                        best.animation_id = descriptor.id;
                        best.frames       = frames;
                        best.dist_sq      = dist_sq;
                        best.end_position = std::move(simulated);
                        best.path_index   = descriptor.path_index;
                    }
                }
            }

            if (!best.valid) {
                if (min_sum != std::numeric_limits<int>::max()) {
                    const SDL_Point delta_pt{ min_stride.delta.world_x(), min_stride.delta.world_y() };
                    world::GridPoint fallback_next = world::grid_math::offset(cursor, delta_pt);
                    const int       fallback_dist_sq = animation_update::detail::distance_sq(fallback_next, checkpoint);
                    if (fallback_dist_sq < current_dist_sq) {
                        plan.strides.push_back(Stride{ min_stride.anim_id, 1, min_stride.path_index });
                        cursor = std::move(fallback_next);
                        plan.final_dest = cursor.to_sdl_point();
                    } else {
                        aborted = true;
                        break;
                    }
                } else {
                    aborted = true;
                    break;
                }
            } else {
                plan.strides.push_back(Stride{ best.animation_id, best.frames, best.path_index });
                cursor = std::move(best.end_position);
                plan.final_dest = cursor.to_sdl_point();
            }

            if (++safeguard > 256) {
                break;
            }
        }

        if (aborted) {
            break;
        }
    }

    return plan;
}
