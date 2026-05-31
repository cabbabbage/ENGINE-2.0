#include <cassert>
#include <string>
#include <vector>

#include "gameplay/map_generation/room_legacy_migration.hpp"

int main() {
    // (a) modern size path
    nlohmann::json modern = {
        {"size", 12}
    };
    std::vector<std::string> modern_events;
    auto modern_size = room_legacy_migration::resolve_room_size(
        modern,
        9,
        7,
        20,
        false,
        [&modern_events](const char* reason) { modern_events.emplace_back(reason ? reason : ""); });
    assert(modern_size.size == 12);
    assert(!modern_size.used_legacy_migration);
    assert(modern_events.empty());

    // (b) missing size defaults to migration value
    nlohmann::json legacy = {
        {"geometry", "circle"},
        {"width", {{"center", 200}, {"span", 100}}}
    };
    std::vector<std::string> legacy_events;
    auto legacy_size = room_legacy_migration::resolve_room_size(
        legacy,
        9,
        7,
        20,
        false,
        [&legacy_events](const char* reason) { legacy_events.emplace_back(reason ? reason : ""); });
    assert(legacy_size.size == 9);
    assert(legacy_size.used_legacy_migration);
    assert(!legacy_events.empty());

    // (c) bounds clamp
    nlohmann::json clamped_low = {{"size", 1}};
    auto low_size = room_legacy_migration::resolve_room_size(clamped_low, 9, 7, 20, false, nullptr);
    assert(low_size.size == 7);

    nlohmann::json clamped_high = {{"size", 100}};
    auto high_size = room_legacy_migration::resolve_room_size(clamped_high, 9, 7, 20, false, nullptr);
    assert(high_size.size == 20);

    // (d) trail out-of-range coerces to default 5
    nlohmann::json trail_invalid = {{"size", 99}};
    auto trail_size = room_legacy_migration::resolve_room_size(trail_invalid, 5, 5, 10, true, nullptr);
    assert(trail_size.size == 5);
}
