#include "room_overlay_renderer.hpp"
#include "utils/sdl_render_conversions.hpp"

#include <array>
#include <cmath>
#include <algorithm>
#include <vector>
#include <optional>

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
    if (area_points.size() >= 2) {
        auto project_point = [&](const SDL_Point& world_point) -> std::optional<SDL_FPoint> {
            SDL_FPoint screen_f{};
            SDL_FPoint world_f{static_cast<float>(world_point.x), static_cast<float>(world_point.y)};
            if (cam.project_world_point(world_f, 0.0f, screen_f)) {
                return screen_f;
            }
            return std::nullopt;
        };

        auto lerp_world = [](const SDL_Point& a, const SDL_Point& b, float t) -> SDL_Point {
            const float x = static_cast<float>(a.x) + (static_cast<float>(b.x - a.x) * t);
            const float y = static_cast<float>(a.y) + (static_cast<float>(b.y - a.y) * t);
            return SDL_Point{static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))};
        };

        auto refine_boundary = [&](float t0, float t1, const SDL_Point& a, const SDL_Point& b, bool keep_upper) -> std::optional<SDL_FPoint> {
            float lo = t0;
            float hi = t1;
            std::optional<SDL_FPoint> last_valid;
            for (int i = 0; i < 12; ++i) {
                float mid = (lo + hi) * 0.5f;
                auto mid_proj = project_point(lerp_world(a, b, mid));
                if (mid_proj) {
                    last_valid = mid_proj;
                    if (keep_upper) {
                        lo = mid;
                    } else {
                        hi = mid;
                    }
                } else {
                    if (keep_upper) {
                        hi = mid;
                    } else {
                        lo = mid;
                    }
                }
            }
            return last_valid;
        };

        auto draw_segment = [&](const SDL_FPoint& a, const SDL_FPoint& b) {
            SDL_Point pa{static_cast<int>(std::lround(a.x)), static_cast<int>(std::lround(a.y))};
            SDL_Point pb{static_cast<int>(std::lround(b.x)), static_cast<int>(std::lround(b.y))};
            const std::array<SDL_Point, 5> offsets = {{
                SDL_Point{0, 0},
                SDL_Point{-1, 0},
                SDL_Point{1, 0},
                SDL_Point{0, -1},
                SDL_Point{0, 1},
            }};
            for (size_t i = 0; i < offsets.size(); ++i) {
                const SDL_Point& o = offsets[i];
                const SDL_Color& color = (i == 0) ? style.outline : style.glow;
                if (color.a == 0) continue;
                SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
                SDL_RenderLine(renderer, pa.x + o.x, pa.y + o.y, pb.x + o.x, pb.y + o.y);
            }
        };

        const size_t count = area_points.size();
        for (size_t i = 0; i < count; ++i) {
            const SDL_Point& w0 = area_points[i];
            const SDL_Point& w1 = area_points[(i + 1) % count];

            auto p0 = project_point(w0);
            auto p1 = project_point(w1);

            if (p0 && p1) {
                draw_segment(*p0, *p1);
                continue;
            }

            // If one is valid, clip toward the valid side.
            if (p0 && !p1) {
                if (auto entry = refine_boundary(0.0f, 1.0f, w0, w1, true)) { // search toward invalid end
                    draw_segment(*p0, *entry);
                }
                continue;
            }
            if (!p0 && p1) {
                if (auto entry = refine_boundary(0.0f, 1.0f, w0, w1, false)) { // search toward valid start
                    draw_segment(*entry, *p1);
                }
                continue;
            }

            // Both invalid: sample along the edge to see if any portion is visible.
            constexpr int kSamples = 10;
            float first_valid_t = -1.0f;
            float last_valid_t = -1.0f;
            for (int s = 0; s <= kSamples; ++s) {
                float t = static_cast<float>(s) / static_cast<float>(kSamples);
                auto proj = project_point(lerp_world(w0, w1, t));
                if (proj) {
                    if (first_valid_t < 0.0f) first_valid_t = t;
                    last_valid_t = t;
                }
            }
            if (first_valid_t >= 0.0f && last_valid_t >= first_valid_t) {
                auto entry = refine_boundary(first_valid_t - 0.05f, first_valid_t, w0, w1, false);
                auto exit = refine_boundary(last_valid_t, last_valid_t + 0.05f, w0, w1, true);
                if (!entry) entry = project_point(lerp_world(w0, w1, first_valid_t));
                if (!exit) exit = project_point(lerp_world(w0, w1, last_valid_t));
                if (entry && exit) {
                    draw_segment(*entry, *exit);
                }
            }
        }
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




