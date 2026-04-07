#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <memory>

#include <nlohmann/json.hpp>

#include "assets/asset/asset_info.hpp"
#include "devtools/asset_editor/animation_editor_window/AnimationEditorWindow.hpp"
#include "devtools/asset_editor/animation_editor_window/AnimationDocument.hpp"
#include "devtools/core/manifest_store.hpp"
#include "core/manifest/manifest_loader.hpp"

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

    CHECK(payload->contains("inherit_data"));
    CHECK_FALSE((*payload)["inherit_data"].get<bool>());
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

    CHECK(payload->contains("inherit_data"));
    CHECK_FALSE((*payload)["inherit_data"].get<bool>());
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

TEST_CASE("AnimationDocument clears dirty state only when no newer edits exist") {
    animation_editor::AnimationDocument document;
    const nlohmann::json manifest = {
        {"animations",
         {{"default",
           {{"source", {{"kind", "folder"}, {"path", "default"}, {"name", ""}}},
            {"number_of_frames", 1},
            {"on_end", "default"}}}}},
    };
    document.load_from_manifest(manifest, std::filesystem::path{}, nullptr);

    auto payload = document.animation_payload_json("default");
    REQUIRE(payload.has_value());
    (*payload)["on_end"] = "loop";
    REQUIRE(document.update_animation_payload("default", *payload));

    const std::uint64_t first_revision = document.revision();
    CHECK(document.clear_dirty_if_revision_not_newer(first_revision));
    CHECK_FALSE(document.consume_dirty_flag());

    payload = document.animation_payload_json("default");
    REQUIRE(payload.has_value());
    (*payload)["on_end"] = "kill";
    REQUIRE(document.update_animation_payload("default", *payload));

    const std::uint64_t newer_revision = document.revision();
    CHECK(newer_revision > first_revision);
    CHECK_FALSE(document.clear_dirty_if_revision_not_newer(first_revision));
    CHECK(document.consume_dirty_flag());
}

TEST_CASE("AnimationEditorWindow manifest save fires single document-saved callback") {
    const std::filesystem::path asset_dir =
        std::filesystem::temp_directory_path() / "engine_animation_editor_save_chain_asset";
    std::error_code ec;
    std::filesystem::create_directories(asset_dir, ec);

    const nlohmann::json asset_manifest = {
        {"asset_name", "save_chain_asset"},
        {"asset_directory", asset_dir.generic_string()},
        {"animations",
         {{"idle",
           {{"source", {{"kind", "folder"}, {"path", "idle"}, {"name", ""}}},
            {"number_of_frames", 1},
            {"on_end", "default"}}}}},
        {"start", "idle"},
    };
    const nlohmann::json manifest_raw = {
        {"assets", {{"save_chain_asset", asset_manifest}}},
        {"maps", nlohmann::json::object()},
    };

    const std::filesystem::path manifest_path =
        std::filesystem::temp_directory_path() / "engine_animation_editor_manifest_save_chain_test.json";
    auto loader = [manifest_raw]() {
        manifest::ManifestData data;
        data.raw = manifest_raw;
        data.assets = manifest_raw.value("assets", nlohmann::json::object());
        data.maps = manifest_raw.value("maps", nlohmann::json::object());
        return data;
    };
    devmode::core::ManifestStore store(manifest_path, loader);

    animation_editor::AnimationEditorWindow window;
    window.set_manifest_store(&store);
    auto info = std::make_shared<AssetInfo>("save_chain_asset", asset_manifest);
    window.set_info(info);

    int saved_callbacks = 0;
    window.set_on_document_saved([&saved_callbacks]() { ++saved_callbacks; });
    saved_callbacks = 0;

    auto document = window.document();
    REQUIRE(document);
    auto payload = document->animation_payload_json("idle");
    REQUIRE(payload.has_value());
    (*payload)["on_end"] = "loop";
    REQUIRE(document->update_animation_payload("idle", *payload));
    CHECK(document->save_to_file_checked(true));

    CHECK(saved_callbacks == 1);

    std::filesystem::remove(manifest_path, ec);
    std::filesystem::remove_all(asset_dir, ec);
}
