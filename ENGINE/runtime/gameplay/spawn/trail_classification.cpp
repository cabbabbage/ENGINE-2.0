#include "gameplay/spawn/trail_classification.hpp"

#include <algorithm>
#include <cctype>

namespace dynamic_spawn {

bool is_trail_area_label(const std::string& value) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lower.find("trail") != std::string::npos || lower.find("path") != std::string::npos;
}

} // namespace dynamic_spawn
