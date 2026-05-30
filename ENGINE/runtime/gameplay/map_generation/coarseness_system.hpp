#pragma once

#include "utils/area.hpp"
#include "utils/map_grid_settings.hpp"
#include "utils/weighted_range.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

class Room;

namespace vibble::mapgen::coarseness {

struct CoarsenessGeometryItem {
    std::string identifier;
    Area* geometry = nullptr;
    std::optional<vibble::weighted_range::WeightedIntRange> coarseness_range;
    MapGridSettings grid_settings{};
};

struct CoarsenessExpansionResult {
    std::string identifier;
    std::unique_ptr<Area> expansion_area;
    int circles_attempted = 0;
    int circles_applied = 0;
    int circles_skipped = 0;
    int circle_intersections_succeeded = 0;
    int uncovered_perimeter_segments = 0;
    double perimeter_length_processed = 0.0;
    double expanded_area_size = 0.0;
};

std::vector<CoarsenessExpansionResult> apply_coarseness_pass(const std::vector<CoarsenessGeometryItem>& items);
void apply_coarseness_expansion(std::vector<Room*>& rooms);

}
