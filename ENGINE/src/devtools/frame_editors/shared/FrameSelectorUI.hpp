#pragma once

#include <SDL.h>

namespace devmode::frame_editors {

class FrameSelectorUI {
public:
    void set_bounds(const SDL_Rect& bounds) { bounds_ = bounds; }
    void set_frame_count(int count) { frame_count_ = count; }
    void set_selected_index(int index) { selected_index_ = index; }
    int selected_index() const { return selected_index_; }

    void update_layout() {}
    bool handle_event(const SDL_Event&) { return false; }
    void render(SDL_Renderer*) const {}

private:
    SDL_Rect bounds_{0, 0, 0, 0};
    int frame_count_ = 0;
    int selected_index_ = 0;
};

}  // namespace devmode::frame_editors
