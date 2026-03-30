#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "gameplay/spawn/spawn_group_codec.hpp"

TEST_CASE("SpawnGroupCodec normalizes exact-position method alias") {
    CHECK(vibble::spawn_group_codec::normalize_method("Exact Position") == "Exact");
    CHECK(vibble::spawn_group_codec::normalize_method("") == "Random");
}

TEST_CASE("SpawnGroupCodec materializes canonical defaults") {
    nlohmann::json entry = nlohmann::json::object({
        {"position", "Exact Position"},
        {"min_number", 0},
        {"max_number", 99},
        {"resolution", 99}
    });

    vibble::spawn_group_codec::EntryDefaults defaults{};
    defaults.display_name = "Spawn Under Test";
    defaults.resolution = 6;

    const bool changed = vibble::spawn_group_codec::ensure_spawn_group_entry_defaults(entry, defaults);
    CHECK(changed);

    CHECK(entry["position"] == "Exact");
    CHECK(entry.contains("spawn_id"));
    CHECK(entry["spawn_id"].is_string());
    CHECK(!entry["spawn_id"].get<std::string>().empty());
    CHECK(entry["display_name"] == "Spawn Under Test");
    CHECK(entry["quantity"] == 1);
    CHECK(entry["min_number"] == 1);
    CHECK(entry["max_number"] == 1);
    CHECK(entry["resolve_geometry_to_room_size"] == true);
    CHECK(entry["resolve_quantity_to_room_size"] == false);
    CHECK(entry["candidates"].is_array());
    REQUIRE(entry["candidates"].size() == 1);
    CHECK(entry["candidates"][0]["name"] == "null");
    CHECK(entry["candidates"][0]["chance"] == 0);
}

TEST_CASE("SpawnGroupCodec sanitizes candidate chance and weight mapping") {
    nlohmann::json entry = nlohmann::json::object({
        {"candidates", nlohmann::json::array({
            nlohmann::json::object({{"name", "A"}, {"weight", "10"}}),
            nlohmann::json::object({{"name", "B"}, {"chance", "2.5"}}),
            nlohmann::json::object({{"name", ""}, {"chance", -3}}),
            123
        })}
    });

    const bool changed = vibble::spawn_group_codec::sanitize_spawn_group_candidates(entry);
    CHECK(changed);

    REQUIRE(entry["candidates"].is_array());
    REQUIRE(entry["candidates"].size() == 3);
    CHECK(entry["candidates"][0]["name"] == "A");
    CHECK(entry["candidates"][0]["chance"] == 10);
    CHECK(entry["candidates"][1]["name"] == "B");
    CHECK(entry["candidates"][1]["chance"] == doctest::Approx(2.5));
    CHECK(entry["candidates"][2]["name"] == "null");
    CHECK(entry["candidates"][2]["chance"] == 0);
}

TEST_CASE("SpawnGroupCodec supports custom fallback candidate defaults") {
    nlohmann::json entry = nlohmann::json::object({
        {"position", "Random"},
        {"name", "Goblin"}
    });

    vibble::spawn_group_codec::EntryDefaults defaults{};
    defaults.display_name = "Goblin Spawn";
    defaults.default_candidate.name = "Goblin";
    defaults.default_candidate.chance = 100.0;

    const bool changed = vibble::spawn_group_codec::ensure_spawn_group_entry_defaults(entry, defaults);
    CHECK(changed);
    REQUIRE(entry["candidates"].is_array());
    REQUIRE(entry["candidates"].size() == 1);
    CHECK(entry["candidates"][0]["name"] == "Goblin");
    CHECK(entry["candidates"][0]["chance"] == 100);
}

TEST_CASE("SpawnGroupCodec sanitizes perimeter and edge field bounds") {
    nlohmann::json perimeter = nlohmann::json::object({
        {"position", "Perimeter"},
        {"min_number", 1},
        {"max_number", 0},
        {"edge_inset_percent", 999}
    });
    nlohmann::json edge = nlohmann::json::object({
        {"position", "Edge"},
        {"min_number", 0},
        {"max_number", -5},
        {"edge_inset_percent", 999}
    });

    CHECK(vibble::spawn_group_codec::sanitize_perimeter_edge_fields(perimeter));
    CHECK(perimeter["min_number"] == 2);
    CHECK(perimeter["max_number"] == 2);
    CHECK(!perimeter.contains("edge_inset_percent"));

    CHECK(vibble::spawn_group_codec::sanitize_perimeter_edge_fields(edge));
    CHECK(edge["min_number"] == 1);
    CHECK(edge["max_number"] == 1);
    CHECK(edge["edge_inset_percent"] == 200);
}
