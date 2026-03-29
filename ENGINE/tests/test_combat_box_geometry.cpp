#include <doctest/doctest.h>

#include <vector>
#include <string>

#include <nlohmann/json.hpp>

#include "animation/combat_geometry.hpp"
#include "devtools/room_box_payload_utils.hpp"

TEST_CASE("FrameBox set_corner_clamped keeps TL from crossing opposite edges") {
    animation_update::FrameHitBox box{};
    box.id = "hb_1";
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
    box.id = "hb_1";
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
    box.id = "hb_1";
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
    box.id = "hb_1";
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

TEST_CASE("FrameBox runtime winding rotates rectangle corners around center") {
    animation_update::FrameHitBox box{};
    box.id = "hb_rot";
    box.name = "hb_rot";
    box.set_rect(animation_update::FrameBoxRect{20, 20, 30, 24});
    box.set_rotation_degrees(90.0f);

    const auto points = box.to_runtime_clockwise_points();
    REQUIRE(points.size() == 4);
    CHECK(points[0].texture_x == 27);  // TL
    CHECK(points[0].texture_y == 17);
    CHECK(points[1].texture_x == 27);  // TR
    CHECK(points[1].texture_y == 27);
    CHECK(points[2].texture_x == 23);  // BR
    CHECK(points[2].texture_y == 27);
    CHECK(points[3].texture_x == 23);  // BL
    CHECK(points[3].texture_y == 17);
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
    hit_box.id = "hitbox_alpha";
    hit_box.type = "hitbox";
    hit_box.name = "hit_box";
    hit_box.enabled = true;
    hit_box.set_rect(animation_update::FrameBoxRect{8, 9, 2, 1});
    hit_box.extrusion_amount = 3;
    hit_box.anchor_link = "root";

    nlohmann::json payload = nlohmann::json::object();
    REQUIRE(devmode::room_box_payload::write_hit_box_frame_to_payload(
        payload,
        1,
        0,
        std::vector<animation_update::FrameHitBox>{hit_box}));

    REQUIRE(payload.contains("hit_boxes"));
    const auto& box = payload["hit_boxes"][0][0];
    CHECK(box["id"] == "hitbox_alpha");
    CHECK(box["type"] == "hitbox");
    CHECK(box["enabled"] == true);
    CHECK(box["frame_range"]["start"] == 0);
    CHECK(box["frame_range"]["end"] == 0);
    CHECK(box["position"]["x"] == 2);
    CHECK(box["position"]["y"] == 1);
    CHECK(box["size"]["w"] == 6);
    CHECK(box["size"]["h"] == 8);
    CHECK(box["anchor_link"] == "root");
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
    attack_box.id = "attack_box_alpha";
    attack_box.type = "attack_box";
    attack_box.name = "attack_box";
    attack_box.enabled = false;
    attack_box.frame_start = 3;
    attack_box.frame_end = 5;
    attack_box.anchor_link = "hand_r";
    attack_box.set_rect(animation_update::FrameBoxRect{11, 3, 4, 15});
    attack_box.extrusion_amount = 2;
    attack_box.damage_amount = 12;
    attack_box.meta_json = R"({"category":"heavy"})";

    nlohmann::json payload = nlohmann::json::object();
    REQUIRE(devmode::room_box_payload::write_attack_box_frame_to_payload(
        payload,
        1,
        0,
        std::vector<animation_update::FrameAttackBox>{attack_box}));

    REQUIRE(payload.contains("attack_boxes"));
    const auto& box = payload["attack_boxes"][0][0];
    CHECK(box["id"] == "attack_box_alpha");
    CHECK(box["type"] == "attack_box");
    CHECK(box["enabled"] == false);
    CHECK(box["frame_range"]["start"] == 3);
    CHECK(box["frame_range"]["end"] == 5);
    CHECK(box["position"]["x"] == 4);
    CHECK(box["position"]["y"] == 3);
    CHECK(box["size"]["w"] == 7);
    CHECK(box["size"]["h"] == 12);
    CHECK(box["anchor_link"] == "hand_r");
    CHECK(box["damage_amount"] == 12);
    REQUIRE(box["meta"].is_object());
    CHECK(box["meta"]["category"] == "heavy");
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

TEST_CASE("make_unique_box_name ignores excluded sanitized name") {
    const std::vector<std::string> names{"hit box"};
    const std::string result =
        devmode::room_box_payload::make_unique_box_name("hit box", names, "hit_box", "hit box");
    CHECK(result == "hit_box");
}

TEST_CASE("Default room box factories populate canonical schema fields") {
    const std::vector<std::string> existing_hit_names;
    const std::vector<std::string> existing_attack_names;

    const auto hit_box = devmode::room_box_payload::make_default_hit_box(existing_hit_names, 64, 32);
    CHECK(!hit_box.id.empty());
    CHECK(hit_box.type == "hitbox");
    CHECK(hit_box.enabled == true);
    CHECK(hit_box.frame_start == -1);
    CHECK(hit_box.frame_end == -1);
    CHECK(hit_box.anchor_link.empty());

    const auto attack_box = devmode::room_box_payload::make_default_attack_box(existing_attack_names, 64, 32);
    CHECK(!attack_box.id.empty());
    CHECK(attack_box.type == "attack_box");
    CHECK(attack_box.enabled == true);
    CHECK(attack_box.frame_start == -1);
    CHECK(attack_box.frame_end == -1);
    CHECK(attack_box.anchor_link.empty());
    CHECK(attack_box.damage_amount == 0);
    CHECK(attack_box.meta_json == "{}");
}

TEST_CASE("Room box payload keeps explicit box id stable across writes") {
    animation_update::FrameHitBox hit_box{};
    hit_box.id = "stable_box_id";
    hit_box.type = "hitbox";
    hit_box.name = "hit_box";
    hit_box.set_position_and_size(10, 12, 3, 7);

    nlohmann::json payload = nlohmann::json::object();
    REQUIRE(devmode::room_box_payload::write_hit_box_frame_to_payload(
        payload,
        2,
        1,
        std::vector<animation_update::FrameHitBox>{hit_box}));
    REQUIRE(devmode::room_box_payload::write_hit_box_frame_to_payload(
        payload,
        2,
        1,
        std::vector<animation_update::FrameHitBox>{hit_box}));

    REQUIRE(payload.contains("hit_boxes"));
    CHECK(payload["hit_boxes"][1][0]["id"] == "stable_box_id");
    CHECK(payload["hit_boxes"][1][0]["frame_range"]["start"] == 1);
    CHECK(payload["hit_boxes"][1][0]["frame_range"]["end"] == 1);
}

TEST_CASE("Room box payload preserves rotation degrees in serialized schema") {
    animation_update::FrameHitBox hit_box{};
    hit_box.id = "rotated_hit";
    hit_box.type = "hitbox";
    hit_box.name = "rotated_hit";
    hit_box.set_rect(animation_update::FrameBoxRect{20, 20, 30, 24});
    hit_box.set_rotation_degrees(90.0f);

    nlohmann::json payload = nlohmann::json::object();
    REQUIRE(devmode::room_box_payload::write_hit_box_frame_to_payload(
        payload,
        1,
        0,
        std::vector<animation_update::FrameHitBox>{hit_box}));

    const auto& box = payload["hit_boxes"][0][0];
    REQUIRE(box.contains("rotation_degrees"));
    CHECK(box["rotation_degrees"].get<double>() == doctest::Approx(90.0));
}
