#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <string>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include "devtools/asset_editor/animation_editor_window/AnimationDocument.hpp"
#include "devtools/asset_editor/animation_editor_window/AnimationInspectorPanel.hpp"
#include "devtools/asset_editor/animation_editor_window/SourceConfigPanel.hpp"

namespace {

std::string source_target(const nlohmann::json& payload) {
    if (!payload.is_object() || !payload.contains("source") || !payload["source"].is_object()) {
        return {};
    }
    const nlohmann::json& source = payload["source"];
    std::string name = source.value("name", std::string{});
    if (!name.empty()) {
        return name;
    }
    return source.value("path", std::string{});
}

std::shared_ptr<animation_editor::AnimationDocument> make_document(const nlohmann::json& manifest) {
    auto document = std::make_shared<animation_editor::AnimationDocument>();
    document->load_from_manifest(manifest, std::filesystem::path{}, nullptr);
    return document;
}

}  // namespace

TEST_CASE("SourceConfigPanel does not carry prior animation source selection when switching animations") {
    const nlohmann::json manifest = {
        {"animations",
         {
             {"base_a",
              {{"source", {{"kind", "folder"}, {"path", "base_a"}, {"name", ""}}},
               {"number_of_frames", 1}}},
             {"base_b",
              {{"source", {{"kind", "folder"}, {"path", "base_b"}, {"name", ""}}},
               {"number_of_frames", 1}}},
             {"child_a",
              {{"source", {{"kind", "animation"}, {"path", ""}, {"name", "base_b"}}},
               {"number_of_frames", 1},
               {"inherit_data", false}}},
             {"child_b",
              {{"source", {{"kind", "animation"}, {"path", ""}, {"name", "base_a"}}},
               {"number_of_frames", 1},
               {"inherit_data", false}}},
         }},
    };

    auto document = make_document(manifest);
    animation_editor::SourceConfigPanel panel;
    panel.set_bounds(SDL_Rect{0, 0, 320, 180});
    panel.set_document(document);

    panel.set_animation_id("child_a");
    panel.update();

    panel.set_animation_id("child_b");
    panel.update();

    const auto child_b_payload = document->animation_payload_json("child_b");
    REQUIRE(child_b_payload.has_value());
    REQUIRE(child_b_payload->is_object());
    CHECK(source_target(*child_b_payload) == "base_a");
}

TEST_CASE("SourceConfigPanel preserves local movement and geometry when switching source kind to animation") {
    const nlohmann::json movement_before =
        nlohmann::json::array({nlohmann::json::array({3, 4, 5}), nlohmann::json::array({6, 7, 8})});
    const nlohmann::json movement_total_before = nlohmann::json::object({{"dx", 9}, {"dy", 11}, {"dz", 13}});
    const nlohmann::json anchors_before = nlohmann::json::array({
        nlohmann::json::array({nlohmann::json{{"name", "hand"}, {"texture_x", 1}, {"texture_y", 2}, {"depth_offset", 0}}}),
        nlohmann::json::array(),
    });
    const nlohmann::json hit_boxes_before = nlohmann::json::array({nlohmann::json::array(), nlohmann::json::array()});
    const nlohmann::json attack_boxes_before =
        nlohmann::json::array({nlohmann::json::array(), nlohmann::json::array()});

    const nlohmann::json manifest = {
        {"animations",
         {
             {"base",
              {{"source", {{"kind", "folder"}, {"path", "base"}, {"name", ""}}},
               {"number_of_frames", 2}}},
             {"local",
              {{"source", {{"kind", "folder"}, {"path", "local"}, {"name", ""}}},
               {"number_of_frames", 2},
               {"inherit_data", false},
               {"movement", movement_before},
               {"movement_total", movement_total_before},
               {"anchor_points", anchors_before},
               {"hit_boxes", hit_boxes_before},
               {"attack_boxes", attack_boxes_before}}},
         }},
    };

    auto document = make_document(manifest);
    animation_editor::SourceConfigPanel panel;
    panel.set_bounds(SDL_Rect{0, 0, 320, 180});
    panel.set_document(document);
    panel.set_animation_id("local");

    panel.set_source_mode(animation_editor::SourceConfigPanel::SourceMode::kAnimation);
    panel.update();

    const auto payload = document->animation_payload_json("local");
    REQUIRE(payload.has_value());
    REQUIRE(payload->is_object());
    REQUIRE(payload->contains("source"));
    CHECK((*payload)["source"]["kind"] == "animation");
    CHECK((*payload)["inherit_data"] == false);
    CHECK((*payload)["movement"] == movement_before);
    CHECK((*payload)["movement_total"] == movement_total_before);
    CHECK((*payload)["anchor_points"] == anchors_before);
    CHECK((*payload)["hit_boxes"] == hit_boxes_before);
    CHECK((*payload)["attack_boxes"] == attack_boxes_before);
}

TEST_CASE("AnimationInspectorPanel deferred dropdown apply is source-idempotent after animation switch") {
    const nlohmann::json manifest = {
        {"animations",
         {
             {"base_a",
              {{"source", {{"kind", "folder"}, {"path", "base_a"}, {"name", ""}}},
               {"number_of_frames", 1}}},
             {"base_b",
              {{"source", {{"kind", "folder"}, {"path", "base_b"}, {"name", ""}}},
               {"number_of_frames", 1}}},
             {"child_a",
              {{"source", {{"kind", "animation"}, {"path", ""}, {"name", "base_b"}}},
               {"number_of_frames", 1},
               {"inherit_data", false}}},
             {"child_b",
              {{"source", {{"kind", "animation"}, {"path", ""}, {"name", "base_a"}}},
               {"number_of_frames", 1},
               {"inherit_data", false}}},
         }},
    };

    auto document = make_document(manifest);

    animation_editor::AnimationInspectorPanel inspector;
    inspector.set_bounds(SDL_Rect{0, 0, 420, 320});
    inspector.set_document(document);
    inspector.set_animation_id("child_a");
    inspector.update();

    inspector.set_animation_id("child_b");
    inspector.apply_dropdown_selections();

    const auto child_b_payload = document->animation_payload_json("child_b");
    REQUIRE(child_b_payload.has_value());
    REQUIRE(child_b_payload->is_object());
    CHECK(source_target(*child_b_payload) == "base_a");
}
