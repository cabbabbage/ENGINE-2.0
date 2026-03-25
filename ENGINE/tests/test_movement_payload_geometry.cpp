#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "devtools/frame_editors/shared/FrameEditState.hpp"
#include "devtools/room_movement_payload.hpp"

TEST_CASE("Shared frame edit payload does not synthesize unrelated geometry arrays") {
    const std::vector<devmode::frame_editors::MovementFrame> frames(3);
    nlohmann::json payload = devmode::frame_editors::build_payload_from_frames(
        frames,
        nlohmann::json::object());

    CHECK(payload.contains("movement"));
    CHECK(payload.contains("movement_total"));
    CHECK(!payload.contains("anchor_points"));
    CHECK(!payload.contains("hit_boxes"));
    CHECK(!payload.contains("attack_boxes"));
    CHECK(!payload.contains("hit_geometry"));
    CHECK(!payload.contains("attack_geometry"));
}

TEST_CASE("Room movement payload does not synthesize unrelated geometry arrays") {
    const std::vector<devmode::room_movement_payload::MovementFrame> frames(2);
    nlohmann::json payload = devmode::room_movement_payload::build_payload_from_frames(
        frames,
        nlohmann::json::object());

    CHECK(payload.contains("movement"));
    CHECK(payload.contains("movement_total"));
    CHECK(!payload.contains("anchor_points"));
    CHECK(!payload.contains("hit_boxes"));
    CHECK(!payload.contains("attack_boxes"));
    CHECK(!payload.contains("hit_geometry"));
    CHECK(!payload.contains("attack_geometry"));
}
