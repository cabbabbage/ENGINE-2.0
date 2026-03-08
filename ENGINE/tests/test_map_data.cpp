#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "core/manifest/map_data.hpp"

TEST_CASE("MapData preserves unknown root and nested keys through round-trip") {
    nlohmann::json entry = {
        {"rooms_data", {
            {"spawn", {
                {"name", "spawn"},
                {"geometry", "Circle"},
                {"custom_room_field", {"a", 1}}
            }}
        }},
        {"trails_data", {
            {"basic", {
                {"name", "basic"},
                {"unknown_trail_key", nlohmann::json::array({1, 2, 3})}
            }}
        }},
        {"map_layers", nlohmann::json::array({
            {
                {"name", "layer_0"},
                {"rooms", nlohmann::json::array()},
                {"custom_layer_flag", true}
            }
        })},
        {"map_layers_settings", {
            {"min_edge_distance", 120},
            {"custom_setting", "persist"}
        }},
        {"map_boundary_data", {
            {"inherits_map_assets", false},
            {"unknown_boundary", 7}
        }},
        {"fog_settings", {
            {"max_random_jitter", 0},
            {"custom_fog", 0.75}
        }},
        {"map_assets_data", {
            {"spawn_groups", nlohmann::json::array()},
            {"custom_assets_key", {"mode", "dense"}}
        }},
        {"dev_map_settings", {
            {"show_grid", true},
            {"custom_dev_key", "value"}
        }},
        {"future_new_section", {
            {"x", 99}
        }},
        {"another_unknown", nlohmann::json::array({"a", "b"})}
    };

    manifest::MapData data = manifest::MapData::from_manifest_entry("map_alpha", entry);
    nlohmann::json round_trip = data.to_manifest_entry();

    CHECK(data.map_id == "map_alpha");
    CHECK(round_trip == entry);
    CHECK(data.extras.contains("future_new_section"));
    CHECK(data.extras.contains("another_unknown"));
}

TEST_CASE("MapData writes known section schema keys when entry is missing") {
    manifest::MapData data = manifest::MapData::from_manifest_entry("empty_map", nlohmann::json::object());
    nlohmann::json out = data.to_manifest_entry();

    CHECK(out.contains("rooms_data"));
    CHECK(out.contains("trails_data"));
    CHECK(out.contains("map_layers"));
    CHECK(out.contains("map_layers_settings"));
    CHECK(out.contains("map_boundary_data"));
    CHECK(out.contains("fog_settings"));
    CHECK(out.contains("map_assets_data"));
    CHECK(out.contains("dev_map_settings"));

    CHECK(out["rooms_data"].is_object());
    CHECK(out["map_layers"].is_array());
}

