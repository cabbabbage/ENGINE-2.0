#include <doctest/doctest.h>

#include <memory>
#include <string>

#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_info.hpp"

TEST_CASE("Asset runtime perspective sample prefers the asset grid point in world tests") {
    auto info = std::make_shared<AssetInfo>("vibble");
    Area spawn_area("sample_spawn", 0);
    Asset asset(info, spawn_area, SDL_Point{64, 96}, 0);

    world::GridPoint* point = asset.grid_point();
    REQUIRE(point != nullptr);
    point->mutable_projection_cache().perspective_scale = 1.75f;

    const Asset::PerspectiveSample sample = asset.runtime_perspective_sample();
    CHECK(sample.scale == doctest::Approx(1.75f));
    CHECK(sample.resolution_layer == point->resolution_layer());
    CHECK(sample.source == Asset::PerspectiveSource::AssetGridPoint);
    CHECK(std::string(Asset::perspective_source_label(sample.source)) == "asset-grid-point");
}

TEST_CASE("Asset runtime perspective sample anchor override takes precedence and clears back to grid point") {
    auto info = std::make_shared<AssetInfo>("vibble");
    Area spawn_area("sample_spawn", 0);
    Asset asset(info, spawn_area, SDL_Point{64, 96}, 0);

    world::GridPoint* point = asset.grid_point();
    REQUIRE(point != nullptr);
    point->mutable_projection_cache().perspective_scale = 1.75f;

    CHECK(asset.set_anchor_perspective_override(2.25f, point->resolution_layer()));
    const Asset::PerspectiveSample override_sample = asset.runtime_perspective_sample();
    CHECK(override_sample.scale == doctest::Approx(2.25f));
    CHECK(override_sample.source == Asset::PerspectiveSource::AnchorBindingOverride);
    CHECK(std::string(Asset::perspective_source_label(override_sample.source)) == "anchor-binding-override");

    CHECK(asset.clear_anchor_perspective_override());
    const Asset::PerspectiveSample restored_sample = asset.runtime_perspective_sample();
    CHECK(restored_sample.scale == doctest::Approx(1.75f));
    CHECK(restored_sample.source == Asset::PerspectiveSource::AssetGridPoint);
}

TEST_CASE("AssetInfo size variation defaults to zero, loads payload, and clamps setter range") {
    AssetInfo info_default("variation_default");
    CHECK(info_default.size_variation_percent == doctest::Approx(0.0f));

    AssetInfo info_loaded("variation_loaded", nlohmann::json{
        {"size_settings", nlohmann::json{
            {"scale_percentage", 100.0},
            {"size_variation", 12.0}
        }}
    });
    CHECK(info_loaded.size_variation_percent == doctest::Approx(12.0f));

    info_loaded.set_size_variation_percentage(-3.0f);
    CHECK(info_loaded.size_variation_percent == doctest::Approx(0.0f));
    CHECK(info_loaded.manifest_payload()["size_settings"]["size_variation"].get<float>() == doctest::Approx(0.0f));

    info_loaded.set_size_variation_percentage(99.0f);
    CHECK(info_loaded.size_variation_percent == doctest::Approx(20.0f));
    CHECK(info_loaded.manifest_payload()["size_settings"]["size_variation"].get<float>() == doctest::Approx(20.0f));
}

TEST_CASE("Asset runtime base scale applies stable per-instance variation and ignores variation for tileable assets") {
    auto info = std::make_shared<AssetInfo>("variation_runtime");
    info->set_scale_factor(1.5f);
    info->set_size_variation_percentage(20.0f);

    Area spawn_area("sample_spawn", 0);
    Asset asset(info, spawn_area, SDL_Point{64, 96}, 0);

    const float sample = asset.size_variation_sample();
    CHECK(sample >= -1.0f);
    CHECK(sample <= 1.0f);

    const float varied_scale = asset.runtime_effective_base_scale();
    CHECK(varied_scale == doctest::Approx(1.5f * (1.0f + sample * 0.2f)));

    const float sample_before = asset.size_variation_sample();
    info->set_size_variation_percentage(10.0f);
    CHECK(asset.size_variation_sample() == doctest::Approx(sample_before));
    CHECK(asset.runtime_effective_base_scale() == doctest::Approx(1.5f * (1.0f + sample_before * 0.1f)));

    info->set_tillable(true);
    CHECK(asset.runtime_effective_base_scale() == doctest::Approx(1.5f));
}

TEST_CASE("Asset runtime camera metrics freshness includes anchor revision guard") {
    auto info = std::make_shared<AssetInfo>("vibble");
    Area spawn_area("sample_spawn", 0);
    Asset asset(info, spawn_area, SDL_Point{0, 0}, 0);

    RuntimeCameraMetrics metrics{};
    metrics.valid = true;
    metrics.frame_id = 42;
    metrics.camera_state_version = 77;
    metrics.anchor_revision = asset.anchor_world_revision();
    metrics.world_z_depth_from_anchor = 12.5;
    asset.runtime_camera_metrics = metrics;

    CHECK(asset.has_fresh_runtime_camera_metrics(42, 77));
    CHECK_FALSE(asset.has_fresh_runtime_camera_metrics(43, 77));
    CHECK_FALSE(asset.has_fresh_runtime_camera_metrics(42, 78));

    asset.runtime_camera_metrics.anchor_revision = asset.anchor_world_revision() + 1;
    CHECK_FALSE(asset.has_fresh_runtime_camera_metrics(42, 77));

    asset.runtime_camera_metrics.anchor_revision = asset.anchor_world_revision();
    CHECK(asset.has_fresh_runtime_camera_metrics(42, 77));
    asset.mark_anchors_dirty();
    CHECK_FALSE(asset.has_fresh_runtime_camera_metrics(42, 77));
}
