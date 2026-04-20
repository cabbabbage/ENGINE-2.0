#include "get_best_path_3d.hpp"

#include <limits>
#include <string>

#include "animation_update.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/animation_frame.hpp"
#include "assets/asset/asset_info.hpp"
#include "assets/asset/asset_types.hpp"
#include "core/AssetsManager.hpp"
#include "gameplay/world/grid_point.hpp"
#include "utils/area.hpp"

namespace {

using CollisionEntryRef3D = CollisionQueryContext::CollisionEntryRef;

world::GridPoint as_grid_point(const axis::WorldPos& pos, int resolution_layer) {
    return world::GridPoint::make_virtual(pos.x, pos.y, pos.z, resolution_layer);
}

bool blocked_step(const world::GridPoint& from,
                  const world::GridPoint& to,
                  const std::vector<CollisionEntryRef3D>& collisions,
                  const Asset& self,
                  const Assets* assets_owner) {
    const world::GridPoint start_bottom = animation_update::detail::bottom_middle_for(self, from);
    const world::GridPoint dest_bottom  = animation_update::detail::bottom_middle_for(self, to);

    if (animation_update::detail::segment_leaves_playable_area(assets_owner, start_bottom, dest_bottom)) {
        return true;
    }

    for (const CollisionEntryRef3D entry : collisions) {
        if (!entry) {
            continue;
        }
        const Asset* other = entry->asset;
        if (!other || other == &self || !other->info) {
            continue;
        }

        if (animation_update::detail::segment_hits_area(from, to, entry->area)) {
            return true;
        }

        bool overlap_check = animation_update::detail::should_consider_overlap(self, *other);
        if (overlap_check &&
            animation_update::detail::distance_sq(dest_bottom, entry->bottom_middle) <
                animation_update::detail::kOverlapDistanceSq) {
            return true;
        }
    }

    return false;
}

struct AnimationDescriptor3D {
    std::string id;
    const Animation* animation = nullptr;
    std::size_t path_index = 0;
    const std::vector<AnimationFrame>* frames = nullptr;
    bool locked = false;
    int frame_count = 0;
};

std::vector<AnimationDescriptor3D> gather_movement_animations_3d(const Asset& self) {
    std::vector<AnimationDescriptor3D> result;
    if (!self.info || !self.isMovementEnabled()) {
        return result;
    }

    for (const auto& [id, anim] : self.info->animations) {
        const std::size_t path_count = anim.movement_path_count();
        for (std::size_t path_index = 0; path_index < path_count; ++path_index) {
            const auto* frames = &anim.movement_path(path_index);
            if (!frames) {
                continue;
            }
            const int frame_count = static_cast<int>(frames->size());
            if (frame_count <= 0) {
                continue;
            }

            bool has_motion = false;
            for (const auto& frame : *frames) {
                if (frame.dx != 0 || frame.dy != 0 || frame.dz != 0) {
                    has_motion = true;
                    break;
                }
            }
            if (!has_motion) {
                continue;
            }

            result.push_back(AnimationDescriptor3D{ id, &anim, path_index, frames, anim.locked, frame_count });
        }
    }

    return result;
}

struct CandidateStride3D {
    std::string animation_id;
    axis::WorldPos end_position{0, 0, 0};
    int frames = 0;
    long long dist_sq = std::numeric_limits<long long>::max();
    bool reaches = false;
    bool valid = false;
    std::size_t path_index = 0;
};

} // namespace

Plan3D GetBestPath3D::operator()(const Asset& self,
                                 const std::vector<axis::WorldPos>& sanitized_checkpoints,
                                 int visited_thresh_px,
                                 const vibble::grid::Grid& grid,
                                 CollisionQueryContext* collision_context) const {
    Plan3D plan;
    plan.sanitized_checkpoints = sanitized_checkpoints;
    plan.world_start = axis::WorldPos{ self.world_x(), self.world_y(), self.world_z() };
    plan.final_dest = plan.world_start;

    if (!self.info) {
        return plan;
    }
    if (!self.isMovementEnabled()) {
        return plan;
    }

    const int resolution_layer = self.grid_resolution;
    CollisionQueryContext local_collision_context;
    CollisionQueryContext& context = collision_context ? *collision_context : local_collision_context;
    const auto& collisions = context.collisions_for(self);
    const Assets* assets = self.get_assets();
    const long long visited_sq = static_cast<long long>(visited_thresh_px) * visited_thresh_px;
    const auto movement_anims = gather_movement_animations_3d(self);

    axis::WorldPos cursor = plan.world_start;
    bool aborted = false;
    for (const axis::WorldPos& checkpoint : sanitized_checkpoints) {
        if (visited_sq > 0 && animation_update::detail::distance_sq_3d(cursor, checkpoint) <= visited_sq) {
            continue;
        }

        int safeguard = 0;
        while (animation_update::detail::distance_sq_3d(cursor, checkpoint) > visited_sq) {
            CandidateStride3D best;
            const long long current_dist_sq = animation_update::detail::distance_sq_3d(cursor, checkpoint);

            for (const auto& descriptor : movement_anims) {
                if (!descriptor.animation || !descriptor.frames || descriptor.frame_count <= 0) {
                    continue;
                }

                const int max_frames = descriptor.frame_count;
                const int min_frames = descriptor.locked ? max_frames : 1;
                for (int frames = min_frames; frames <= max_frames; ++frames) {
                    axis::WorldPos simulated = cursor;
                    bool blocked = false;

                    for (int i = 0; i < frames; ++i) {
                        const AnimationFrame& frame = (*descriptor.frames)[i];
                        const axis::WorldPos delta = animation_update::detail::frame_world_delta_3d(frame, self, grid);
                        const axis::WorldPos next_pos{
                            simulated.x + delta.x,
                            simulated.y + delta.y,
                            simulated.z + delta.z
                        };

                        const world::GridPoint simulated_gp = as_grid_point(simulated, resolution_layer);
                        const world::GridPoint next_gp = as_grid_point(next_pos, resolution_layer);
                        if (blocked_step(simulated_gp, next_gp, collisions, self, assets)) {
                            blocked = true;
                            break;
                        }
                        simulated = next_pos;
                    }

                    if (blocked) {
                        continue;
                    }

                    const long long dist_sq = animation_update::detail::distance_sq_3d(simulated, checkpoint);
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
                        best.valid = true;
                        best.reaches = reaches;
                        best.animation_id = descriptor.id;
                        best.frames = frames;
                        best.dist_sq = dist_sq;
                        best.end_position = simulated;
                        best.path_index = descriptor.path_index;
                    }
                }
            }

            if (!best.valid) {
                CandidateStride3D fallback;
                for (const auto& descriptor : movement_anims) {
                    if (!descriptor.animation || !descriptor.frames || descriptor.frame_count <= 0) {
                        continue;
                    }

                    const int max_frames = descriptor.frame_count;
                    const int min_frames = descriptor.locked ? max_frames : 1;
                    for (int frames = min_frames; frames <= max_frames; ++frames) {
                        axis::WorldPos simulated = cursor;
                        bool blocked = false;
                        for (int i = 0; i < frames; ++i) {
                            const AnimationFrame& frame = (*descriptor.frames)[i];
                            const axis::WorldPos delta = animation_update::detail::frame_world_delta_3d(frame, self, grid);
                            const axis::WorldPos next_pos{ simulated.x + delta.x, simulated.y + delta.y, simulated.z + delta.z };
                            const world::GridPoint simulated_gp = as_grid_point(simulated, resolution_layer);
                            const world::GridPoint next_gp = as_grid_point(next_pos, resolution_layer);
                            if (blocked_step(simulated_gp, next_gp, collisions, self, assets)) {
                                blocked = true;
                                break;
                            }
                            simulated = next_pos;
                        }

                        if (blocked) {
                            continue;
                        }

                        const long long dist_sq = animation_update::detail::distance_sq_3d(simulated, checkpoint);
                        const bool reaches = (visited_sq == 0) ? (dist_sq == 0) : (dist_sq <= visited_sq);
                        const bool progress = dist_sq < current_dist_sq;
                        if (!reaches && !progress) {
                            continue;
                        }

                        bool use_candidate = false;
                        if (!fallback.valid) {
                            use_candidate = true;
                        } else if (reaches != fallback.reaches) {
                            use_candidate = reaches;
                        } else if (reaches && frames < fallback.frames) {
                            use_candidate = true;
                        } else if (!reaches && dist_sq < fallback.dist_sq) {
                            use_candidate = true;
                        } else if (!reaches && dist_sq == fallback.dist_sq && frames < fallback.frames) {
                            use_candidate = true;
                        }

                        if (use_candidate) {
                            fallback.valid = true;
                            fallback.reaches = reaches;
                            fallback.animation_id = descriptor.id;
                            fallback.frames = frames;
                            fallback.dist_sq = dist_sq;
                            fallback.end_position = simulated;
                            fallback.path_index = descriptor.path_index;
                        }
                    }
                }

                if (fallback.valid) {
                    plan.strides.push_back(Stride{ fallback.animation_id, fallback.frames, fallback.path_index });
                    cursor = fallback.end_position;
                    plan.final_dest = cursor;
                } else {
                    aborted = true;
                    break;
                }
            } else {
                plan.strides.push_back(Stride{ best.animation_id, best.frames, best.path_index });
                cursor = best.end_position;
                plan.final_dest = cursor;
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
