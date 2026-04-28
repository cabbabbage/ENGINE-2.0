#include <doctest/doctest.h>

#include <filesystem>

#include "core/manifest/map_data.hpp"
#include "core/manifest/map_manifest_normalizer.hpp"
#include "utils/map_grid_settings.hpp"

TEST_CASE("normalize_map_manifest keeps default map manifest stable") {
    nlohmann::json map_manifest = manifest::build_default_map_manifest("base_case");
    map_manifest["schema_version"] = manifest::kMapSchemaVersion;

    const std::filesystem::path root = std::filesystem::path("C:/tmp/manifest_normalizer_test");

    const manifest::MapManifestNormalizationResult first =
        manifest::normalize_map_manifest(map_manifest, "base_case", root);

    CHECK(first.map_manifest.is_object());
    CHECK(first.map_manifest.contains("map_layers"));
    CHECK(first.map_manifest.contains("map_grid_settings"));
    CHECK(first.map_manifest["map_grid_settings"].value("grid_resolution", -1) == 8);

    const manifest::MapManifestNormalizationResult second =
        manifest::normalize_map_manifest(first.map_manifest, "base_case", root);
    CHECK_FALSE(second.changed);
    CHECK(second.map_manifest == first.map_manifest);
}

TEST_CASE("map data manifest roundtrip preserves canonical sections") {
    nlohmann::json map_manifest = manifest::build_default_map_manifest("roundtrip_case");
    map_manifest["schema_version"] = manifest::kMapSchemaVersion;

    const manifest::MapData data = manifest::MapData::from_manifest_entry("roundtrip_case", map_manifest);
    const nlohmann::json roundtrip = data.to_manifest_entry();

    CHECK(roundtrip.contains("schema_version"));
    CHECK(roundtrip.contains("rooms_data"));
    CHECK(roundtrip.contains("trails_data"));
    CHECK(roundtrip.contains("map_layers"));
    CHECK(roundtrip.contains("map_layers_settings"));
    CHECK(roundtrip.contains("map_boundary_data"));
    CHECK(roundtrip.contains("dev_map_settings"));
}

TEST_CASE("normalize_map_manifest hard-removes deprecated map_assets_data section") {
    nlohmann::json map_manifest = manifest::build_default_map_manifest("remove_map_assets_case");
    map_manifest["schema_version"] = manifest::kMapSchemaVersion;
    map_manifest["map_assets_data"] = nlohmann::json::object({{"spawn_groups", nlohmann::json::array()}});

    const std::filesystem::path root = std::filesystem::path("C:/tmp/manifest_normalizer_test");
    const manifest::MapManifestNormalizationResult normalized =
        manifest::normalize_map_manifest(map_manifest, "remove_map_assets_case", root);

    CHECK_FALSE(normalized.map_manifest.contains("map_assets_data"));
}

TEST_CASE("normalize_map_manifest self-heals malformed room config entries and migrates legacy radius") {
    nlohmann::json map_manifest = manifest::build_default_map_manifest("room_repair_case");
    map_manifest["schema_version"] = manifest::kMapSchemaVersion;
    map_manifest["rooms_data"]["legacy_circle"] = nlohmann::json::object({
        {"name", "legacy_circle"},
        {"geometry", "Circle"},
        {"min_radius", 100},
        {"max_radius", 150},
        {"radius", 150},
        {"spawn_groups", "bad"},
        {"edge_smoothness", 500}
    });
    map_manifest["rooms_data"]["broken"] = "invalid";

    const std::filesystem::path root = std::filesystem::path("C:/tmp/manifest_normalizer_test");
    const manifest::MapManifestNormalizationResult normalized =
        manifest::normalize_map_manifest(map_manifest, "room_repair_case", root);

    const nlohmann::json& legacy = normalized.map_manifest["rooms_data"]["legacy_circle"];
    CHECK(legacy.is_object());
    CHECK(legacy.value("min_width", 0) == 200);
    CHECK(legacy.value("max_width", 0) == 300);
    CHECK(legacy.value("min_height", 0) == 200);
    CHECK(legacy.value("max_height", 0) == 300);
    CHECK_FALSE(legacy.contains("radius"));
    CHECK_FALSE(legacy.contains("min_radius"));
    CHECK_FALSE(legacy.contains("max_radius"));
    CHECK(legacy.value("edge_smoothness", 0) == 101);
    CHECK(legacy.contains("spawn_groups"));
    CHECK(legacy["spawn_groups"].is_array());

    const nlohmann::json& broken = normalized.map_manifest["rooms_data"]["broken"];
    CHECK(broken.is_object());
    CHECK(broken.value("name", std::string{}) == "broken");
    CHECK(broken.value("min_width", 0) >= 1);
    CHECK(broken.value("max_width", 0) >= broken.value("min_width", 0));
    CHECK(broken.contains("spawn_groups"));
    CHECK(broken["spawn_groups"].is_array());
}

TEST_CASE("MapGridSettings defaults missing grid_resolution to 8") {
    nlohmann::json section = nlohmann::json::object();
    MapGridSettings settings = MapGridSettings::from_json(&section);
    CHECK(settings.grid_resolution == 8);
}

TEST_CASE("normalize_map_manifest inserts default grid_resolution when missing") {
    nlohmann::json map_manifest = manifest::build_default_map_manifest("missing_grid_case");
    map_manifest["schema_version"] = manifest::kMapSchemaVersion;
    map_manifest["map_grid_settings"] = nlohmann::json::object();

    const std::filesystem::path root = std::filesystem::path("C:/tmp/manifest_normalizer_test");
    const manifest::MapManifestNormalizationResult normalized =
        manifest::normalize_map_manifest(map_manifest, "missing_grid_case", root);

    REQUIRE(normalized.map_manifest.contains("map_grid_settings"));
    CHECK(normalized.map_manifest["map_grid_settings"].value("grid_resolution", -1) == 8);
}

TEST_CASE("normalize_map_manifest preserves explicit grid_resolution") {
    nlohmann::json map_manifest = manifest::build_default_map_manifest("explicit_grid_case");
    map_manifest["schema_version"] = manifest::kMapSchemaVersion;
    map_manifest["map_grid_settings"]["grid_resolution"] = 3;

    const std::filesystem::path root = std::filesystem::path("C:/tmp/manifest_normalizer_test");
    const manifest::MapManifestNormalizationResult normalized =
        manifest::normalize_map_manifest(map_manifest, "explicit_grid_case", root);

    REQUIRE(normalized.map_manifest.contains("map_grid_settings"));
    CHECK(normalized.map_manifest["map_grid_settings"].value("grid_resolution", -1) == 3);
}

TEST_CASE("normalize_map_manifest normalizes trail connection sector defaults and clamps") {
    nlohmann::json map_manifest = manifest::build_default_map_manifest("sector_defaults_case");
    map_manifest["schema_version"] = manifest::kMapSchemaVersion;

    map_manifest["rooms_data"]["missing_sector"] = nlohmann::json::object({
        {"name", "missing_sector"},
        {"geometry", "Square"},
        {"min_width", 300},
        {"max_width", 300},
        {"min_height", 300},
        {"max_height", 300}
    });

    map_manifest["rooms_data"]["clamped_sector"] = nlohmann::json::object({
        {"name", "clamped_sector"},
        {"geometry", "Square"},
        {"min_width", 300},
        {"max_width", 300},
        {"min_height", 300},
        {"max_height", 300},
        {"trail_connection_sector", nlohmann::json::object({
             {"direction_deg", -450.0},
             {"width_percent", 5}
         })}
    });

    map_manifest["trails_data"]["trail_template"] = nlohmann::json::object({
        {"name", "trail_template"},
        {"trail_connection_sector", nlohmann::json::object({
             {"direction_deg", 90.0},
             {"width_percent", 30}
         })}
    });

    const std::filesystem::path root = std::filesystem::path("C:/tmp/manifest_normalizer_test");
    const manifest::MapManifestNormalizationResult normalized =
        manifest::normalize_map_manifest(map_manifest, "sector_defaults_case", root);

    const nlohmann::json& missing = normalized.map_manifest["rooms_data"]["missing_sector"]["trail_connection_sector"];
    CHECK(missing.is_object());
    CHECK(missing.value("direction_deg", -1.0) == doctest::Approx(0.0));
    CHECK(missing.value("width_percent", -1) == 100);

    const nlohmann::json& clamped = normalized.map_manifest["rooms_data"]["clamped_sector"]["trail_connection_sector"];
    CHECK(clamped.is_object());
    CHECK(clamped.value("direction_deg", -1.0) == doctest::Approx(270.0));
    CHECK(clamped.value("width_percent", -1) == 25);

    CHECK_FALSE(normalized.map_manifest["trails_data"]["trail_template"].contains("trail_connection_sector"));
}

TEST_CASE("normalize_map_manifest migrates legacy spawn fields to layer-0 authority") {
    nlohmann::json map_manifest = manifest::build_default_map_manifest("spawn_migration_case");
    map_manifest["schema_version"] = manifest::kMapSchemaVersion;
    map_manifest["rooms_data"]["legacy_spawn"] = nlohmann::json::object({
        {"name", "legacy_spawn"},
        {"geometry", "Square"},
        {"min_width", 400},
        {"max_width", 400},
        {"min_height", 400},
        {"max_height", 400},
        {"is_spawn", true}
    });
    map_manifest["map_layers"] = nlohmann::json::array({
        nlohmann::json::object({
            {"level", 0},
            {"name", "layer_0"},
            {"rooms", nlohmann::json::array({
                nlohmann::json::object({
                    {"source_type", "room_tag"},
                    {"value", "forest"}
                }),
                nlohmann::json::object({
                    {"source_type", "room_name"},
                    {"value", "legacy_spawn"}
                })
            })}
        })
    });

    const std::filesystem::path root = std::filesystem::path("C:/tmp/manifest_normalizer_test");
    const manifest::MapManifestNormalizationResult normalized =
        manifest::normalize_map_manifest(map_manifest, "spawn_migration_case", root);

    REQUIRE(normalized.map_manifest["map_layers"].is_array());
    REQUIRE(normalized.map_manifest["map_layers"].size() >= 1);
    const nlohmann::json& layer0 = normalized.map_manifest["map_layers"][0];
    REQUIRE(layer0.contains("rooms"));
    REQUIRE(layer0["rooms"].is_array());
    REQUIRE(layer0["rooms"].size() == 1);
    CHECK(layer0["rooms"][0].value("source_type", std::string()) == "room_name");
    CHECK(layer0["rooms"][0].value("min_instances", 0) == 1);
    CHECK(layer0["rooms"][0].value("max_instances", 0) == 1);
    CHECK(layer0.value("min_rooms", 0) == 1);
    CHECK(layer0.value("max_rooms", 0) == 1);

    bool found_legacy_spawn_flag = false;
    for (auto it = normalized.map_manifest["rooms_data"].begin(); it != normalized.map_manifest["rooms_data"].end(); ++it) {
        if (it.value().is_object() && it.value().contains("is_spawn")) {
            found_legacy_spawn_flag = true;
            break;
        }
    }
    CHECK_FALSE(found_legacy_spawn_flag);
}
