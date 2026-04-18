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
    REQUIRE(payload["movement"].is_array());
    REQUIRE(payload["movement"].size() == 3);
    for (const auto& frame : payload["movement"]) {
        REQUIRE(frame.is_array());
        CHECK(frame.size() >= 4);
        CHECK(frame[0] == 0);
        CHECK(frame[1] == 0);
        CHECK(frame[2] == 0);
        CHECK(frame[3] == doctest::Approx(0.0));
    }
    CHECK(payload["movement_total"]["dx"] == 0);
    CHECK(payload["movement_total"]["dy"] == 0);
    CHECK(payload["movement_total"]["dz"] == doctest::Approx(0.0));
    CHECK(payload["movement_total"]["dr"] == doctest::Approx(0.0));
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
    REQUIRE(payload["movement"].is_array());
    REQUIRE(payload["movement"].size() == 2);
    for (const auto& frame : payload["movement"]) {
        REQUIRE(frame.is_array());
        CHECK(frame.size() >= 4);
        CHECK(frame[0] == 0);
        CHECK(frame[1] == 0);
        CHECK(frame[2] == 0);
        CHECK(frame[3] == doctest::Approx(0.0));
    }
    CHECK(payload["movement_total"]["dx"] == 0);
    CHECK(payload["movement_total"]["dy"] == 0);
    CHECK(payload["movement_total"]["dz"] == doctest::Approx(0.0));
    CHECK(payload["movement_total"]["dr"] == doctest::Approx(0.0));
    CHECK(!payload.contains("anchor_points"));
    CHECK(!payload.contains("hit_boxes"));
    CHECK(!payload.contains("attack_boxes"));
    CHECK(!payload.contains("hit_geometry"));
    CHECK(!payload.contains("attack_geometry"));
}

TEST_CASE("Movement payload totals include frame zero for shared and room builders") {
    std::vector<devmode::frame_editors::MovementFrame> shared_frames(2);
    shared_frames[0].dx = 2.0f;
    shared_frames[0].dy = -3.0f;
    shared_frames[0].dz = 4.0f;
    shared_frames[0].rotation_degrees = 11.5f;
    shared_frames[1].dx = 5.0f;
    shared_frames[1].dy = 7.0f;
    shared_frames[1].dz = -2.0f;
    shared_frames[1].rotation_degrees = -2.5f;

    const nlohmann::json shared_payload =
        devmode::frame_editors::build_payload_from_frames(shared_frames, nlohmann::json::object());
    CHECK(shared_payload["movement_total"]["dx"] == 7);
    CHECK(shared_payload["movement_total"]["dy"] == 4);
    CHECK(shared_payload["movement_total"]["dz"] == doctest::Approx(2.0));
    CHECK(shared_payload["movement_total"]["dr"] == doctest::Approx(9.0));
    CHECK(shared_payload["movement"][0] == nlohmann::json::array({2, -3, 4, 11.5}));
    CHECK(shared_payload["movement"][1] == nlohmann::json::array({5, 7, -2, -2.5}));

    std::vector<devmode::room_movement_payload::MovementFrame> room_frames(2);
    room_frames[0].dx = -6.0f;
    room_frames[0].dy = 1.0f;
    room_frames[0].dz = 3.0f;
    room_frames[0].rotation_degrees = 45.0f;
    room_frames[1].dx = 4.0f;
    room_frames[1].dy = -5.0f;
    room_frames[1].dz = 9.0f;
    room_frames[1].rotation_degrees = -15.0f;

    const nlohmann::json room_payload =
        devmode::room_movement_payload::build_payload_from_frames(room_frames, nlohmann::json::object());
    CHECK(room_payload["movement_total"]["dx"] == -2);
    CHECK(room_payload["movement_total"]["dy"] == -4);
    CHECK(room_payload["movement_total"]["dz"] == doctest::Approx(12.0));
    CHECK(room_payload["movement_total"]["dr"] == doctest::Approx(30.0));
    CHECK(room_payload["movement"][0] == nlohmann::json::array({-6, 1, 3, 45.0}));
    CHECK(room_payload["movement"][1] == nlohmann::json::array({4, -5, 9, -15.0}));
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

TEST_CASE("Room movement payload parses frame rotation and defaults missing rotation to zero") {
    nlohmann::json payload = nlohmann::json::object();
    payload["movement"] = nlohmann::json::array({
        nlohmann::json::array({0, 0, 0}),
        nlohmann::json::array({10, 0, 5, 30.0}),
        nlohmann::json::object({{"dx", 3}, {"dy", 2}, {"dz", 1}, {"rotation_degrees", -45.0}})
    });

    const auto frames = devmode::room_movement_payload::parse_frames_from_payload(payload);
    REQUIRE(frames.size() == 3);
    CHECK(frames[0].rotation_degrees == doctest::Approx(0.0f));
    CHECK(frames[1].rotation_degrees == doctest::Approx(30.0f));
    CHECK(frames[2].rotation_degrees == doctest::Approx(-45.0f));
}
