#pragma once

#include <SDL3/SDL.h>

#include "utils/sdl_mouse_utils.hpp"
#include "utils/sdl_render_conversions.hpp"

// Shared helpers for UI widgets: geometry, hover tracking, and basic drawing.
class WidgetBase {
public:
    WidgetBase() = default;
    explicit WidgetBase(SDL_Rect rect) : rect_(rect) {}

    void set_position(SDL_Point p) { rect_.x = p.x; rect_.y = p.y; }
    void set_rect(const SDL_Rect& r) { rect_ = r; }
    const SDL_Rect& rect() const { return rect_; }

    bool hovered() const { return hovered_; }

    bool update_hover(const SDL_Point& p) {
        hovered_ = SDL_PointInRect(&p, &rect_);
        return hovered_;
    }
    bool update_hover(const SDL_MouseMotionEvent& motion) { return update_hover(sdl_mouse_util::MotionPoint(motion)); }
    bool update_hover(const SDL_MouseButtonEvent& button) { return update_hover(sdl_mouse_util::ButtonPoint(button)); }

    static void FillRect(SDL_Renderer* renderer, const SDL_Rect& rc, SDL_Color color) {
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        sdl_render::FillRect(renderer, &rc);
    }

    static void StrokeRect(SDL_Renderer* renderer, const SDL_Rect& rc, SDL_Color color) {
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a ? color.a : 255);
        sdl_render::Rect(renderer, &rc);
    }

    static void DrawFrame(SDL_Renderer* renderer, const SDL_Rect& rc, SDL_Color color, bool double_frame = false) {
        StrokeRect(renderer, rc, color);
        if (double_frame && rc.w > 2 && rc.h > 2) {
            SDL_Rect inner{ rc.x + 1, rc.y + 1, rc.w - 2, rc.h - 2 };
            StrokeRect(renderer, inner, color);
        }
    }

    static void DrawPanel(SDL_Renderer* renderer, const SDL_Rect& rc, SDL_Color background, SDL_Color frame, bool double_frame = false) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        FillRect(renderer, rc, background);
        DrawFrame(renderer, rc, frame, double_frame);
    }

protected:
    SDL_Rect rect_{};
    bool hovered_ = false;
};
