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
