#include "devtools/animation_source_navigation.hpp"

#include <algorithm>
#include <optional>
#include <unordered_set>

#include "assets/asset/animation.hpp"
#include "assets/asset/asset_info.hpp"

namespace devmode {

namespace {

StackAnimationRow describe_stack_animation_row(const AssetInfo& info, const std::string& animation_id) {
    StackAnimationRow row;
    row.animation_id = animation_id;

    auto animation_it = info.animations.find(animation_id);
    if (animation_it == info.animations.end()) {
        row.editable_in_stack_mode = false;
        row.reason = StackAnimationEditabilityReason::MissingAnimationSource;
        return row;
    }

    const Animation& animation = animation_it->second;
    if (animation.source.kind == "animation" && animation.inherit_data) {
        row.editable_in_stack_mode = false;
        row.reason = StackAnimationEditabilityReason::InheritedFromAnimationSource;
        std::string source_id = animation.source.name;
        if (source_id.empty()) {
            source_id = animation.source.path;
        }
        if (!source_id.empty()) {
            row.resolved_edit_target_id = source_id;
        }
        return row;
    }

    if (!animation.has_frames()) {
        row.editable_in_stack_mode = false;
        row.reason = StackAnimationEditabilityReason::MissingFrames;
        return row;
    }

    row.editable_in_stack_mode = true;
    row.reason = StackAnimationEditabilityReason::Editable;
    row.resolved_edit_target_id = animation_id;
    return row;
}

}  // namespace

StackAnimationListModel resolve_stack_animation_list_model(const AssetInfo* info,
                                                           const std::string& requested_animation_id) {
    StackAnimationListModel result;
    result.requested_animation_id = requested_animation_id;
    if (!info) {
        return result;
    }

    std::vector<std::string> all_ids;
    all_ids.reserve(info->animations.size());
    for (const auto& [animation_id, _] : info->animations) {
        (void)_;
        all_ids.push_back(animation_id);
    }
    std::sort(all_ids.begin(), all_ids.end());

    result.rows.reserve(all_ids.size());
    for (const std::string& id : all_ids) {
        result.rows.push_back(describe_stack_animation_row(*info, id));
    }

    auto row_for = [&](const std::string& id) -> const StackAnimationRow* {
        for (const auto& row : result.rows) {
            if (row.animation_id == id) {
                return &row;
            }
        }
        return nullptr;
    };

    if (!requested_animation_id.empty() && row_for(requested_animation_id)) {
        result.resolved_animation_id = requested_animation_id;
        const StackAnimationRow* requested_row = row_for(requested_animation_id);
        if (requested_row) {
            result.requested_was_derived =
                requested_row->reason == StackAnimationEditabilityReason::InheritedFromAnimationSource;
        }
        return result;
    }

    for (const auto& row : result.rows) {
        if (row.editable_in_stack_mode) {
            result.resolved_animation_id = row.animation_id;
            result.used_fallback = true;
            break;
        }
    }
    return result;
}

FileSourcedAnimationSelection resolve_file_sourced_animation_selection(const AssetInfo* info,
                                                                       const std::string& requested_animation_id) {
    FileSourcedAnimationSelection result;
    result.requested_animation_id = requested_animation_id;
    if (!info) {
        return result;
    }

    result.navigable_animation_ids.reserve(info->animations.size());
    for (const auto& [animation_id, animation] : info->animations) {
        if (!animation.has_frames()) {
            continue;
        }
        if (animation.source.kind != "animation" || !animation.inherit_data) {
            result.navigable_animation_ids.push_back(animation_id);
        }
    }
    std::sort(result.navigable_animation_ids.begin(), result.navigable_animation_ids.end());

    if (result.navigable_animation_ids.empty()) {
        return result;
    }

    std::string current_id = requested_animation_id;
    if (current_id.empty() || info->animations.find(current_id) == info->animations.end()) {
        result.resolved_animation_id = result.navigable_animation_ids.front();
        result.used_fallback = true;
        return result;
    }

    std::unordered_set<std::string> visited;
    while (!current_id.empty()) {
        if (!visited.insert(current_id).second) {
            break;
        }

        auto animation_it = info->animations.find(current_id);
        if (animation_it == info->animations.end()) {
            break;
        }

        const Animation& animation = animation_it->second;
        if (animation.source.kind != "animation" || !animation.inherit_data) {
            if (animation.has_frames()) {
                result.resolved_animation_id = current_id;
                result.requested_was_derived = (requested_animation_id != current_id);
                return result;
            }
            break;
        }

        result.requested_was_derived = true;
        std::string next_id = animation.source.name;
        if (next_id.empty()) {
            next_id = animation.source.path;
        }
        if (next_id.empty()) {
            break;
        }
        current_id = next_id;
    }

    result.resolved_animation_id = result.navigable_animation_ids.front();
    result.used_fallback = true;
    return result;
}

}  // namespace devmode
