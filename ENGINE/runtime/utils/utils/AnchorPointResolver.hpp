#pragma once

#include "assets/asset/anchor_point.hpp"

class Asset;

namespace anchor_points {

struct AnchorWorldPoint3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    bool valid = false;
};

// Compute the normalized camera->point world direction for a flat anchor point.
// Returns false if the direction cannot be resolved safely.
bool compute_camera_to_point_ray(const Asset& asset,
                                 const AnchorWorldPoint3& flat_point,
                                 AnchorWorldPoint3& out_direction);

// Apply a signed world-pixel displacement along the normalized camera->point ray.
// Positive values move farther from camera; negative values move closer.
bool displace_along_camera_to_point_ray(const Asset& asset,
                                        const AnchorWorldPoint3& flat_point,
                                        float signed_offset,
                                        AnchorWorldPoint3& out_point,
                                        AnchorWorldPoint3* out_direction = nullptr);

// Build asymmetric extrusion endpoints around a flat point along the camera->point ray.
// out_near_point is closer to camera (backward extrusion); out_far_point is farther from camera (forward extrusion).
bool build_asymmetric_camera_ray_extrusion(const Asset& asset,
                                           const AnchorWorldPoint3& flat_point,
                                           float extrusion_backward,
                                           float extrusion_forward,
                                           AnchorWorldPoint3& out_near_point,
                                           AnchorWorldPoint3& out_far_point,
                                           AnchorWorldPoint3* out_direction = nullptr);

struct FrameAnchorSample {
    SDL_FPoint uv{0.5f, 0.5f};
    ResolvedAnchor resolved{};
    AnchorWorldPoint3 flat_relative_pixel_point{};
    AnchorWorldPoint3 final_anchor_point{};
    SDL_FPoint flat_screen_px{0.0f, 0.0f};
    bool has_flat_screen_px = false;
    SDL_FPoint final_screen_px{0.0f, 0.0f};
    bool has_final_screen_px = false;
    SDL_FPoint screen_px{0.0f, 0.0f};
};

ResolvedAnchor resolve_anchor_point(const Asset& asset,
                                    const DisplacedAssetAnchorPoint& anchor,
                                    GridMaterialization grid_policy = GridMaterialization::None);

FrameAnchorSample resolve_frame_anchor_sample(const Asset& asset,
                                              const DisplacedAssetAnchorPoint& anchor,
                                              GridMaterialization grid_policy = GridMaterialization::None);

struct PixelLockedAnchor {
    ResolvedAnchor resolved{};
    SDL_FPoint screen_px{0.0f, 0.0f};
};

PixelLockedAnchor resolve_pixel_locked_anchor(const Asset& asset,
                                              const DisplacedAssetAnchorPoint& anchor,
                                              GridMaterialization grid_policy = GridMaterialization::None);

// Shared height helper so runtime/editor conversions agree on the anchor basis.
float anchor_height_px(const Asset& asset);

}
