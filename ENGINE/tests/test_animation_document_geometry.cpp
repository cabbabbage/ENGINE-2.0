#include <doctest/doctest.h>

#include <filesystem>

#include <nlohmann/json.hpp>

#include "devtools/asset_editor/animation_editor_window/AnimationDocument.hpp"

namespace {

nlohmann::json make_box(const char* name, int left, int top, int right, int bottom) {
    return nlohmann::json{
        {"name", name},
        {"corners",
         nlohmann::json::array({
             nlohmann::json{{"texture_x", left}, {"texture_y", top}},
             nlohmann::json{{"texture_x", right}, {"texture_y", top}},
             nlohmann::json{{"texture_x", right}, {"texture_y", bottom}},
             nlohmann::json{{"texture_x", left}, {"texture_y", bottom}},
         })},
    };
}

}  // namespace

TEST_CASE("AnimationDocument materializes legacy movement inheritance when non-movement geometry is local") {
    animation_editor::AnimationDocument document;
    const nlohmann::json manifest = {
        {"animations",
         {
             {"base",
              {
                  {"source", {{"kind", "folder"}, {"path", "base"}, {"name", ""}}},
                  {"number_of_frames", 2},
                  {"movement", nlohmann::json::array({nlohmann::json::array({0, 0, 0}),
                                                      nlohmann::json::array({3, 0, 4})})},
                  {"anchor_points",
                   nlohmann::json::array({
                       nlohmann::json::array(),
                       nlohmann::json::array(
                           {nlohmann::json{{"name", "hand"},
                                           {"texture_x", 2},
                                           {"texture_y", 1},
                                           {"depth_offset", 0}}}),
                   })},
                  {"hit_boxes",
                   nlohmann::json::array(
                       {nlohmann::json::array(), nlohmann::json::array({make_box("hurt", 1, 2, 5, 6)})})},
                  {"attack_boxes",
                   nlohmann::json::array(
                       {nlohmann::json::array(),
                        nlohmann::json::array(
                            {nlohmann::json::object({{"name", "slash"},
                                                     {"damage_amount", 7},
                                                     {"corners", make_box("slash", 3, 4, 8, 9)["corners"]}})})})},
              }},
             {"derived",
              {
                  {"source", {{"kind", "animation"}, {"path", ""}, {"name", "base"}}},
                  {"number_of_frames", 2},
                  {"inherit_source_movement", true},
                  {"anchor_points",
                   nlohmann::json::array({
                       nlohmann::json::array(),
                       nlohmann::json::array(
                           {nlohmann::json{{"name", "local_anchor"},
                                           {"texture_x", 9},
                                           {"texture_y", 8},
                                           {"depth_offset", 1}}}),
                   })},
                  {"hit_boxes",
                   nlohmann::json::array({nlohmann::json::array(),
                                          nlohmann::json::array({make_box("local_hurt", 4, 5, 7, 8)})})},
                  {"attack_boxes",
                   nlohmann::json::array(
                       {nlohmann::json::array(),
                        nlohmann::json::array(
                            {nlohmann::json::object({{"name", "local_attack"},
                                                     {"damage_amount", 11},
                                                     {"corners", make_box("local_attack", 6, 7, 9, 10)["corners"]}})})})},
              }},
         }},
    };

    document.load_from_manifest(manifest, std::filesystem::path{}, nullptr);

    const auto payload = document.animation_payload_json("derived");
    REQUIRE(payload.has_value());
    REQUIRE(payload->is_object());

    CHECK(payload->contains("inherit_source_geometry"));
    CHECK_FALSE((*payload)["inherit_source_geometry"].get<bool>());
    CHECK_FALSE(payload->contains("inherit_source_movement"));

    REQUIRE(payload->contains("movement"));
    REQUIRE((*payload)["movement"].is_array());
    REQUIRE((*payload)["movement"].size() == 2);
    CHECK((*payload)["movement"][1][0] == 3);
    CHECK((*payload)["movement"][1][1] == 0);
    CHECK((*payload)["movement"][1][2] == 4);

    REQUIRE(payload->contains("movement_total"));
    CHECK((*payload)["movement_total"]["dx"] == 3);
    CHECK((*payload)["movement_total"]["dy"] == 0);
    CHECK((*payload)["movement_total"]["dz"] == 4);

    REQUIRE(payload->contains("anchor_points"));
    CHECK((*payload)["anchor_points"][1][0]["name"] == "local_anchor");
    REQUIRE(payload->contains("hit_boxes"));
    CHECK((*payload)["hit_boxes"][1][0]["name"] == "local_hurt");
    REQUIRE(payload->contains("attack_boxes"));
    CHECK((*payload)["attack_boxes"][1][0]["name"] == "local_attack");
}

TEST_CASE("AnimationDocument materializes legacy movement-only flips into local movement") {
    animation_editor::AnimationDocument document;
    const nlohmann::json manifest = {
        {"animations",
         {
             {"base",
              {
                  {"source", {{"kind", "folder"}, {"path", "base"}, {"name", ""}}},
                  {"number_of_frames", 2},
                  {"movement", nlohmann::json::array({nlohmann::json::array({0, 0, 0}),
                                                      nlohmann::json::array({2, 0, 1})})},
              }},
             {"derived",
              {
                  {"source", {{"kind", "animation"}, {"path", ""}, {"name", "base"}}},
                  {"number_of_frames", 2},
                  {"inherit_source_movement", true},
                  {"derived_modifiers", {{"flipMovementX", true}}},
              }},
         }},
    };

    document.load_from_manifest(manifest, std::filesystem::path{}, nullptr);

    const auto payload = document.animation_payload_json("derived");
    REQUIRE(payload.has_value());
    REQUIRE(payload->is_object());

    CHECK(payload->contains("inherit_source_geometry"));
    CHECK_FALSE((*payload)["inherit_source_geometry"].get<bool>());
    CHECK_FALSE(payload->contains("inherit_source_movement"));

    REQUIRE(payload->contains("derived_modifiers"));
    CHECK_FALSE((*payload)["derived_modifiers"].contains("flipMovementX"));
    CHECK_FALSE((*payload)["derived_modifiers"].contains("flipMovementY"));

    REQUIRE(payload->contains("movement"));
    REQUIRE((*payload)["movement"].is_array());
    REQUIRE((*payload)["movement"].size() == 2);
    CHECK((*payload)["movement"][1][0] == -2);
    CHECK((*payload)["movement"][1][1] == 0);
    CHECK((*payload)["movement"][1][2] == 1);

    REQUIRE(payload->contains("anchor_points"));
    CHECK((*payload)["anchor_points"][0].empty());
    CHECK((*payload)["anchor_points"][1].empty());
}

TEST_CASE("AnimationDocument normalizes missing on_end and strips legacy loop") {
    animation_editor::AnimationDocument document;
    const nlohmann::json manifest = {
        {"animations",
         {{"default",
           {{"source", {{"kind", "folder"}, {"path", "default"}, {"name", ""}}},
            {"number_of_frames", 1},
            {"loop", true}}}}},
    };

    document.load_from_manifest(manifest, std::filesystem::path{}, nullptr);

    const auto payload = document.animation_payload_json("default");
    REQUIRE(payload.has_value());
    REQUIRE(payload->is_object());
    CHECK(payload->contains("on_end"));
    CHECK((*payload)["on_end"] == "default");
    CHECK_FALSE(payload->contains("loop"));
}

TEST_CASE("AnimationDocument create_animation emits on_end without loop") {
    animation_editor::AnimationDocument document;
    document.load_from_manifest(nlohmann::json::object(), std::filesystem::path{}, nullptr);
    document.create_animation("swing");

    const auto payload = document.animation_payload_json("swing");
    REQUIRE(payload.has_value());
    REQUIRE(payload->is_object());
    CHECK(payload->contains("on_end"));
    CHECK((*payload)["on_end"] == "default");
    CHECK_FALSE(payload->contains("loop"));
}
