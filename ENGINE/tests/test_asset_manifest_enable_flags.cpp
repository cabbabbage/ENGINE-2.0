#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

#include "assets/asset/Asset.hpp"
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

TEST_CASE("AssetInfo does not infer enable flags from payload presence") {
    const nlohmann::json metadata = {
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
         }}};

    AssetInfo info("manifest_no_inference_test_asset", metadata);
    const nlohmann::json payload = info.manifest_payload();
    CHECK(payload.contains("movement_enabled"));
    CHECK(payload.contains("attack_box_enabled"));
    CHECK(payload.contains("hitbox_enabled"));
    CHECK(payload.contains("floor_boxes_enabled"));
    CHECK_FALSE(payload.value("movement_enabled", true));
    CHECK_FALSE(payload.value("attack_box_enabled", true));
    CHECK_FALSE(payload.value("hitbox_enabled", true));
    CHECK_FALSE(payload.value("floor_boxes_enabled", true));

    const nlohmann::json default_anim = info.animation_payload("default");
    CHECK(default_anim.is_object());
    CHECK_FALSE(default_anim.contains("movement"));
    CHECK_FALSE(default_anim.contains("movement_paths"));
    CHECK_FALSE(default_anim.contains("movement_total"));
    CHECK_FALSE(default_anim.contains("hit_boxes"));
    CHECK_FALSE(default_anim.contains("attack_boxes"));
}

TEST_CASE("AssetInfo floor boxes normalize canonical fields and tags") {
    const nlohmann::json metadata = {
        {"movement_enabled", false},
        {"attack_box_enabled", false},
        {"hitbox_enabled", false},
        {"floor_boxes_enabled", true},
        {"floor_boxes",
         nlohmann::json::array({
             {{"id", "Boundary Box"}, {"name", "Boundary"}, {"is_boundary", true}, {"position_x", 1.0}, {"position_z", 2.0}, {"width", 16.0}, {"depth", 8.0}, {"enabled", true}},
             {{"id", "second-boundary"}, {"name", "Boundary 2"}, {"tags", nlohmann::json::array({"enemy_block", "Boundary", "enemy_block"})}, {"position_x", 5.0}, {"position_z", 6.0}, {"width", 10.0}, {"depth", 6.0}, {"enabled", true}},
         })},
    };

    AssetInfo info("floor_boxes_invariant_test_asset", metadata);
    const nlohmann::json payload = info.manifest_payload();
    REQUIRE(payload.value("floor_boxes_enabled", false));
    REQUIRE(payload.contains("floor_boxes"));
    REQUIRE(payload["floor_boxes"].is_array());
    REQUIRE(payload["floor_boxes"].size() == 2);

    for (const auto& floor_box : payload["floor_boxes"]) {
        REQUIRE(floor_box.is_object());
        CHECK(floor_box.contains("id"));
        CHECK(floor_box.contains("name"));
        CHECK(floor_box.contains("position_x"));
        CHECK(floor_box.contains("position_z"));
        CHECK(floor_box.contains("width"));
        CHECK(floor_box.contains("depth"));
        CHECK(floor_box.contains("enabled"));
        CHECK(floor_box.contains("tags"));
        CHECK(floor_box["tags"].is_array());
        CHECK_FALSE(floor_box.contains("is_boundary"));
        CHECK_FALSE(floor_box.contains("rotation_degrees"));
    }

    const auto& first_tags = payload["floor_boxes"][0]["tags"];
    CHECK(std::find(first_tags.begin(), first_tags.end(), "boundary") != first_tags.end());

    const auto& second_tags = payload["floor_boxes"][1]["tags"];
    CHECK(std::find(second_tags.begin(), second_tags.end(), "enemy_block") != second_tags.end());
    CHECK(std::find(second_tags.begin(), second_tags.end(), "boundary") != second_tags.end());
}

TEST_CASE("AssetInfo omits floor boxes payload when disabled") {
    const nlohmann::json metadata = {
        {"movement_enabled", false},
        {"attack_box_enabled", false},
        {"hitbox_enabled", false},
        {"floor_boxes_enabled", false},
        {"floor_boxes",
         nlohmann::json::array({
             {{"id", "floor_1"}, {"name", "box"}, {"tags", nlohmann::json::array({"boundary"})}, {"position_x", 0.0}, {"position_z", 0.0}, {"width", 1.0}, {"depth", 1.0}, {"enabled", true}},
         })},
    };

    AssetInfo info("floor_boxes_disabled_test_asset", metadata);
    const nlohmann::json payload = info.manifest_payload();
    CHECK(payload.contains("floor_boxes_enabled"));
    CHECK_FALSE(payload.value("floor_boxes_enabled", true));
    CHECK_FALSE(payload.contains("floor_boxes"));
}

TEST_CASE("AssetInfo omits floor boxes payload when enabled but empty") {
    const nlohmann::json metadata = {
        {"movement_enabled", false},
        {"attack_box_enabled", false},
        {"hitbox_enabled", false},
        {"floor_boxes_enabled", true},
    };

    AssetInfo info("floor_boxes_enabled_but_empty_test_asset", metadata);
    info.floor_boxes.clear();
    const nlohmann::json payload = info.manifest_payload();
    CHECK(payload.contains("floor_boxes_enabled"));
    CHECK(payload.value("floor_boxes_enabled", false));
    CHECK_FALSE(payload.contains("floor_boxes"));
}

TEST_CASE("AssetInfo save-time floor box sanitization applies defaults and canonical shape") {
    const nlohmann::json metadata = {
        {"movement_enabled", false},
        {"attack_box_enabled", false},
        {"hitbox_enabled", false},
        {"floor_boxes_enabled", true},
    };

    AssetInfo info("floor_boxes_save_sanitize_test_asset", metadata);
    AssetInfo::FloorBox first{};
    first.id = "";
    first.name = "   ";
    first.position_x = std::numeric_limits<float>::quiet_NaN();
    first.position_z = std::numeric_limits<float>::infinity();
    first.width = -32.0f;
    first.depth = std::numeric_limits<float>::quiet_NaN();
    first.enabled = true;
    first.tags = {"", "  ", "boundary", "Boundary", "enemy_block"};

    AssetInfo::FloorBox second{};
    second.id = "";
    second.name = "";
    second.position_x = 2.0f;
    second.position_z = -3.0f;
    second.width = 10.0f;
    second.depth = 4.0f;
    second.enabled = false;
    second.tags = {"enemy_block", "enemy_block", "ally_block"};

    info.floor_boxes = {first, second};

    const nlohmann::json payload = info.manifest_payload();
    REQUIRE(payload.contains("floor_boxes"));
    REQUIRE(payload["floor_boxes"].is_array());
    REQUIRE(payload["floor_boxes"].size() == 2);

    std::unordered_set<std::string> ids;
    for (const auto& floor_box : payload["floor_boxes"]) {
        REQUIRE(floor_box.is_object());
        CHECK(floor_box.contains("id"));
        CHECK(floor_box.contains("name"));
        CHECK(floor_box.contains("position_x"));
        CHECK(floor_box.contains("position_z"));
        CHECK(floor_box.contains("width"));
        CHECK(floor_box.contains("depth"));
        CHECK(floor_box.contains("enabled"));
        CHECK(floor_box.contains("tags"));
        CHECK(floor_box["tags"].is_array());
        CHECK_FALSE(floor_box.contains("is_boundary"));
        CHECK_FALSE(floor_box.contains("rotation_degrees"));

        REQUIRE(floor_box["id"].is_string());
        REQUIRE(floor_box["name"].is_string());
        CHECK_FALSE(floor_box["id"].get<std::string>().empty());
        CHECK_FALSE(floor_box["name"].get<std::string>().empty());
        CHECK(ids.insert(floor_box["id"].get<std::string>()).second);

        REQUIRE(floor_box["position_x"].is_number());
        REQUIRE(floor_box["position_z"].is_number());
        REQUIRE(floor_box["width"].is_number());
        REQUIRE(floor_box["depth"].is_number());
        CHECK(std::isfinite(floor_box["position_x"].get<float>()));
        CHECK(std::isfinite(floor_box["position_z"].get<float>()));
        CHECK(std::isfinite(floor_box["width"].get<float>()));
        CHECK(std::isfinite(floor_box["depth"].get<float>()));
        CHECK(floor_box["width"].get<float>() >= 0.0f);
        CHECK(floor_box["depth"].get<float>() >= 0.0f);
    }

    const auto& first_tags = payload["floor_boxes"][0]["tags"];
    CHECK(std::find(first_tags.begin(), first_tags.end(), "boundary") != first_tags.end());
    CHECK(std::find(first_tags.begin(), first_tags.end(), "enemy_block") != first_tags.end());

    const auto& second_tags = payload["floor_boxes"][1]["tags"];
    CHECK(std::find(second_tags.begin(), second_tags.end(), "enemy_block") != second_tags.end());
    CHECK(std::find(second_tags.begin(), second_tags.end(), "ally_block") != second_tags.end());
}

TEST_CASE("Runtime floor box boundary lookup uses cached boundary flag") {
    Asset::RuntimeFloorBox box{};
    box.boundary_tag = true;
    CHECK(box.has_tag("boundary"));

    box.boundary_tag = false;
    box.tags = {"enemy_block"};
    CHECK_FALSE(box.has_tag("boundary"));
    CHECK(box.has_tag("enemy_block"));

    box.tags.push_back("boundary");
    CHECK(box.has_tag("boundary"));
}
