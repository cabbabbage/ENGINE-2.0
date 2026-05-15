#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "assets/asset/asset_library.hpp"
#include "assets/asset/asset_types.hpp"
#include "core/AssetsManager.hpp"
#include "core/manifest/map_data.hpp"
#include "core/runtime_world_context.hpp"
#include "gameplay/map_generation/generate_rooms.hpp"
#include "gameplay/map_generation/room.hpp"
#include "utils/map_grid_settings.hpp"

namespace {

nlohmann::json make_asset_metadata(const std::string& type = std::string(asset_types::object),
                                   nlohmann::json tags = nlohmann::json::array(),
                                   int y_pos_min = 0,
                                   int y_pos_max = 0,
                                   int tilt_min_deg = 0,
                                   int tilt_max_deg = 0) {
    return nlohmann::json::object({
        {"asset_type", type},
        {"tags", std::move(tags)},
        {"canvas_width", 16},
        {"canvas_height", 16},
        {"movement_enabled", false},
        {"attack_box_enabled", false},
        {"hitbox_enabled", false},
        {"impassable_enabled", false},
        {"floor_boxes_enabled", false},
        {"y_pos_min", y_pos_min},
        {"y_pos_max", y_pos_max},
        {"tilt_range_min_deg", tilt_min_deg},
        {"tilt_range_max_deg", tilt_max_deg}
    });
}

nlohmann::json make_live_selector(const std::string& spawn_id,
                                  const std::string& asset_name,
                                  int grid_resolution) {
    return nlohmann::json::object({
        {"spawn_id", spawn_id},
        {"display_name", spawn_id},
        {"execution_mode", "batch_grid"},
        {"grid_resolution", grid_resolution},
        {"candidates",
         nlohmann::json::array({
             nlohmann::json::object({{"name", asset_name}, {"chance", 100}})
         })}
    });
}

nlohmann::json make_null_live_selector(const std::string& spawn_id, int grid_resolution) {
    return nlohmann::json::object({
        {"spawn_id", spawn_id},
        {"display_name", spawn_id},
        {"execution_mode", "batch_grid"},
        {"grid_resolution", grid_resolution},
        {"candidates",
         nlohmann::json::array({
             nlohmann::json::object({{"name", "null"}, {"chance", 100}})
         })}
    });
}

Area make_rect_area(const std::string& name, int half_extent) {
    std::vector<Area::Point> points{
        Area::Point{-half_extent, -half_extent},
        Area::Point{ half_extent, -half_extent},
        Area::Point{ half_extent,  half_extent},
        Area::Point{-half_extent,  half_extent}
    };
    return Area(name, points, 4);
}

Area make_rect_area_at(const std::string& name, SDL_Point center, int half_extent) {
    std::vector<Area::Point> points{
        Area::Point{center.x - half_extent, center.y - half_extent},
        Area::Point{center.x + half_extent, center.y - half_extent},
        Area::Point{center.x + half_extent, center.y + half_extent},
        Area::Point{center.x - half_extent, center.y + half_extent}
    };
    return Area(name, points, 4);
}

void add_trail_named_area(Room& room, const std::string& name, SDL_Point center, int half_extent) {
    Room::NamedArea trail_area;
    trail_area.name = name;
    trail_area.type = "trail";
    trail_area.kind = "trail";
    trail_area.area = std::make_unique<Area>(make_rect_area_at(name, center, half_extent));
    if (trail_area.area) {
        trail_area.area->set_type("trail");
    }
    room.areas.push_back(std::move(trail_area));
}

nlohmann::json make_room_data(bool inherits_live_dynamic_assets) {
    return nlohmann::json::object({
        {"name", "Spawn"},
        {"geometry", "Square"},
        {"min_width", 64},
        {"max_width", 64},
        {"min_height", 64},
        {"max_height", 64},
        {"edge_smoothness", 100},
        {"inherits_live_dynamic_assets", inherits_live_dynamic_assets},
        {"spawn_groups", nlohmann::json::array()}
    });
}

std::unique_ptr<Room> make_runtime_room(AssetLibrary& library,
                                        Area& area,
                                        nlohmann::json& room_data) {
    MapGridSettings settings = MapGridSettings::defaults();
    settings.grid_resolution = 4;
    return std::make_unique<Room>(
        Room::Point{0, 0},
        "room",
        "Spawn",
        nullptr,
        "live_dynamic_test",
        &library,
        &area,
        &room_data,
        settings,
        256.0,
        "rooms_data",
        nullptr,
        nullptr,
        std::string{},
        Room::ManifestWriter{},
        false);
}

std::vector<LayerSpec> make_single_room_layers() {
    return std::vector<LayerSpec>{
        LayerSpec{0, 1, std::vector<RoomSpec>{RoomSpec{"Spawn", 1, {}}}}
    };
}

nlohmann::json make_rooms_manifest_data(bool inherits_live_dynamic_assets) {
    return nlohmann::json::object({{"Spawn", make_room_data(inherits_live_dynamic_assets)}});
}

std::vector<Asset*> live_dynamic_assets(const Assets& assets) {
    std::vector<Asset*> out;
    for (Asset* asset : assets.all) {
        if (asset && asset->is_dynamic_spawned_asset()) {
            out.push_back(asset);
        }
    }
    return out;
}

int count_live_assets_named(const Assets& assets, const std::string& name) {
    int count = 0;
    for (const Asset* asset : assets.all) {
        if (asset && asset->is_dynamic_spawned_asset() && asset->info && asset->info->name == name) {
            ++count;
        }
    }
    return count;
}

std::set<std::pair<int, int>> collect_live_positions_named(const Assets& assets, const std::string& name) {
    std::set<std::pair<int, int>> out;
    for (const Asset* asset : assets.all) {
        if (asset && asset->is_dynamic_spawned_asset() && asset->info && asset->info->name == name) {
            out.emplace(asset->world_x(), asset->world_z());
        }
    }
    return out;
}

bool point_in_bounds(const world::GridBounds& bounds, int world_x, int world_z) {
    return world_x >= bounds.min.world_x() && world_x <= bounds.max.world_x() &&
           world_z >= bounds.min.world_z() && world_z <= bounds.max.world_z();
}

std::unique_ptr<Assets> make_assets(AssetLibrary& library,
                                    nlohmann::json manifest,
                                    Area room_area,
                                    nlohmann::json room_data,
                                    const std::string& map_id = "live_dynamic_render_bounds_test") {
    std::vector<std::unique_ptr<Room>> owned_rooms;
    owned_rooms.push_back(make_runtime_room(library, room_area, room_data));
    auto world_context = std::make_shared<RuntimeWorldContext>(std::move(owned_rooms));
    return std::make_unique<Assets>(
        library,
        nullptr,
        world_context,
        800,
        600,
        0,
        0,
        256,
        nullptr,
        nullptr,
        map_id,
        manifest,
        std::string{},
        world::WorldGrid{});
}

}  // namespace

TEST_CASE("GenerateRooms does not materialize live dynamic config into room assets") {
    AssetLibrary library(false);
    library.add_asset("candidate_boundary",
                      make_asset_metadata(std::string(asset_types::object),
                                          nlohmann::json::array({"boundary"})));

    nlohmann::json map_manifest = nlohmann::json::object();
    nlohmann::json rooms_data = make_rooms_manifest_data(true);
    nlohmann::json trails_data = nlohmann::json::object();
    nlohmann::json live_dynamic_spawns = nlohmann::json::object({
        {"boundary_area_selectors",
         nlohmann::json::array({make_live_selector("spn-boundary", "candidate_boundary", 4)})}
    });

    MapGridSettings settings = MapGridSettings::defaults();
    settings.grid_resolution = 4;

    GenerateRooms generator(make_single_room_layers(), 0, 0, "live_dynamic_test", map_manifest, 0.0);
    auto rooms = generator.build(&library, 256.0, {}, live_dynamic_spawns, rooms_data, trails_data, settings);

    REQUIRE_FALSE(rooms.empty());
    for (const auto& room : rooms) {
        REQUIRE(room != nullptr);
        CHECK(room->assets.empty());
    }
}

TEST_CASE("Live dynamic render-bounds sync creates only assets inside render bounds") {
    AssetLibrary library(false);
    library.add_asset("boundary_asset",
                      make_asset_metadata(std::string(asset_types::object),
                                          nlohmann::json::array({"boundary"})));
    library.add_asset("normal_asset", make_asset_metadata());

    nlohmann::json manifest = nlohmann::json::object({
        {"schema_version", manifest::kMapSchemaVersion},
        {"map_grid_settings", nlohmann::json::object({{"grid_resolution", 4}, {"position_jitter_px", 0}})},
        {"live_dynamic_spawns",
         nlohmann::json::object({
             {"boundary_area_selectors",
              nlohmann::json::array({
                  make_live_selector("spn-boundary", "boundary_asset", 4),
                  make_live_selector("spn-normal", "normal_asset", 4)
              })}
         })}
    });

    auto assets = make_assets(library, manifest, make_rect_area("Spawn", 32), make_room_data(true));
    const world::GridBounds render_bounds = world::GridBounds::from_xywh(-80, -80, 160, 160, 0, 4);
    assets->test_sync_live_dynamic_assets_for_bounds(render_bounds);

    CHECK(count_live_assets_named(*assets, "boundary_asset") > 0);
    CHECK(count_live_assets_named(*assets, "normal_asset") > 0);
    for (const Asset* asset : live_dynamic_assets(*assets)) {
        REQUIRE(asset != nullptr);
        CHECK(point_in_bounds(render_bounds, asset->world_x(), asset->world_z()));
    }
}

TEST_CASE("Live dynamic sync keeps stable assets while points remain in render bounds") {
    AssetLibrary library(false);
    library.add_asset("normal_asset", make_asset_metadata());

    nlohmann::json manifest = nlohmann::json::object({
        {"schema_version", manifest::kMapSchemaVersion},
        {"map_grid_settings", nlohmann::json::object({{"grid_resolution", 4}, {"position_jitter_px", 0}})},
        {"live_dynamic_spawns",
         nlohmann::json::object({
             {"boundary_area_selectors",
              nlohmann::json::array({make_live_selector("spn-stable", "normal_asset", 4)})}
         })}
    });

    auto assets = make_assets(library, manifest, make_rect_area("Spawn", 48), make_room_data(true));
    const world::GridBounds render_bounds = world::GridBounds::from_xywh(-48, -48, 96, 96, 0, 4);
    assets->test_sync_live_dynamic_assets_for_bounds(render_bounds);
    const auto first_assets = live_dynamic_assets(*assets);
    REQUIRE_FALSE(first_assets.empty());
    const auto first_positions = collect_live_positions_named(*assets, "normal_asset");

    assets->test_sync_live_dynamic_assets_for_bounds(render_bounds);
    const auto second_assets = live_dynamic_assets(*assets);
    CHECK(second_assets.size() == first_assets.size());
    for (Asset* asset : first_assets) {
        CHECK(std::find(second_assets.begin(), second_assets.end(), asset) != second_assets.end());
    }
    CHECK(collect_live_positions_named(*assets, "normal_asset") == first_positions);
}

TEST_CASE("Live dynamic sync deletes dynamic assets after they leave render bounds") {
    AssetLibrary library(false);
    library.add_asset("normal_asset", make_asset_metadata());

    nlohmann::json manifest = nlohmann::json::object({
        {"schema_version", manifest::kMapSchemaVersion},
        {"map_grid_settings", nlohmann::json::object({{"grid_resolution", 4}, {"position_jitter_px", 0}})},
        {"live_dynamic_spawns",
         nlohmann::json::object({
             {"boundary_area_selectors",
              nlohmann::json::array({make_live_selector("spn-delete", "normal_asset", 4)})}
         })}
    });

    auto assets = make_assets(library, manifest, make_rect_area("Spawn", 48), make_room_data(true));
    const world::GridBounds first_bounds = world::GridBounds::from_xywh(-48, -48, 96, 96, 0, 4);
    assets->test_sync_live_dynamic_assets_for_bounds(first_bounds);
    REQUIRE_FALSE(live_dynamic_assets(*assets).empty());

    const world::GridBounds far_bounds = world::GridBounds::from_xywh(1200, 1200, 96, 96, 0, 4);
    assets->test_sync_live_dynamic_assets_for_bounds(far_bounds);
    CHECK(live_dynamic_assets(*assets).empty());
}

TEST_CASE("Live dynamic sync respects grid occupancy and retries after occupancy clears") {
    AssetLibrary library(false);
    library.add_asset("normal_asset", make_asset_metadata());
    library.add_asset("blocking_asset", make_asset_metadata());

    nlohmann::json manifest = nlohmann::json::object({
        {"schema_version", manifest::kMapSchemaVersion},
        {"map_grid_settings", nlohmann::json::object({{"grid_resolution", 4}, {"position_jitter_px", 0}})},
        {"live_dynamic_spawns",
         nlohmann::json::object({
             {"boundary_area_selectors",
              nlohmann::json::array({make_live_selector("spn-occupied", "normal_asset", 4)})}
         })}
    });

    auto assets = make_assets(library, manifest, make_rect_area("Spawn", 16), make_room_data(true));
    const world::GridBounds render_bounds = world::GridBounds::from_xywh(0, 0, 1, 1, 0, 4);

    std::unique_ptr<Asset> blocker = assets->create_unattached_asset("blocking_asset", SDL_Point{0, 0});
    REQUIRE(blocker != nullptr);
    Asset* blocker_raw = assets->attach_asset(std::move(blocker), 0, 4);
    REQUIRE(blocker_raw != nullptr);

    assets->test_sync_live_dynamic_assets_for_bounds(render_bounds);
    CHECK(count_live_assets_named(*assets, "normal_asset") == 0);

    std::unique_ptr<Asset> extracted = assets->extract_asset(blocker_raw);
    REQUIRE(extracted != nullptr);
    assets->test_sync_live_dynamic_assets_for_bounds(render_bounds);
    CHECK(count_live_assets_named(*assets, "normal_asset") > 0);
}

TEST_CASE("Live dynamic null selections create no assets and no persistent blockers") {
    AssetLibrary library(false);
    library.add_asset("normal_asset", make_asset_metadata());

    nlohmann::json null_manifest = nlohmann::json::object({
        {"schema_version", manifest::kMapSchemaVersion},
        {"map_grid_settings", nlohmann::json::object({{"grid_resolution", 4}, {"position_jitter_px", 0}})},
        {"live_dynamic_spawns",
         nlohmann::json::object({
             {"boundary_area_selectors",
              nlohmann::json::array({make_null_live_selector("spn-null", 4)})}
         })}
    });

    auto null_assets = make_assets(library, null_manifest, make_rect_area("Spawn", 32), make_room_data(true));
    const world::GridBounds render_bounds = world::GridBounds::from_xywh(-32, -32, 64, 64, 0, 4);
    null_assets->test_sync_live_dynamic_assets_for_bounds(render_bounds);
    null_assets->test_sync_live_dynamic_assets_for_bounds(render_bounds);
    CHECK(live_dynamic_assets(*null_assets).empty());

    nlohmann::json normal_manifest = nlohmann::json::object({
        {"schema_version", manifest::kMapSchemaVersion},
        {"map_grid_settings", nlohmann::json::object({{"grid_resolution", 4}, {"position_jitter_px", 0}})},
        {"live_dynamic_spawns",
         nlohmann::json::object({
             {"boundary_area_selectors",
              nlohmann::json::array({make_live_selector("spn-normal-after-null", "normal_asset", 4)})}
         })}
    });

    auto normal_assets = make_assets(library, normal_manifest, make_rect_area("Spawn", 32), make_room_data(true));
    normal_assets->test_sync_live_dynamic_assets_for_bounds(render_bounds);
    CHECK(count_live_assets_named(*normal_assets, "normal_asset") > 0);
}

TEST_CASE("Live dynamic shared selector list renders trail candidates") {
    AssetLibrary library(false);
    library.add_asset("grass_asset",
                      make_asset_metadata(std::string(asset_types::object),
                                          nlohmann::json::array({"grass"})));

    nlohmann::json manifest = nlohmann::json::object({
        {"schema_version", manifest::kMapSchemaVersion},
        {"map_grid_settings", nlohmann::json::object({{"grid_resolution", 4}, {"position_jitter_px", 0}})},
        {"live_dynamic_spawns",
         nlohmann::json::object({
             {"boundary_area_selectors",
              nlohmann::json::array({make_live_selector("spn-trail", "grass_asset", 4)})}
         })}
    });

    Area room_area = make_rect_area("Spawn", 16);
    nlohmann::json room_data = make_room_data(true);
    std::vector<std::unique_ptr<Room>> owned_rooms;
    owned_rooms.push_back(make_runtime_room(library, room_area, room_data));
    add_trail_named_area(*owned_rooms.front(), "trail_patch", SDL_Point{80, 0}, 16);
    auto world_context = std::make_shared<RuntimeWorldContext>(std::move(owned_rooms));

    Assets assets(library,
                  nullptr,
                  world_context,
                  800,
                  600,
                  0,
                  0,
                  256,
                  nullptr,
                  nullptr,
                  "live_dynamic_trail_sync_test",
                  manifest,
                  std::string{},
                  world::WorldGrid{});

    const world::GridBounds render_bounds = world::GridBounds::from_xywh(64, -16, 32, 32, 0, 4);
    assets.test_sync_live_dynamic_assets_for_bounds(render_bounds);
    CHECK(count_live_assets_named(assets, "grass_asset") > 0);
    for (const Asset* asset : live_dynamic_assets(assets)) {
        REQUIRE(asset != nullptr);
        CHECK(point_in_bounds(render_bounds, asset->world_x(), asset->world_z()));
    }
}

TEST_CASE("Live dynamic spawned assets preserve initialization metadata") {
    AssetLibrary library(false);
    library.add_asset("normal_asset", make_asset_metadata(
        std::string(asset_types::object),
        nlohmann::json::array(),
        7,
        7,
        -12,
        -12));

    nlohmann::json manifest = nlohmann::json::object({
        {"schema_version", manifest::kMapSchemaVersion},
        {"map_grid_settings", nlohmann::json::object({{"grid_resolution", 4}, {"position_jitter_px", 0}})},
        {"live_dynamic_spawns",
         nlohmann::json::object({
             {"boundary_area_selectors",
              nlohmann::json::array({make_live_selector("spn-metadata", "normal_asset", 4)})}
         })}
    });

    auto assets = make_assets(library, manifest, make_rect_area("Spawn", 32), make_room_data(true));
    const world::GridBounds render_bounds = world::GridBounds::from_xywh(-32, -32, 64, 64, 0, 4);
    assets->test_sync_live_dynamic_assets_for_bounds(render_bounds);
    REQUIRE_FALSE(live_dynamic_assets(*assets).empty());

    const Asset* asset = live_dynamic_assets(*assets).front();
    REQUIRE(asset != nullptr);
    CHECK(asset->world_y() == 7);
    CHECK(asset->is_dynamic_spawned_asset());
    CHECK(asset->base_spawn_tilt_degrees() == doctest::Approx(-12.0));
}
