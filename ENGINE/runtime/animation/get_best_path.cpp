#include "get_best_path.hpp"

#include <limits>
#include <string>

#include "assets/asset/Asset.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/animation_frame.hpp"
#include "assets/asset/asset_info.hpp"
#include "assets/asset/asset_types.hpp"
#include "animation_update.hpp"
#include "core/AssetsManager.hpp"
#include "utils/area.hpp"
#include "gameplay/world/grid_point.hpp"

namespace {
using CollisionEntryRef = const Assets::FrameCollisionEntry*;

std::vector<CollisionEntryRef> gather_collision_entries(const Asset& self) {
    std::vector<CollisionEntryRef> entries;
    const Assets* assets = self.get_assets();
    if (!assets) {
        return entries;
    }
    const int search_radius = (self.info && self.info->NeighborSearchRadius > 0)
        ? self.info->NeighborSearchRadius
        : 0;
    assets->query_impassable_entries(self, search_radius, entries);
    return entries;
}

bool blocked_step(const world::GridPoint& from,
                  const world::GridPoint& to,
                  const std::vector<CollisionEntryRef>& collisions,
                  const Asset& self,
                  const Assets* assets_owner) {
    const world::GridPoint start_bottom = animation_update::detail::bottom_middle_for(self, from);
    const world::GridPoint dest_bottom  = animation_update::detail::bottom_middle_for(self, to);

    if (animation_update::detail::segment_leaves_playable_area(assets_owner, start_bottom, dest_bottom)) {
        return true;
    }

    for (const CollisionEntryRef entry : collisions) {
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

        if (overlap_check) {
            if (animation_update::detail::distance_sq(dest_bottom, entry->bottom_middle) <
                animation_update::detail::kOverlapDistanceSq) {
                return true;
            }
        }
    }

    return false;
}

struct AnimationDescriptor {
    std::string id;
    const Animation* animation = nullptr;
    std::size_t path_index = 0;
    const std::vector<AnimationFrame>* frames    = nullptr;
    bool locked = false;
    int frame_count = 0;
};

std::vector<AnimationDescriptor> gather_movement_animations(const Asset& self) {
    std::vector<AnimationDescriptor> result;
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
            const int   frame_count = static_cast<int>(frames->size());
            if (frame_count <= 0) {
                continue;
            }

            bool has_motion = false;
            for (const auto& frame : *frames) {
                if (frame.dx != 0 || frame.dz != 0) {
                    has_motion = true;
                    break;
                }
            }
            if (!has_motion) {
                continue;
            }

            result.push_back(AnimationDescriptor{ id, &anim, path_index, frames, anim.locked, frame_count });
        }
    }

    return result;
}

struct CandidateStride {
    std::string animation_id;
    world::GridPoint end_position = world::GridPoint::make_virtual(0, 0, 0, 0);
    int frames = 0;
    int dist_sq = std::numeric_limits<int>::max();
    bool reaches = false;
    bool valid = false;
    std::size_t path_index = 0;
};

void copy_position(world::GridPoint& dst, const world::GridPoint& src) {
    dst.update_world_position(src.world_x(), src.world_y(), src.world_z());
}

}

Plan GetBestPath::operator()(const Asset& self,
                             const std::vector<SDL_Point>& sanitized_checkpoints,
                             int visited_thresh_px,
                             const vibble::grid::Grid& grid) const {
    Plan plan;
    plan.sanitized_checkpoints = sanitized_checkpoints;

    const int world_y = self.world_y();
    const int layer   = self.grid_resolution;
    std::vector<world::GridPoint> checkpoints;
    checkpoints.reserve(sanitized_checkpoints.size());
    for (const auto& cp : sanitized_checkpoints) {
        checkpoints.emplace_back(world::grid_math::from_sdl(cp, world_y, layer));
    }

    world::GridPoint cursor = world::grid_math::from_sdl(self.world_xz_point(), world_y, layer);
    plan.final_dest  = cursor.to_sdl_point();
    plan.world_start = cursor.to_sdl_point();

    if (!self.info) {
        return plan;
    }
    if (!self.isMovementEnabled()) {
        return plan;
    }

    const auto collisions  = gather_collision_entries(self);
    const Assets* assets   = self.get_assets();
    const int visited_sq   = visited_thresh_px * visited_thresh_px;
    auto movement_anims    = gather_movement_animations(self);

    bool aborted = false;
    for (const world::GridPoint& checkpoint : checkpoints) {
        if (visited_sq > 0 && animation_update::detail::distance_sq(cursor, checkpoint) <= visited_sq) {
            continue;
        }

        int safeguard = 0;
        while (animation_update::detail::distance_sq(cursor, checkpoint) > visited_sq) {
            CandidateStride best;
            const int current_dist_sq = animation_update::detail::distance_sq(cursor, checkpoint);

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
                    bool blocked = false;

                    for (int i = 0; i < frames; ++i) {
                        const AnimationFrame& frame = (*frames_path)[i];
                        SDL_Point delta = animation_update::detail::frame_world_delta(frame, self, grid);
                        world::GridPoint next = world::grid_math::offset(simulated, delta);
                        if (blocked_step(simulated, next, collisions, self, assets)) {
                            blocked = true;
                            break;
                        }
                        copy_position(simulated, next);
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
                        copy_position(best.end_position, simulated);
                        best.path_index   = descriptor.path_index;
                    }
                }
            }

            if (!best.valid) {
                CandidateStride fallback;
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
                        for (int i = 0; i < frames; ++i) {
                            const AnimationFrame& frame = (*frames_path)[i];
                            SDL_Point delta = animation_update::detail::frame_world_delta(frame, self, grid);
                            world::GridPoint next = world::grid_math::offset(simulated, delta);
                            copy_position(simulated, next);
                        }

                        const int dist_sq = animation_update::detail::distance_sq(simulated, checkpoint);
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
                            fallback.valid        = true;
                            fallback.reaches      = reaches;
                            fallback.animation_id = descriptor.id;
                            fallback.frames       = frames;
                            fallback.dist_sq      = dist_sq;
                            copy_position(fallback.end_position, simulated);
                            fallback.path_index   = descriptor.path_index;
                        }
                    }
                }

                if (fallback.valid) {
                    plan.strides.push_back(Stride{ fallback.animation_id, fallback.frames, fallback.path_index });
                    copy_position(cursor, fallback.end_position);
                    plan.final_dest = cursor.to_sdl_point();
                } else {
                    aborted = true;
                    break;
                }
            } else {
                plan.strides.push_back(Stride{ best.animation_id, best.frames, best.path_index });
                copy_position(cursor, best.end_position);
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
