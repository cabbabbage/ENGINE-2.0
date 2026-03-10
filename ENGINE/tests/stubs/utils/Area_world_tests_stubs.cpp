#include "utils/area.hpp"

#if defined(ENGINE_WORLD_TESTS)

Area::Area(const std::string& name, int resolution)
    : pos{0, 0}
    , area_name_(name) {
    resolution_ = vibble::grid::clamp_resolution(resolution);
}

Area::Area(const std::string& name, const std::vector<Point>& pts, int resolution)
    : points(pts)
    , area_name_(name) {
    resolution_ = vibble::grid::clamp_resolution(resolution);
}

#endif // ENGINE_WORLD_TESTS
