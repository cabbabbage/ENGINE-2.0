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
    CHECK(roundtrip.contains("map_assets_data"));
    CHECK(roundtrip.contains("dev_map_settings"));
}
