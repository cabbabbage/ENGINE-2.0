#include <doctest/doctest.h>

#include "assets/asset/asset_info.hpp"

TEST_CASE("AssetInfo strips animation payload systems when disabled") {
    const nlohmann::json metadata = {
        {"movement_enabled", false},
        {"attack_box_enabled", false},
        {"hitbox_enabled", false},
        {"floor_boxes_enabled", false},
        {"animations",
         {
             {"default",
              {
                  {"source", {{"kind", "folder"}, {"path", "default"}}},
                  {"movement", nlohmann::json::array({nlohmann::json::array({1, 0, 0})})},
                  {"movement_total", {{"dx", 1}, {"dy", 0}, {"dz", 0}}},
                  {"hit_boxes", nlohmann::json::array({nlohmann::json::array()})},
                  {"attack_boxes", nlohmann::json::array({nlohmann::json::array()})},
              }},
         }},
    };

    AssetInfo info("manifest_enable_flags_test_asset", metadata);
    const nlohmann::json default_anim = info.animation_payload("default");
    CHECK(default_anim.is_object());
    CHECK_FALSE(default_anim.contains("movement"));
    CHECK_FALSE(default_anim.contains("movement_paths"));
    CHECK_FALSE(default_anim.contains("movement_total"));
    CHECK_FALSE(default_anim.contains("hit_boxes"));
    CHECK_FALSE(default_anim.contains("attack_boxes"));
}

TEST_CASE("AssetInfo floor boxes normalize canonical fields and single-boundary invariant") {
    const nlohmann::json metadata = {
        {"movement_enabled", false},
        {"attack_box_enabled", false},
        {"hitbox_enabled", false},
        {"floor_boxes_enabled", true},
        {"floor_boxes",
         nlohmann::json::array({
             {{"id", "Boundary Box"}, {"name", "Boundary"}, {"is_boundary", true}, {"position_x", 1.0}, {"position_z", 2.0}, {"width", 16.0}, {"depth", 8.0}, {"rotation_degrees", 0.0}, {"enabled", true}},
             {{"id", "second-boundary"}, {"name", "Boundary 2"}, {"is_boundary", true}, {"position_x", 5.0}, {"position_z", 6.0}, {"width", 10.0}, {"depth", 6.0}, {"rotation_degrees", 45.0}, {"enabled", true}},
         })},
    };

    AssetInfo info("floor_boxes_invariant_test_asset", metadata);
    const nlohmann::json payload = info.manifest_payload();
    REQUIRE(payload.value("floor_boxes_enabled", false));
    REQUIRE(payload.contains("floor_boxes"));
    REQUIRE(payload["floor_boxes"].is_array());
    REQUIRE(payload["floor_boxes"].size() == 2);

    std::size_t boundary_count = 0;
    for (const auto& floor_box : payload["floor_boxes"]) {
        REQUIRE(floor_box.is_object());
        CHECK(floor_box.contains("id"));
        CHECK(floor_box.contains("name"));
        CHECK(floor_box.contains("is_boundary"));
        CHECK(floor_box.contains("position_x"));
        CHECK(floor_box.contains("position_z"));
        CHECK(floor_box.contains("width"));
        CHECK(floor_box.contains("depth"));
        CHECK(floor_box.contains("rotation_degrees"));
        CHECK(floor_box.contains("enabled"));
        if (floor_box.value("is_boundary", false)) {
            ++boundary_count;
        }
    }
    CHECK(boundary_count == 1);
}

TEST_CASE("AssetInfo omits floor boxes payload when disabled") {
    const nlohmann::json metadata = {
        {"movement_enabled", false},
        {"attack_box_enabled", false},
        {"hitbox_enabled", false},
        {"floor_boxes_enabled", false},
        {"floor_boxes",
         nlohmann::json::array({
             {{"id", "floor_1"}, {"name", "box"}, {"is_boundary", false}, {"position_x", 0.0}, {"position_z", 0.0}, {"width", 1.0}, {"depth", 1.0}, {"rotation_degrees", 0.0}, {"enabled", true}},
         })},
    };

    AssetInfo info("floor_boxes_disabled_test_asset", metadata);
    const nlohmann::json payload = info.manifest_payload();
    CHECK(payload.contains("floor_boxes_enabled"));
    CHECK_FALSE(payload.value("floor_boxes_enabled", true));
    CHECK_FALSE(payload.contains("floor_boxes"));
}
