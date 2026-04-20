#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_library.hpp"
#include "gameplay/map_generation/room.hpp"
#include "gameplay/spawn/asset_spawner.hpp"
#include "utils/grid.hpp"
#include "utils/grid_occupancy.hpp"

namespace {

Area make_rect_area(const std::string& name, int half_width, int half_depth) {
    std::vector<Area::Point> points{
        Area::Point{-half_width, -half_depth},
        Area::Point{half_width, -half_depth},
        Area::Point{half_width, half_depth},
        Area::Point{-half_width, half_depth}
    };
    return Area(name, points, 0);
}

nlohmann::json make_base_metadata() {
    return nlohmann::json::object({
        {"movement_enabled", false},
        {"attack_box_enabled", false},
        {"hitbox_enabled", false},
        {"impassable_enabled", false},
        {"floor_boxes_enabled", false}
    });
}

nlohmann::json make_floor_box_entry(const std::string& id,
                                    const nlohmann::json& candidates,
                                    int grid_resolution) {
    return nlohmann::json::object({
        {"id", id},
        {"name", id},
        {"position_x", 0.0},
        {"position_z", 0.0},
        {"width", 24.0},
        {"depth", 24.0},
        {"enabled", true},
        {"tags", nlohmann::json::array()},
        {"candidate",
         nlohmann::json::object({
             {"candidates", candidates},
             {"grid_resolution", grid_resolution}
         })}
    });
}

nlohmann::json make_metadata_with_floor_box_candidate(const std::string& box_id,
                                                      const nlohmann::json& candidates,
                                                      int grid_resolution) {
    nlohmann::json metadata = make_base_metadata();
    metadata["floor_boxes_enabled"] = true;
    metadata["floor_boxes"] = nlohmann::json::array(
        {make_floor_box_entry(box_id, candidates, grid_resolution)});
    return metadata;
}

nlohmann::json make_owner_spawn_groups(const std::string& owner_asset_name) {
    return nlohmann::json::object({
        {"spawn_groups",
         nlohmann::json::array({
             nlohmann::json::object({
                 {"spawn_id", "owner_spawn"},
                 {"display_name", "Owner Spawn"},
                 {"position", "Center"},
                 {"min_number", 1},
                 {"max_number", 1},
                 {"enforce_spacing", false},
                 {"resolve_geometry_to_room_size", true},
                 {"resolve_quantity_to_room_size", false},
                 {"resolution", 0},
                 {"candidates",
                  nlohmann::json::array({
                      nlohmann::json::object({{"name", "null"}, {"chance", 0}}),
                      nlohmann::json::object({{"name", owner_asset_name}, {"chance", 100}})
                  })}
             })
         })}
    });
}

std::unique_ptr<Room> make_room(AssetLibrary& library,
                                Area& precomputed_area,
                                nlohmann::json& room_data,
                                const std::string& room_name) {
    MapGridSettings settings = MapGridSettings::defaults();
    return std::make_unique<Room>(Room::Point{0, 0},
                                  "room",
                                  room_name,
                                  nullptr,
                                  "test_manifest",
                                  &library,
                                  &precomputed_area,
                                  &room_data,
                                  settings,
                                  5000.0,
                                  "rooms_data",
                                  nullptr,
                                  nullptr,
                                  std::string{},
                                  Room::ManifestWriter{},
                                  true);
}

int count_assets_named(const Room& room, const std::string& name) {
    int count = 0;
    for (const auto& asset_uptr : room.assets) {
        if (!asset_uptr || !asset_uptr->info) {
            continue;
        }
        if (asset_uptr->info->name == name) {
            ++count;
        }
    }
    return count;
}

Asset* find_asset_named(Room& room, const std::string& name) {
    for (auto& asset_uptr : room.assets) {
        if (!asset_uptr || !asset_uptr->info) {
            continue;
        }
        if (asset_uptr->info->name == name) {
            return asset_uptr.get();
        }
    }
    return nullptr;
}

bool has_asset_named_at(const Room& room, const std::string& name, SDL_Point point) {
    for (const auto& asset_uptr : room.assets) {
        if (!asset_uptr || !asset_uptr->info) {
            continue;
        }
        if (asset_uptr->info->name != name) {
            continue;
        }
        if (asset_uptr->world_x() == point.x && asset_uptr->world_z() == point.y) {
            return true;
        }
    }
    return false;
}

int expected_floor_vertices_excluding_owner(const Asset& owner, const Asset::RuntimeFloorBox& floor_box) {
    if (!floor_box.candidate.has_value()) {
        return 0;
    }

    const int grid_resolution = std::clamp(vibble::grid::clamp_resolution(floor_box.candidate->grid_resolution), 2, 8);
    const float half_width = std::max(0.0f, floor_box.width * 0.5f);
    const float half_depth = std::max(0.0f, floor_box.depth * 0.5f);
    if (half_width <= 0.0f || half_depth <= 0.0f) {
        return 0;
    }

    const float center_x = static_cast<float>(owner.world_x()) + floor_box.position_x;
    const float center_z = static_cast<float>(owner.world_z()) + floor_box.position_z;
    const int min_x = static_cast<int>(std::floor(center_x - half_width));
    const int max_x = static_cast<int>(std::ceil(center_x + half_width));
    const int min_z = static_cast<int>(std::floor(center_z - half_depth));
    const int max_z = static_cast<int>(std::ceil(center_z + half_depth));
    if (max_x <= min_x || max_z <= min_z) {
        return 0;
    }

    std::vector<SDL_Point> polygon{
        SDL_Point{min_x, min_z},
        SDL_Point{max_x, min_z},
        SDL_Point{max_x, max_z},
        SDL_Point{min_x, max_z}
    };
    Area area("expected_floor_box_area", polygon);
    vibble::grid::Occupancy occupancy(area, grid_resolution, vibble::grid::global_grid());
    const auto vertices = occupancy.vertices_in_area(area);
    const SDL_Point owner_anchor = vibble::grid::global_grid().snap_to_vertex(owner.world_xz_point(), grid_resolution);

    int count = 0;
    for (const auto* vertex : vertices) {
        if (!vertex) {
            continue;
        }
        if (vertex->world.x == owner_anchor.x && vertex->world.y == owner_anchor.y) {
            continue;
        }
        ++count;
    }
    return count;
}

} // namespace

TEST_CASE("AssetSpawner floor-box candidate pass spawns per grid point, skips owner point, and does not cascade") {
    AssetLibrary library(false);
    library.add_asset("cascade_asset", make_base_metadata());
    library.add_asset(
        "floor_spawn_asset",
        make_metadata_with_floor_box_candidate(
            "floor_spawn_box",
            nlohmann::json::array({
                nlohmann::json::object({{"name", "null"}, {"chance", 0}}),
                nlohmann::json::object({{"name", "cascade_asset"}, {"chance", 100}})
            }),
            2));
    library.add_asset(
        "owner_asset",
        make_metadata_with_floor_box_candidate(
            "owner_floor_box",
            nlohmann::json::array({
                nlohmann::json::object({{"name", "null"}, {"chance", 0}}),
                nlohmann::json::object({{"name", "floor_spawn_asset"}, {"chance", 100}})
            }),
            2));

    Area room_area = make_rect_area("floor_spawn_room_area", 64, 64);
    nlohmann::json room_data = make_owner_spawn_groups("owner_asset");
    auto room = make_room(library, room_area, room_data, "floor_spawn_room");
    REQUIRE(room != nullptr);
    REQUIRE(room->planner != nullptr);

    AssetSpawner spawner(&library, {});
    spawner.spawn(*room);

    CHECK(count_assets_named(*room, "owner_asset") == 1);
    Asset* owner = find_asset_named(*room, "owner_asset");
    REQUIRE(owner != nullptr);
    REQUIRE_FALSE(owner->getFloorBoxes().empty());

    const auto& owner_floor_box = owner->getFloorBoxes().front();
    REQUIRE(owner_floor_box.candidate.has_value());

    const int expected_floor_count = expected_floor_vertices_excluding_owner(*owner, owner_floor_box);
    CHECK(expected_floor_count > 0);
    CHECK(count_assets_named(*room, "floor_spawn_asset") == expected_floor_count);
    CHECK(count_assets_named(*room, "cascade_asset") == 0);

    const int grid_resolution =
        std::clamp(vibble::grid::clamp_resolution(owner_floor_box.candidate->grid_resolution), 2, 8);
    const SDL_Point owner_anchor =
        vibble::grid::global_grid().snap_to_vertex(owner->world_xz_point(), grid_resolution);
    CHECK_FALSE(has_asset_named_at(*room, "floor_spawn_asset", owner_anchor));
}

TEST_CASE("AssetSpawner floor-box candidate pass skips null-only and zero-weight payloads") {
    const nlohmann::json zero_weight_candidates = nlohmann::json::array({
        nlohmann::json::object({{"name", "null"}, {"chance", 0}}),
        nlohmann::json::object({{"name", "floor_spawn_asset"}, {"chance", 0}})
    });
    const nlohmann::json null_only_candidates = nlohmann::json::array({
        nlohmann::json::object({{"name", "null"}, {"chance", 100}})
    });

    auto run_case = [&](const nlohmann::json& floor_candidates) {
        AssetLibrary library(false);
        library.add_asset("floor_spawn_asset", make_base_metadata());
        library.add_asset("owner_asset",
                          make_metadata_with_floor_box_candidate("owner_floor_box", floor_candidates, 2));

        Area room_area = make_rect_area("floor_spawn_skip_room_area", 64, 64);
        nlohmann::json room_data = make_owner_spawn_groups("owner_asset");
        auto room = make_room(library, room_area, room_data, "floor_spawn_skip_room");
        REQUIRE(room != nullptr);
        REQUIRE(room->planner != nullptr);

        AssetSpawner spawner(&library, {});
        spawner.spawn(*room);

        CHECK(count_assets_named(*room, "owner_asset") == 1);
        CHECK(count_assets_named(*room, "floor_spawn_asset") == 0);
    };

    SUBCASE("null-only payload") {
        run_case(null_only_candidates);
    }
    SUBCASE("zero-weight non-null payload") {
        run_case(zero_weight_candidates);
    }
}
