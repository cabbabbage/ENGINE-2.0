#include "startup_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace app::startup_runtime {

bool env_flag_enabled(const char* name, bool default_value) {
    if (!name || !*name) return default_value;
    const char* raw = std::getenv(name);
    if (!raw || !*raw) return default_value;
    std::string value(raw);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (value == "1" || value == "true" || value == "yes" || value == "on" || value == "y" || value == "t") return true;
    if (value == "0" || value == "false" || value == "no" || value == "off" || value == "n" || value == "f") return false;
    return default_value;
}

int env_int_clamped(const char* name, int default_value, int min_value, int max_value) {
    const int safe_min = std::min(min_value, max_value);
    const int safe_max = std::max(min_value, max_value);
    const int safe_default = std::clamp(default_value, safe_min, safe_max);
    if (!name || !*name) return safe_default;
    const char* raw = std::getenv(name);
    if (!raw || !*raw) return safe_default;
    try { return std::clamp(std::stoi(raw), safe_min, safe_max); } catch (...) { return safe_default; }
}

}
