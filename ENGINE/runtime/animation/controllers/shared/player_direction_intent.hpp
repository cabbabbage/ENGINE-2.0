#ifndef PLAYER_DIRECTION_INTENT_HPP
#define PLAYER_DIRECTION_INTENT_HPP

#include <cmath>

namespace vibble::player_direction {

struct DirectionIntent {
    int screen_x = 0;
    int screen_y = 0;
    int world_x = 0;
    int world_y = 0;
};

inline int sign_of(int value) {
    return (value > 0) - (value < 0);
}

inline int horizontal_world_sign_from_camera_right(double camera_right_x,
                                                   bool has_camera_basis) {
    constexpr double kBasisEpsilon = 1e-6;
    if (has_camera_basis &&
        std::isfinite(camera_right_x) &&
        std::abs(camera_right_x) > kBasisEpsilon) {
        return (camera_right_x > 0.0) ? 1 : -1;
    }

    // Keep canonical controls when camera basis is temporarily unavailable.
    return 1;
}

inline DirectionIntent resolve_direction_intent(int screen_x,
                                                int screen_y,
                                                double camera_right_x,
                                                bool has_camera_basis) {
    DirectionIntent intent{};
    intent.screen_x = sign_of(screen_x);
    intent.screen_y = sign_of(screen_y);

    const int world_horizontal_sign =
        horizontal_world_sign_from_camera_right(camera_right_x, has_camera_basis);
    intent.world_x = intent.screen_x * world_horizontal_sign;
    intent.world_y = intent.screen_y;
    return intent;
}

} // namespace vibble::player_direction

#endif
