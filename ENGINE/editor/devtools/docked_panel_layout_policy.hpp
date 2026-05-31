#pragma once

#include <SDL3/SDL.h>

namespace devmode::docked_panels {

enum class DockedPanelLayoutPolicy {
    FullHeightDefault,
    SpecialHeightException,
};

SDL_Rect apply_layout_policy(const SDL_Rect& rect,
                             int screen_w,
                             int screen_h,
                             DockedPanelLayoutPolicy policy);

void set_qualifying_panel_open(const char* source, bool open);
bool any_qualifying_panel_open();

} // namespace devmode::docked_panels

