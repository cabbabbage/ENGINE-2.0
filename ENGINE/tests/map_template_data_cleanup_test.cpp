#include "core/manifest/map_manifest_normalizer.hpp"

#include <cassert>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

int main() {
    nlohmann::json map = nlohmann::json::object();
    map["map_name"] = "cleanup";
    map["map_layers"] = nlohmann::json::array({
        nlohmann::json::object({
            {"name", "layer_0"},
            {"rooms", nlohmann::json::array()}
        })
    });

    map["rooms_data"] = nlohmann::json::object({
        {"MainTrail", nlohmann::json::object({
            {"name", "MainTrail"},
            {"geometry", "Square"},
            {"width", nlohmann::json::object({{"center", 1200}, {"span", 0}})},
            {"height", nlohmann::json::object({{"center", 1300}, {"span", 0}})},
            {"edge_smoothness", 99},
            {"curvyness", nlohmann::json::object({{"center", 8}, {"span", 0}})},
            {"trail_connection_sector", nlohmann::json::object({{"direction_deg", 0}, {"width_percent", 100}})}
        })}
    });
    map["trails_data"] = nlohmann::json::object({
        {"MainTrail", nlohmann::json::object({
            {"name", "MainTrail"},
            {"geometry", "Line"},
            {"width", nlohmann::json::object({{"center", 700}, {"span", 0}})},
            {"height", nlohmann::json::object({{"center", 900}, {"span", 0}})},
            {"edge_smoothness", 101},
            {"curviness", nlohmann::json::object({{"center", 10}, {"span", 0}})},
            {"is_boss", true},
            {"inherits_live_dynamic_assets", true},
            {"inherit_map_floor_color", false},
            {"room_floor_color", nlohmann::json::array({1, 2, 3})},
            {"trail_connection_sector", nlohmann::json::object({{"direction_deg", 180}, {"width_percent", 25}})}
        })}
    });

    const auto result = manifest::normalize_map_manifest(map, "cleanup", std::filesystem::current_path(), nullptr);
    const auto& normalized = result.map_manifest;
    const auto& room = normalized["rooms_data"]["MainTrail"];
    assert(!room.contains("edge_smoothness"));
    assert(!room.contains("curvyness"));
    assert(!room.contains("curviness"));
    assert(room.contains("coarseness"));
    assert(room["coarseness"].is_number_integer());
    assert(room.contains("edge_detail_candidates"));
    assert(room["edge_detail_candidates"].is_object());
    assert(room.contains("height"));
    assert(room.contains("trail_connection_sector"));

    assert(!normalized["trails_data"].contains("MainTrail"));
    assert(normalized["trails_data"].contains("MainTrail_Trail"));
    const auto& trail = normalized["trails_data"]["MainTrail_Trail"];
    assert(trail.value("name", std::string{}) == "MainTrail_Trail");
    assert(trail.contains("width"));
    assert(!trail.contains("height"));
    assert(!trail.contains("edge_smoothness"));
    assert(!trail.contains("curvyness"));
    assert(!trail.contains("curviness"));
    assert(trail.contains("coarseness"));
    assert(trail["coarseness"].is_number_integer());
    assert(trail.contains("edge_detail_candidates"));
    assert(trail["edge_detail_candidates"].is_object());
    assert(!trail.contains("is_boss"));
    assert(!trail.contains("inherits_live_dynamic_assets"));
    assert(!trail.contains("inherit_map_floor_color"));
    assert(!trail.contains("room_floor_color"));
    assert(!trail.contains("trail_connection_sector"));
    return 0;
}
