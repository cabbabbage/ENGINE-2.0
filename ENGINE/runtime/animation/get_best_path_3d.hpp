#pragma once

#include <vector>

#include "core/axis_convention.hpp"
#include "stride_types.hpp"
#include "utils/grid.hpp"

class Asset;

class GetBestPath3D {
public:
    Plan3D operator()(const Asset& self,
                      const std::vector<axis::WorldPos>& sanitized_checkpoints,
                      int visited_thresh_px,
                      const vibble::grid::Grid& grid) const;
};
