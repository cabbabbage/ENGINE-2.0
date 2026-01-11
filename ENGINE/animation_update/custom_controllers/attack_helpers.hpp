#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "asset/Asset.hpp"
#include "core/asset_list.hpp"
#include "animation_update/attack_validation.hpp"
#include "animation_update/animation_update.hpp"

namespace animation_update {
namespace custom_controllers {

namespace attack_helpers {

namespace detail {

inline bool contains_asset(const std::vector<Asset*>& bucket, Asset* target) {
    return std::find(bucket.begin(), bucket.end(), target) != bucket.end();
}

inline bool target_is_neighbor(const AssetList* list, Asset* target) {
    if (!list) {
        return false;
    }
    if (contains_asset(list->top_unsorted(), target) ||
        contains_asset(list->middle_sorted(), target) ||
        contains_asset(list->bottom_unsorted(), target)) {
        return true;
    }
    for (const auto& child : list->children()) {
        if (child && target_is_neighbor(child.get(), target)) {
            return true;
        }
    }
    return false;
}

} // namespace detail

inline float sanitize_scale(float value) {
    return (std::isfinite(value) && value > 0.0f) ? value : 1.0f;
}

inline GeometryContext geometry_for(const Asset& asset) {
    GeometryContext context{};
    context.anchor = animation_update::detail::bottom_middle_for(asset, asset.pos);
    context.scale = sanitize_scale(asset.smoothed_scale());
    context.flipped = asset.flipped;
    context.plane = CombatPlane::XY;
    return context;
}

inline CombatantSnapshot snapshot_from_asset(const Asset& asset) {
    CombatantSnapshot snapshot;
    snapshot.asset_id = asset.spawn_id.empty() ? (asset.info ? asset.info->name : std::string{}) : asset.spawn_id;
    snapshot.asset_name = asset.info ? asset.info->name : std::string{};
    snapshot.frame = asset.current_animation_frame();
    snapshot.transform = geometry_for(asset);
    return snapshot;
}

inline bool send_attack_if_hit(Asset* attacker, Asset* target) {
    if (!attacker || !target || attacker == target) {
        return false;
    }
    if (!attacker->info || !target->info) {
        return false;
    }
    if (!attacker->current_animation_frame() || !target->current_animation_frame()) {
        return false;
    }
    if (attacker->dead || target->dead || !attacker->active || !target->active) {
        return false;
    }

    const AssetList* neighbors = attacker->get_neighbors_list();
    if (neighbors && !detail::target_is_neighbor(neighbors, target)) {
        return false;
    }

    CombatantSnapshot attacker_snapshot = snapshot_from_asset(*attacker);
    CombatantSnapshot target_snapshot = snapshot_from_asset(*target);
    const std::vector<ChildAttachmentSnapshot> child_frames{};
    auto attack =
        AttackValidation::compute_attack_if_hit(attacker_snapshot, target_snapshot, child_frames);
    if (!attack.has_value()) {
        return false;
    }

    target->send_attack(*attack);
    return true;
}

} // namespace attack_helpers

} // namespace custom_controllers
} // namespace animation_update
