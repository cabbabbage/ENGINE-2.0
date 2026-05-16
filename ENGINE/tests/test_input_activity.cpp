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
    event.button.x = 123.0f;
    event.button.y = 234.0f;
    return event;
}

SDL_Event make_mouse_wheel_event(int x, int y, int mouse_x, int mouse_y) {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_WHEEL;
    event.wheel.integer_x = x;
    event.wheel.integer_y = y;
    event.wheel.mouse_x = static_cast<float>(mouse_x);
    event.wheel.mouse_y = static_cast<float>(mouse_y);
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

TEST_CASE("Input updates pointer position on mouse button events") {
    Input input;
    input.handleEvent(make_mouse_button_event(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT));
    CHECK(input.getX() == 123);
    CHECK(input.getY() == 234);
}

TEST_CASE("Input updates pointer position on mouse wheel events") {
    Input input;
    input.handleEvent(make_mouse_wheel_event(0, 1, 345, 456));
    CHECK(input.getX() == 345);
    CHECK(input.getY() == 456);
}
