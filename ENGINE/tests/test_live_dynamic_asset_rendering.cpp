#include <doctest/doctest.h>

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
                                   int y_pos_max = 0) {
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
        {"y_pos_max", y_pos_max}
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

int count_live_assets_named(const Assets& assets, const std::string& name) {
    int count = 0;
    for (const Asset* asset : assets.getLiveDynamicRenderAssets()) {
        if (asset && asset->info && asset->info->name == name) {
            ++count;
        }
    }
    return count;
}

std::set<std::pair<int, int>> collect_live_positions_named(const Assets& assets, const std::string& name) {
    std::set<std::pair<int, int>> out;
    for (const Asset* asset : assets.getLiveDynamicRenderAssets()) {
        if (!asset || !asset->info || asset->info->name != name) {
            continue;
        }
        out.emplace(asset->world_x(), asset->world_z());
    }
    return out;
}

}  // namespace

TEST_CASE("GenerateRooms does not materialize live dynamic config into room assets") {
    AssetLibrary library(false);
    library.add_asset("candidate_boundary", make_asset_metadata(std::string(asset_types::boundary)));

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

TEST_CASE("Live dynamic assets reconcile into render-only state without persistence") {
    AssetLibrary library(false);
    library.add_asset("boundary_asset", make_asset_metadata(std::string(asset_types::boundary)));
    library.add_asset("normal_asset", make_asset_metadata());

    nlohmann::json manifest = nlohmann::json::object({
        {"schema_version", manifest::kMapSchemaVersion},
        {"map_grid_settings", nlohmann::json::object({{"grid_resolution", 4}})},
        {"live_dynamic_spawns",
         nlohmann::json::object({
             {"boundary_area_selectors", nlohmann::json::array({make_live_selector("spn-boundary", "boundary_asset", 4)})},
             {"inherited_map_selectors", nlohmann::json::array({make_live_selector("spn-normal", "normal_asset", 4)})}
         })}
    });

    Area room_area = make_rect_area("Spawn", 32);
    nlohmann::json room_data = make_room_data(true);
    std::vector<std::unique_ptr<Room>> owned_rooms;
    owned_rooms.push_back(make_runtime_room(library, room_area, room_data));
    Room* room = owned_rooms.front().get();
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
                  "live_dynamic_test",
                  manifest,
                  std::string{},
                  world::WorldGrid{});

    const world::GridBounds visible = world::GridBounds::from_xywh(-64, -64, 128, 128, 0, 4);
    assets.test_reconcile_live_dynamic_assets_for_bounds(visible);

    CHECK(count_live_assets_named(assets, "boundary_asset") > 0);
    CHECK(count_live_assets_named(assets, "normal_asset") > 0);
    CHECK(assets.all.empty());
    CHECK(assets.world_grid().all_assets().empty());
    REQUIRE(room != nullptr);
    CHECK(room->assets.empty());

    const std::size_t state_count = assets.test_live_dynamic_state_count();
    const std::size_t render_count = assets.getLiveDynamicRenderAssets().size();
    assets.test_reconcile_live_dynamic_assets_for_bounds(visible);
    CHECK(assets.test_live_dynamic_state_count() == state_count);
    CHECK(assets.getLiveDynamicRenderAssets().size() == render_count);

    const world::GridBounds outside = world::GridBounds::from_xywh(512, 512, 64, 64, 0, 4);
    assets.test_reconcile_live_dynamic_assets_for_bounds(outside);
    CHECK(assets.test_live_dynamic_state_count() == 0);
    CHECK(assets.getLiveDynamicRenderAssets().empty());
}

TEST_CASE("Live dynamic null selections reserve visible points without render assets") {
    AssetLibrary library(false);
    library.add_asset("normal_asset", make_asset_metadata());

    nlohmann::json manifest = nlohmann::json::object({
        {"schema_version", manifest::kMapSchemaVersion},
        {"map_grid_settings", nlohmann::json::object({{"grid_resolution", 4}})},
        {"live_dynamic_spawns",
         nlohmann::json::object({
             {"inherited_map_selectors", nlohmann::json::array({make_null_live_selector("spn-null", 4)})}
         })}
    });

    Area room_area = make_rect_area("Spawn", 48);
    nlohmann::json room_data = make_room_data(true);
    std::vector<std::unique_ptr<Room>> owned_rooms;
    owned_rooms.push_back(make_runtime_room(library, room_area, room_data));
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
                  "live_dynamic_null_test",
                  manifest,
                  std::string{},
                  world::WorldGrid{});

    const world::GridBounds visible = world::GridBounds::from_xywh(-32, -32, 64, 64, 0, 4);
    assets.test_reconcile_live_dynamic_assets_for_bounds(visible);

    CHECK(assets.test_live_dynamic_state_count() > 0);
    CHECK(assets.getLiveDynamicRenderAssets().empty());
    CHECK(assets.all.empty());
    CHECK(assets.world_grid().all_assets().empty());
}

TEST_CASE("Live dynamic spawned assets preserve initialized world height and get runtime perspective override") {
    AssetLibrary library(false);
    library.add_asset("normal_asset", make_asset_metadata(std::string(asset_types::object),
                                                           nlohmann::json::array(),
                                                           12,
                                                           12));

    nlohmann::json manifest = nlohmann::json::object({
        {"schema_version", manifest::kMapSchemaVersion},
        {"map_grid_settings", nlohmann::json::object({{"grid_resolution", 4}})},
        {"live_dynamic_spawns",
         nlohmann::json::object({
             {"inherited_map_selectors", nlohmann::json::array({make_live_selector("spn-height", "normal_asset", 4)})}
         })}
    });

    Area room_area = make_rect_area("Spawn", 48);
    nlohmann::json room_data = make_room_data(true);
    std::vector<std::unique_ptr<Room>> owned_rooms;
    owned_rooms.push_back(make_runtime_room(library, room_area, room_data));
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
                  "live_dynamic_height_test",
                  manifest,
                  std::string{},
                  world::WorldGrid{});

    const world::GridBounds visible = world::GridBounds::from_xywh(-32, -32, 64, 64, 0, 4);
    assets.test_reconcile_live_dynamic_assets_for_bounds(visible);

    REQUIRE_FALSE(assets.getLiveDynamicRenderAssets().empty());
    for (const Asset* asset : assets.getLiveDynamicRenderAssets()) {
        REQUIRE(asset != nullptr);
        CHECK(asset->world_y() == 12);
        CHECK(asset->has_anchor_perspective_override());
    }
}

TEST_CASE("Live dynamic jittered coordinates are deterministic across reconciles") {
    AssetLibrary library(false);
    library.add_asset("normal_asset", make_asset_metadata());

    auto selector = make_live_selector("spn-jitter", "normal_asset", 4);
    selector["jitter"] = 40;
    nlohmann::json manifest = nlohmann::json::object({
        {"schema_version", manifest::kMapSchemaVersion},
        {"map_grid_settings", nlohmann::json::object({{"grid_resolution", 4}, {"position_jitter_px", 0}})},
        {"live_dynamic_spawns",
         nlohmann::json::object({
             {"inherited_map_selectors", nlohmann::json::array({selector})}
         })}
    });

    Area room_area = make_rect_area("Spawn", 64);
    nlohmann::json room_data = make_room_data(true);
    std::vector<std::unique_ptr<Room>> owned_rooms;
    owned_rooms.push_back(make_runtime_room(library, room_area, room_data));
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
                  "live_dynamic_jitter_test",
                  manifest,
                  std::string{},
                  world::WorldGrid{});

    const world::GridBounds visible = world::GridBounds::from_xywh(-64, -64, 128, 128, 0, 4);
    assets.test_reconcile_live_dynamic_assets_for_bounds(visible);
    const auto first_positions = collect_live_positions_named(assets, "normal_asset");
    const std::size_t first_state_count = assets.test_live_dynamic_state_count();
    REQUIRE_FALSE(first_positions.empty());

    assets.test_reconcile_live_dynamic_assets_for_bounds(visible);
    const auto second_positions = collect_live_positions_named(assets, "normal_asset");
    CHECK(second_positions == first_positions);
    CHECK(assets.test_live_dynamic_state_count() == first_state_count);
}

TEST_CASE("Live dynamic boundary and inherited selectors both render with distributed positions") {
    AssetLibrary library(false);
    library.add_asset("boundary_asset", make_asset_metadata(std::string(asset_types::boundary)));
    library.add_asset("normal_asset", make_asset_metadata());

    auto boundary_selector = make_live_selector("spn-boundary-distribution", "boundary_asset", 4);
    boundary_selector["jitter"] = 24;
    auto inherited_selector = make_live_selector("spn-normal-distribution", "normal_asset", 4);
    inherited_selector["jitter"] = 24;

    nlohmann::json manifest = nlohmann::json::object({
        {"schema_version", manifest::kMapSchemaVersion},
        {"map_grid_settings", nlohmann::json::object({{"grid_resolution", 4}, {"position_jitter_px", 0}})},
        {"live_dynamic_spawns",
         nlohmann::json::object({
             {"boundary_area_selectors", nlohmann::json::array({boundary_selector})},
             {"inherited_map_selectors", nlohmann::json::array({inherited_selector})}
         })}
    });

    Area room_area = make_rect_area("Spawn", 48);
    nlohmann::json room_data = make_room_data(true);
    std::vector<std::unique_ptr<Room>> owned_rooms;
    owned_rooms.push_back(make_runtime_room(library, room_area, room_data));
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
                  "live_dynamic_distribution_test",
                  manifest,
                  std::string{},
                  world::WorldGrid{});

    const world::GridBounds visible = world::GridBounds::from_xywh(-96, -96, 192, 192, 0, 4);
    assets.test_reconcile_live_dynamic_assets_for_bounds(visible);

    const auto boundary_positions = collect_live_positions_named(assets, "boundary_asset");
    const auto normal_positions = collect_live_positions_named(assets, "normal_asset");
    CHECK(boundary_positions.size() > 1);
    CHECK(normal_positions.size() > 1);
}

TEST_CASE("Live dynamic boundary selectors use map-local center instead of world origin") {
    AssetLibrary library(false);
    library.add_asset("boundary_asset", make_asset_metadata(std::string(asset_types::boundary)));

    nlohmann::json manifest = nlohmann::json::object({
        {"schema_version", manifest::kMapSchemaVersion},
        {"map_grid_settings", nlohmann::json::object({{"grid_resolution", 4}, {"position_jitter_px", 0}})},
        {"live_dynamic_spawns",
         nlohmann::json::object({
             {"boundary_area_selectors", nlohmann::json::array({make_live_selector("spn-boundary-offset", "boundary_asset", 4)})}
         })}
    });

    Area room_area = make_rect_area("Spawn", 48);
    nlohmann::json room_data = make_room_data(false);
    std::vector<std::unique_ptr<Room>> owned_rooms;
    owned_rooms.push_back(make_runtime_room(library, room_area, room_data));
    auto world_context = std::make_shared<RuntimeWorldContext>(std::move(owned_rooms));

    Assets assets(library,
                  nullptr,
                  world_context,
                  800,
                  600,
                  6000,
                  6000,
                  256,
                  nullptr,
                  "live_dynamic_boundary_offset_test",
                  manifest,
                  std::string{},
                  world::WorldGrid{});

    const world::GridBounds visible_near_center = world::GridBounds::from_xywh(5960, 5960, 80, 80, 0, 4);
    assets.test_reconcile_live_dynamic_assets_for_bounds(visible_near_center);
    CHECK(count_live_assets_named(assets, "boundary_asset") > 0);

    const world::GridBounds origin_window = world::GridBounds::from_xywh(-32, -32, 64, 64, 0, 4);
    assets.test_reconcile_live_dynamic_assets_for_bounds(origin_window);
    CHECK(count_live_assets_named(assets, "boundary_asset") == 0);
}
