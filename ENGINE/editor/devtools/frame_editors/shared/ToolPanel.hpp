#pragma once

#include <memory>
#include <string>

#include <SDL3/SDL_rect.h>

#include "DockableCollapsible.hpp"

class Input;
union SDL_Event;
struct SDL_Renderer;

namespace devmode::frame_editors {

// Lightweight helper that wraps DockableCollapsible with a consistent
// floating-tool styling for all frame editor modes.
class FrameToolPanel {
public:
    FrameToolPanel(std::string title, std::string stack_key = {});
    ~FrameToolPanel();

    DockableCollapsible* panel();
    const DockableCollapsible* panel() const;

    void set_rows(const DockableCollapsible::Rows& rows) const;
    void set_position_if_unset(int x, int y) const;
    void set_work_area(SDL_Rect area) const;

    void update(const Input& input, int screen_w, int screen_h) const;
    bool handle_event(const SDL_Event& e) const;
    void render(SDL_Renderer* renderer) const;

    bool contains_point(const SDL_Point& p) const;

private:
    mutable std::unique_ptr<DockableCollapsible> panel_;
    mutable bool has_position_ = false;
    mutable bool registered_with_manager_ = false;
    std::string stack_key_;
};

}  // namespace devmode::frame_editors
