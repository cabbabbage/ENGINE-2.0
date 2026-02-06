#pragma once

#include "sdl3_render_compat.hpp"
#include <functional>
#include <memory>
#include <string>

#include "devtools/widgets.hpp"

namespace devmode::frame_editors {

class FrameNavigator {
public:
    FrameNavigator();
    ~FrameNavigator();

    // Configuration
    void set_frame_count(int count);
    void set_current_frame(int frame);
    void set_on_frame_changed(std::function<void(int)> callback);
    void set_enabled(bool enabled);

    // UI Management
    void set_rect(const SDL_Rect& rect);
    const SDL_Rect& get_rect() const;
    SDL_Rect get_preferred_rect() const;

    // Event Handling & Rendering
    bool handle_event(const SDL_Event& e);
    void render(SDL_Renderer* renderer);

    // State Queries
    int get_current_frame() const { return current_frame_; }
    int get_frame_count() const { return frame_count_; }
    bool is_enabled() const { return enabled_; }

private:
    void update_button_states();
    void validate_frame_index();
    void notify_frame_changed();
    bool handle_keyboard_navigation(const SDL_Event& e);
    void update_textbox_value();

    int current_frame_ = 0;
    int frame_count_ = 0;
    bool enabled_ = true;

    std::unique_ptr<DMButton> btn_prev_;
    std::unique_ptr<DMButton> btn_next_;
    std::unique_ptr<DMTextBox> tb_frame_number_;

    std::function<void(int)> on_frame_changed_;

    SDL_Rect rect_{0, 0, 0, 0};
};

}  // namespace devmode::frame_editors
