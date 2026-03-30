#pragma once

#include <functional>
#include <memory>
#include <string>

#include <SDL3/SDL.h>

class DMButton;
struct SDL_Renderer;

class BottomNavigationPanel {
public:
    BottomNavigationPanel();
    ~BottomNavigationPanel();

    void set_visible(bool visible);
    bool is_visible() const { return visible_; }

    void set_screen_dimensions(int width, int height);

    void set_dpad_navigation(const std::string& center_value,
                             std::function<void()> on_up,
                             std::function<void()> on_down,
                             std::function<void()> on_left,
                             std::function<void()> on_right,
                             bool visible = true);
    void clear_navigation();

    bool handle_event(const SDL_Event& event);
    void render(SDL_Renderer* renderer) const;

    bool is_point_inside(int x, int y) const;
    const SDL_Rect& rect() const;

private:
    struct DpadNavigation {
        std::string center_value;
        bool visible = false;
        std::function<void()> on_up;
        std::function<void()> on_down;
        std::function<void()> on_left;
        std::function<void()> on_right;
        mutable SDL_Rect bounds{0, 0, 0, 0};
        mutable SDL_Rect center_rect{0, 0, 0, 0};
    };

    void ensure_widgets();
    void update_layout() const;
    static bool point_in_rect(int x, int y, const SDL_Rect& rect);

    bool visible_ = false;
    int screen_w_ = 0;
    int screen_h_ = 0;

    DpadNavigation dpad_{};
    std::unique_ptr<DMButton> dpad_up_button_;
    std::unique_ptr<DMButton> dpad_down_button_;
    std::unique_ptr<DMButton> dpad_left_button_;
    std::unique_ptr<DMButton> dpad_right_button_;

    mutable bool layout_dirty_ = true;
    mutable SDL_Rect panel_rect_{0, 0, 0, 0};
};
