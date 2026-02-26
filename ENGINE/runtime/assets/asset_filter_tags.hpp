#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace asset_filters {

inline std::string canonicalize_spawn_method(std::string_view method) {
    std::string canonical;
    canonical.reserve(method.size());
    for (unsigned char ch : method) {
        if (std::isalnum(ch)) {
            canonical.push_back(static_cast<char>(std::tolower(ch)));
        } else if (std::isspace(ch) || ch == '_' || ch == '-') {
            if (canonical.empty() || canonical.back() == '_') {
                continue;
            }
            canonical.push_back('_');
        }
    }
    return canonical;
}

} // namespace asset_filters
