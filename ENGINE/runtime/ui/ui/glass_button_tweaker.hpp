#pragma once

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <variant>
#include <string>
#include <vector>

#include "button_settings.hpp"

class GlassButtonTweaker {
public:
    enum class FieldKind {
        Integer,
        Float,
        Boolean,
        Color,
    };

    struct FieldDefinition {
        const char* label;
        FieldKind kind;
        std::variant<int GlassButtonStyle::*,
                     float GlassButtonStyle::*,
                     bool GlassButtonStyle::*,
                     SDL_Color GlassButtonStyle::*> member;
        float step;
        float large_step;
    };

    GlassButtonTweaker();
    ~GlassButtonTweaker();

    void toggle();
    void open();
    void close();
    bool is_active() const;

    bool handle_event(const SDL_Event& e, int screen_w, int screen_h);
    void render(SDL_Renderer* renderer, int screen_w, int screen_h);

private:
    void update_layout(int screen_w, int screen_h);
    void adjust_current_field(bool increase, bool fast);
    void toggle_current_bool();
    void cycle_color_channel();
    std::string format_field_value(const GlassButtonStyle& style, const FieldDefinition& field) const;
    bool is_point_inside(const SDL_Point& point, const SDL_Rect& rect) const;
    void update_status(const std::string& text);

    std::vector<FieldDefinition> fields_;
    int selected_index_ = 0;
    int color_channel_ = 0;

    SDL_Rect panel_rect_{};
    SDL_Rect save_button_rect_{};
    SDL_Rect random_button_rect_{};
    SDL_Rect close_button_rect_{};

    bool active_ = false;
    std::string status_text_;
    Uint64 status_expire_ticks_ = 0;
};
