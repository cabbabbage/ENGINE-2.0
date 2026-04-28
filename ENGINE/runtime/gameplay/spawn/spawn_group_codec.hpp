#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "utils/grid.hpp"
#include "utils/map_grid_settings.hpp"
#include "utils/string_utils.hpp"

namespace vibble::spawn_group_codec {

constexpr int kPerimeterRadiusDefault = 200;
constexpr int kDefaultMinNumber = 1;
constexpr int kPerimeterMinNumber = 2;
constexpr int kEdgeInsetPercentMin = 0;
constexpr int kEdgeInsetPercentMax = 200;
constexpr int kEdgeInsetPercentDefault = 100;
constexpr int kDefaultPositionY = 0;

struct CandidateDefaults {
    std::string name = "null";
    double chance = 0.0;
};

struct EntryDefaults {
    std::string display_name = "New Spawn";
    std::optional<int> resolution = std::nullopt;
    CandidateDefaults default_candidate{};
};

namespace detail {

inline bool set_field(nlohmann::json& obj, const char* key, nlohmann::json value) {
    if (!obj.is_object() || key == nullptr) return false;
    const auto it = obj.find(key);
    if (it != obj.end() && *it == value) {
        return false;
    }
    obj[key] = std::move(value);
    return true;
}

inline bool erase_field(nlohmann::json& obj, const char* key) {
    if (!obj.is_object() || key == nullptr) return false;
    const auto it = obj.find(key);
    if (it == obj.end()) return false;
    obj.erase(it);
    return true;
}

inline bool is_integral(double value) {
    if (!std::isfinite(value)) return false;
    return std::fabs(value - std::round(value)) < 1e-9;
}

inline int read_int_like(const nlohmann::json& value, int fallback) {
    if (value.is_number_integer()) {
        return value.get<int>();
    }
    if (value.is_number_float()) {
        const double parsed = value.get<double>();
        if (std::isfinite(parsed)) {
            return static_cast<int>(std::lround(parsed));
        }
        return fallback;
    }
    if (value.is_string()) {
        try {
            const std::string text = value.get<std::string>();
            size_t consumed = 0;
            const int parsed = std::stoi(text, &consumed);
            if (consumed == text.size()) {
                return parsed;
            }
        } catch (...) {
        }
    }
    return fallback;
}

inline double read_number_like(const nlohmann::json& value, double fallback) {
    if (value.is_number_float()) {
        return value.get<double>();
    }
    if (value.is_number_integer()) {
        return static_cast<double>(value.get<int>());
    }
    if (value.is_string()) {
        try {
            const std::string text = value.get<std::string>();
            size_t consumed = 0;
            const double parsed = std::stod(text, &consumed);
            if (consumed == text.size()) {
                return parsed;
            }
        } catch (...) {
        }
    }
    return fallback;
}

inline bool read_bool_like(const nlohmann::json& value, bool fallback) {
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        return value.get<int>() != 0;
    }
    if (value.is_string()) {
        std::string text = value.get<std::string>();
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (text == "true" || text == "1" || text == "yes") return true;
        if (text == "false" || text == "0" || text == "no") return false;
    }
    return fallback;
}

} // namespace detail

inline std::string generate_spawn_id() {
    static std::mt19937 rng(std::random_device{}());
    static const char* hex = "0123456789abcdef";
    std::uniform_int_distribution<int> dist(0, 15);
    std::string id = "spn-";
    for (int i = 0; i < 12; ++i) {
        id.push_back(hex[dist(rng)]);
    }
    return id;
}

inline nlohmann::json& ensure_spawn_groups_array(nlohmann::json& root) {
    if (root.is_array()) {
        return root;
    }
    if (!root.is_object()) {
        root = nlohmann::json::object();
    }
    if (root.contains("spawn_groups") && root["spawn_groups"].is_array()) {
        return root["spawn_groups"];
    }
    root["spawn_groups"] = nlohmann::json::array();
    return root["spawn_groups"];
}

inline const nlohmann::json* find_spawn_groups_array(const nlohmann::json& root) {
    if (root.is_array()) {
        return &root;
    }
    if (root.is_object() && root.contains("spawn_groups") && root["spawn_groups"].is_array()) {
        return &root["spawn_groups"];
    }
    return nullptr;
}

inline std::string normalize_method(std::string method, const std::string& fallback = "Random") {
    if (method == "Exact Position") {
        method = "Exact";
    }
    if (method.empty()) {
        return fallback;
    }
    return method;
}

inline std::string normalize_method_from_entry(const nlohmann::json& entry,
                                               const char* key = "position",
                                               const std::string& fallback = "Random") {
    if (!entry.is_object() || key == nullptr) {
        return fallback;
    }
    const auto it = entry.find(key);
    if (it == entry.end() || !it->is_string()) {
        return fallback;
    }
    return normalize_method(it->get<std::string>(), fallback);
}

inline bool is_exact_method(const std::string& method) {
    return normalize_method(method) == "Exact";
}

inline bool uses_geometry_resolution_by_default(const std::string& method) {
    const std::string normalized = normalize_method(method);
    return normalized == "Exact" || normalized == "Perimeter";
}

inline int read_int_field(const nlohmann::json& obj, const char* key, int fallback) {
    if (!obj.is_object() || key == nullptr) return fallback;
    const auto it = obj.find(key);
    if (it == obj.end()) return fallback;
    return detail::read_int_like(*it, fallback);
}

inline double read_number_field(const nlohmann::json& obj, const char* key, double fallback) {
    if (!obj.is_object() || key == nullptr) return fallback;
    const auto it = obj.find(key);
    if (it == obj.end()) return fallback;
    return detail::read_number_like(*it, fallback);
}

inline bool read_bool_field(const nlohmann::json& obj, const char* key, bool fallback) {
    if (!obj.is_object() || key == nullptr) return fallback;
    const auto it = obj.find(key);
    if (it == obj.end()) return fallback;
    return detail::read_bool_like(*it, fallback);
}

inline std::string read_string_field(const nlohmann::json& obj,
                                     const char* key,
                                     const std::string& fallback = {}) {
    if (!obj.is_object() || key == nullptr) return fallback;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

inline double read_candidate_chance(const nlohmann::json& candidate, double fallback = 0.0) {
    if (!candidate.is_object()) return fallback;
    const auto chance_it = candidate.find("chance");
    if (chance_it != candidate.end()) {
        return detail::read_number_like(*chance_it, fallback);
    }
    const auto weight_it = candidate.find("weight");
    if (weight_it != candidate.end()) {
        return detail::read_number_like(*weight_it, fallback);
    }
    return fallback;
}

inline bool is_null_candidate_name(std::string_view name) {
    const std::string trimmed = vibble::strings::trim_copy(std::string(name));
    if (trimmed.size() != 4) {
        return false;
    }
    return std::tolower(static_cast<unsigned char>(trimmed[0])) == 'n' &&
           std::tolower(static_cast<unsigned char>(trimmed[1])) == 'u' &&
           std::tolower(static_cast<unsigned char>(trimmed[2])) == 'l' &&
           std::tolower(static_cast<unsigned char>(trimmed[3])) == 'l';
}

inline bool is_null_candidate_entry(const nlohmann::json& candidate) {
    if (!candidate.is_object()) {
        return false;
    }
    const auto name_it = candidate.find("name");
    if (name_it == candidate.end() || !name_it->is_string()) {
        return false;
    }
    return is_null_candidate_name(name_it->get<std::string>());
}

inline bool sanitize_spawn_group_candidates(nlohmann::json& entry,
                                            const CandidateDefaults& defaults = CandidateDefaults{}) {
    bool changed = false;
    if (!entry.is_object()) {
        entry = nlohmann::json::object();
        changed = true;
    }

    if (!entry.contains("candidates") || !entry["candidates"].is_array()) {
        entry["candidates"] = nlohmann::json::array();
        changed = true;
    }

    auto& candidates = entry["candidates"];
    nlohmann::json sanitized = nlohmann::json::array();
    bool has_null_candidate = false;
    for (auto& candidate : candidates) {
        if (!candidate.is_object()) {
            changed = true;
            continue;
        }

        nlohmann::json sanitized_candidate = candidate;
        std::string name = "null";
        if (sanitized_candidate.contains("name") && sanitized_candidate["name"].is_string()) {
            name = sanitized_candidate["name"].get<std::string>();
        }
        if (is_null_candidate_name(name) || name.empty()) {
            const bool canonical_name = (name == "null");
            sanitized_candidate["name"] = "null";
            if (has_null_candidate) {
                changed = true;
                continue;
            }
            has_null_candidate = true;
            if (!canonical_name || name.empty()) {
                changed = true;
            }
        }

        const double previous_chance = read_number_field(sanitized_candidate, "chance", std::numeric_limits<double>::quiet_NaN());
        double chance = read_candidate_chance(sanitized_candidate, defaults.chance);
        if (!std::isfinite(chance) || chance < 0.0) {
            chance = 0.0;
        }

        if (!std::isfinite(previous_chance) || std::fabs(previous_chance - chance) > 1e-9) {
            changed = true;
        }

        if (detail::is_integral(chance)) {
            sanitized_candidate["chance"] = static_cast<int>(std::llround(chance));
        } else {
            sanitized_candidate["chance"] = chance;
        }

        sanitized.push_back(std::move(sanitized_candidate));
    }

    if (sanitized.empty()) {
        nlohmann::json fallback_candidate = nlohmann::json::object();
        fallback_candidate["name"] = defaults.name.empty() ? std::string("null") : defaults.name;
        double fallback_chance = defaults.chance;
        if (!std::isfinite(fallback_chance) || fallback_chance < 0.0) {
            fallback_chance = 0.0;
        }
        if (detail::is_integral(fallback_chance)) {
            fallback_candidate["chance"] = static_cast<int>(std::llround(fallback_chance));
        } else {
            fallback_candidate["chance"] = fallback_chance;
        }
        sanitized.push_back(std::move(fallback_candidate));
        changed = true;
    }

    if (!has_null_candidate) {
        bool found_existing_null = false;
        for (const auto& candidate : sanitized) {
            if (is_null_candidate_entry(candidate)) {
                found_existing_null = true;
                break;
            }
        }
        if (!found_existing_null) {
            nlohmann::json null_candidate = nlohmann::json::object();
            null_candidate["name"] = "null";
            null_candidate["chance"] = 0;
            sanitized.push_back(std::move(null_candidate));
            changed = true;
        }
    }

    if (sanitized != candidates) {
        candidates = std::move(sanitized);
        changed = true;
    }

    return changed;
}

inline bool sanitize_perimeter_edge_fields(nlohmann::json& entry) {
    if (!entry.is_object()) return false;

    bool changed = false;
    const bool randomize_y = read_bool_field(entry, "randomize_y", false);
    int position_y = read_int_field(entry, "position_y", kDefaultPositionY);
    int min_y = read_int_field(entry, "min_y", position_y);
    int max_y = read_int_field(entry, "max_y", position_y);
    if (max_y < min_y) {
        std::swap(min_y, max_y);
    }
    changed = detail::set_field(entry, "randomize_y", randomize_y) || changed;
    if (randomize_y) {
        changed = detail::set_field(entry, "min_y", min_y) || changed;
        changed = detail::set_field(entry, "max_y", max_y) || changed;
        changed = detail::erase_field(entry, "position_y") || changed;
    } else {
        changed = detail::set_field(entry, "position_y", position_y) || changed;
        changed = detail::erase_field(entry, "min_y") || changed;
        changed = detail::erase_field(entry, "max_y") || changed;
    }

    const std::string method = normalize_method_from_entry(entry);
    changed = detail::set_field(entry, "position", method) || changed;

    if (method == "Perimeter") {
        int min_number = read_int_field(entry, "min_number",
                                        read_int_field(entry, "max_number", kPerimeterMinNumber));
        int max_number = read_int_field(entry, "max_number", min_number);
        min_number = std::max(kPerimeterMinNumber, min_number);
        max_number = std::max(min_number, max_number);
        changed = detail::set_field(entry, "min_number", min_number) || changed;
        changed = detail::set_field(entry, "max_number", max_number) || changed;
        changed = detail::erase_field(entry, "edge_inset_percent") || changed;
    } else if (method == "Edge") {
        int min_number = read_int_field(entry, "min_number",
                                        read_int_field(entry, "max_number", kDefaultMinNumber));
        int max_number = read_int_field(entry, "max_number", min_number);
        min_number = std::max(kDefaultMinNumber, min_number);
        max_number = std::max(min_number, max_number);
        int inset = read_int_field(entry, "edge_inset_percent", kEdgeInsetPercentDefault);
        inset = std::clamp(inset, kEdgeInsetPercentMin, kEdgeInsetPercentMax);
        changed = detail::set_field(entry, "min_number", min_number) || changed;
        changed = detail::set_field(entry, "max_number", max_number) || changed;
        changed = detail::set_field(entry, "edge_inset_percent", inset) || changed;
    } else {
        changed = detail::erase_field(entry, "edge_inset_percent") || changed;
    }

    return changed;
}

inline bool ensure_spawn_group_entry_defaults(nlohmann::json& entry,
                                              const EntryDefaults& defaults = EntryDefaults{}) {
    bool changed = false;
    if (!entry.is_object()) {
        entry = nlohmann::json::object();
        changed = true;
    }

    const std::string display_fallback =
        defaults.display_name.empty() ? std::string("New Spawn") : defaults.display_name;

    std::string spawn_id = read_string_field(entry, "spawn_id");
    if (spawn_id.empty()) {
        spawn_id = generate_spawn_id();
        changed = detail::set_field(entry, "spawn_id", spawn_id) || changed;
    }

    std::string display_name = read_string_field(entry, "display_name");
    if (display_name.empty()) {
        display_name = display_fallback;
        changed = detail::set_field(entry, "display_name", display_name) || changed;
    }

    const std::string method = normalize_method_from_entry(entry);
    changed = detail::set_field(entry, "position", method) || changed;

    int min_default = (method == "Perimeter") ? kPerimeterMinNumber : kDefaultMinNumber;
    int min_number = read_int_field(entry, "min_number", min_default);
    int max_number = read_int_field(entry, "max_number", min_number);

    min_number = std::max(min_default, min_number);
    max_number = std::max(min_number, max_number);

    if (method == "Exact") {
        int quantity = read_int_field(entry, "quantity", min_number);
        quantity = std::max(1, quantity);
        min_number = quantity;
        max_number = quantity;
        changed = detail::set_field(entry, "quantity", quantity) || changed;
    }

    changed = detail::set_field(entry, "min_number", min_number) || changed;
    changed = detail::set_field(entry, "max_number", max_number) || changed;

    if (method != "Exact") {
        changed = detail::erase_field(entry, "quantity") || changed;
    }

    if (method == "Edge") {
        int inset = read_int_field(entry, "edge_inset_percent", kEdgeInsetPercentDefault);
        inset = std::clamp(inset, kEdgeInsetPercentMin, kEdgeInsetPercentMax);
        changed = detail::set_field(entry, "edge_inset_percent", inset) || changed;
    } else {
        changed = detail::erase_field(entry, "edge_inset_percent") || changed;
    }

    if (method == "Perimeter") {
        const int radius =
            read_int_field(entry, "radius", read_int_field(entry, "perimeter_radius", kPerimeterRadiusDefault));
        changed = detail::set_field(entry, "radius", radius) || changed;
        changed = detail::set_field(entry, "perimeter_radius", radius) || changed;
    }

    const bool spacing = read_bool_field(entry, "enforce_spacing", false);
    changed = detail::set_field(entry, "enforce_spacing", spacing) || changed;

    const bool resolve_geometry = read_bool_field(entry,
                                                  "resolve_geometry_to_room_size",
                                                  uses_geometry_resolution_by_default(method));
    changed = detail::set_field(entry, "resolve_geometry_to_room_size", resolve_geometry) || changed;

    const bool resolve_quantity = read_bool_field(entry, "resolve_quantity_to_room_size", false);
    changed = detail::set_field(entry, "resolve_quantity_to_room_size", resolve_quantity) || changed;

    const bool locked = read_bool_field(entry, "locked", false);
    changed = detail::set_field(entry, "locked", locked) || changed;

    const int fallback_resolution = defaults.resolution
        ? vibble::grid::clamp_resolution(*defaults.resolution)
        : vibble::grid::clamp_resolution(MapGridSettings::defaults().grid_resolution);
    int resolution = read_int_field(entry, "resolution", fallback_resolution);
    resolution = vibble::grid::clamp_resolution(resolution);
    changed = detail::set_field(entry, "resolution", resolution) || changed;

    const bool explicit_flip = read_bool_field(entry, "explicit_flip", false);
    changed = detail::set_field(entry, "explicit_flip", explicit_flip) || changed;
    const bool force_flipped = read_bool_field(entry, "force_flipped", false);
    changed = detail::set_field(entry, "force_flipped", force_flipped) || changed;

    const bool randomize_y = read_bool_field(entry, "randomize_y", false);
    int position_y = read_int_field(entry, "position_y", kDefaultPositionY);
    int min_y = read_int_field(entry, "min_y", position_y);
    int max_y = read_int_field(entry, "max_y", position_y);
    if (max_y < min_y) {
        std::swap(min_y, max_y);
    }
    changed = detail::set_field(entry, "randomize_y", randomize_y) || changed;
    if (randomize_y) {
        changed = detail::set_field(entry, "min_y", min_y) || changed;
        changed = detail::set_field(entry, "max_y", max_y) || changed;
        changed = detail::erase_field(entry, "position_y") || changed;
    } else {
        changed = detail::set_field(entry, "position_y", position_y) || changed;
        changed = detail::erase_field(entry, "min_y") || changed;
        changed = detail::erase_field(entry, "max_y") || changed;
    }

    if (sanitize_spawn_group_candidates(entry, defaults.default_candidate)) {
        changed = true;
    }

    return changed;
}

} // namespace vibble::spawn_group_codec
