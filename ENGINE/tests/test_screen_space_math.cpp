#include <doctest/doctest.h>

#include "rendering/render/screen_space_math.hpp"

TEST_CASE("screen space math uses top-left origin with positive Y downward") {
    const int width = 1280;
    const int height = 720;

    const SDL_FPoint top_left = render::screen_space::ndc_to_screen(-1.0, 1.0, width, height);
    const SDL_FPoint bottom_left = render::screen_space::ndc_to_screen(-1.0, -1.0, width, height);

    CHECK(top_left.x == doctest::Approx(0.0f));
    CHECK(top_left.y == doctest::Approx(0.0f));
    CHECK(bottom_left.x == doctest::Approx(0.0f));
    CHECK(bottom_left.y == doctest::Approx(static_cast<float>(height)));
}

TEST_CASE("screen space math round-trips screen and NDC coordinates") {
    const int width = 1920;
    const int height = 1080;
    const double zoom = 1.35;
    const double pan_y = 42.0;
    const double input_x = 712.25;
    const double input_y = 501.5;

    const auto [ndc_x, ndc_y] =
        render::screen_space::screen_to_ndc(input_x, input_y, width, height, zoom, pan_y);
    const SDL_FPoint screen =
        render::screen_space::ndc_to_screen(ndc_x, ndc_y, width, height, zoom, pan_y);

    CHECK(screen.x == doctest::Approx(static_cast<float>(input_x)).epsilon(1e-5));
    CHECK(screen.y == doctest::Approx(static_cast<float>(input_y)).epsilon(1e-5));
}
