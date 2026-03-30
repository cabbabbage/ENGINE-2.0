#include <doctest/doctest.h>

#include <filesystem>
#include <memory>

#include <SDL3/SDL.h>

#include "devtools/asset_editor/animation_editor_window/AnimationDocument.hpp"
#include "devtools/asset_editor/animation_editor_window/PanelLayoutConstants.hpp"
#include "devtools/asset_editor/animation_editor_window/PlaybackSettingsPanel.hpp"

#include <nlohmann/json.hpp>

TEST_CASE("PlaybackSettingsPanel coerces hidden inherit/invert state off for frame-sourced source chains") {
    auto document = std::make_shared<animation_editor::AnimationDocument>();
    const nlohmann::json manifest = {
        {"animations",
         {
             {"base", {{"source", {{"kind", "folder"}, {"path", "base"}, {"name", ""}}}, {"number_of_frames", 1}}},
             {"mid", {{"source", {{"kind", "animation"}, {"path", ""}, {"name", "base"}}}, {"number_of_frames", 1}}},
             {"derived",
              {{"source", {{"kind", "animation"}, {"path", ""}, {"name", "mid"}}},
               {"number_of_frames", 1},
               {"inherit_source_geometry", true},
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

    SDL_Event down{};
    down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    down.button.button = SDL_BUTTON_LEFT;
    down.button.x = animation_editor::kPanelPadding + 6;
    down.button.y = animation_editor::kPanelPadding + 6;

    SDL_Event up{};
    up.type = SDL_EVENT_MOUSE_BUTTON_UP;
    up.button.button = SDL_BUTTON_LEFT;
    up.button.x = down.button.x;
    up.button.y = down.button.y;

    panel.handle_event(down);
    panel.handle_event(up);

    const auto payload = document->animation_payload_json("derived");
    REQUIRE(payload.has_value());
    REQUIRE(payload->is_object());

    CHECK((*payload)["reverse_source"] == true);
    CHECK((*payload)["inherit_source_geometry"] == false);
    CHECK((*payload)["flipped_source"] == false);
    CHECK((*payload)["flip_vertical_source"] == false);
    REQUIRE(payload->contains("derived_modifiers"));
    CHECK((*payload)["derived_modifiers"]["flipX"] == false);
    CHECK((*payload)["derived_modifiers"]["flipY"] == false);
    CHECK((*payload)["derived_modifiers"]["reverse"] == true);
}
