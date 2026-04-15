#pragma once

#include <SDL3/SDL.h>

#include <string>
#include <unordered_map>
#include <vector>

class Asset;
class WarpedScreenGrid;
struct AnchorPoint;
struct AnimationFrame;

namespace render_debug {

struct MovementDebugPathSnapshot {
    std::vector<SDL_Point> world_points;
    SDL_Color color{48, 200, 255, 220};
};

struct MovementDebugAssetSnapshot {
    std::vector<MovementDebugPathSnapshot> paths;
};

struct MovementDebugObservedState {
    std::string animation_id;
    const ::AnimationFrame* frame = nullptr;
    bool frame_is_first = false;
    bool frame_is_last = false;
};

struct RuntimeLightDebugOverlayEntry {
    SDL_FPoint center{0.0f, 0.0f};
    float radius = 0.0f;
    bool rendered = false;
};

} // namespace render_debug

class DebugOverlayRenderer {
public:
    explicit DebugOverlayRenderer(SDL_Renderer* renderer);

    void render_light_culling(const std::vector<render_debug::RuntimeLightDebugOverlayEntry>& entries) const;

    void render_movement_debug(const WarpedScreenGrid& cam,
                               int screen_width,
                               int screen_height,
                               const std::unordered_map<const Asset*, render_debug::MovementDebugAssetSnapshot>& snapshots,
                               const std::vector<Asset*>& visible_assets) const;

    void render_anchor_debug(const WarpedScreenGrid& cam,
                             int screen_width,
                             int screen_height,
                             const std::vector<Asset*>& visible_assets,
                             bool dev_mode) const;

private:
    SDL_Renderer* renderer_ = nullptr;
};