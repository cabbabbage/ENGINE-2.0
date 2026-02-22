#pragma once

#include "assets/asset/anchor_point.hpp"

class Asset;

namespace anchor_points {

enum class AnchorDepthPolicy {
    MatchOwner,
    InFront,
    Behind
};

struct FrameAnchorSample {
    SDL_FPoint uv{0.5f, 0.5f};
    ResolvedAnchor resolved{};
    SDL_FPoint screen_px{0.0f, 0.0f};
};

ResolvedAnchor resolve_anchor_point(const Asset& asset,
                                    const DisplacedAssetAnchorPoint& anchor,
                                    GridMaterialization grid_policy = GridMaterialization::None);

FrameAnchorSample resolve_frame_anchor_sample(const Asset& asset,
                                              const DisplacedAssetAnchorPoint& anchor,
                                              AnchorDepthPolicy depth_policy,
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
