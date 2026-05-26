#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <nlohmann/json.hpp>
#include "utils/display_color.hpp"
#include "utils/weighted_range.hpp"

namespace map_layers {

constexpr int kCandidateRangeMax = 128;

inline int clamp_candidate_min(int value) {
    return std::clamp(value, 0, kCandidateRangeMax);
}

inline int clamp_candidate_max(int min_value, int max_value) {
    const int clamped_min = clamp_candidate_min(min_value);
    return std::clamp(max_value, clamped_min, kCandidateRangeMax);
}

inline bool template_name_exists(const nlohmann::json& section,
                                 const std::string& candidate,
                                 const std::string& exclude_key = {}) {
    return section.is_object() && section.contains(candidate) && candidate != exclude_key;
}

inline std::string make_unique_template_key(const nlohmann::json& map_info,
                                            const std::string& preferred_base,
                                            const std::string& fallback_base,
                                            const std::string& exclude_key = {}) {
    const auto rooms_it = map_info.find("rooms_data");
    const auto trails_it = map_info.find("trails_data");
    const nlohmann::json empty = nlohmann::json::object();
    const nlohmann::json& rooms =
        (rooms_it != map_info.end() && rooms_it->is_object()) ? *rooms_it : empty;
    const nlohmann::json& trails =
        (trails_it != map_info.end() && trails_it->is_object()) ? *trails_it : empty;
    std::string base = preferred_base.empty() ? fallback_base : preferred_base;
    if (base.empty()) {
        base = "Template";
    }
    std::string candidate = base;
    int suffix = 1;
    while (template_name_exists(rooms, candidate, exclude_key) ||
           template_name_exists(trails, candidate, exclude_key)) {
        candidate = base + std::to_string(suffix++);
    }
    return candidate;
}

inline std::string create_room_entry(nlohmann::json& map_info) {
    if (!map_info.is_object()) {
        return std::string{};
    }
    nlohmann::json& rooms = map_info["rooms_data"];
    if (!rooms.is_object()) {
        rooms = nlohmann::json::object();
    }
    std::string key = make_unique_template_key(map_info, "NewRoom", "NewRoom");
    std::vector<SDL_Color> colors = utils::display_color::collect(rooms);
    nlohmann::json& entry = rooms[key];
    entry = nlohmann::json{
        {"name", key},
        {"geometry", "Square"},
        {"width", vibble::weighted_range::to_json(vibble::weighted_range::make_legacy_uniform(1200, 1800))},
        {"height", vibble::weighted_range::to_json(vibble::weighted_range::make_legacy_uniform(1200, 1800))},
        {"is_boss", false},
        {"inherits_live_dynamic_assets", false},
        {"inherit_map_floor_color", true},
        {"tags", nlohmann::json::array()},
        {"spawn_groups", nlohmann::json::array()},
    };
    utils::display_color::ensure(entry, colors);
    return key;
}

inline void rename_room_references_in_layers(nlohmann::json& map_info,
                                             const std::string& old_name,
                                             const std::string& new_name) {
    if (old_name == new_name) {
        return;
    }

    auto lit = map_info.find("map_layers");
    if (lit == map_info.end() || !lit->is_array()) {
        return;
    }

    for (auto& layer : *lit) {
        auto rooms_it = layer.find("rooms");
        if (rooms_it == layer.end() || !rooms_it->is_array()) {
            continue;
        }

        for (auto& entry : *rooms_it) {
            if (!entry.is_object()) {
                continue;
            }

            std::string source_type = entry.value("source_type", std::string());
            std::string value = entry.value("value", std::string());
            if (source_type.empty()) {
                // Legacy candidate shape.
                value = entry.value("name", std::string());
                source_type = "room_name";
            }
            if (source_type == "room_name" && value == old_name) {
                entry["source_type"] = "room_name";
                entry["value"] = new_name;
            }

            auto& children = entry["required_children"];
            if (!children.is_array()) {
                continue;
            }

            for (auto& child : children) {
                if (child.is_string() && child.get<std::string>() == old_name) {
                    child = new_name;
                }
            }
        }
    }
}

}
