#include <doctest/doctest.h>

#include "animation/controllers/custom_controllers/player_direction_intent.hpp"

namespace {

using vibble::player_direction::DirectionIntent;
using vibble::player_direction::resolve_direction_intent;

void check_intent(const DirectionIntent& intent,
                  int expected_screen_x,
                  int expected_screen_y,
                  int expected_world_x,
                  int expected_world_y) {
    CHECK(intent.screen_x == expected_screen_x);
    CHECK(intent.screen_y == expected_screen_y);
    CHECK(intent.world_x == expected_world_x);
    CHECK(intent.world_y == expected_world_y);
}

} // namespace

TEST_CASE("Direction intent uses camera right sign for world horizontal mapping") {
    const DirectionIntent camera_right_positive =
        resolve_direction_intent(1, -1, 0.6, true);
    check_intent(camera_right_positive, 1, -1, 1, -1);

    const DirectionIntent camera_right_negative =
        resolve_direction_intent(1, -1, -0.6, true);
    check_intent(camera_right_negative, 1, -1, -1, -1);
}

TEST_CASE("Direction intent fallback keeps canonical horizontal mapping without camera basis") {
    const DirectionIntent fallback = resolve_direction_intent(-1, 1, 0.0, false);
    check_intent(fallback, -1, 1, -1, 1);

    const DirectionIntent near_zero_basis = resolve_direction_intent(-1, 1, 0.0, true);
    check_intent(near_zero_basis, -1, 1, -1, 1);
}

TEST_CASE("Direction intent normalizes raw axis magnitudes to signed intent") {
    const DirectionIntent normalized = resolve_direction_intent(5, -2, -1.0, true);
    check_intent(normalized, 1, -1, -1, -1);

    const DirectionIntent idle = resolve_direction_intent(0, 0, -1.0, true);
    check_intent(idle, 0, 0, 0, 0);
}
