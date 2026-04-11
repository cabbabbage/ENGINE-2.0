#include <doctest/doctest.h>

#include <filesystem>
#include <memory>

#include <SDL3/SDL.h>

#include "devtools/asset_editor/animation_editor_window/AnimationDocument.hpp"
#include "devtools/asset_editor/animation_editor_window/PanelLayoutConstants.hpp"
#include "devtools/asset_editor/animation_editor_window/PlaybackSettingsPanel.hpp"

#include <nlohmann/json.hpp>

TEST_CASE("PlaybackSettingsPanel keeps inherit/invert state for animation-sourced chains") {
    auto document = std::make_shared<animation_editor::AnimationDocument>();
    const nlohmann::json manifest = {
        {"animations",
         {
             {"base", {{"source", {{"kind", "folder"}, {"path", "base"}, {"name", ""}}}, {"number_of_frames", 1}}},
             {"mid", {{"source", {{"kind", "animation"}, {"path", ""}, {"name", "base"}}}, {"number_of_frames", 1}}},
             {"derived",
             {{"source", {{"kind", "animation"}, {"path", ""}, {"name", "mid"}}},
              {"number_of_frames", 1},
              {"inherit_data", true},
              {"invert_frames_vertical", true},
              {"flipped_source", true},
              {"flip_vertical_source", true},
              {"reverse_source", false},
               {"derived_modifiers", {{"flipX", true}, {"flipY", true}, {"reverse", false}}}}},
         }},
    };

    document->load_from_manifest(manifest, std::filesystem::path{}, nullptr);

    animation_editor::PlaybackSettingsPanel panel;
    panel.set_bounds(SDL_Rect{0, 0, 360, 240});
    panel.set_document(document);
    panel.set_animation_id("derived");
    panel.update();

    const auto payload = document->animation_payload_json("derived");
    REQUIRE(payload.has_value());
    REQUIRE(payload->is_object());

    CHECK((*payload)["reverse_source"] == false);
    CHECK((*payload)["inherit_data"] == true);
    CHECK((*payload)["invert_frames_vertical"] == false);
    CHECK((*payload)["flipped_source"] == true);
    CHECK((*payload)["flip_vertical_source"] == true);
    REQUIRE(payload->contains("derived_modifiers"));
    CHECK((*payload)["derived_modifiers"]["flipX"] == true);
    CHECK((*payload)["derived_modifiers"]["flipY"] == true);
    CHECK((*payload)["derived_modifiers"]["reverse"] == false);
}

TEST_CASE("PlaybackSettingsPanel updates on_end when inherit source data is toggled") {
    auto document = std::make_shared<animation_editor::AnimationDocument>();
    const nlohmann::json manifest = {
        {"animations",
         {
             {"source_anim",
              {{"source", {{"kind", "folder"}, {"path", "source_anim"}, {"name", ""}}},
               {"number_of_frames", 1},
               {"on_end", "reverse"}}},
             {"derived",
              {{"source", {{"kind", "animation"}, {"path", ""}, {"name", "source_anim"}}},
               {"number_of_frames", 1},
               {"inherit_data", false},
               {"on_end", "loop"}}},
         }},
    };

    document->load_from_manifest(manifest, std::filesystem::path{}, nullptr);

    animation_editor::PlaybackSettingsPanel panel;
    panel.set_bounds(SDL_Rect{0, 0, 360, 260});
    panel.set_document(document);
    panel.set_animation_id("derived");
    panel.update();

    auto click_checkbox = [&](int x, int y) {
        SDL_Event down{};
        down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        down.button.button = SDL_BUTTON_LEFT;
        down.button.x = x;
        down.button.y = y;

        SDL_Event up{};
        up.type = SDL_EVENT_MOUSE_BUTTON_UP;
        up.button.button = SDL_BUTTON_LEFT;
        up.button.x = x;
        up.button.y = y;

        panel.handle_event(down);
        panel.handle_event(up);
    };

    const int checkbox_stride = DMCheckbox::height() + 8;
    click_checkbox(animation_editor::kPanelPadding + 6, animation_editor::kPanelPadding + 6);

    auto after_inherit_on = document->animation_payload_json("derived");
    REQUIRE(after_inherit_on.has_value());
    REQUIRE(after_inherit_on->is_object());
    CHECK((*after_inherit_on)["inherit_data"] == true);
    CHECK((*after_inherit_on)["on_end"] == "reverse");

    panel.update();
    click_checkbox(animation_editor::kPanelPadding + 6,
                   animation_editor::kPanelPadding + checkbox_stride * 2 + 6);

    auto after_inherit_off = document->animation_payload_json("derived");
    REQUIRE(after_inherit_off.has_value());
    REQUIRE(after_inherit_off->is_object());
    CHECK((*after_inherit_off)["inherit_data"] == false);
    CHECK((*after_inherit_off)["on_end"] == "default");
}
