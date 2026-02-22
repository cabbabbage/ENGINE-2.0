#include "room_overlay_renderer.hpp"
#include "utils/sdl_render_conversions.hpp"

#include <array>
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
    SDL_Color outline = LightenColor(base_color, 0.16f);
    outline.a = 220;
    SDL_Color fill = LightenColor(base_color, 0.04f);
    fill.a = 60;
    SDL_Color center = LightenColor(base_color, 0.25f);
    center.a = 235;
    SDL_Color glow = LightenColor(base_color, 0.35f);
    glow.a = 140;
    style.outline = outline;
    style.fill = fill;
    style.center = center;
    style.glow = glow;
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
    std::vector<SDL_Point> screen_points;
    screen_points.reserve(area_points.size());
    for (const SDL_Point& world_point : area_points) {
        SDL_FPoint world_f{static_cast<float>(world_point.x), static_cast<float>(world_point.y)};
        SDL_FPoint screen_f{};
        if (!cam.project_world_point(world_f, 0.0f, screen_f)) {
            continue;
        }
        SDL_Point screen_pt{static_cast<int>(std::lround(screen_f.x)), static_cast<int>(std::lround(screen_f.y))};
        if (!screen_points.empty()) {
            const SDL_Point& prev = screen_points.back();
            if (prev.x == screen_pt.x && prev.y == screen_pt.y) {
                continue;
            }
        }
        screen_points.push_back(screen_pt);
    }
    if (screen_points.size() >= 2) {
        const SDL_Point& first = screen_points.front();
        const SDL_Point& last = screen_points.back();
        if (first.x != last.x || first.y != last.y) {
            screen_points.push_back(first);
        }
        std::vector<SDL_Point> offset_points;
        offset_points.reserve(screen_points.size());
        auto draw_with_offset = [&](const SDL_Color& color, int ox, int oy) {
            if (color.a == 0) return;
            offset_points.clear();
            for (const SDL_Point& pt : screen_points) {
                offset_points.push_back(SDL_Point{pt.x + ox, pt.y + oy});
            }
            SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
            sdl_render::Lines(renderer, offset_points.data(), static_cast<int>(offset_points.size()));
        };

        if (style.glow.a > 0) {
            const std::array<SDL_Point, 4> glow_offsets = {{
                SDL_Point{-1, 0},
                SDL_Point{1, 0},
                SDL_Point{0, -1},
                SDL_Point{0, 1},
            }};
            for (const SDL_Point& offset : glow_offsets) {
                draw_with_offset(style.glow, offset.x, offset.y);
            }
        }
        draw_with_offset(style.outline, 0, 0);
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




