#pragma once

#include <SDL3/SDL.h>
#include <string>

#include "widget_base.hpp"

class Checkbox : public WidgetBase {
public:
    Checkbox(const std::string& label, bool value);
    void set_position(SDL_Point p);
    void set_rect(const SDL_Rect& r);
    const SDL_Rect& rect() const;
    void set_label(const std::string& s);
    const std::string& label() const;
    void set_value(bool v);
    bool value() const;
    bool handle_event(const SDL_Event& e);
    void render(SDL_Renderer* r) const;
    static int width();
    static int height();

private:
    std::string label_;
    bool value_ = false;
};
