#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "utils/utils/string_utils.hpp"

namespace animation_update::tag_utils {

inline std::string normalize_tag(std::string_view tag) {
    return vibble::strings::to_lower_copy(vibble::strings::trim_copy(tag));
}

inline bool has_normalized_tag(const std::vector<std::string>& tags, std::string_view expected_tag) {
    const std::string expected = normalize_tag(expected_tag);
    if (expected.empty()) {
        return false;
    }

    for (const std::string& tag : tags) {
        if (normalize_tag(tag) == expected) {
            return true;
        }
    }

    return false;
}

} // namespace animation_update::tag_utils
