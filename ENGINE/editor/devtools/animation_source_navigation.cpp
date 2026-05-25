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

    std::string current_id = animation_id;
    std::unordered_set<std::string> visited;
    while (!current_id.empty()) {
        if (!visited.insert(current_id).second) {
            row.editable_in_stack_mode = false;
            row.reason = StackAnimationEditabilityReason::CyclicAnimationSource;
            return row;
        }

        auto current_it = info.animations.find(current_id);
        if (current_it == info.animations.end()) {
            row.editable_in_stack_mode = false;
            row.reason = StackAnimationEditabilityReason::MissingAnimationSource;
            return row;
        }

        const Animation& animation = current_it->second;
        if (animation.source.kind != "animation" || !animation.inherit_data) {
            if (!animation.has_frames()) {
                row.editable_in_stack_mode = false;
                row.reason = StackAnimationEditabilityReason::MissingFrames;
                return row;
            }
            row.editable_in_stack_mode = true;
            row.reason = StackAnimationEditabilityReason::Editable;
            row.resolved_edit_target_id = current_id;
            return row;
        }

        row.reason = StackAnimationEditabilityReason::InheritedFromAnimationSource;
        ++row.source_chain_depth;

        std::string next_id = animation.source.name;
        if (next_id.empty()) {
            next_id = animation.source.path;
        }
        if (next_id.empty()) {
            row.editable_in_stack_mode = false;
            row.reason = StackAnimationEditabilityReason::MissingAnimationSource;
            return row;
        }
        current_id = next_id;
    }

    row.editable_in_stack_mode = false;
    row.reason = StackAnimationEditabilityReason::MissingAnimationSource;
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

    if (!requested_animation_id.empty()) {
        const StackAnimationRow* requested_row = row_for(requested_animation_id);
        if (requested_row) {
            result.requested_was_derived =
                requested_row->reason == StackAnimationEditabilityReason::InheritedFromAnimationSource ||
                (requested_row->resolved_edit_target_id.has_value() &&
                 *requested_row->resolved_edit_target_id != requested_animation_id);
            if (requested_row->editable_in_stack_mode) {
                result.resolved_animation_id = requested_row->animation_id;
                return result;
            }
            if (requested_row->resolved_edit_target_id.has_value()) {
                const StackAnimationRow* resolved_row = row_for(*requested_row->resolved_edit_target_id);
                if (resolved_row && resolved_row->editable_in_stack_mode) {
                    result.resolved_animation_id = resolved_row->animation_id;
                    result.used_fallback = (result.resolved_animation_id != requested_animation_id);
                    return result;
                }
            }
        } else {
            result.used_fallback = true;
        }
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
    const StackAnimationListModel model = resolve_stack_animation_list_model(info, requested_animation_id);
    FileSourcedAnimationSelection result;
    result.requested_animation_id = requested_animation_id;
    result.resolved_animation_id = model.resolved_animation_id;
    result.requested_was_derived = model.requested_was_derived;
    result.used_fallback = model.used_fallback;

    if (!info) {
        return result;
    }

    result.navigable_animation_ids.reserve(model.rows.size());
    for (const auto& row : model.rows) {
        if (row.editable_in_stack_mode) {
            result.navigable_animation_ids.push_back(row.animation_id);
        }
    }
    return result;
}

}  // namespace devmode
