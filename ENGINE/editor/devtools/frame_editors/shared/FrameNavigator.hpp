#pragma once

#include <SDL3/SDL.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "devtools/widgets.hpp"

namespace animation_editor {
class PreviewProvider;
}

namespace devmode::frame_editors {

class FrameNavigator {
public:
    FrameNavigator();
    ~FrameNavigator();

    // Configuration
    void set_frame_count(int count);
    void set_current_frame(int frame);
    void set_on_frame_changed(std::function<void(int)> callback);
    void set_on_before_change(std::function<bool(int, int)> callback);
    void set_on_apply_next(std::function<void()> callback);
    void set_on_apply_animation(std::function<void()> callback);
    void set_on_apply_all(std::function<void()> callback);
    void set_confirmation_handler(std::function<bool(const std::string&, const std::string&)> callback);
    void set_preview_source(std::weak_ptr<animation_editor::PreviewProvider> provider,
                            const std::string& animation_id);
    void set_enabled(bool enabled);

    // UI Management
    void set_rect(const SDL_Rect& rect);
    const SDL_Rect& get_rect() const;
    SDL_Rect get_preferred_rect() const;

    // Event Handling & Rendering
    bool handle_event(const SDL_Event& e);
    void render(SDL_Renderer* renderer);

    // State Queries
    int get_current_frame() const { return current_frame_; }
    int get_frame_count() const { return frame_count_; }
    bool is_enabled() const { return enabled_; }

private:
    void request_frame_change(int frame);
    void ensure_frame_visible(int frame);
    void clamp_scroll();
    int frame_index_at_point(const SDL_Point& p) const;
    void update_hover(const SDL_Point& p);
    void reset_hover();
    SDL_Rect compute_thumb_rect(int index) const;
    void render_background(SDL_Renderer* renderer) const;
    void render_thumbnails(SDL_Renderer* renderer);
    void render_badge(SDL_Renderer* renderer, const SDL_Rect& thumb_rect, int index, bool active) const;
    bool confirm_action(const std::string& title, const std::string& message) const;
    void handle_apply_next();
    void handle_apply_animation();
    void handle_apply_all();
    void update_button_states();
    void validate_frame_index();
    void notify_frame_changed();

    int current_frame_ = 0;
    int frame_count_ = 0;
    bool enabled_ = true;

    std::unique_ptr<DMButton> btn_prev_;
    std::unique_ptr<DMButton> btn_next_;
    std::unique_ptr<DMButton> btn_apply_next_;
    std::unique_ptr<DMButton> btn_apply_animation_;
    std::unique_ptr<DMButton> btn_apply_all_;

    std::function<void(int)> on_frame_changed_;
    std::function<bool(int, int)> on_before_change_;
    std::function<void()> on_apply_next_;
    std::function<void()> on_apply_animation_;
    std::function<void()> on_apply_all_;
    std::function<bool(const std::string&, const std::string&)> on_confirm_;
    std::weak_ptr<animation_editor::PreviewProvider> preview_provider_;
    std::string animation_id_;

    SDL_Rect rect_{0, 0, 0, 0};
    SDL_Rect strip_rect_{0, 0, 0, 0};
    float scroll_offset_ = 0.0f;
    int hovered_index_ = -1;
    int pressed_thumb_index_ = -1;
    bool prev_enabled_ = false;
    bool next_enabled_ = false;
};

}  // namespace devmode::frame_editors
