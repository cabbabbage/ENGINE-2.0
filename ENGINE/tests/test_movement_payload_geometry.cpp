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

TEST_CASE("Room movement payload parses legacy 2-element arrays as floor depth") {
    nlohmann::json payload = nlohmann::json::object();
    payload["movement"] = nlohmann::json::array({
        nlohmann::json::array({0, 0}),
        nlohmann::json::array({12, -34})
    });

    const auto frames = devmode::room_movement_payload::parse_frames_from_payload(payload);
    REQUIRE(frames.size() == 2);
    CHECK(frames[1].dx == doctest::Approx(12.0f));
    CHECK(frames[1].dy == doctest::Approx(0.0f));
    CHECK(frames[1].dz == doctest::Approx(-34.0f));
}

TEST_CASE("Room movement payload parses legacy object depth stored in y/dy") {
    nlohmann::json payload = nlohmann::json::object();
    payload["movement"] = nlohmann::json::array({
        nlohmann::json::object(),
        nlohmann::json::object({{"dx", 5}, {"dy", -20}})
    });

    const auto frames = devmode::room_movement_payload::parse_frames_from_payload(payload);
    REQUIRE(frames.size() == 2);
    CHECK(frames[1].dx == doctest::Approx(5.0f));
    CHECK(frames[1].dy == doctest::Approx(0.0f));
    CHECK(frames[1].dz == doctest::Approx(-20.0f));
}

TEST_CASE("Room movement payload parses x/y fallback and treats y-only as legacy depth") {
    nlohmann::json payload = nlohmann::json::object();
    payload["movement"] = nlohmann::json::array({
        nlohmann::json::object(),
        nlohmann::json::object({{"x", 9}, {"y", -7}})
    });

    const auto frames = devmode::room_movement_payload::parse_frames_from_payload(payload);
    REQUIRE(frames.size() == 2);
    CHECK(frames[1].dx == doctest::Approx(9.0f));
    CHECK(frames[1].dy == doctest::Approx(0.0f));
    CHECK(frames[1].dz == doctest::Approx(-7.0f));
}
