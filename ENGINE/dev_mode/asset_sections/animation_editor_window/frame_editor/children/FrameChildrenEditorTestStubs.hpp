#pragma once

#include <SDL.h>
#include <functional>
#include <string>
#include <vector>

namespace animation_editor {

class FrameToolsPanel {
  public:
    void set_children_callbacks(std::function<void(int)> /*on_child_selected*/,
                                std::function<void()> /*on_apply_to_next*/,
                                std::function<void(bool)> /*on_visible_changed*/,
                                std::function<void(int)> /*on_mode_changed*/,
                                std::function<void(const std::string&)> /*on_add_or_rename*/,
                                std::function<void()> /*on_remove_child*/) {}

    void set_children_state(const std::vector<std::string>& /*options*/,
                            int /*selected_index*/,
                            bool /*visible*/,
                            bool /*enabled*/,
                            int /*mode_index*/,
                            const std::string& /*current_name*/ = std::string{}) {}

    void set_work_area_bounds(const SDL_Rect&) {}
    bool handle_event(const SDL_Event&) { return false; }
    void render(SDL_Renderer*) const {}
};

class MovementCanvas {
  public:
    SDL_FPoint frame_anchor_world(int /*frame_index*/) const { return SDL_FPoint{0.0f, 0.0f}; }
    SDL_FPoint frame_anchor_screen(int /*frame_index*/) const { return SDL_FPoint{0.0f, 0.0f}; }
    SDL_FPoint screen_to_world(SDL_Point /*screen*/) const { return SDL_FPoint{0.0f, 0.0f}; }
    SDL_FPoint world_to_screen(const SDL_FPoint& /*world*/) const { return SDL_FPoint{0.0f, 0.0f}; }
    float screen_pixels_per_unit() const { return 1.0f; }
    float document_scale_factor() const { return 1.0f; }
    const SDL_Rect& bounds() const {
        static SDL_Rect rect{0, 0, 0, 0};
        return rect;
    }
};

}
