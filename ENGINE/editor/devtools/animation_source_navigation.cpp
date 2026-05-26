#include "devtools/animation_source_navigation.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "assets/asset/animation.hpp"
#include "assets/asset/asset_info.hpp"

namespace devmode {

namespace {

bool animation_is_inherited_noneditable(const Animation& animation) {
    return animation.source.kind == "animation" && animation.inherit_data;
}

bool animation_is_navigable(const Animation& animation) {
    if (!animation.has_frames()) {
        return false;
    }
    return !animation_is_inherited_noneditable(animation);
}

struct NodeInfo {
    std::string id;
    std::optional<std::string> parent;
    bool missing_source = false;
    std::vector<std::string> children;
};

struct FlattenedNode {
    std::string id;
    int level = 0;
    bool missing_source = false;
};

std::vector<FlattenedNode> build_animation_hierarchy(const AssetInfo& info) {
    std::vector<std::string> ids;
    ids.reserve(info.animations.size());
    for (const auto& [id, _] : info.animations) {
        (void)_;
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());

    std::unordered_set<std::string> id_set(ids.begin(), ids.end());
    std::unordered_map<std::string, NodeInfo> nodes_by_id;
    nodes_by_id.reserve(ids.size());

    for (const auto& id : ids) {
        NodeInfo node;
        node.id = id;

        auto animation_it = info.animations.find(id);
        if (animation_it != info.animations.end()) {
            const Animation& animation = animation_it->second;
            if (animation.source.kind == "animation") {
                std::string parent_id = animation.source.name;
                if (parent_id.empty()) {
                    parent_id = animation.source.path;
                }
                if (!parent_id.empty()) {
                    if (parent_id == id) {
                        node.missing_source = true;
                    } else if (id_set.count(parent_id) != 0) {
                        node.parent = parent_id;
                    } else {
                        node.missing_source = true;
                    }
                }
            }
        }

        nodes_by_id.emplace(id, std::move(node));
    }

    for (auto& [id, node] : nodes_by_id) {
        (void)id;
        if (!node.parent.has_value()) {
            continue;
        }
        auto parent_it = nodes_by_id.find(*node.parent);
        if (parent_it != nodes_by_id.end()) {
            parent_it->second.children.push_back(node.id);
        }
    }

    for (auto& [id, node] : nodes_by_id) {
        (void)id;
        std::sort(node.children.begin(), node.children.end());
    }

    std::vector<std::string> roots;
    roots.reserve(nodes_by_id.size());
    for (const auto& [id, node] : nodes_by_id) {
        if (!node.parent.has_value() || nodes_by_id.find(*node.parent) == nodes_by_id.end()) {
            roots.push_back(id);
        }
    }
    std::sort(roots.begin(), roots.end());

    std::vector<FlattenedNode> flattened;
    flattened.reserve(nodes_by_id.size());
    std::unordered_set<std::string> visited;
    visited.reserve(nodes_by_id.size());

    std::function<void(const std::string&, int)> visit = [&](const std::string& id, int level) {
        if (visited.count(id) != 0) {
            return;
        }
        auto it = nodes_by_id.find(id);
        if (it == nodes_by_id.end()) {
            visited.insert(id);
            return;
        }
        visited.insert(id);
        flattened.push_back(FlattenedNode{id, level, it->second.missing_source});
        for (const auto& child_id : it->second.children) {
            visit(child_id, level + 1);
        }
    };

    for (const auto& root : roots) {
        visit(root, 0);
    }
    for (const auto& id : ids) {
        if (visited.count(id) == 0) {
            visit(id, 0);
        }
    }

    return flattened;
}

}  // namespace

StackAnimationListModel resolve_stack_animation_list_model(const AssetInfo* info,
                                                           const std::string& requested_animation_id) {
    StackAnimationListModel result;
    result.requested_animation_id = requested_animation_id;
    if (!info) {
        return result;
    }

    auto hierarchy = build_animation_hierarchy(*info);
    result.rows.reserve(hierarchy.size());

    for (const auto& node : hierarchy) {
        StackAnimationRow row;
        row.animation_id = node.id;
        row.level = std::max(0, node.level);
        row.missing_source = node.missing_source;

        auto animation_it = info->animations.find(node.id);
        if (animation_it == info->animations.end()) {
            row.editable_in_stack_mode = false;
            row.reason = StackAnimationEditabilityReason::MissingAnimationSource;
        } else if (animation_is_inherited_noneditable(animation_it->second)) {
            row.editable_in_stack_mode = false;
            row.reason = StackAnimationEditabilityReason::InheritedFromAnimationSource;
            result.requested_was_derived = result.requested_was_derived || (requested_animation_id == node.id);
        } else if (!animation_it->second.has_frames()) {
            row.editable_in_stack_mode = false;
            row.reason = StackAnimationEditabilityReason::MissingFrames;
        } else {
            row.editable_in_stack_mode = true;
            row.reason = StackAnimationEditabilityReason::Editable;
        }
        result.rows.push_back(std::move(row));
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
            if (requested_row->editable_in_stack_mode) {
                result.resolved_animation_id = requested_animation_id;
                return result;
            }
            if (requested_row->reason == StackAnimationEditabilityReason::InheritedFromAnimationSource) {
                result.requested_was_derived = true;
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
    FileSourcedAnimationSelection result;
    result.requested_animation_id = requested_animation_id;
    if (!info) {
        return result;
    }

    result.navigable_animation_ids.reserve(info->animations.size());
    for (const auto& [animation_id, animation] : info->animations) {
        if (animation_is_navigable(animation)) {
            result.navigable_animation_ids.push_back(animation_id);
        }
    }
    std::sort(result.navigable_animation_ids.begin(), result.navigable_animation_ids.end());

    if (result.navigable_animation_ids.empty()) {
        return result;
    }

    if (!requested_animation_id.empty()) {
        auto requested_it = info->animations.find(requested_animation_id);
        if (requested_it != info->animations.end()) {
            const Animation& requested_animation = requested_it->second;
            if (animation_is_navigable(requested_animation)) {
                result.resolved_animation_id = requested_animation_id;
                return result;
            }
            result.requested_was_derived = animation_is_inherited_noneditable(requested_animation);
        }
    }

    result.resolved_animation_id = result.navigable_animation_ids.front();
    result.used_fallback = true;
    return result;
}

}  // namespace devmode
