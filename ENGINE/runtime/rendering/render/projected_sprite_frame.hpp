#pragma once

#include <SDL3/SDL.h>

#include "gameplay/world/grid_point.hpp"

class WarpedScreenGrid;

namespace render_projection {

struct WorldPoint3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    bool valid = false;
};

struct CameraRay {
    WorldPoint3 origin{};
    WorldPoint3 direction{};
    bool valid = false;
};

struct SpriteProjectionInput {
    float world_x = 0.0f;
    float world_y = 0.0f;
    float world_z = 0.0f;
    float perspective_scale = 1.0f;
    int frame_width_px = 0;
    int frame_height_px = 0;
    int final_width_px = 0;
    int final_height_px = 0;
    SDL_FlipMode flip = SDL_FLIP_NONE;
    double angle = 0.0;
    SDL_FPoint anchor_uv{0.5f, 1.0f};
};

struct ProjectedSpriteFrame {
    SDL_FPoint screen_tl{0.0f, 0.0f};
    SDL_FPoint screen_tr{0.0f, 0.0f};
    SDL_FPoint screen_br{0.0f, 0.0f};
    SDL_FPoint screen_bl{0.0f, 0.0f};
    // Screen-space crop basis before sprite-local rotation is applied. Floor/sink
    // clipping uses this ground-parallel basis so the cut line follows the
    // projected world X/horizon direction instead of drifting with asset tilt.
    SDL_FPoint crop_screen_tl{0.0f, 0.0f};
    SDL_FPoint crop_screen_tr{0.0f, 0.0f};
    SDL_FPoint crop_screen_br{0.0f, 0.0f};
    SDL_FPoint crop_screen_bl{0.0f, 0.0f};
    bool has_crop_screen_basis = false;
    int frame_width_px = 0;
    int frame_height_px = 0;
    int final_width_px = 0;
    int final_height_px = 0;
    SDL_FlipMode flip = SDL_FLIP_NONE;
    SDL_FPoint anchor_uv{0.5f, 1.0f};
    float world_x = 0.0f;
    float world_y = 0.0f;
    float world_z = 0.0f;
    float world_width = 0.0f;
    float world_height = 0.0f;
    float perspective_scale = 1.0f;
    bool valid = false;

    SDL_FPoint anchor_uv_from_texture_pixel(SDL_Point texture_px) const;
    SDL_FPoint sample_screen_from_uv(SDL_FPoint uv) const;
};

bool build_projected_sprite_frame(const WarpedScreenGrid& cam,
                                  const SpriteProjectionInput& input,
                                  ProjectedSpriteFrame& out);

bool build_camera_ray_from_screen(const world::CameraProjectionParams& params,
                                  const SDL_FPoint& screen_point,
                                  CameraRay& out_ray);

bool intersect_camera_ray_on_world_z(const world::CameraProjectionParams& params,
                                     const CameraRay& ray,
                                     float target_world_z,
                                     WorldPoint3& out_world_point);

bool project_world_to_screen(const WarpedScreenGrid& cam,
                             const WorldPoint3& world_point,
                             SDL_FPoint& out_screen);

}  // namespace render_projection
