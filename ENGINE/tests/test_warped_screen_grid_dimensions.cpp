#include <doctest/doctest.h>

#include <vector>

#include "rendering/render/warped_screen_grid.hpp"

namespace {
Area make_starting_area() {
    std::vector<SDL_Point> corners{
        SDL_Point{-100, -100},
        SDL_Point{100, -100},
        SDL_Point{100, 100},
        SDL_Point{-100, 100}};
    return Area("starting", corners, 0);
}
}  // namespace

TEST_CASE("WarpedScreenGrid updates projection dimensions immediately on resize") {
    WarpedScreenGrid grid(1280, 720, make_starting_area());

    const world::CameraProjectionParams initial = grid.projection_params();
    CHECK(initial.screen_width == 1280);
    CHECK(initial.screen_height == 720);

    grid.set_screen_dimensions(1600, 900);
    const world::CameraProjectionParams resized = grid.projection_params();
    CHECK(resized.screen_width == 1600);
    CHECK(resized.screen_height == 900);
    CHECK(resized.state_version != initial.state_version);
}

TEST_CASE("WarpedScreenGrid updates visible bounds immediately on resize") {
    WarpedScreenGrid grid(1280, 720, make_starting_area());

    const WarpedScreenGrid::GridBounds initial_bounds = grid.get_bounds();
    const float initial_width = initial_bounds.right - initial_bounds.left;
    const float initial_height = initial_bounds.bottom - initial_bounds.top;
    REQUIRE(initial_width > 0.0f);
    REQUIRE(initial_height > 0.0f);

    grid.set_screen_dimensions(1600, 900);
    const WarpedScreenGrid::GridBounds resized_bounds = grid.get_bounds();
    const float resized_width = resized_bounds.right - resized_bounds.left;
    const float resized_height = resized_bounds.bottom - resized_bounds.top;

    CHECK(resized_width == doctest::Approx(initial_width * (1600.0f / 1280.0f)).epsilon(1e-5));
    CHECK(resized_height == doctest::Approx(initial_height * (900.0f / 720.0f)).epsilon(1e-5));
}

TEST_CASE("WarpedScreenGrid screen-to-world depth plane roundtrip is stable") {
    WarpedScreenGrid grid(1280, 720, make_starting_area());

    const world::CameraProjectionParams params = grid.projection_params();
    const SDL_FPoint source_screen{
        static_cast<float>(params.screen_width) * 0.35f,
        static_cast<float>(params.screen_height) * 0.72f
    };
    const float depth_sign = (params.forward_z >= 0.0) ? 1.0f : -1.0f;
    const float target_world_z = static_cast<float>(params.anchor_world_z) + depth_sign * 300.0f;

    render_projection::WorldPoint3 world_point{};
    REQUIRE(grid.screen_to_world_on_depth_plane(source_screen, target_world_z, world_point));
    REQUIRE(world_point.valid);

    SDL_FPoint reprojected_screen{};
    REQUIRE(grid.project_world_point(SDL_FPoint{world_point.x, world_point.y}, world_point.z, reprojected_screen));

    CHECK(reprojected_screen.x == doctest::Approx(source_screen.x).epsilon(1e-4));
    CHECK(reprojected_screen.y == doctest::Approx(source_screen.y).epsilon(1e-4));
}
