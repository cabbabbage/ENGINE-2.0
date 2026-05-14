#pragma once

#include <algorithm>
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

inline std::string create_room_entry(nlohmann::json& map_info) {
    if (!map_info.is_object()) {
        return std::string{};
    }
    nlohmann::json& rooms = map_info["rooms_data"];
    if (!rooms.is_object()) {
        rooms = nlohmann::json::object();
    }
    std::string base = "NewRoom";
    std::string key = base;
    int suffix = 1;
    while (rooms.contains(key)) {
        key = base + std::to_string(suffix++);
    }
    std::vector<SDL_Color> colors = utils::display_color::collect(rooms);
    nlohmann::json& entry = rooms[key];
    entry = nlohmann::json{
        {"name", key},
        {"geometry", "Square"},
        {"width", vibble::weighted_range::to_json(vibble::weighted_range::make_legacy_uniform(1200, 1800))},
        {"height", vibble::weighted_range::to_json(vibble::weighted_range::make_legacy_uniform(1200, 1800))},
        {"edge_smoothness", 4},
        {"curvyness", vibble::weighted_range::to_json(vibble::weighted_range::make_flat(2))},
        {"is_boss", false},
        {"inherits_map_assets", false},
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
                entry["name"] = new_name; // Backward-compat mirror for older editor/runtime readers.
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

