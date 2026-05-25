#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "devtools/animation_source_navigation.hpp"

class AssetStackAnimationListPanel {
  public:
    struct Row {
        std::string animation_id;
        bool editable = false;
        devmode::StackAnimationEditabilityReason reason = devmode::StackAnimationEditabilityReason::Editable;
    };

    AssetStackAnimationListPanel();

    void set_visible(bool visible);
    bool is_visible() const { return visible_; }

    void set_screen_dimensions(int width, int height);
    void set_panel_bounds_override(const SDL_Rect& bounds);
    void clear_panel_bounds_override();

    void set_rows(std::vector<Row> rows);
    void set_selected_animation_id(const std::optional<std::string>& animation_id);
    void set_on_select(std::function<void(const std::string&)> callback);

    bool handle_event(const SDL_Event& e);
    void render(SDL_Renderer* renderer) const;

    bool is_point_inside(int x, int y) const;
    const SDL_Rect& rect() const { return panel_rect_; }

  private:
    SDL_Rect row_rect(int index) const;
    int row_index_at(SDL_Point p) const;
    void recalculate_scroll_range();
    static std::string row_subtitle(const Row& row);

  private:
    bool visible_ = false;
    int screen_w_ = 0;
    int screen_h_ = 0;
    bool panel_override_active_ = false;
    SDL_Rect panel_override_{0, 0, 0, 0};
    SDL_Rect panel_rect_{0, 0, 0, 0};
    std::vector<Row> rows_;
    std::optional<std::string> selected_animation_id_;
    std::function<void(const std::string&)> on_select_;
    int scroll_px_ = 0;
    int max_scroll_px_ = 0;
    int hovered_row_ = -1;
};

