#include "get_best_path_3d.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "animation_update.hpp"
#include "animation_tag_utils.hpp"
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
                  const Assets* assets_owner,
                  const animation_update::detail::PathBlockingContext& context) {
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

        const std::string other_id = animation_update::detail::stable_asset_id(*other);
        const bool is_engagement_target =
            context.allow_engagement_target_overlap &&
            context.engagement_target_asset_id.has_value() &&
            !other_id.empty() &&
            other_id == *context.engagement_target_asset_id;
        if (!is_engagement_target && animation_update::detail::segment_hits_area(from, to, entry->area)) {
            return true;
        }

        const int overlap_distance_sq =
            animation_update::detail::overlap_distance_sq_for_pair(self, *other, context);
        if (overlap_distance_sq > 0 &&
            animation_update::detail::distance_sq(dest_bottom, entry->bottom_middle) < overlap_distance_sq) {
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

struct MovementAnimationBuckets3D {
    std::vector<AnimationDescriptor3D> locomotion;
    std::vector<AnimationDescriptor3D> attack;
};

MovementAnimationBuckets3D gather_movement_animations_3d(const Asset& self) {
    MovementAnimationBuckets3D buckets;
    if (!self.info || !self.isMovementEnabled()) {
        return buckets;
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

            AnimationDescriptor3D descriptor{ id, &anim, path_index, frames, anim.locked, frame_count };
            if (animation_update::tag_utils::has_normalized_tag(anim.tags, "attack")) {
                buckets.attack.push_back(std::move(descriptor));
            } else {
                buckets.locomotion.push_back(std::move(descriptor));
            }
        }
    }

    return buckets;
}

bool descriptor_matches_tag_filter(const AnimationDescriptor3D& descriptor,
                                   const CollisionQueryContext& context) {
    if (!descriptor.animation) {
        return false;
    }

    const auto& tags = descriptor.animation->tags;
    for (const std::string& required_tag : context.required_animation_tags) {
        if (!animation_update::tag_utils::has_normalized_tag(tags, required_tag)) {
            return false;
        }
    }

    for (const std::string& excluded_tag : context.excluded_animation_tags) {
        if (animation_update::tag_utils::has_normalized_tag(tags, excluded_tag)) {
            return false;
        }
    }

    return true;
}

struct CandidateStride3D {
    std::string animation_id;
    axis::WorldPos end_position{0, 0, 0};
    int frames = 0;
    long long dist_sq = std::numeric_limits<long long>::max();
    bool reaches = false;
    bool valid = false;
    std::size_t path_index = 0;
    std::uint64_t tie_breaker = 0;
};

std::uint64_t fnv1a_64(std::string_view text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : text) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint64_t mix64(std::uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

std::uint64_t stride_tie_breaker_seed(std::uint64_t path_seed_base,
                                      std::size_t checkpoint_index,
                                      const AnimationDescriptor3D& descriptor,
                                      int frames) {
    std::uint64_t seed = path_seed_base;
    seed ^= mix64(static_cast<std::uint64_t>(checkpoint_index) * 0x9e3779b97f4a7c15ULL);
    seed ^= mix64(static_cast<std::uint64_t>(descriptor.path_index + 1) * 0xd6e8feb86659fd93ULL);
    seed ^= mix64(static_cast<std::uint64_t>(frames + 1) * 0xa0761d6478bd642fULL);
    seed ^= fnv1a_64(descriptor.id);
    return mix64(seed);
}

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
    plan.engagement_target_asset_id = context.engagement_target_asset_id;
    plan.attacking_enabled = plan.engagement_target_asset_id.has_value();
    const auto& collisions = context.collisions_for(self);
    const Assets* assets = self.get_assets();
    const animation_update::detail::PathBlockingContext blocking_context{
        context.engagement_target_asset_id,
        plan.attacking_enabled
    };
    const long long visited_sq = static_cast<long long>(visited_thresh_px) * visited_thresh_px;
    const MovementAnimationBuckets3D animation_buckets = gather_movement_animations_3d(self);
    std::vector<AnimationDescriptor3D> movement_anims;
    movement_anims.reserve(animation_buckets.locomotion.size() + animation_buckets.attack.size());
    for (const auto& descriptor : animation_buckets.locomotion) {
        if (descriptor_matches_tag_filter(descriptor, context)) {
            movement_anims.push_back(descriptor);
        }
    }
    for (const auto& descriptor : animation_buckets.attack) {
        if (descriptor_matches_tag_filter(descriptor, context)) {
            movement_anims.push_back(descriptor);
        }
    }

    axis::WorldPos cursor = plan.world_start;
    const std::uint64_t path_seed_base = context.path_variance_seed;
    bool aborted = false;
    for (std::size_t checkpoint_index = 0; checkpoint_index < sanitized_checkpoints.size(); ++checkpoint_index) {
        const axis::WorldPos& checkpoint = sanitized_checkpoints[checkpoint_index];
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
                        if (blocked_step(simulated_gp, next_gp, collisions, self, assets, blocking_context)) {
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
                    const std::uint64_t tie_breaker =
                        stride_tie_breaker_seed(path_seed_base, checkpoint_index, descriptor, frames);

                    bool use_candidate = false;
                    if (!best.valid) {
                        use_candidate = true;
                    } else if (reaches != best.reaches) {
                        use_candidate = reaches;
                    } else if (reaches && frames < best.frames) {
                        use_candidate = true;
                    } else if (reaches && frames == best.frames && tie_breaker < best.tie_breaker) {
                        use_candidate = true;
                    } else if (!reaches && dist_sq < best.dist_sq) {
                        use_candidate = true;
                    } else if (!reaches && dist_sq == best.dist_sq && frames < best.frames) {
                        use_candidate = true;
                    } else if (!reaches && dist_sq == best.dist_sq && frames == best.frames &&
                               tie_breaker < best.tie_breaker) {
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
                        best.tie_breaker = tie_breaker;
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
                            if (blocked_step(simulated_gp, next_gp, collisions, self, assets, blocking_context)) {
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
                        const std::uint64_t tie_breaker =
                            stride_tie_breaker_seed(path_seed_base, checkpoint_index, descriptor, frames);

                        bool use_candidate = false;
                        if (!fallback.valid) {
                            use_candidate = true;
                        } else if (reaches != fallback.reaches) {
                            use_candidate = reaches;
                        } else if (reaches && frames < fallback.frames) {
                            use_candidate = true;
                        } else if (reaches && frames == fallback.frames &&
                                   tie_breaker < fallback.tie_breaker) {
                            use_candidate = true;
                        } else if (!reaches && dist_sq < fallback.dist_sq) {
                            use_candidate = true;
                        } else if (!reaches && dist_sq == fallback.dist_sq && frames < fallback.frames) {
                            use_candidate = true;
                        } else if (!reaches && dist_sq == fallback.dist_sq && frames == fallback.frames &&
                                   tie_breaker < fallback.tie_breaker) {
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
                            fallback.tie_breaker = tie_breaker;
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

namespace get_best_path_3d::test_hooks {

AnimationTagBuckets3D classify_animation_tag_buckets(const Asset& self) {
    AnimationTagBuckets3D result;
    const MovementAnimationBuckets3D buckets = gather_movement_animations_3d(self);
    result.locomotion_animation_ids.reserve(buckets.locomotion.size());
    result.attack_animation_ids.reserve(buckets.attack.size());

    for (const auto& descriptor : buckets.locomotion) {
        result.locomotion_animation_ids.push_back(descriptor.id);
    }
    for (const auto& descriptor : buckets.attack) {
        result.attack_animation_ids.push_back(descriptor.id);
    }

    return result;
}

} // namespace get_best_path_3d::test_hooks
