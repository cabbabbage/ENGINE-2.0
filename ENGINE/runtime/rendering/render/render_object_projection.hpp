#pragma once

#include <SDL3/SDL.h>

#include "rendering/render/render_object.hpp"
#include "rendering/render/projected_sprite_frame.hpp"

class WarpedScreenGrid;

namespace render_projection {

float sanitize_perspective_scale(float perspective_scale);

bool assemble_render_object_projection_input(const RenderObject& obj,
                                             float perspective_scale,
                                             float world_z,
                                             SpriteProjectionInput& out_input);

bool build_render_object_projected_frame(const WarpedScreenGrid& cam,
                                         const RenderObject& obj,
                                         float perspective_scale,
                                         float world_z,
                                         ProjectedSpriteFrame& out_projection);

} // namespace render_projection
