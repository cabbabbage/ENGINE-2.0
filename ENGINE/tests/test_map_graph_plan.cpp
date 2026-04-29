#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "gameplay/map_generation/map_graph.hpp"

namespace {

bool contains_name(const std::vector<RoomSpec>& specs, const std::string& name) {
    return std::any_of(specs.begin(), specs.end(), [&](const RoomSpec& spec) {
        return spec.name == name;
    });
}

}

TEST_CASE("map_graph plan leaves layer-0 unresolved when center-room candidate is missing") {
    nlohmann::json manifest = nlohmann::json::object({
        {"rooms_data", nlohmann::json::object()},
        {"map_layers", nlohmann::json::array()}
    });

    map_graph::MapGraphPlan plan = map_graph::build_map_graph_plan(&manifest);
    CHECK_FALSE(plan.valid);
    REQUIRE(manifest.contains("map_layers"));
    REQUIRE(manifest["map_layers"].is_array());
    REQUIRE(!manifest["map_layers"].empty());
    REQUIRE(manifest["map_layers"][0].contains("rooms"));
    REQUIRE(manifest["map_layers"][0]["rooms"].is_array());
    CHECK(manifest["map_layers"][0]["rooms"].empty());
    CHECK(manifest["map_layers"][0].value("min_rooms", -1) == 0);
    CHECK(manifest["map_layers"][0].value("max_rooms", -1) == 0);
    CHECK(manifest["rooms_data"].empty());
}

TEST_CASE("map_graph plan resolves room-tag candidates into concrete rooms") {
    nlohmann::json manifest = nlohmann::json::object({
        {"rooms_data", nlohmann::json::object({
            {"Spawn", nlohmann::json::object({{"name", "Spawn"}})},
            {"forest_a", nlohmann::json::object({{"name", "forest_a"}, {"room_tags", nlohmann::json::array({"forest"})}})},
            {"forest_b", nlohmann::json::object({{"name", "forest_b"}, {"room_tags", nlohmann::json::array({"forest"})}})}
        })},
        {"map_layers", nlohmann::json::array({
            nlohmann::json::object({
                {"level", 0},
                {"max_rooms", 1},
                {"rooms", nlohmann::json::array({
                    nlohmann::json::object({
                        {"source_type", "room_name"},
                        {"value", "Spawn"},
                        {"min_instances", 1},
                        {"max_instances", 1}
                    })
                })}
            }),
            nlohmann::json::object({
                {"level", 1},
                {"max_rooms", 2},
                {"rooms", nlohmann::json::array({
                    nlohmann::json::object({
                        {"source_type", "room_tag"},
                        {"value", "forest"},
                        {"min_instances", 0},
                        {"max_instances", 1}
                    })
                })}
            })
        })}
    });

    map_graph::MapGraphPlan plan = map_graph::build_map_graph_plan(&manifest);
    CHECK(plan.valid);
    REQUIRE(plan.resolved_layers.size() >= 2);
    CHECK(contains_name(plan.resolved_layers[1].rooms, "forest_a"));
    CHECK(contains_name(plan.resolved_layers[1].rooms, "forest_b"));
}

TEST_CASE("map_graph plan fails when required child is missing on next layer") {
    nlohmann::json manifest = nlohmann::json::object({
        {"rooms_data", nlohmann::json::object({
            {"Spawn", nlohmann::json::object({{"name", "Spawn"}})},
            {"parent_room", nlohmann::json::object({{"name", "parent_room"}})}
        })},
        {"map_layers", nlohmann::json::array({
            nlohmann::json::object({
                {"level", 0},
                {"max_rooms", 1},
                {"rooms", nlohmann::json::array({
                    nlohmann::json::object({
                        {"source_type", "room_name"},
                        {"value", "Spawn"},
                        {"min_instances", 1},
                        {"max_instances", 1}
                    })
                })}
            }),
            nlohmann::json::object({
                {"level", 1},
                {"max_rooms", 1},
                {"rooms", nlohmann::json::array({
                    nlohmann::json::object({
                        {"source_type", "room_name"},
                        {"value", "parent_room"},
                        {"min_instances", 1},
                        {"max_instances", 1},
                        {"required_children", nlohmann::json::array({"missing_child"})}
                    })
                })}
            }),
            nlohmann::json::object({
                {"level", 2},
                {"max_rooms", 0},
                {"rooms", nlohmann::json::array()}
            })
        })}
    });

    map_graph::MapGraphPlan plan = map_graph::build_map_graph_plan(&manifest);
    CHECK_FALSE(plan.valid);
    CHECK(std::any_of(plan.diagnostics.begin(), plan.diagnostics.end(), [](const std::string& diagnostic) {
        return diagnostic.find("required child 'missing_child'") != std::string::npos;
    }));
}
