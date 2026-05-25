#pragma once

#include <optional>
#include <string>
#include <vector>

class AssetInfo;

namespace devmode {

enum class StackAnimationEditabilityReason {
    Editable,
    InheritedFromAnimationSource,
    MissingFrames,
    MissingAnimationSource,
    CyclicAnimationSource,
};

struct StackAnimationRow {
    std::string animation_id;
    bool editable_in_stack_mode = false;
    StackAnimationEditabilityReason reason = StackAnimationEditabilityReason::Editable;
    std::optional<std::string> resolved_edit_target_id;
    int source_chain_depth = 0;
};

struct StackAnimationListModel {
    std::string requested_animation_id;
    std::string resolved_animation_id;
    std::vector<StackAnimationRow> rows;
    bool requested_was_derived = false;
    bool used_fallback = false;

    bool has_selection() const { return !resolved_animation_id.empty(); }
};

struct FileSourcedAnimationSelection {
    std::string requested_animation_id;
    std::string resolved_animation_id;
    std::vector<std::string> navigable_animation_ids;
    bool requested_was_derived = false;
    bool used_fallback = false;

    bool has_selection() const { return !resolved_animation_id.empty(); }
};

FileSourcedAnimationSelection resolve_file_sourced_animation_selection(const AssetInfo* info,
                                                                       const std::string& requested_animation_id);

StackAnimationListModel resolve_stack_animation_list_model(const AssetInfo* info,
                                                           const std::string& requested_animation_id);

}  // namespace devmode
