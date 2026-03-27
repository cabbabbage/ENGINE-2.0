#include <doctest/doctest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "core/manifest/manifest_loader.hpp"
#include "config/room_config/tag_editor_widget.hpp"
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

nlohmann::json similarity_fixture_assets() {
    return nlohmann::json::object({
        {"hero", nlohmann::json::object({
            {"tags", nlohmann::json::array({"enemy", "small"})},
            {"anti_tags", nlohmann::json::array({"urban"})}
        })},
        {"wolf", nlohmann::json::object({
            {"tags", nlohmann::json::array({"enemy", "forest", "pack"})},
            {"anti_tags", nlohmann::json::array({"urban"})}
        })},
        {"slime", nlohmann::json::object({
            {"tags", nlohmann::json::array({"enemy", "cave", "small"})},
            {"anti_tags", nlohmann::json::array({"water"})}
        })},
        {"flower", nlohmann::json::object({
            {"tags", nlohmann::json::array({"nature", "forest", "pretty"})},
            {"anti_tags", nlohmann::json::array()}
        })},
        {"barrel", nlohmann::json::object({
            {"tags", nlohmann::json::array({"urban", "wood"})},
            {"anti_tags", nlohmann::json::array({"nature"})}
        })},
        {"mage", nlohmann::json::object({
            {"tags", nlohmann::json::array({"enemy", "magic", "caster"})},
            {"anti_tags", nlohmann::json::array({"holy"})}
        })}
    });
}

nlohmann::json large_fixture_assets() {
    nlohmann::json assets = nlohmann::json::object();
    assets["subject"] = nlohmann::json::object({
        {"tags", nlohmann::json::array()},
        {"anti_tags", nlohmann::json::array()}
    });
    for (int i = 0; i < 40; ++i) {
        char tag_buf[16] = {0};
        std::snprintf(tag_buf, sizeof(tag_buf), "tag%02d", i);
        const std::string asset_name = std::string("asset_") + tag_buf;
        assets[asset_name] = nlohmann::json::object({
            {"tags", nlohmann::json::array({std::string(tag_buf)})},
            {"anti_tags", nlohmann::json::array()}
        });
    }
    return assets;
}

}  // namespace

TEST_CASE("Asset tag recommendations load from manifest and exclude current selections") {
    write_assets_manifest(similarity_fixture_assets());
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path(PROJECT_ROOT) / "ENGINE" / "tags.csv"));

    TagEditorWidget widget(TagEditorWidget::Mode::AssetInfoOverhaul);
    widget.set_subject_asset_name("hero");
    widget.set_tags({"enemy", "small"}, {"urban"});
    TagEditorWidgetTestAccess::set_query(widget, "");

    const auto& recs = TagEditorWidgetTestAccess::recommended_tags(widget);
    CHECK(!recs.empty());
    CHECK(recs.size() <= 25);
    CHECK(std::find(recs.begin(), recs.end(), "enemy") == recs.end());
    CHECK(std::find(recs.begin(), recs.end(), "urban") == recs.end());
}

TEST_CASE("Asset tag recommendations use text tier first and backfill to 25") {
    write_assets_manifest(large_fixture_assets());

    TagEditorWidget widget(TagEditorWidget::Mode::AssetInfoOverhaul);
    widget.set_subject_asset_name("subject");
    widget.set_tags({}, {});
    TagEditorWidgetTestAccess::set_query(widget, "tag39");

    const auto& recs = TagEditorWidgetTestAccess::recommended_tags(widget);
    REQUIRE(recs.size() == 25);
    CHECK(recs.front() == "tag39");
    CHECK(std::find(recs.begin(), recs.end(), "tag00") != recs.end());
}

TEST_CASE("Asset mode keeps orange virtual chip and removes duplicate from yellow list") {
    write_assets_manifest(similarity_fixture_assets());

    TagEditorWidget widget(TagEditorWidget::Mode::AssetInfoOverhaul);
    widget.set_subject_asset_name("hero");
    widget.set_tags({"enemy", "small"}, {"urban"});
    TagEditorWidgetTestAccess::set_query(widget, "custom_tag");

    CHECK(TagEditorWidgetTestAccess::has_search_virtual_chip(widget));
    CHECK(TagEditorWidgetTestAccess::search_virtual_value(widget) == "custom_tag");
    const auto& recs = TagEditorWidgetTestAccess::recommended_tags(widget);
    CHECK(std::find(recs.begin(), recs.end(), "custom_tag") == recs.end());
}

TEST_CASE("Asset mode updates ranking from changed positive and anti tag lists") {
    write_assets_manifest(similarity_fixture_assets());

    TagEditorWidget widget(TagEditorWidget::Mode::AssetInfoOverhaul);
    widget.set_subject_asset_name("hero");
    widget.set_tags({"enemy", "small"}, {"urban"});
    TagEditorWidgetTestAccess::set_query(widget, "");
    const auto& first_pass = TagEditorWidgetTestAccess::recommended_tags(widget);
    REQUIRE(!first_pass.empty());
    const std::string first_before = first_pass.front();

    widget.set_tags({"urban"}, {"nature"});
    TagEditorWidgetTestAccess::set_query(widget, "");
    const auto& second_pass = TagEditorWidgetTestAccess::recommended_tags(widget);
    REQUIRE(!second_pass.empty());
    CHECK(first_before != second_pass.front());
    CHECK(second_pass.front() == "wood");
}

TEST_CASE("Asset similarity pruning matches exact and excludes current asset") {
    write_assets_manifest(similarity_fixture_assets());

    TagEditorWidget widget(TagEditorWidget::Mode::AssetInfoOverhaul);
    widget.set_subject_asset_name("hero");
    widget.set_tags({"enemy", "small"}, {"urban"});
    TagEditorWidgetTestAccess::set_query(widget, "");

    CHECK(TagEditorWidgetTestAccess::pruning_matches_exact(widget));
    const auto names = TagEditorWidgetTestAccess::top_similar_asset_names(widget);
    CHECK(std::find(names.begin(), names.end(), "hero") == names.end());
}
