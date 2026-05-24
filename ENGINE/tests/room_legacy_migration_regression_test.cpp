#include <cassert>
#include <string>
#include <vector>

#include "gameplay/map_generation/room_legacy_migration.hpp"

int main() {
    using vibble::weighted_range::WeightedIntRange;
    const WeightedIntRange fallback = vibble::weighted_range::make_legacy_uniform(64, 64);

    // (a) modern manifest-only path
    nlohmann::json modern = {
        {"width", { {"center", 120}, {"span", 0} }},
        {"height", { {"center", 88}, {"span", 0} }},
        {"geometry", "square"}
    };
    std::vector<std::string> modern_events;
    auto modern_ranges = room_legacy_migration::resolve_dimension_ranges(
        modern,
        fallback,
        [&modern_events](const char* reason) { modern_events.emplace_back(reason ? reason : ""); });
    assert(modern_ranges.width.center == 120);
    assert(modern_ranges.height.center == 88);
    assert(!modern_ranges.used_legacy_migration);
    assert(modern_events.empty());

    // (b) legacy migration path
    nlohmann::json legacy = {
        {"geometry", "circle"},
        {"min_radius", 11},
        {"max_radius", 13}
    };
    std::vector<std::string> legacy_events;
    auto legacy_ranges = room_legacy_migration::resolve_dimension_ranges(
        legacy,
        fallback,
        [&legacy_events](const char* reason) { legacy_events.emplace_back(reason ? reason : ""); });
    assert(legacy_ranges.used_legacy_migration);
    assert(legacy_ranges.width.center == 12);
    assert(legacy_ranges.width.span == 1);
    assert(legacy_ranges.height.center == legacy_ranges.width.center);
    assert(!legacy_events.empty());

    // (c) boundary handoff correctness (manifest width + legacy height fallback)
    nlohmann::json hybrid = {
        {"width", { {"center", 150}, {"span", 0} }},
        {"min_height", 20},
        {"max_height", 22},
        {"geometry", "square"}
    };
    bool saw_boundary_migration = false;
    auto hybrid_ranges = room_legacy_migration::resolve_dimension_ranges(
        hybrid,
        fallback,
        [&saw_boundary_migration](const char* reason) {
            if (reason && std::string(reason) == "legacy_dimension_bounds") {
                saw_boundary_migration = true;
            }
        });
    assert(hybrid_ranges.width.center == 150);
    assert(hybrid_ranges.height.center == 21);
    assert(hybrid_ranges.used_legacy_migration);
    assert(saw_boundary_migration);
}
