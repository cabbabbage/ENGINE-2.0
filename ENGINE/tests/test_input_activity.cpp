#include <doctest/doctest.h>

#include "utils/input.hpp"

namespace {

SDL_Event make_key_event(Uint32 type, SDL_Scancode scancode) {
    SDL_Event event{};
    event.type = type;
    event.key.scancode = scancode;
    return event;
}

SDL_Event make_mouse_button_event(Uint32 type, Uint8 button) {
    SDL_Event event{};
    event.type = type;
    event.button.button = button;
    return event;
}

}

TEST_CASE("Input::has_activity remains true while a key is held") {
    Input input;
    CHECK_FALSE(input.has_activity());

    input.handleEvent(make_key_event(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_A));
    CHECK(input.has_activity());

    input.update();
    CHECK(input.isScancodeDown(SDL_SCANCODE_A));
    CHECK(input.has_activity());

    input.handleEvent(make_key_event(SDL_EVENT_KEY_UP, SDL_SCANCODE_A));
    CHECK(input.has_activity());

    input.update();
    CHECK_FALSE(input.isScancodeDown(SDL_SCANCODE_A));
    CHECK_FALSE(input.has_activity());
}

TEST_CASE("Input::has_activity remains true while a mouse button is held") {
    Input input;
    CHECK_FALSE(input.has_activity());

    input.handleEvent(make_mouse_button_event(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT));
    CHECK(input.has_activity());

    input.update();
    CHECK(input.isDown(Input::LEFT));
    CHECK(input.has_activity());

    input.handleEvent(make_mouse_button_event(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT));
    CHECK(input.has_activity());

    input.update();
    input.clearClickBuffer();
    input.update();
    CHECK_FALSE(input.isDown(Input::LEFT));
    CHECK_FALSE(input.has_activity());
}
