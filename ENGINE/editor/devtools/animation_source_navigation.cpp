#include "devtools/animation_source_navigation.hpp"

#include <algorithm>
#include <unordered_set>

#include "assets/asset/animation.hpp"
#include "assets/asset/asset_info.hpp"

namespace devmode {

namespace {

bool animation_is_navigable(const Animation& animation) {
    return animation.source.kind != "animation" && !animation.frames.empty();
}

std::string first_navigable_animation_id(const AssetInfo& info) {
    std::vector<std::string> ids;
    ids.reserve(info.animations.size());
    for (const auto& [animation_id, animation] : info.animations) {
        if (animation_is_navigable(animation)) {
            ids.push_back(animation_id);
        }
    }
    std::sort(ids.begin(), ids.end());
    if (ids.empty()) {
        return {};
    }
    return ids.front();
}

}  // namespace

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
        if (animation.source.kind != "animation") {
            if (!animation.frames.empty()) {
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
