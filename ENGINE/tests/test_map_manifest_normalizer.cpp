#include <doctest/doctest.h>

#include <filesystem>

#include "core/manifest/depth_cue_settings.hpp"
#include "core/manifest/map_manifest_normalizer.hpp"

TEST_CASE("normalize_map_manifest rewrites invalid depth cue ordering to defaults and stabilizes") {
    nlohmann::json map_manifest = manifest::build_default_map_manifest("depth_case");
    map_manifest[depth_cue::kMapEntryKey] = nlohmann::json::object({
        {depth_cue::kCenterDepthOffsetKey, 150.0f},
        {depth_cue::kForegroundMaxDepthOffsetKey, 500.0f},
        {depth_cue::kBackgroundMaxDepthOffsetKey, -120.0f},
    });

    const std::filesystem::path root = std::filesystem::path("C:/tmp/manifest_normalizer_test");

    const manifest::MapManifestNormalizationResult first =
        manifest::normalize_map_manifest(map_manifest, "depth_case", root);

    CHECK(first.changed);
    REQUIRE(first.map_manifest.contains(depth_cue::kMapEntryKey));

    depth_cue::DepthCueSettings expected_defaults{};
    depth_cue::clamp(expected_defaults);

    const depth_cue::DepthCueSettings normalized = depth_cue::from_map_entry(first.map_manifest);
    CHECK(depth_cue::nearly_equal(normalized, expected_defaults));
    CHECK(first.map_manifest[depth_cue::kMapEntryKey] == depth_cue::to_json(expected_defaults));

    const manifest::MapManifestNormalizationResult second =
        manifest::normalize_map_manifest(first.map_manifest, "depth_case", root);
    CHECK_FALSE(second.changed);
    CHECK(second.map_manifest == first.map_manifest);
}
