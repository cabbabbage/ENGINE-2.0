#include "room_overlay_renderer.hpp"
#include "utils/sdl_render_conversions.hpp"

#include <cmath>
#include <algorithm>
#include <vector>

#include "draw_utils.hpp"
#include "utils/area.hpp"
#include "rendering/render/warped_screen_grid.hpp"

namespace {

int compute_center_arm(const WarpedScreenGrid&) {
    return 6;
}

}

namespace dm_draw {

RoomBoundsOverlayStyle ResolveRoomBoundsOverlayStyle(SDL_Color base_color) {
    RoomBoundsOverlayStyle style{};
    base_color.a = 255;
    SDL_Color outline = LightenColor(base_color, 0.12f);
    outline.a = 210;
    SDL_Color fill = LightenColor(base_color, 0.02f);
    fill.a = 56;
    SDL_Color center = LightenColor(base_color, 0.2f);
    center.a = 235;
    style.outline = outline;
    style.fill = fill;
    style.center = center;
    return style;
}

void RenderRoomBoundsOverlay(
    SDL_Renderer* renderer,
    const WarpedScreenGrid& cam,
    const Area& area,
    const RoomBoundsOverlayStyle& style) {
    if (!renderer) return;

    SDL_BlendMode prev_mode = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(renderer, &prev_mode);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    Uint8 prev_r = 0, prev_g = 0, prev_b = 0, prev_a = 0;
    SDL_GetRenderDrawColor(renderer, &prev_r, &prev_g, &prev_b, &prev_a);

    const auto& area_points = area.get_points();
    if (!area_points.empty()) {
        std::vector<SDL_Point> screen_segment;
        screen_segment.reserve(area_points.size());
        auto flush_segment = [&]() {
            if (screen_segment.size() < 2) {
                screen_segment.clear();
                return;
            }
            const SDL_Point& first = screen_segment.front();
            const SDL_Point& last = screen_segment.back();
            if (first.x != last.x || first.y != last.y) {
                screen_segment.push_back(first);
            }
            SDL_SetRenderDrawColor(renderer, style.outline.r, style.outline.g, style.outline.b, style.outline.a);
            sdl_render::Lines(renderer, screen_segment.data(), static_cast<int>(screen_segment.size()));
            screen_segment.clear();
        };

        for (const SDL_Point& world_point : area_points) {
            SDL_FPoint world_f{static_cast<float>(world_point.x), static_cast<float>(world_point.y)};
            SDL_FPoint screen_f{};
            if (!cam.project_world_point(world_f, 0.0f, screen_f)) {
                flush_segment();
                continue;
            }
            SDL_Point screen_pt{static_cast<int>(std::lround(screen_f.x)), static_cast<int>(std::lround(screen_f.y))};
            screen_segment.push_back(screen_pt);
        }
        flush_segment();
    }

    SDL_FPoint center_screen_f = cam.map_to_screen(area.get_center());
    SDL_Point center_screen{static_cast<int>(std::lround(center_screen_f.x)),
                            static_cast<int>(std::lround(center_screen_f.y))};
    int arm = compute_center_arm(cam);
    SDL_SetRenderDrawColor(renderer, style.center.r, style.center.g, style.center.b, style.center.a);
    SDL_RenderLine(renderer, center_screen.x - arm, center_screen.y, center_screen.x + arm, center_screen.y);
    SDL_RenderLine(renderer, center_screen.x, center_screen.y - arm, center_screen.x, center_screen.y + arm);

    SDL_SetRenderDrawColor(renderer, prev_r, prev_g, prev_b, prev_a);
    SDL_SetRenderDrawBlendMode(renderer, prev_mode);
}

}




