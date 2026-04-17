#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/manifest/manifest_loader.hpp"
#include "devtools/asset_editor/animation_editor_window/AnimationDocument.hpp"
#include "devtools/asset_editor/animation_editor_window/AnimationTagsPanel.hpp"
#include "tag_utils.hpp"

namespace {

void write_assets_manifest(const nlohmann::json& assets) {
    manifest::ManifestData data = manifest::load_manifest();
    data.raw["assets"] = assets;
    if (!data.raw.contains("maps") || !data.raw["maps"].is_object()) {
        data.raw["maps"] = nlohmann::json::object();
    }
    data.assets = data.raw["assets"];
    data.maps = data.raw["maps"];
    manifest::save_manifest(data);
    tag_utils::notify_tags_changed();
}

std::shared_ptr<animation_editor::AnimationDocument> make_document_with_tags() {
    auto document = std::make_shared<animation_editor::AnimationDocument>();
    document->load_from_manifest(
        nlohmann::json{
            {"animations",
             {
                 {"idle",
                  {{"source", {{"kind", "folder"}, {"path", "idle"}, {"name", ""}}},
                   {"number_of_frames", 1},
                   {"tags", nlohmann::json::array({"locomotion"})}}},
             }},
        },
        std::filesystem::path{},
        nullptr);
    return document;
}

bool json_string_array_contains(const nlohmann::json& values, const std::string& target) {
    if (!values.is_array()) {
        return false;
    }
    for (const auto& entry : values) {
        if (entry.is_string() && entry.get<std::string>() == target) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("AnimationTagsPanel recommendations come from animation tags across manifest assets") {
    write_assets_manifest(
        nlohmann::json::object({
            {"hero",
             nlohmann::json::object({
                 {"animations",
                  nlohmann::json::object({
                      {"idle", nlohmann::json::object({{"tags", nlohmann::json::array({"locomotion", "ground"})}})},
                      {"attack", nlohmann::json::object({{"tags", nlohmann::json::array({"combat", "melee"})}})},
                  })},
             })},
            {"mage",
             nlohmann::json::object({
                 {"animations",
                  nlohmann::json::object({
                      {"cast", nlohmann::json::object({{"tags", nlohmann::json::array({"magic", "combat"})}})},
                  })},
             })},
        }));

    animation_editor::AnimationTagsPanel panel;
    panel.set_document(make_document_with_tags());
    panel.set_animation_id("idle");
    AnimationTagsPanelTestAccess::refresh_pool(panel);
    AnimationTagsPanelTestAccess::set_query(panel, "");

    const auto& recs = AnimationTagsPanelTestAccess::recommended_tags(panel);
    CHECK(std::find(recs.begin(), recs.end(), "combat") != recs.end());
    CHECK(std::find(recs.begin(), recs.end(), "magic") != recs.end());
    CHECK(std::find(recs.begin(), recs.end(), "locomotion") == recs.end());
}

TEST_CASE("AnimationTagsPanel updates recommendations from query and persists add remove") {
    write_assets_manifest(
        nlohmann::json::object({
            {"hero",
             nlohmann::json::object({
                 {"animations",
                  nlohmann::json::object({
                      {"idle", nlohmann::json::object({{"tags", nlohmann::json::array({"locomotion", "ground"})}})},
                      {"attack", nlohmann::json::object({{"tags", nlohmann::json::array({"combat", "melee"})}})},
                  })},
             })},
        }));

    auto document = make_document_with_tags();
    animation_editor::AnimationTagsPanel panel;
    panel.set_document(document);
    panel.set_animation_id("idle");
    AnimationTagsPanelTestAccess::refresh_pool(panel);

    AnimationTagsPanelTestAccess::set_query(panel, "com");
    const auto& filtered = AnimationTagsPanelTestAccess::recommended_tags(panel);
    REQUIRE(!filtered.empty());
    CHECK(filtered.front() == "com");
    CHECK(std::find(filtered.begin(), filtered.end(), "combat") != filtered.end());

    std::vector<std::string> callback_tags;
    panel.set_on_tags_changed([&callback_tags](const std::vector<std::string>& tags) {
        callback_tags = tags;
    });

    AnimationTagsPanelTestAccess::add_tag(panel, "combat");
    auto payload = document->animation_payload_json("idle");
    REQUIRE(payload.has_value());
    REQUIRE(payload->contains("tags"));
    CHECK(json_string_array_contains((*payload)["tags"], "combat"));
    CHECK(std::find(callback_tags.begin(), callback_tags.end(), "combat") != callback_tags.end());

    AnimationTagsPanelTestAccess::remove_tag(panel, "combat");
    payload = document->animation_payload_json("idle");
    REQUIRE(payload.has_value());
    CHECK_FALSE(json_string_array_contains((*payload)["tags"], "combat"));
}
