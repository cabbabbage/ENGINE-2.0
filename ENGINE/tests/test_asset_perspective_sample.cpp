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
