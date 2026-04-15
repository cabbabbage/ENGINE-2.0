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

    std::vector<std::unique_ptr<Room>> generate_trails(
        const std::vector<std::pair<Room*, Room*>>& room_pairs,
        const std::vector<Area>& existing_areas,
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
    std::vector<SDL_Color> trail_colors_;
};

#ifdef ENGINE_WORLD_TESTS
namespace trail_generation::debug {
struct SectionDebug {
    double distance_along_centerline = 0.0;
    int width = 0;
    double shift = 0.0;
    SDL_Point left{0, 0};
    SDL_Point right{0, 0};
};

struct TrailLayoutDebug {
    SDL_Point start_tip{0, 0};
    SDL_Point end_tip{0, 0};
    std::vector<SectionDebug> sections;
    std::vector<SDL_Point> polygon;
};

bool build_layout_for_tests(const SDL_Point& start_tip,
                            const SDL_Point& end_tip,
                            int min_width,
                            int max_width,
                            int curvyness,
                            const std::vector<Area>& existing_trails,
                            std::uint32_t seed,
                            TrailLayoutDebug* out_layout);
} // namespace trail_generation::debug
#endif
