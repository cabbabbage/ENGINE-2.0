#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "gameplay/map_generation/room.hpp"
#include "rendering/render/layer_submission_builder.hpp"
#include "rendering/render/render.hpp"
#include "rendering/render/warped_screen_grid.hpp"

namespace {

Area make_starting_area() {
    std::vector<Area::Point> corners{
        Area::Point{-1600, -1200},
        Area::Point{1600, -1200},
        Area::Point{1600, 1200},
        Area::Point{-1600, 1200}
    };
    return Area("layer_submission_split_test_area", corners, 0);
}

} // namespace

TEST_CASE("Layer submission selects player layer from dynamic split world depth") {
    WarpedScreenGrid grid(1280, 720, make_starting_area());
    grid.set_screen_center(SDL_Point{0, 350}, true);

    WarpedScreenGrid::RealismSettings settings = grid.get_settings();
    settings.layer_depth_interval = 100.0f;
    settings.layer_depth_curve = 0.0f;
    grid.set_realism_settings(settings);

    GeometryBatcher geometry_batcher(nullptr);
    LayerSubmissionBuilder builder;
    constexpr double kMaxCullDepth = 400.0;

    const double anchor_world_z = grid.anchor_world_z();
    REQUIRE(std::isfinite(anchor_world_z));

    const render_pipeline::LayerBuildResult centered_split =
        builder.build(geometry_batcher, grid, anchor_world_z, kMaxCullDepth);
    REQUIRE(centered_split.layer_count > 0);
    REQUIRE(centered_split.player_layer_index >= 0);
    REQUIRE(centered_split.player_layer_index < centered_split.layer_count);
    CHECK(centered_split.player_layer_index == (centered_split.layer_count / 2) - 1);

    const render_pipeline::LayerBuildResult front_split =
        builder.build(geometry_batcher, grid, anchor_world_z + 120.0, kMaxCullDepth);
    REQUIRE(front_split.player_layer_index >= 0);
    REQUIRE(front_split.player_layer_index < front_split.layer_count);
    CHECK(front_split.player_layer_index < centered_split.player_layer_index);

    const render_pipeline::LayerBuildResult behind_split =
        builder.build(geometry_batcher, grid, anchor_world_z - 120.0, kMaxCullDepth);
    REQUIRE(behind_split.player_layer_index >= 0);
    REQUIRE(behind_split.player_layer_index < behind_split.layer_count);
    CHECK(behind_split.player_layer_index > centered_split.player_layer_index);
}
