#include <doctest/doctest.h>

#include "rendering/render/camera_controller.hpp"

TEST_CASE("CameraController applies room targets once manual overrides are cleared") {
    CameraController camera(1000.0);

    CameraParams initial{};
    initial.height_px = 1000.0;
    initial.tilt_deg = 25.0;
    initial.zoom_percent = 0.0;
    camera.reset(initial, SDL_Point{0, 0}, 1000.0);

    camera.set_manual_height_override(true);
    camera.set_manual_zoom_override(true);

    CameraParams room_target{};
    room_target.height_px = 1400.0;
    room_target.tilt_deg = 35.0;
    room_target.zoom_percent = 40.0;

    camera.apply_room_targets(room_target, room_target, 0.0, false, 0, true);
    CHECK(camera.state().params.height_px == doctest::Approx(1000.0));
    CHECK(camera.state().params.zoom_percent == doctest::Approx(0.0));

    camera.set_manual_height_override(false);
    camera.set_manual_zoom_override(false);
    camera.apply_room_targets(room_target, room_target, 0.0, false, 0, false);

    CHECK(camera.state().params.height_px == doctest::Approx(1400.0));
    CHECK(camera.state().params.zoom_percent == doctest::Approx(40.0));
}
