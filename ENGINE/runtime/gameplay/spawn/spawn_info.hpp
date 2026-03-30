#pragma once

#include <optional>
#include <random>
#include <string>
#include <unordered_set>

#include <SDL3/SDL.h>

#include "runtime_candidates.hpp"

struct SpawnInfo {
    enum class ExecutionMode {
        Standard = 0,
        BatchGrid = 1,
    };

    std::string name;
    std::string position;
    std::string spawn_id;
    int priority = 0;
    int quantity = 0;
    bool check_min_spacing = false;
    int grid_resolution = 0;

    std::string link_area_name;

    SDL_Point exact_offset{0, 0};
    int exact_origin_w = 0;
    int exact_origin_h = 0;
    SDL_Point exact_point{-1, -1};

    int perimeter_radius = 0;

    int edge_inset_percent = 100;

    bool adjust_geometry_to_room = false;
    ExecutionMode execution_mode = ExecutionMode::Standard;

    vibble::spawn::RuntimeCandidates candidates;

    bool has_candidates() const { return !candidates.empty(); }
    bool uses_batch_grid() const { return execution_mode == ExecutionMode::BatchGrid; }

    std::optional<vibble::spawn::ResolvedCandidate> select_candidate(
        std::mt19937& rng,
        const vibble::spawn::RuntimeCandidates::AssetCatalogView& catalog) const {
        return candidates.pick_random(rng, catalog, vibble::spawn::ZeroWeightPolicy::UniformFallback);
    }

    std::optional<vibble::spawn::ResolvedCandidate> select_candidate_excluding(
        std::mt19937& rng,
        const vibble::spawn::RuntimeCandidates::AssetCatalogView& catalog,
        const std::unordered_set<int>& excluded_entry_indices) const {
        return candidates.pick_random_excluding(rng,
                                                catalog,
                                                excluded_entry_indices,
                                                vibble::spawn::ZeroWeightPolicy::UniformFallback);
    }
};
