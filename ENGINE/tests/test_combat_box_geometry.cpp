#include <doctest/doctest.h>

#include <vector>

#include <nlohmann/json.hpp>

#include "animation/combat_geometry.hpp"
#include "devtools/room_box_payload_utils.hpp"

TEST_CASE("FrameBox set_corner_clamped keeps TL from crossing opposite edges") {
    animation_update::FrameHitBox box{};
    box.name = "hb";
    box.set_rect(animation_update::FrameBoxRect{10, 20, 30, 40});

    box.set_corner_clamped(animation_update::FrameBoxCornerId::TL, 35, 60);
    CHECK(box.rect.left == 30);
    CHECK(box.rect.top == 40);
    CHECK(box.rect.right == 30);
    CHECK(box.rect.bottom == 40);
}

TEST_CASE("FrameBox set_corner_clamped keeps BR from crossing opposite edges") {
    animation_update::FrameHitBox box{};
    box.name = "hb";
    box.set_rect(animation_update::FrameBoxRect{10, 20, 30, 40});

    box.set_corner_clamped(animation_update::FrameBoxCornerId::BR, 5, 10);
    CHECK(box.rect.left == 10);
    CHECK(box.rect.top == 20);
    CHECK(box.rect.right == 10);
    CHECK(box.rect.bottom == 20);
}

TEST_CASE("FrameBox translate_clamped preserves size and clamps at zero") {
    animation_update::FrameHitBox box{};
    box.name = "hb";
    box.set_rect(animation_update::FrameBoxRect{5, 7, 15, 17});
    const int width = box.rect.width();
    const int height = box.rect.height();

    box.translate_clamped(-20, -3);
    CHECK(box.rect.left == 0);
    CHECK(box.rect.top == 4);
    CHECK(box.rect.width() == width);
    CHECK(box.rect.height() == height);
}

TEST_CASE("FrameBox runtime winding is TL TR BR BL") {
    animation_update::FrameHitBox box{};
    box.name = "hb";
    box.set_rect(animation_update::FrameBoxRect{1, 2, 5, 6});

    const auto points = box.to_runtime_clockwise_points();
    REQUIRE(points.size() == 4);
    CHECK(points[0].texture_x == 1);
    CHECK(points[0].texture_y == 2);
    CHECK(points[1].texture_x == 5);
    CHECK(points[1].texture_y == 2);
    CHECK(points[2].texture_x == 5);
    CHECK(points[2].texture_y == 6);
    CHECK(points[3].texture_x == 1);
    CHECK(points[3].texture_y == 6);
}

TEST_CASE("FrameBoxRect::from_points normalizes unsorted corners") {
    const std::vector<animation_update::FrameBoxCorner> points{
        animation_update::FrameBoxCorner{9, 4},
        animation_update::FrameBoxCorner{2, 8},
        animation_update::FrameBoxCorner{5, 1},
        animation_update::FrameBoxCorner{7, 6},
    };
    const animation_update::FrameBoxRect rect = animation_update::FrameBoxRect::from_points(points);
    CHECK(rect.left == 2);
    CHECK(rect.top == 1);
    CHECK(rect.right == 9);
    CHECK(rect.bottom == 8);
}

TEST_CASE("Room box payload writes canonical corners from normalized rect") {
    animation_update::FrameHitBox hit_box{};
    hit_box.name = "hit_box";
    hit_box.set_rect(animation_update::FrameBoxRect{8, 9, 2, 1});
    hit_box.extrusion_amount = 3;

    nlohmann::json payload = nlohmann::json::object();
    REQUIRE(devmode::room_box_payload::write_hit_box_frame_to_payload(
        payload,
        1,
        0,
        std::vector<animation_update::FrameHitBox>{hit_box}));

    REQUIRE(payload.contains("hit_boxes"));
    const auto& corners = payload["hit_boxes"][0][0]["corners"];
    REQUIRE(corners.is_array());
    REQUIRE(corners.size() == 4);
    CHECK(corners[0]["texture_x"] == 2);  // TL
    CHECK(corners[0]["texture_y"] == 1);
    CHECK(corners[1]["texture_x"] == 8);  // TR
    CHECK(corners[1]["texture_y"] == 1);
    CHECK(corners[2]["texture_x"] == 8);  // BR
    CHECK(corners[2]["texture_y"] == 9);
    CHECK(corners[3]["texture_x"] == 2);  // BL
    CHECK(corners[3]["texture_y"] == 9);
}

TEST_CASE("Attack box payload writes canonical corners and preserves damage") {
    animation_update::FrameAttackBox attack_box{};
    attack_box.name = "attack_box";
    attack_box.set_rect(animation_update::FrameBoxRect{11, 3, 4, 15});
    attack_box.extrusion_amount = 2;
    attack_box.damage_amount = 12;

    nlohmann::json payload = nlohmann::json::object();
    REQUIRE(devmode::room_box_payload::write_attack_box_frame_to_payload(
        payload,
        1,
        0,
        std::vector<animation_update::FrameAttackBox>{attack_box}));

    REQUIRE(payload.contains("attack_boxes"));
    const auto& box = payload["attack_boxes"][0][0];
    CHECK(box["damage_amount"] == 12);
    const auto& corners = box["corners"];
    REQUIRE(corners.is_array());
    REQUIRE(corners.size() == 4);
    CHECK(corners[0]["texture_x"] == 4);   // TL
    CHECK(corners[0]["texture_y"] == 3);
    CHECK(corners[1]["texture_x"] == 11);  // TR
    CHECK(corners[1]["texture_y"] == 3);
    CHECK(corners[2]["texture_x"] == 11);  // BR
    CHECK(corners[2]["texture_y"] == 15);
    CHECK(corners[3]["texture_x"] == 4);   // BL
    CHECK(corners[3]["texture_y"] == 15);
}
