#pragma once

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace room_editor_test_payload_contract {

inline nlohmann::json make_full_mixed_animation_payload() {
    return nlohmann::json::object({
        {"source", nlohmann::json::object({{"kind", "folder"}, {"path", "default"}})},
        {"inherit_data", false},
        {"derived_modifiers", nlohmann::json::object({{"reverse", false}})},
        {"on_end", "default"},
        {"anchor_points", nlohmann::json::array({
                              nlohmann::json::array({
                                  nlohmann::json::object({
                                      {"name", "a0"},
                                      {"texture_x", 1},
                                      {"texture_y", 2},
                                      {"depth_offset", 3.0},
                                      {"flip_horizontal", true},
                                      {"flip_vertical", true},
                                      {"rotation_degrees", 0.0},
                                      {"hidden", false},
                                      {"resolve_x", true},
                                      {"scaling_method", "parent"},
                                  })}),
                          })},
        {"movement", nlohmann::json::array({nlohmann::json::array({1, 2, 3, 4.0})})},
        {"movement_total", nlohmann::json::object({{"dx", 1}, {"dy", 2}, {"dz", 3.0}, {"dr", 4.0}})},
        {"oval_anchor_mappings", nlohmann::json::array({nlohmann::json::object({
                                      {"name", "oval_1"},
                                      {"asset_name", "asset_a"},
                                      {"width_radius_x", 32.0},
                                      {"height_radius_z", 16.0},
                                      {"center_texture_x", 0},
                                      {"center_texture_y", 0},
                                      {"center_depth_offset", 0.0},
                                      {"points", nlohmann::json::array()},
                                  })})},
        {"hit_boxes", nlohmann::json::array({nlohmann::json::array({
                          nlohmann::json::object({
                              {"id", "hit_a"},
                              {"name", "hit_a"},
                              {"enabled", true},
                              {"type", "hitbox"},
                              {"x", 0},
                              {"y", 0},
                              {"w", 10},
                              {"h", 10},
                          })}),
                      })},
        {"attack_boxes", nlohmann::json::array({nlohmann::json::array({
                             nlohmann::json::object({
                                 {"id", "atk_a"},
                                 {"name", "atk_a"},
                                 {"enabled", true},
                                 {"type", "attack_box"},
                                 {"x", 0},
                                 {"y", 0},
                                 {"w", 10},
                                 {"h", 10},
                             })}),
                         })},
        {"box_schema_version", 1},
        {"custom_metadata", nlohmann::json::object({{"token", "keep_me"}})},
    });
}

inline std::unordered_map<std::string, std::string> snapshot_key_bytes(const nlohmann::json& payload) {
    std::unordered_map<std::string, std::string> snapshot;
    if (!payload.is_object()) {
        return snapshot;
    }
    snapshot.reserve(payload.size());
    for (auto it = payload.begin(); it != payload.end(); ++it) {
        snapshot.emplace(it.key(), it.value().dump());
    }
    return snapshot;
}

inline std::vector<std::string> object_keys(const nlohmann::json& payload) {
    std::vector<std::string> keys;
    if (!payload.is_object()) {
        return keys;
    }
    keys.reserve(payload.size());
    for (auto it = payload.begin(); it != payload.end(); ++it) {
        keys.push_back(it.key());
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

inline std::vector<std::string> unchanged_keys_excluding(const nlohmann::json& payload,
                                                         const std::vector<std::string>& changed_keys) {
    std::unordered_set<std::string> changed(changed_keys.begin(), changed_keys.end());
    std::vector<std::string> keys;
    if (!payload.is_object()) {
        return keys;
    }
    for (auto it = payload.begin(); it != payload.end(); ++it) {
        if (changed.find(it.key()) == changed.end()) {
            keys.push_back(it.key());
        }
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

}  // namespace room_editor_test_payload_contract
