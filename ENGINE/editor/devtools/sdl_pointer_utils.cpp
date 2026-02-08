#include "devtools/sdl_pointer_utils.hpp"
#include "utils/sdl_mouse_utils.hpp"
#include <cmath>

namespace devmode::sdl {

bool is_pointer_event(const SDL_Event& e) {
    return e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_MOTION;
}

SDL_Point event_point(const SDL_Event& e) {
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        return SDL_Point{
            static_cast<int>(std::lround(e.motion.x)),
            static_cast<int>(std::lround(e.motion.y))};
    }
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        return SDL_Point{
            static_cast<int>(std::lround(e.button.x)),
            static_cast<int>(std::lround(e.button.y))};
    }
    int mx = 0;
    int my = 0;
    sdl_mouse_util::GetMouseState(&mx, &my);
    return SDL_Point{mx, my};
}

}

