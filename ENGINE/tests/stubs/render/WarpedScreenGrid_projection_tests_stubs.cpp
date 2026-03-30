#include "rendering/render/warped_screen_grid.hpp"

bool WarpedScreenGrid::project_world_point(SDL_FPoint world, float world_z, SDL_FPoint& out) const {
    out.x = world.x;
    out.y = world.y - world_z;
    return true;
}
