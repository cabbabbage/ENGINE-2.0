#include "doctest/doctest.h"

#include <cmath>

#include "utils/AnchorPointResolver.hpp"
#include "utils/FramePointResolver.hpp"
#include "assets/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "assets/asset/anchor_point.hpp"

using devmode::frame_editors::FramePointResolver;

TEST_CASE("FramePointResolver matches anchor_points anchor resolution with runtime scaling") {
    Assets assets;
    Asset asset;
    asset.pos = SDL_Point{64, 96};
    asset.pos_z = 12;
    asset.set_assets(&assets);
    asset.set_height_px(120);
    asset.set_remainder_scale(1.25f);  // runtime height = 150

    FramePointResolver resolver(&asset);

    DisplacedAssetAnchorPoint anchor{
        "pivot",
        0.2f,
        -0.15f,
        0.6f,
        0.0f
    };

    const auto resolved = anchor_points::resolve_anchor_point(asset, anchor, anchor_points::GridMaterialization::Ensure);
    const SDL_Point base = resolver.anchor_world();

    const int expected_x = base.x + static_cast<int>(std::lround(resolver.to_world_xy(anchor.px)));
    const int expected_y = base.y + static_cast<int>(std::lround(resolver.to_world_xy(anchor.py)));
    const int expected_z = static_cast<int>(std::lround(resolver.to_world_z(anchor.pz)));

    CHECK(resolver.parent_height_px() == doctest::Approx(asset.runtime_height_px()));
    CHECK(resolved.world_px.x == expected_x);
    CHECK(resolved.world_px.y == expected_y);
    CHECK(resolved.world_z == expected_z);
    REQUIRE(resolved.grid_point != nullptr);
    CHECK(resolved.grid_point->world_z() == expected_z);
}

TEST_CASE("Displacement percents respect runtime height remainder scale") {
    Asset source;
    source.set_height_px(200);
    source.set_remainder_scale(1.2f);  // runtime height = 240
    FramePointResolver resolver(&source);

    const auto percents = resolver.to_percent_displacement(72, -24, 48, &source);

    CHECK(percents.dx_percent == doctest::Approx(0.3f));
    CHECK(percents.dy_percent == doctest::Approx(-0.1f));
    CHECK(percents.dz_percent == doctest::Approx(0.2f));
}
