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

    void set_action(const std::string& label, std::function<void()> callback, bool emphasized);
    void clear_action();
    void set_primary_navigation(const std::string& label,
                                const std::string& value,
                                std::function<void()> on_prev,
                                std::function<void()> on_next,
                                bool visible = true);
    void set_secondary_navigation(const std::string& label,
                                  const std::string& value,
                                  std::function<void()> on_prev,
                                  std::function<void()> on_next,
                                  bool visible = true);
    void clear_navigation();

    bool handle_event(const SDL_Event& event);
    void render(SDL_Renderer* renderer) const;

    bool is_point_inside(int x, int y) const;
    const SDL_Rect& rect() const;

private:
    struct NavigationAxis {
        std::string label;
        std::string value;
        bool visible = false;
        std::function<void()> on_prev;
        std::function<void()> on_next;
        mutable SDL_Rect bounds{0, 0, 0, 0};
        mutable SDL_Rect value_rect{0, 0, 0, 0};
    };

    void ensure_widgets();
    void update_layout() const;
    void set_action_style();
    static bool point_in_rect(int x, int y, const SDL_Rect& rect);

    bool visible_ = false;
    int screen_w_ = 0;
    int screen_h_ = 0;

    std::string action_label_;
    bool action_emphasized_ = false;
    bool action_visible_ = false;
    std::function<void()> on_action_;

    NavigationAxis primary_axis_{};
    NavigationAxis secondary_axis_{};

    std::unique_ptr<DMButton> action_button_;
    std::unique_ptr<DMButton> primary_prev_button_;
    std::unique_ptr<DMButton> primary_next_button_;
    std::unique_ptr<DMButton> secondary_prev_button_;
    std::unique_ptr<DMButton> secondary_next_button_;

    mutable bool layout_dirty_ = true;
    mutable SDL_Rect panel_rect_{0, 0, 0, 0};
};
