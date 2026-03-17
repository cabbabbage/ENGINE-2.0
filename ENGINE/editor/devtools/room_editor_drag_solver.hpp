#pragma once

#include "assets/asset/anchor_point.hpp"

class Asset;

namespace devmode::room_editor_drag_solver {

bool solve_texture_point_for_world_target(const Asset& asset,
                                          const DisplacedAssetAnchorPoint& anchor_template,
                                          float desired_world_x,
                                          float desired_world_y,
                                          int initial_texture_x,
                                          int initial_texture_y,
                                          int& out_texture_x,
                                          int& out_texture_y);

}  // namespace devmode::room_editor_drag_solver

