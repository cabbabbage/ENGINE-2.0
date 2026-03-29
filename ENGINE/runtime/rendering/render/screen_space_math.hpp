#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace render::screen_space {

inline double sanitize_zoom(double zoom) {
    return (std::isfinite(zoom) && zoom > 0.0) ? zoom : 1.0;
}

inline SDL_FPoint ndc_to_screen(double ndc_x,
                                double ndc_y,
                                int screen_width,
                                int screen_height,
                                double zoom = 1.0,
                                double pan_y_px = 0.0) {
    const double safe_w = static_cast<double>(std::max(1, screen_width));
    const double safe_h = static_cast<double>(std::max(1, screen_height));
    const double safe_zoom = sanitize_zoom(zoom);
    const double scaled_x = ndc_x * safe_zoom;
    const double scaled_y = ndc_y * safe_zoom;
    const double screen_x = (scaled_x * 0.5 + 0.5) * safe_w;
    const double screen_y = (0.5 - scaled_y * 0.5) * safe_h + pan_y_px;
    return SDL_FPoint{static_cast<float>(screen_x), static_cast<float>(screen_y)};
}

inline std::pair<double, double> screen_to_ndc(double screen_x,
                                                double screen_y,
                                                int screen_width,
                                                int screen_height,
                                                double zoom = 1.0,
                                                double pan_y_px = 0.0) {
    const double safe_w = static_cast<double>(std::max(1, screen_width));
    const double safe_h = static_cast<double>(std::max(1, screen_height));
    const double safe_zoom = sanitize_zoom(zoom);
    const double inv_zoom = 1.0 / safe_zoom;
    const double ndc_x_scaled = (screen_x / safe_w) * 2.0 - 1.0;
    const double ndc_y_scaled = 1.0 - ((screen_y - pan_y_px) / safe_h) * 2.0;
    return std::pair<double, double>{ndc_x_scaled * inv_zoom, ndc_y_scaled * inv_zoom};
}

}  // namespace render::screen_space
