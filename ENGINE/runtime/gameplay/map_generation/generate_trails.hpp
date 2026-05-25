#pragma once

#include "room.hpp"
#include "assets/asset/asset_library.hpp"
#include "utils/area.hpp"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

struct TrailGenerationCounters {
    int total_connections = 0;
    int successful_connections = 0;
    int failed_connections = 0;
    int total_asset_attempts = 0;
    int total_layout_attempts = 0;
    int total_straight_attempts = 0;
    int total_curved_attempts = 0;
    int total_room_rejections = 0;
    int total_trail_rejections = 0;
    int total_section_failures = 0;
    int total_polygon_failures = 0;
    int total_sector_contact_failures = 0;
    int total_sector_boundary_failures = 0;
    int total_route_budget_failures = 0;
    int total_rooms_considered = 0;
    int split_attempts = 0;
    int split_successes = 0;
    int split_exhausted = 0;
};

struct TrailConnectionFailure {
    Room* a = nullptr;
    Room* b = nullptr;
    std::string reason;
};

struct TrailUnresolvedRoom {
    Room* room = nullptr;
    std::string phase;
    std::string reason;
};

struct TrailGenerationResult {
    std::vector<std::unique_ptr<Room>> trail_rooms;
    std::vector<TrailConnectionFailure> required_failures;
    std::vector<TrailConnectionFailure> optional_skips;
    std::vector<TrailUnresolvedRoom> unresolved_rooms;
    bool all_required_connected = false;
    TrailGenerationCounters counters;
};

namespace devmode::core {
class ManifestStore;
}

// World-space distance between interior perpendicular sampling lines along a trail centerline.
// Increase for fewer, straighter sections; decrease for denser, more varied sections.
inline constexpr int kTrailPerpendicularSectionSpacingWorldPx = 200;

class GenerateTrails {
public:
    explicit GenerateTrails(nlohmann::json& trail_data, std::vector<SDL_Color> reserved_colors = {});

    void set_all_rooms_reference(const std::vector<Room*>& rooms);

    TrailGenerationResult generate_trails(
        const std::vector<std::pair<Room*, Room*>>& room_pairs,
        const std::string& manifest_context,
        AssetLibrary* asset_lib,
        double map_radius,
        nlohmann::json* map_manifest,
        devmode::core::ManifestStore* manifest_store,
        Room::ManifestWriter manifest_writer);

private:
    struct TrailTemplateRef {
        std::string name;
        nlohmann::json* data = nullptr;
    };

    const TrailTemplateRef* pick_random_asset();
    std::vector<std::pair<Room*, Room*>> plan_maze_connections(
        const std::vector<Room*>& rooms,
        const std::vector<std::pair<Room*, Room*>>& forced_connections);

    std::vector<TrailTemplateRef> available_assets_;
    std::vector<Room*> all_rooms_reference_;
    std::mt19937 rng_;
    bool testing_ = false;
    nlohmann::json* trails_data_ = nullptr;
    nlohmann::json fallback_trails_data_ = nlohmann::json::object();
    std::vector<SDL_Color> trail_colors_;
};

