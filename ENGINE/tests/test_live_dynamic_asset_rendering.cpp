#include <doctest/doctest.h>

#include <algorithm>
#include <limits>
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
#include "rendering/render/render_object.hpp"
#include "rendering/render/render_object_builder.hpp"
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

nlohmann::json make_live_tag_selector(const std::string& spawn_id,
                                      const std::string& tag_name,
                                      int grid_resolution) {
    return nlohmann::json::object({
        {"spawn_id", spawn_id},
        {"display_name", spawn_id},
        {"execution_mode", "batch_grid"},
        {"grid_resolution", grid_resolution},
        {"candidates",
         nlohmann::json::array({
             nlohmann::json::object({{"name", std::string("#") + tag_name}, {"chance", 100}})
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
    library.add_asset("candidate_boundary",
                      make_asset_metadata(std::string(asset_types::boundary),
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

TEST_CASE("Live dynamic assets reconcile into render-only state without persistence") {
    AssetLibrary library(false);
    library.add_asset("boundary_asset",
                      make_asset_metadata(std::string(asset_types::boundary),
                                          nlohmann::json::array({"boundary"})));
    library.add_asset("normal_asset", make_asset_metadata());

    nlohmann::json manifest = nlohmann::json::object({
        {"schema_version", manifest::kMapSchemaVersion},
        {"map_grid_settings", nlohmann::json::object({{"grid_resolution", 4}})},
        {"live_dynamic_spawns",
         nlohmann::json::object({
             {"boundary_area_selectors",
              nlohmann::json::array({
                  make_live_selector("spn-boundary", "boundary_asset", 4),
                  make_live_selector("spn-normal", "normal_asset", 4)
              })}
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

TEST_CASE("Live dynamic shared selectors render tag candidates in trail areas") {
    AssetLibrary library(false);
    library.add_asset("grass_asset",
                      make_asset_metadata(std::string(asset_types::object),
                                          nlohmann::json::array({"grass"})));
    library.add_asset("boundary_asset",
                      make_asset_metadata(std::string(asset_types::boundary),
                                          nlohmann::json::array({"boundary"})));

    nlohmann::json manifest = nlohmann::json::object({
        {"schema_version", manifest::kMapSchemaVersion},
        {"map_grid_settings", nlohmann::json::object({{"grid_resolution", 4}, {"position_jitter_px", 0}})},
        {"live_dynamic_spawns",
         nlohmann::json::object({
             {"boundary_area_selectors",
              nlohmann::json::array({
                  make_live_tag_selector("spn-grass", "grass", 4),
                  make_live_selector("spn-boundary", "boundary_asset", 4)
              })}
         })}
    });

    Area room_area = make_rect_area("Spawn", 48);
    nlohmann::json room_data = make_room_data(true);
    std::vector<std::unique_ptr<Room>> owned_rooms;
    owned_rooms.push_back(make_runtime_room(library, room_area, room_data));
    Room* room = owned_rooms.front().get();
    REQUIRE(room != nullptr);
    add_trail_named_area(*room, "SpawnTrail", SDL_Point{256, 0}, 64);
    REQUIRE_FALSE(room->areas.empty());
    const Area* trail_area = room->areas.back().area.get();
    REQUIRE(trail_area != nullptr);

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
                  "live_dynamic_trail_inherit_test",
                  manifest,
                  std::string{},
                  world::WorldGrid{});

    const world::GridBounds visible = world::GridBounds::from_xywh(192, -64, 128, 128, 0, 4);
    assets.test_reconcile_live_dynamic_assets_for_bounds(visible);

    const auto grass_positions = collect_live_positions_named(assets, "grass_asset");
    const auto boundary_positions = collect_live_positions_named(assets, "boundary_asset");
    REQUIRE_FALSE(grass_positions.empty());
    CHECK(boundary_positions.empty());

    for (const auto& [x, z] : grass_positions) {
        CHECK(trail_area->contains_point(SDL_Point{x, z}));
    }
}

TEST_CASE("Live dynamic null selections reserve visible points without render assets") {
    AssetLibrary library(false);
    library.add_asset("normal_asset", make_asset_metadata());

    nlohmann::json manifest = nlohmann::json::object({
        {"schema_version", manifest::kMapSchemaVersion},
        {"map_grid_settings", nlohmann::json::object({{"grid_resolution", 4}})},
        {"live_dynamic_spawns",
         nlohmann::json::object({
             {"boundary_area_selectors", nlohmann::json::array({make_null_live_selector("spn-null", 4)})}
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
             {"boundary_area_selectors", nlohmann::json::array({make_live_selector("spn-height", "normal_asset", 4)})}
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
             {"boundary_area_selectors", nlohmann::json::array({selector})}
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

TEST_CASE("Live dynamic shared selector list renders boundary and room candidates with distributed positions") {
    AssetLibrary library(false);
    library.add_asset("boundary_asset",
                      make_asset_metadata(std::string(asset_types::boundary),
                                          nlohmann::json::array({"boundary"})));
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
             {"boundary_area_selectors", nlohmann::json::array({boundary_selector, inherited_selector})}
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

TEST_CASE("Live dynamic spawned assets apply normal tilt and sink render metadata") {
    AssetLibrary library(false);
    library.add_asset("tilted_sink_asset",
                      make_asset_metadata(std::string(asset_types::object),
                                          nlohmann::json::array(),
                                          -18,
                                          -18,
                                          13,
                                          13));

    nlohmann::json manifest = nlohmann::json::object({
        {"schema_version", manifest::kMapSchemaVersion},
        {"map_grid_settings", nlohmann::json::object({{"grid_resolution", 4}})},
        {"live_dynamic_spawns",
         nlohmann::json::object({
             {"boundary_area_selectors", nlohmann::json::array({make_live_selector("spn-tilt-sink", "tilted_sink_asset", 4)})}
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
                  "live_dynamic_tilt_sink_test",
                  manifest,
                  std::string{},
                  world::WorldGrid{});

    const world::GridBounds visible = world::GridBounds::from_xywh(-32, -32, 64, 64, 0, 4);
    assets.test_reconcile_live_dynamic_assets_for_bounds(visible);

    REQUIRE_FALSE(assets.getLiveDynamicRenderAssets().empty());
    const Asset* asset = assets.getLiveDynamicRenderAssets().front();
    REQUIRE(asset != nullptr);
    CHECK(asset->world_y() == -18);
    CHECK(asset->effective_render_angle() == doctest::Approx(13.0));

    render_build::DirectAssetRenderCacheRecord cache{};
    cache.texture = reinterpret_cast<SDL_Texture*>(0x1);
    cache.atlas_w = 32;
    cache.atlas_h = 32;
    cache.has_atlas_size = true;
    cache.frame_w = 32;
    cache.frame_h = 32;
    cache.has_texture_size = true;

    RenderObject object{};
    REQUIRE(render_build::build_direct_asset_render_object(const_cast<Asset*>(asset), cache, object));
    CHECK(object.sink_clip_enabled);
    CHECK(object.sink_height_offset_px == doctest::Approx(-18.0f));
    CHECK(object.angle == doctest::Approx(13.0));
}

TEST_CASE("Live dynamic boundary selectors do not starve inherited room selectors") {
    AssetLibrary library(false);
    library.add_asset("boundary_asset",
                      make_asset_metadata(std::string(asset_types::boundary),
                                          nlohmann::json::array({"boundary"})));
    library.add_asset("normal_asset", make_asset_metadata());

    nlohmann::json manifest = nlohmann::json::object({
        {"schema_version", manifest::kMapSchemaVersion},
        {"map_grid_settings", nlohmann::json::object({{"grid_resolution", 4}, {"position_jitter_px", 0}})},
        {"live_dynamic_spawns",
         nlohmann::json::object({
             {"boundary_area_selectors",
              nlohmann::json::array({
                  make_live_selector("spn-boundary-budget", "boundary_asset", 4),
                  make_live_selector("spn-normal-budget", "normal_asset", 4)
              })}
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
                  512,
                  nullptr,
                  "live_dynamic_budget_fairness_test",
                  manifest,
                  std::string{},
                  world::WorldGrid{});

    const world::GridBounds visible = world::GridBounds::from_xywh(-512, -512, 1024, 1024, 0, 4);
    assets.test_reconcile_live_dynamic_assets_for_bounds(visible);

    CHECK(count_live_assets_named(assets, "boundary_asset") > 0);
    CHECK(count_live_assets_named(assets, "normal_asset") > 0);
}


TEST_CASE("Live dynamic clamped selector sampling remains spatially distributed across wide windows") {
    AssetLibrary library(false);
    library.add_asset("normal_asset", make_asset_metadata());

    nlohmann::json manifest = nlohmann::json::object({
        {"schema_version", manifest::kMapSchemaVersion},
        {"map_grid_settings", nlohmann::json::object({{"grid_resolution", 4}, {"position_jitter_px", 0}})},
        {"live_dynamic_spawns",
         nlohmann::json::object({
             {"boundary_area_selectors", nlohmann::json::array({make_live_selector("spn-wide-clamped", "normal_asset", 4)})}
         })}
    });

    Area room_area = make_rect_area("Spawn", 256);
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
                  "live_dynamic_clamped_distribution_test",
                  manifest,
                  std::string{},
                  world::WorldGrid{});

    const world::GridBounds wide_visible = world::GridBounds::from_xywh(-400, -32, 800, 64, 0, 4);
    for (int i = 0; i < 16; ++i) {
        assets.test_reconcile_live_dynamic_assets_for_bounds(wide_visible);
    }

    const auto positions = collect_live_positions_named(assets, "normal_asset");
    REQUIRE(positions.size() > 32);

    int min_x = std::numeric_limits<int>::max();
    int max_x = std::numeric_limits<int>::min();
    int min_z = std::numeric_limits<int>::max();
    int max_z = std::numeric_limits<int>::min();
    for (const auto& [x, z] : positions) {
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_z = std::min(min_z, z);
        max_z = std::max(max_z, z);
    }

    CHECK((max_x - min_x) >= 480);
    CHECK(min_x >= -256);
    CHECK(max_x <= 256);
    CHECK((max_z - min_z) <= 96);
}

TEST_CASE("Live dynamic boundary selectors use map-local center instead of world origin") {
    AssetLibrary library(false);
    library.add_asset("boundary_asset",
                      make_asset_metadata(std::string(asset_types::boundary),
                                          nlohmann::json::array({"boundary"})));

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

TEST_CASE("Live dynamic retries occupied points after occupancy clears") {
    AssetLibrary library(false);
    library.add_asset("normal_asset", make_asset_metadata());
    library.add_asset("blocking_asset", make_asset_metadata());

    nlohmann::json manifest = nlohmann::json::object({
        {"schema_version", manifest::kMapSchemaVersion},
        {"map_grid_settings", nlohmann::json::object({{"grid_resolution", 4}, {"position_jitter_px", 0}})},
        {"live_dynamic_spawns",
         nlohmann::json::object({
             {"boundary_area_selectors", nlohmann::json::array({make_live_selector("spn-retry-occupied", "normal_asset", 4)})}
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
                  "live_dynamic_retry_occupied_test",
                  manifest,
                  std::string{},
                  world::WorldGrid{});

    const world::GridBounds visible = world::GridBounds::from_xywh(0, 0, 1, 1, 0, 4);

    std::unique_ptr<Asset> blocker = assets.create_unattached_asset("blocking_asset", SDL_Point{0, 0});
    REQUIRE(blocker != nullptr);
    Asset* blocker_raw = assets.attach_asset(std::move(blocker), 0, 4);
    REQUIRE(blocker_raw != nullptr);

    assets.test_reconcile_live_dynamic_assets_for_bounds(visible);
    const std::size_t occupied_state_count = assets.test_live_dynamic_state_count();
    CHECK(count_live_assets_named(assets, "normal_asset") == 0);

    std::unique_ptr<Asset> extracted = assets.extract_asset(blocker_raw);
    REQUIRE(extracted != nullptr);
    CHECK(extracted->dead == false);
    CHECK(assets.world_grid().all_assets().empty());

    assets.test_reconcile_live_dynamic_assets_for_bounds(visible);
    CHECK(count_live_assets_named(assets, "normal_asset") > 0);
    CHECK(assets.test_live_dynamic_state_count() >= occupied_state_count);
}
