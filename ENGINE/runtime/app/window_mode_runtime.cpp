#include "window_mode_runtime.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <string>

namespace app::window_mode_runtime {

WindowedPlacement compute_windowed_fallback(SDL_Window* window) {
    WindowedPlacement placement{};
    if (!window) return placement;
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    if (display != 0) {
        if (const SDL_DisplayMode* desktop_mode = SDL_GetDesktopDisplayMode(display)) {
            const int margin = 120;
            const int preferred_w = (desktop_mode->w * 3) / 4;
            const int preferred_h = (desktop_mode->h * 3) / 4;
            const int max_w = std::max(640, desktop_mode->w - margin);
            const int max_h = std::max(360, desktop_mode->h - margin);
            placement.w = std::clamp(preferred_w, 960, max_w);
            placement.h = std::clamp(preferred_h, 540, max_h);
        }
    }
    return placement;
}

bool toggle_fullscreen(SDL_Window* window,
                       bool& is_fullscreen,
                       int& windowed_x,
                       int& windowed_y,
                       int& windowed_width,
                       int& windowed_height) {
    if (!window) return false;
    if (is_fullscreen) {
        WindowedPlacement target{windowed_x, windowed_y, windowed_width, windowed_height};
        if (target.w <= 0 || target.h <= 0) {
            target = compute_windowed_fallback(window);
        }
        const bool result = SDL_SetWindowFullscreen(window, false);
        if (!result) {
            vibble::log::warn(std::string("[MainApp] Failed to switch to windowed mode: ") + SDL_GetError());
            return false;
        }
        SDL_SetWindowResizable(window, true);
        SDL_SetWindowBordered(window, true);
        SDL_SetWindowSize(window, target.w, target.h);
        SDL_SetWindowPosition(window, target.x, target.y);
        is_fullscreen = false;
        windowed_x = target.x; windowed_y = target.y; windowed_width = target.w; windowed_height = target.h;
        vibble::log::info("[MainApp] Window mode switched to windowed.");
        return true;
    }

    int current_x = 0, current_y = 0, current_width = 0, current_height = 0;
    SDL_GetWindowPosition(window, &current_x, &current_y);
    SDL_GetWindowSize(window, &current_width, &current_height);
    if (current_width > 0 && current_height > 0) {
        windowed_x = current_x; windowed_y = current_y; windowed_width = current_width; windowed_height = current_height;
    }
    const bool result = SDL_SetWindowFullscreen(window, true);
    if (!result) {
        vibble::log::warn(std::string("[MainApp] Failed to switch to fullscreen mode: ") + SDL_GetError());
        return false;
    }
    is_fullscreen = true;
    vibble::log::info("[MainApp] Window mode switched to fullscreen.");
    return true;
}

} // namespace
