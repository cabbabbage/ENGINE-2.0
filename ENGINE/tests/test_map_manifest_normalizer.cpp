#include <doctest/doctest.h>

#include <filesystem>

#include "core/manifest/map_data.hpp"
#include "core/manifest/map_manifest_normalizer.hpp"

TEST_CASE("normalize_map_manifest keeps default map manifest stable") {
    nlohmann::json map_manifest = manifest::build_default_map_manifest("base_case");
    map_manifest["schema_version"] = manifest::kMapSchemaVersion;

    const std::filesystem::path root = std::filesystem::path("C:/tmp/manifest_normalizer_test");

    const manifest::MapManifestNormalizationResult first =
        manifest::normalize_map_manifest(map_manifest, "base_case", root);

    CHECK(first.map_manifest.is_object());
    CHECK(first.map_manifest.contains("map_layers"));
    CHECK(first.map_manifest.contains("map_grid_settings"));

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
