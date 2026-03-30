#pragma once

#include <memory>
#include <string>

#include <SDL3/SDL.h>

class DMCheckbox;

class RoomMovementToolsPanel {
public:
    RoomMovementToolsPanel();
    ~RoomMovementToolsPanel();

    void set_visible(bool visible);
    bool is_visible() const { return visible_; }

    void set_screen_dimensions(int width, int height);
    void set_panel_bounds_override(const SDL_Rect& bounds);
    void clear_panel_bounds_override();

    void set_smooth_enabled(bool enabled);
    bool smooth_enabled() const;
    void set_curve_enabled(bool enabled);
    bool curve_enabled() const;

    bool handle_event(const SDL_Event& event);
    void render(SDL_Renderer* renderer) const;
    bool is_point_inside(int x, int y) const;

private:
    void update_layout() const;
    static bool point_in_rect(int x, int y, const SDL_Rect& rect);

private:
    bool visible_ = false;
    int screen_w_ = 0;
    int screen_h_ = 0;
    bool panel_bounds_override_active_ = false;
    SDL_Rect panel_bounds_override_{0, 0, 0, 0};

    mutable bool layout_dirty_ = true;
    mutable SDL_Rect panel_rect_{12, 56, 300, 210};
    mutable SDL_Rect header_rect_{0, 0, 0, 0};
    mutable SDL_Rect hint_rect_{0, 0, 0, 0};
    mutable SDL_Rect smooth_rect_{0, 0, 0, 0};
    mutable SDL_Rect curve_rect_{0, 0, 0, 0};

    std::unique_ptr<DMCheckbox> smooth_checkbox_;
    std::unique_ptr<DMCheckbox> curve_checkbox_;
};
