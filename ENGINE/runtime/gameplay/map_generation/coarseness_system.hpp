#pragma once

#include "utils/area.hpp"
#include "utils/map_grid_settings.hpp"
#include "utils/weighted_range.hpp"

#include <memory>
#include <optional>
#include <string>
#include <cstdint>
#include <vector>

class Room;

namespace vibble::mapgen::coarseness {

struct CoarsenessGeometryItem {
    std::string identifier;
    Area* geometry = nullptr;
    std::optional<vibble::weighted_range::WeightedIntRange> coarseness_range;
    MapGridSettings grid_settings{};
    std::uint64_t deterministic_seed = 0;
};

struct CoarsenessExpansionResult {
    std::string identifier;
    std::unique_ptr<Area> base_area;
    std::unique_ptr<Area> expanded_area;
    std::unique_ptr<Area> expansion_area;
    std::vector<Area> expanded_areas;
    std::vector<Area> soft_boundary_areas;
    int perimeter_samples = 0;
    int expanded_paths = 0;
    int soft_boundary_paths = 0;
    int uncovered_perimeter_segments = 0;
    double perimeter_length_processed = 0.0;
    double expanded_area_size = 0.0;
    double soft_boundary_area_size = 0.0;
};

std::vector<CoarsenessExpansionResult> apply_coarseness_pass(const std::vector<CoarsenessGeometryItem>& items);
void apply_coarseness_expansion(std::vector<Room*>& rooms, std::uint64_t deterministic_seed = 0);

}
