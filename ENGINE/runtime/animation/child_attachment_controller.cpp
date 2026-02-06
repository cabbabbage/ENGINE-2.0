#include "animation/child_attachment_controller.hpp"

#include <mutex>
#include <random>
#include <iostream>
#include <cmath>

#include "animation/child_attachment_math.hpp"
#include "animation/child_3d_world_position.hpp"

namespace {
constexpr bool kChildAttachmentDebug = false;

std::mt19937& child_rng() {
    static std::mt19937 rng{ std::random_device{}() };
    return rng;
}

std::mutex& child_rng_mutex() {
    static std::mutex m;
    return m;
}

const AnimationFrame* pick_start_frame(const Animation& animation) {
    const AnimationFrame* start = animation.get_first_frame();
    if (!start) {
        return nullptr;
    }
    const bool should_randomize =
        (animation.randomize || animation.rnd_start) && animation.frames.size() > 1;
    if (!should_randomize) {
        return start;
    }
    std::uniform_int_distribution<int> dist(0, static_cast<int>(animation.frames.size()) - 1);
    int idx = 0;
    {
        std::lock_guard<std::mutex> lock(child_rng_mutex());
        idx = dist(child_rng());
    }
    const AnimationFrame* frame = start;
    while (idx-- > 0 && frame && frame->next) {
        frame = frame->next;
    }
    return frame ? frame : start;
}
}

namespace animation_update::child_attachments {

void update_dimensions(Asset::AnimationChildAttachment& slot) {
    slot.cached_w = 0;
    slot.cached_h = 0;
    if (!slot.animation || !slot.current_frame || slot.current_frame->variants.empty()) {
        return;
    }
    SDL_Texture* texture = slot.current_frame->variants[0].base_texture;
    if (!texture) {
        return;
    }
    float width = 0.0f;
    float height = 0.0f;
    if (SDL_GetTextureSize(texture, &width, &height)) {
        slot.cached_w = static_cast<int>(std::lround(width));
        slot.cached_h = static_cast<int>(std::lround(height));
    }
}

void restart(Asset::AnimationChildAttachment& slot) {
    slot.frame_progress = 0.0f;
    slot.cached_w = 0;
    slot.cached_h = 0;
    slot.world_z = 0.0f;
    if (!slot.animation) {
        slot.current_frame = nullptr;
        return;
    }
    slot.current_frame = pick_start_frame(*slot.animation);
    update_dimensions(slot);
}

void advance_frames(std::vector<Asset::AnimationChildAttachment>& slots,
                    const ParentState& parent_state,
                    float dt) {
    if (slots.empty()) {
        return;
    }
    if (!(dt > 0.0f)) {
        dt = 1.0f / 60.0f;
    }
    for (auto& slot : slots) {
        if (!slot.animation || !slot.current_frame || !slot.visible || slot.child_index < 0) {
            continue;
        }
        const AnimationFrame* previous_frame = slot.current_frame;
        const float interval = 1.0f / static_cast<float>(kBaseAnimationFps);
        slot.frame_progress += dt;
        while (slot.frame_progress >= interval) {
            slot.frame_progress -= interval;
            if (slot.current_frame->next) {
                slot.current_frame = slot.current_frame->next;
            } else if (slot.animation->loop ||
                       parent_state.animation_id == animation_update::detail::kDefaultAnimation) {
                slot.current_frame = slot.animation->get_first_frame();
            } else {
                break;
            }
        }
        if (slot.current_frame != previous_frame) {
            update_dimensions(slot);
            if constexpr (kChildAttachmentDebug) {
                std::cout << "[ChildAttachments] Slot " << slot.child_index
                          << " advanced to frame "
                          << (slot.current_frame ? slot.current_frame->frame_index : -1) << " (asset='" << slot.asset_name << "')\n";
            }
        }
    }
}

void apply_frame_data(std::vector<Asset::AnimationChildAttachment>& slots,
                      const ParentState& parent_state,
                      const AnimationFrame* frame,
                      const std::vector<AnimationChildFrameData>* override_children) {
    if (slots.empty()) {
        return;
    }
    const float parent_scale = std::isfinite(parent_state.scale) && parent_state.scale > 0.0f ? parent_state.scale : 1.0f;
    const int parent_frame_index = frame ? frame->frame_index : -1;
    if constexpr (kChildAttachmentDebug) {
        std::cout << "[ChildAttachments] Applying frame data (parent_frame_index=" << parent_frame_index << ")\n";
    }
    for (auto& slot : slots) {
        const bool inactive = slot.child_index < 0;
        const bool parent_looped = parent_frame_index != -1 &&
                                   slot.last_parent_frame_index != -1 &&
                                   parent_frame_index < slot.last_parent_frame_index;
        if (parent_looped && !inactive) {
            restart(slot);
        }
        slot.last_parent_frame_index = parent_frame_index;
        slot.visible = false;
        slot.rotation_degrees = 0.0f;
        if (inactive) {
            continue;
        }
    }

    // Build parent 3D state for consistent world position calculations
    // Use floating-point anchor for maximum precision in 3D calculations
    const SDL_FPoint parent_anchor_float{
        static_cast<float>(parent_state.base_position.x),
        static_cast<float>(parent_state.base_position.y)
    };
    const child_3d::Parent3DState parent_3d_state{
        parent_anchor_float,
        parent_state.world_z,
        parent_scale,
        parent_state.flipped
    };

    // If override_children provided, use that; otherwise read from each slot's timeline
    if (override_children) {
        for (const auto& child_data : *override_children) {
            if (child_data.child_index < 0 ||
                child_data.child_index >= static_cast<int>(slots.size())) {
                continue;
            }
            auto& slot = slots[child_data.child_index];
            if (!slot.animation) {
                continue;
            }
            if (!child_data.visible) {
                slot.visible = false;
                continue;
            }
            const bool became_visible = !slot.was_visible;
            if (became_visible) {
                restart(slot);
            }
            slot.visible = true;

            // Calculate 3D world position from percentage offsets
            // Convert percentage to world displacement: world = height * percent
            const float parent_height = parent_state.height > 0.0f ? parent_state.height : 1.0f;
            const child_3d::Child3DDisplacement displacement{
                child_data.offset.px * parent_height,
                child_data.offset.py * parent_height,
                child_data.offset.pz * parent_height
            };
            const auto world_pos_3d = child_3d::calculate_child_world_position(
                parent_3d_state, displacement);

            // Store as integer screen coordinates for rendering
            slot.world_pos.x = static_cast<int>(std::lround(world_pos_3d.x));
            slot.world_pos.y = static_cast<int>(std::lround(world_pos_3d.y));
            slot.world_z = world_pos_3d.z;
            slot.rotation_degrees = mirrored_child_rotation(parent_state.flipped, child_data.degree);
        }
    } else {
        // Read directly from each slot's child_timelines entry
        for (auto& slot : slots) {
            if (slot.child_index < 0 || !slot.animation) {
                continue;
            }
            // Only static timelines have per-frame data
            if (!slot.timeline || !slot.timeline->is_static()) {
                continue;
            }
            const std::size_t frame_idx = (parent_frame_index >= 0) ? static_cast<std::size_t>(parent_frame_index) : 0;
            if (frame_idx >= slot.timeline->frames.size()) {
                continue;
            }
            const AnimationChildFrameData& child_data = slot.timeline->frames[frame_idx];
            if (!child_data.visible) {
                slot.visible = false;
                if constexpr (kChildAttachmentDebug) {
                    std::cout << "[ChildAttachments] Setting slot " << slot.child_index << " ('" << slot.asset_name
                              << "') visible=false (from timeline)\n";
                }
                continue;
            }
            const bool became_visible = !slot.was_visible;
            if (became_visible) {
                restart(slot);
            }
            slot.visible = true;
            if constexpr (kChildAttachmentDebug) {
                std::cout << "[ChildAttachments] Setting slot " << slot.child_index << " ('" << slot.asset_name
                          << "') visible=true px=" << child_data.offset.px << " py=" << child_data.offset.py
                          << " pz=" << child_data.offset.pz << " deg=" << child_data.degree << " (from timeline)\n";
            }

            // Calculate 3D world position from percentage offsets
            // Convert percentage to world displacement: world = height * percent
            const float parent_height = parent_state.height > 0.0f ? parent_state.height : 1.0f;
            const child_3d::Child3DDisplacement displacement{
                child_data.offset.px * parent_height,
                child_data.offset.py * parent_height,
                child_data.offset.pz * parent_height
            };
            const auto world_pos_3d = child_3d::calculate_child_world_position(
                parent_3d_state, displacement);

            // Store as integer screen coordinates for rendering, with precise Z
            slot.world_pos.x = static_cast<int>(std::lround(world_pos_3d.x));
            slot.world_pos.y = static_cast<int>(std::lround(world_pos_3d.y));
            slot.world_z = world_pos_3d.z;
            slot.rotation_degrees = mirrored_child_rotation(parent_state.flipped, child_data.degree);
        }
    }

    for (auto& slot : slots) {
        slot.was_visible = slot.visible;
    }
}

}
