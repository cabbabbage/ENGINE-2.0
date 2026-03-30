#pragma once

#include <string>
#include <vector>

class AssetInfo;

namespace devmode {

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

}  // namespace devmode
