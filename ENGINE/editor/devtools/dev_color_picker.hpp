#pragma once

#include <SDL3/SDL.h>

#include <functional>
#include <memory>
#include <string>

class DMButton;

class DevColorPicker {
public:
    using ApplyCallback = std::function<void(SDL_Color)>;
    using CancelCallback = std::function<void()>;

    DevColorPicker();
    ~DevColorPicker();

    void open(SDL_Color initial_color, ApplyCallback on_apply, CancelCallback on_cancel = {});
    void close(bool apply_changes);
    bool is_open() const { return open_; }

    void set_screen_size(int width, int height);
    bool handle_event(const SDL_Event& event);
    void render(SDL_Renderer* renderer) const;

    SDL_Color preview_color() const;

private:
    void layout();
    void set_from_color(SDL_Color color);
    void update_preview_from_hsv();
    bool handle_picker_pointer(int x, int y);
    void render_hue_ring(SDL_Renderer* renderer) const;
    void render_sv_square(SDL_Renderer* renderer) const;
    static void rgb_to_hsv(SDL_Color color, float& out_h, float& out_s, float& out_v);

    bool open_ = false;
    int screen_w_ = 0;
    int screen_h_ = 0;

    SDL_Rect panel_rect_{0, 0, 0, 0};
    SDL_Rect hue_ring_rect_{0, 0, 0, 0};
    SDL_Rect sv_square_rect_{0, 0, 0, 0};
    SDL_Rect preview_rect_{0, 0, 0, 0};
    SDL_Rect label_rect_{0, 0, 0, 0};

    std::unique_ptr<DMButton> apply_button_;
    std::unique_ptr<DMButton> cancel_button_;

    float hue_deg_ = 0.0f;
    float saturation_ = 0.0f;
    float value_ = 0.0f;
    SDL_Color preview_color_{0, 0, 0, 255};

    ApplyCallback on_apply_{};
    CancelCallback on_cancel_{};
};

