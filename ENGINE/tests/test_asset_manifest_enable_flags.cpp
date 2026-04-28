#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_info.hpp"

namespace {

bool has_candidate_named(const nlohmann::json& candidates, const std::string& name) {
    if (!candidates.is_array()) {
        return false;
    }
    for (const auto& candidate : candidates) {
        if (!candidate.is_object()) {
            continue;
        }
        if (candidate.value("name", std::string{}) == name) {
            return true;
        }
    }
    return false;
}

} // namespace

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
    CHECK(payload.contains("impassable_enabled"));
    CHECK(payload.contains("floor_boxes_enabled"));
    CHECK_FALSE(payload.value("movement_enabled", true));
    CHECK_FALSE(payload.value("attack_box_enabled", true));
    CHECK_FALSE(payload.value("hitbox_enabled", true));
    CHECK_FALSE(payload.value("impassable_enabled", true));
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
    CHECK(std::find(first_tags.begin(), first_tags.end(), "boundary") == first_tags.end());

    const auto& second_tags = payload["floor_boxes"][1]["tags"];
    CHECK(std::find(second_tags.begin(), second_tags.end(), "enemy_block") != second_tags.end());
    CHECK(std::find(second_tags.begin(), second_tags.end(), "boundary") == second_tags.end());
}

TEST_CASE("AssetInfo floor box candidate payload remains absent when authoring omits it") {
    const nlohmann::json metadata = {
        {"floor_boxes_enabled", true},
        {"floor_boxes",
         nlohmann::json::array({
             {{"id", "floor_box_1"},
              {"name", "Floor Box 1"},
              {"position_x", 0.0},
              {"position_z", 0.0},
              {"width", 12.0},
              {"depth", 10.0},
              {"enabled", true},
              {"tags", nlohmann::json::array()}}
         })}
    };

    AssetInfo info("floor_boxes_candidate_absent_test_asset", metadata);
    REQUIRE(info.floor_boxes_payload().size() == 1);
    CHECK_FALSE(info.floor_boxes_payload()[0].candidate.has_value());

    const nlohmann::json payload = info.manifest_payload();
    REQUIRE(payload.contains("floor_boxes"));
    REQUIRE(payload["floor_boxes"].is_array());
    REQUIRE(payload["floor_boxes"].size() == 1);
    CHECK_FALSE(payload["floor_boxes"][0].contains("candidate"));
}

TEST_CASE("AssetInfo floor box candidate payload sanitizes missing and invalid fields") {
    const nlohmann::json metadata = {
        {"floor_boxes_enabled", true},
        {"floor_boxes",
         nlohmann::json::array({
             {{"id", "box_missing_resolution"},
              {"name", "Box Missing Resolution"},
              {"position_x", 0.0},
              {"position_z", 0.0},
              {"width", 16.0},
              {"depth", 16.0},
              {"enabled", true},
              {"tags", nlohmann::json::array()},
              {"candidate",
               nlohmann::json::object({
                   {"candidates", nlohmann::json::array({nlohmann::json::object({{"name", "spawn_a"}, {"chance", 25}})})}
               })}},
             {{"id", "box_invalid_payload"},
              {"name", "Box Invalid Payload"},
              {"position_x", 0.0},
              {"position_z", 0.0},
              {"width", 16.0},
              {"depth", 16.0},
              {"enabled", true},
              {"tags", nlohmann::json::array()},
              {"candidate",
               nlohmann::json::object({
                   {"candidates", "invalid"},
                   {"grid_resolution", "invalid"}
               })}},
             {{"id", "box_high_resolution"},
              {"name", "Box High Resolution"},
              {"position_x", 0.0},
              {"position_z", 0.0},
              {"width", 16.0},
              {"depth", 16.0},
              {"enabled", true},
              {"tags", nlohmann::json::array()},
              {"candidate",
               nlohmann::json::object({
                   {"candidates", nlohmann::json::array({nlohmann::json::object({{"name", "spawn_b"}, {"chance", 10}})})},
                   {"grid_resolution", 999}
               })}},
             {{"id", "box_low_resolution"},
              {"name", "Box Low Resolution"},
              {"position_x", 0.0},
              {"position_z", 0.0},
              {"width", 16.0},
              {"depth", 16.0},
              {"enabled", true},
              {"tags", nlohmann::json::array()},
              {"candidate",
               nlohmann::json::object({
                   {"candidates", nlohmann::json::array({nlohmann::json::object({{"name", "spawn_c"}, {"chance", 10}})})},
                   {"grid_resolution", 1}
               })}},
         })}
    };

    AssetInfo info("floor_boxes_candidate_sanitize_test_asset", metadata);
    const nlohmann::json payload = info.manifest_payload();
    REQUIRE(payload.contains("floor_boxes"));
    REQUIRE(payload["floor_boxes"].is_array());
    REQUIRE(payload["floor_boxes"].size() == 4);

    const auto& missing_resolution = payload["floor_boxes"][0]["candidate"];
    REQUIRE(missing_resolution.is_object());
    REQUIRE(missing_resolution.contains("grid_resolution"));
    CHECK(missing_resolution["grid_resolution"] == 4);
    REQUIRE(missing_resolution.contains("candidates"));
    REQUIRE(missing_resolution["candidates"].is_array());
    CHECK(has_candidate_named(missing_resolution["candidates"], "null"));

    const auto& invalid_payload = payload["floor_boxes"][1]["candidate"];
    REQUIRE(invalid_payload.is_object());
    REQUIRE(invalid_payload.contains("grid_resolution"));
    CHECK(invalid_payload["grid_resolution"] == 4);
    REQUIRE(invalid_payload.contains("candidates"));
    REQUIRE(invalid_payload["candidates"].is_array());
    CHECK(has_candidate_named(invalid_payload["candidates"], "null"));

    const auto& high_resolution = payload["floor_boxes"][2]["candidate"];
    REQUIRE(high_resolution.is_object());
    CHECK(high_resolution["grid_resolution"] == 8);
    CHECK(has_candidate_named(high_resolution["candidates"], "null"));

    const auto& low_resolution = payload["floor_boxes"][3]["candidate"];
    REQUIRE(low_resolution.is_object());
    CHECK(low_resolution["grid_resolution"] == 2);
    CHECK(has_candidate_named(low_resolution["candidates"], "null"));
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
    CHECK(std::find(first_tags.begin(), first_tags.end(), "boundary") == first_tags.end());
    CHECK(std::find(first_tags.begin(), first_tags.end(), "enemy_block") != first_tags.end());

    const auto& second_tags = payload["floor_boxes"][1]["tags"];
    CHECK(std::find(second_tags.begin(), second_tags.end(), "enemy_block") != second_tags.end());
    CHECK(std::find(second_tags.begin(), second_tags.end(), "ally_block") != second_tags.end());
}

TEST_CASE("Runtime floor box tag lookup uses canonical tag vector") {
    Asset::RuntimeFloorBox box{};
    box.tags = {"enemy_block"};
    CHECK_FALSE(box.has_tag("boundary"));
    CHECK(box.has_tag("enemy_block"));

    box.tags.push_back("boundary");
    CHECK(box.has_tag("boundary"));
}

TEST_CASE("AssetInfo omits impassable shapes payload when disabled") {
    const nlohmann::json metadata = {
        {"impassable_enabled", false},
        {"impassable_shapes",
         nlohmann::json::array({
             {{"id", "imp_shape_1"},
              {"name", "Impassable Shape 1"},
              {"enabled", true},
              {"points",
               nlohmann::json::array({
                   {{"x", 4}, {"y", 8}},
                   {{"x", 24}, {"y", 8}},
                   {{"x", 14}, {"y", 26}},
               })}}
         })},
    };

    AssetInfo info("impassable_shapes_disabled_test_asset", metadata);
    const nlohmann::json payload = info.manifest_payload();
    CHECK(payload.contains("impassable_enabled"));
    CHECK_FALSE(payload.value("impassable_enabled", true));
    CHECK_FALSE(payload.contains("impassable_shapes"));
}

TEST_CASE("AssetInfo omits impassable shapes payload when enabled but empty") {
    const nlohmann::json metadata = {
        {"impassable_enabled", true},
        {"impassable_shapes", nlohmann::json::array()},
    };

    AssetInfo info("impassable_shapes_enabled_empty_test_asset", metadata);
    info.impassable_shapes.clear();
    const nlohmann::json payload = info.manifest_payload();
    CHECK(payload.contains("impassable_enabled"));
    CHECK(payload.value("impassable_enabled", false));
    CHECK_FALSE(payload.contains("impassable_shapes"));
}

TEST_CASE("AssetInfo impassable shapes normalize canonical fields") {
    const nlohmann::json metadata = {
        {"impassable_enabled", true},
        {"impassable_shapes",
         nlohmann::json::array({
             {{"id", "Shape A"},
              {"name", "Impassable"},
              {"enabled", true},
              {"points",
               nlohmann::json::array({
                   {{"x", 10}, {"y", 10}},
                   {{"x", 40}, {"y", 10}},
                   {{"x", 40}, {"y", 34}},
                   {{"x", 10}, {"y", 34}},
               })}},
             {{"id", "Shape A"},
              {"name", "Impassable"},
              {"enabled", false},
              {"points",
               nlohmann::json::array({
                   {{"x", 60}, {"y", 70}},
                   {{"x", 100}, {"y", 70}},
                   {{"x", 100}, {"y", 110}},
                   {{"x", 60}, {"y", 110}},
               })}},
             {{"id", "too_small"},
              {"name", "Too Small"},
              {"enabled", true},
              {"points",
               nlohmann::json::array({
                   {{"x", 0}, {"y", 0}},
                   {{"x", 10}, {"y", 0}},
               })}},
             {{"id", "self_intersect"},
              {"name", "Self Intersect"},
              {"enabled", true},
              {"points",
               nlohmann::json::array({
                   {{"x", 0}, {"y", 0}},
                   {{"x", 20}, {"y", 20}},
                   {{"x", 0}, {"y", 20}},
                   {{"x", 20}, {"y", 0}},
               })}},
         })},
    };

    AssetInfo info("impassable_shapes_normalize_test_asset", metadata);
    const nlohmann::json payload = info.manifest_payload();
    CHECK(payload.contains("impassable_enabled"));
    CHECK(payload.value("impassable_enabled", false));
    REQUIRE(payload.contains("impassable_shapes"));
    REQUIRE(payload["impassable_shapes"].is_array());
    REQUIRE(payload["impassable_shapes"].size() == 2);

    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> names;
    for (const auto& shape : payload["impassable_shapes"]) {
        REQUIRE(shape.is_object());
        CHECK(shape.contains("id"));
        CHECK(shape.contains("name"));
        CHECK(shape.contains("enabled"));
        CHECK(shape.contains("points"));
        CHECK_FALSE(shape.contains("type"));
        CHECK_FALSE(shape.contains("extrusion_amount"));
        CHECK_FALSE(shape.contains("anchor_link"));
        CHECK_FALSE(shape.contains("rotation_degrees"));
        CHECK_FALSE(shape.contains("position"));
        CHECK_FALSE(shape.contains("size"));
        CHECK_FALSE(shape.contains("corners"));

        REQUIRE(shape["id"].is_string());
        REQUIRE(shape["name"].is_string());
        CHECK_FALSE(shape["id"].get<std::string>().empty());
        CHECK_FALSE(shape["name"].get<std::string>().empty());
        CHECK(ids.insert(shape["id"].get<std::string>()).second);
        CHECK(names.insert(shape["name"].get<std::string>()).second);

        REQUIRE(shape["points"].is_array());
        CHECK(shape["points"].size() >= 3);

        long long area2 = 0;
        const auto& points = shape["points"];
        for (std::size_t i = 0; i < points.size(); ++i) {
            const auto& a = points[i];
            const auto& b = points[(i + 1) % points.size()];
            REQUIRE(a.is_object());
            REQUIRE(b.is_object());
            REQUIRE(a.contains("x"));
            REQUIRE(a.contains("y"));
            REQUIRE(b.contains("x"));
            REQUIRE(b.contains("y"));
            REQUIRE(a["x"].is_number_integer());
            REQUIRE(a["y"].is_number_integer());
            REQUIRE(b["x"].is_number_integer());
            REQUIRE(b["y"].is_number_integer());
            area2 += static_cast<long long>(a["x"].get<int>()) * static_cast<long long>(b["y"].get<int>()) -
                     static_cast<long long>(b["x"].get<int>()) * static_cast<long long>(a["y"].get<int>());
        }
        CHECK(area2 > 0);
    }
}
