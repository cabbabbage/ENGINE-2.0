#pragma once

#include <vector>
#include <SDL3/SDL.h>

class Area;
class Assets;

namespace dynamic_spawn::geometry {

struct AreaGeometry {
    struct Segment { SDL_Point a{}; SDL_Point b{}; };
    std::vector<const Area*> areas;
    std::vector<Segment> segments;
    int min_x = 0;
    int min_z = 0;
    int max_x = -1;
    int max_z = -1;
    bool valid = false;
};

bool point_inside_any_area(SDL_Point point, const AreaGeometry& geometry);
bool point_near_geometry(SDL_Point point, const AreaGeometry& geometry, int threshold_px);
AreaGeometry collect_area_geometry(const Assets& assets);
int max_allowed_boundary_z_for_x(const AreaGeometry& geometry, int world_x, int max_spawn_from_room_px);

} // namespace dynamic_spawn::geometry
