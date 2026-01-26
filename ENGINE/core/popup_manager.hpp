#pragma once

#include <SDL.h>
#include <memory>
#include <string>

#include "dev_mode/dm_styles.hpp"

class SDL_Renderer;

class PopupManager {
public:
    PopupManager();

    void show_toast(const std::string& message, Uint32 duration_ms);
    void notify_camera_activity(const std::string& room_name, bool active, Uint32 timestamp_ms);
    void notify_room_change(const std::string& room_name, Uint32 timestamp_ms);

    void update(Uint32 now);
    void render(SDL_Renderer* renderer, int screen_w, int screen_h, Uint32 now);

private:
    using TexturePtr = std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)>;

    struct ToastState {
        std::string message;
        bool visible = false;
        bool dirty = false;
        Uint32 expiry_ms = 0;
        TexturePtr texture{nullptr, SDL_DestroyTexture};
        int width = 0;
        int height = 0;
    };

    struct IndicatorState {
        std::string room_name;
        std::string label_text;
        bool showing = false;
        bool active = false;
        bool dirty = false;
        Uint32 hold_until_ms = 0;
        Uint32 fade_start_ms = 0;
        Uint32 fade_end_ms = 0;
        TexturePtr texture{nullptr, SDL_DestroyTexture};
        int width = 0;
        int height = 0;
    };

    void rebuild_toast_texture(SDL_Renderer* renderer);
    void rebuild_indicator_texture(SDL_Renderer* renderer);
    float compute_indicator_alpha(Uint32 now) const;

    ToastState toast_;
    IndicatorState indicator_;
    DMLabelStyle toast_style_;
    DMLabelStyle indicator_style_;
};
