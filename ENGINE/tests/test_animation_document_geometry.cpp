#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

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

TEST_CASE("AnimationDocument keeps local geometry and local movement when inherit_data resolves false") {
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
    CHECK((*payload)["movement"][0][0] == 0);
    CHECK((*payload)["movement"][0][1] == 0);
    CHECK((*payload)["movement"][0][2] == 0);
    CHECK((*payload)["movement"][1][0] == 0);
    CHECK((*payload)["movement"][1][1] == 0);
    CHECK((*payload)["movement"][1][2] == 0);

    REQUIRE(payload->contains("movement_total"));
    CHECK((*payload)["movement_total"]["dx"] == 0);
    CHECK((*payload)["movement_total"]["dy"] == 0);
    CHECK((*payload)["movement_total"]["dz"] == 0);

    REQUIRE(payload->contains("anchor_points"));
    CHECK((*payload)["anchor_points"][1][0]["name"] == "local_anchor");
    REQUIRE(payload->contains("hit_boxes"));
    CHECK((*payload)["hit_boxes"][1][0]["name"] == "local_hurt");
    REQUIRE(payload->contains("attack_boxes"));
    CHECK((*payload)["attack_boxes"][1][0]["name"] == "local_attack");
}

TEST_CASE("AnimationDocument strips legacy inversion keys without mapping to invert_x_y_z") {
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
                  {"flipped_source", true},
                  {"flip_vertical_source", true},
                  {"flip_movement_horizontal", true},
                  {"flip_movement_vertical", true},
                  {"derived_modifiers",
                   {{"flipX", true},
                    {"flipY", true},
                    {"flipMovementX", true},
                    {"flipMovementY", true},
                    {"reverse", false}}},
              }},
         }},
    };

    document.load_from_manifest(manifest, std::filesystem::path{}, nullptr);

    const auto payload = document.animation_payload_json("derived");
    REQUIRE(payload.has_value());
    REQUIRE(payload->is_object());

    CHECK(payload->contains("inherit_data"));
    CHECK((*payload)["inherit_data"].get<bool>());
    CHECK_FALSE(payload->contains("inherit_source_movement"));

    REQUIRE(payload->contains("derived_modifiers"));
    CHECK_FALSE((*payload)["derived_modifiers"].contains("flipMovementX"));
    CHECK_FALSE((*payload)["derived_modifiers"].contains("flipMovementY"));
    CHECK_FALSE((*payload)["derived_modifiers"].contains("flipX"));
    CHECK_FALSE((*payload)["derived_modifiers"].contains("flipY"));
    CHECK((*payload)["derived_modifiers"]["reverse"] == false);

    CHECK_FALSE(payload->contains("flipped_source"));
    CHECK_FALSE(payload->contains("flip_vertical_source"));
    CHECK_FALSE(payload->contains("flip_movement_horizontal"));
    CHECK_FALSE(payload->contains("flip_movement_vertical"));

    CHECK((*payload)["invert_x"] == false);
    CHECK((*payload)["invert_y"] == false);
    CHECK((*payload)["invert_z"] == false);

    CHECK_FALSE(payload->contains("movement"));
    CHECK_FALSE(payload->contains("movement_total"));
    CHECK_FALSE(payload->contains("anchor_points"));
    CHECK_FALSE(payload->contains("hit_boxes"));
    CHECK_FALSE(payload->contains("attack_boxes"));
}

TEST_CASE("AnimationDocument movement_total includes frame zero deltas") {
    animation_editor::AnimationDocument document;
    const nlohmann::json manifest = {
        {"animations",
         {
             {"default",
              {
                  {"source", {{"kind", "folder"}, {"path", "default"}, {"name", ""}}},
                  {"number_of_frames", 2},
                  {"movement",
                   nlohmann::json::array({nlohmann::json::array({2, -1, 3}),
                                          nlohmann::json::array({4, 5, -2})})},
              }},
         }},
    };

    document.load_from_manifest(manifest, std::filesystem::path{}, nullptr);

    const auto payload = document.animation_payload_json("default");
    REQUIRE(payload.has_value());
    REQUIRE(payload->is_object());
    REQUIRE(payload->contains("movement_total"));
    CHECK((*payload)["movement_total"]["dx"] == 6);
    CHECK((*payload)["movement_total"]["dy"] == 4);
    CHECK((*payload)["movement_total"]["dz"] == 1);
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

TEST_CASE("AnimationDocument preserves vertical frame inversion for animation-sourced payloads") {
    animation_editor::AnimationDocument document;
    const nlohmann::json manifest = {
        {"animations",
         {
             {"base",
              {
                  {"source", {{"kind", "folder"}, {"path", "base"}, {"name", ""}}},
                  {"number_of_frames", 2},
              }},
             {"derived",
              {
                  {"source", {{"kind", "animation"}, {"path", ""}, {"name", "base"}}},
                  {"number_of_frames", 2},
                  {"invert_frames_horizontal", true},
                  {"invert_frames_vertical", true},
                  {"inherit_data", false},
                  {"movement",
                   nlohmann::json::array({nlohmann::json::array({0, 0, 0}),
                                          nlohmann::json::array({4, 0, -2})})},
              }},
             {"folder_anim",
              {
                  {"source", {{"kind", "folder"}, {"path", "folder_anim"}, {"name", ""}}},
                  {"number_of_frames", 1},
                  {"invert_frames_vertical", true},
              }},
         }},
    };

    document.load_from_manifest(manifest, std::filesystem::path{}, nullptr);

    const auto derived_payload = document.animation_payload_json("derived");
    REQUIRE(derived_payload.has_value());
    REQUIRE(derived_payload->is_object());
    CHECK((*derived_payload)["invert_frames_horizontal"] == true);
    CHECK((*derived_payload)["invert_frames_vertical"] == true);

    const auto folder_payload = document.animation_payload_json("folder_anim");
    REQUIRE(folder_payload.has_value());
    REQUIRE(folder_payload->is_object());
    CHECK_FALSE(folder_payload->contains("invert_frames_horizontal"));
    CHECK_FALSE(folder_payload->contains("invert_frames_vertical"));
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

TEST_CASE("AnimationDocument canonicalizes animation tags and keeps local derived tags") {
    animation_editor::AnimationDocument document;
    const nlohmann::json manifest = {
        {"animations",
         {
             {"base",
              {
                  {"source", {{"kind", "folder"}, {"path", "base"}, {"name", ""}}},
                  {"number_of_frames", 2},
                  {"tags", nlohmann::json::array({" Run ", "run", "Combat"})},
              }},
             {"derived",
              {
                  {"source", {{"kind", "animation"}, {"path", ""}, {"name", "base"}}},
                  {"number_of_frames", 2},
                  {"inherit_data", true},
                  {"tags", nlohmann::json::array({" Variant ", "combat", "variant"})},
              }},
         }},
    };

    document.load_from_manifest(manifest, std::filesystem::path{}, nullptr);

    const auto base_payload = document.animation_payload_json("base");
    REQUIRE(base_payload.has_value());
    REQUIRE(base_payload->is_object());
    REQUIRE(base_payload->contains("tags"));
    CHECK((*base_payload)["tags"] == nlohmann::json::array({"run", "combat"}));

    const auto derived_payload = document.animation_payload_json("derived");
    REQUIRE(derived_payload.has_value());
    REQUIRE(derived_payload->is_object());
    REQUIRE(derived_payload->contains("tags"));
    CHECK((*derived_payload)["tags"] == nlohmann::json::array({"variant", "combat"}));
    CHECK((*derived_payload)["inherit_data"] == true);
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

TEST_CASE("AnimationEditorWindow programmatic close does not fire closed callback") {
    animation_editor::AnimationEditorWindow window;
    int closed_callbacks = 0;
    window.set_on_closed([&closed_callbacks]() { ++closed_callbacks; });

    window.set_visible(true, false);
    window.set_visible(false, false);

    CHECK(closed_callbacks == 0);
}

TEST_CASE("AnimationEditorWindow user close fires closed callback once") {
    animation_editor::AnimationEditorWindow window;
    int closed_callbacks = 0;
    window.set_on_closed([&closed_callbacks]() { ++closed_callbacks; });

    window.set_visible(true, false);
    window.set_visible(false, true);
    window.set_visible(false, true);

    CHECK(closed_callbacks == 1);
}

TEST_CASE("AnimationDocument emits structure change callbacks for create/delete/rename") {
    animation_editor::AnimationDocument document;
    document.load_from_manifest(nlohmann::json::object(), std::filesystem::path{}, nullptr);

    std::vector<animation_editor::AnimationDocument::StructureChangeEvent> events;
    document.set_on_structure_changed_callback(
        [&events](const animation_editor::AnimationDocument::StructureChangeEvent& event) {
            events.push_back(event);
        });

    document.create_animation("swing");
    document.rename_animation("swing", "swing_alt");
    document.delete_animation("swing_alt");

    REQUIRE(events.size() == 3);
    CHECK(events[0].kind == animation_editor::AnimationDocument::StructureChangeKind::Created);
    CHECK(events[0].animation_id == "swing");
    CHECK(events[1].kind == animation_editor::AnimationDocument::StructureChangeKind::Renamed);
    CHECK(events[1].previous_animation_id == "swing");
    CHECK(events[1].animation_id == "swing_alt");
    CHECK(events[2].kind == animation_editor::AnimationDocument::StructureChangeKind::Deleted);
    CHECK(events[2].animation_id == "swing_alt");
}

TEST_CASE("AnimationEditorWindow immediately invalidates full asset cache on structural edits") {
    const std::string asset_name = "cache_invalidate_structural_asset";
    const std::filesystem::path asset_dir =
        std::filesystem::temp_directory_path() / "engine_cache_invalidate_structural_asset";
    const std::filesystem::path cache_dir = std::filesystem::path("cache") / asset_name;
    std::error_code ec;
    std::filesystem::remove_all(cache_dir, ec);
    std::filesystem::create_directories(cache_dir, ec);
    REQUIRE_FALSE(ec);
    {
        std::ofstream marker(cache_dir / "marker.txt", std::ios::binary);
        marker << "marker";
    }
    REQUIRE(std::filesystem::exists(cache_dir / "marker.txt"));

    const nlohmann::json asset_manifest = {
        {"asset_name", asset_name},
        {"asset_directory", asset_dir.generic_string()},
        {"animations",
         {{"idle",
           {{"source", {{"kind", "folder"}, {"path", "idle"}, {"name", ""}}},
            {"number_of_frames", 1},
            {"on_end", "default"}}}}},
        {"start", "idle"},
    };

    auto info = std::make_shared<AssetInfo>(asset_name, asset_manifest);
    info->mark_bundle_refresh_on_close();

    animation_editor::AnimationEditorWindow window;
    window.set_info(info);
    auto document = window.document();
    REQUIRE(document);

    document->create_animation("new_anim");
    CHECK_FALSE(std::filesystem::exists(cache_dir));

    const auto pending = info->consume_pending_texture_rebuild_on_close();
    CHECK(pending.empty());

    std::filesystem::remove_all(cache_dir, ec);
    std::filesystem::remove_all(asset_dir, ec);
}
