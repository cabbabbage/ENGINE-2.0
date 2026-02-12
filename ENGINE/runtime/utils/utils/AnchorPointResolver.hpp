#pragma once

#include "assets/asset/anchor_point.hpp"

class Asset;

namespace anchor_points {

ResolvedAnchor resolve_anchor_point(const Asset& asset, const DisplacedAssetAnchorPoint& anchor);

}

