#include <doctest/doctest.h>
#include <cmath>
#include <limits>
#include <unordered_set>

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
    info.floor_boxes = {
        AssetInfo::FloorBox{
            "",
            "   ",
            true,
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity(),
            -32.0f,
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity(),
            true,
        },
        AssetInfo::FloorBox{
            "",
            "",
            true,
            2.0f,
            -3.0f,
            10.0f,
            4.0f,
            45.0f,
            false,
        },
    };

    const nlohmann::json payload = info.manifest_payload();
    REQUIRE(payload.contains("floor_boxes"));
    REQUIRE(payload["floor_boxes"].is_array());
    REQUIRE(payload["floor_boxes"].size() == 2);

    std::size_t boundary_count = 0;
    std::unordered_set<std::string> ids;
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

        REQUIRE(floor_box["id"].is_string());
        REQUIRE(floor_box["name"].is_string());
        CHECK_FALSE(floor_box["id"].get<std::string>().empty());
        CHECK_FALSE(floor_box["name"].get<std::string>().empty());
        CHECK(ids.insert(floor_box["id"].get<std::string>()).second);

        REQUIRE(floor_box["position_x"].is_number());
        REQUIRE(floor_box["position_z"].is_number());
        REQUIRE(floor_box["width"].is_number());
        REQUIRE(floor_box["depth"].is_number());
        REQUIRE(floor_box["rotation_degrees"].is_number());
        CHECK(std::isfinite(floor_box["position_x"].get<float>()));
        CHECK(std::isfinite(floor_box["position_z"].get<float>()));
        CHECK(std::isfinite(floor_box["width"].get<float>()));
        CHECK(std::isfinite(floor_box["depth"].get<float>()));
        CHECK(std::isfinite(floor_box["rotation_degrees"].get<float>()));
        CHECK(floor_box["width"].get<float>() >= 0.0f);
        CHECK(floor_box["depth"].get<float>() >= 0.0f);

        if (floor_box.value("is_boundary", false)) {
            ++boundary_count;
        }
    }
    CHECK(boundary_count == 1);
}
