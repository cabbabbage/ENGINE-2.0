#pragma once

#include "assets/asset/anchor_point.hpp"

class Asset;

namespace anchor_points {

ResolvedAnchor resolve_anchor_point(const Asset& asset,
                                    const DisplacedAssetAnchorPoint& anchor,
                                    GridMaterialization grid_policy = GridMaterialization::None);

// Shared height helper so runtime/editor conversions agree on the anchor basis.
float anchor_height_px(const Asset& asset);

}
