#include "docked_panel_layout_policy.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>

namespace devmode::docked_panels {
namespace {
std::unordered_map<std::string, bool> g_open_sources;
}

SDL_Rect apply_layout_policy(const SDL_Rect& rect,
                             int screen_w,
                             int screen_h,
                             DockedPanelLayoutPolicy policy) {
    SDL_Rect out = rect;
    if (policy == DockedPanelLayoutPolicy::FullHeightDefault) {
        out.y = 0;
        out.h = std::max(0, screen_h);
    }

    out.w = std::max(0, out.w);
    out.h = std::max(0, out.h);

    if (screen_w > 0) {
        if (out.w > screen_w) out.w = screen_w;
        const int max_x = std::max(0, screen_w - out.w);
        out.x = std::clamp(out.x, 0, max_x);
    } else {
        out.x = 0;
        out.w = 0;
    }

    if (screen_h > 0) {
        if (out.h > screen_h) out.h = screen_h;
        const int max_y = std::max(0, screen_h - out.h);
        out.y = std::clamp(out.y, 0, max_y);
    } else {
        out.y = 0;
        out.h = 0;
    }

    return out;
}

void set_qualifying_panel_open(const char* source, bool open) {
    if (!source || *source == '\0') {
        return;
    }
    g_open_sources[std::string(source)] = open;
}

bool any_qualifying_panel_open() {
    for (const auto& entry : g_open_sources) {
        if (entry.second) {
            return true;
        }
    }
    return false;
}

} // namespace devmode::docked_panels

