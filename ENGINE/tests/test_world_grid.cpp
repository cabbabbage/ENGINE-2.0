#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "assets/asset/Asset.hpp"
#include "core/manifest/map_data.hpp"
#include "core/manifest/manifest_loader.hpp"
#include "devtools/core/manifest_store.hpp"
#include "gameplay/world/chunk.hpp"
#include "gameplay/world/grid_point.hpp"
#include "gameplay/world/world_grid.hpp"
#include "rendering/render/layer_effect_processor.hpp"
#include "rendering/render/warped_screen_grid.hpp"

namespace {

struct FakeManifestBackend {
    FakeManifestBackend()
        : data(nlohmann::json::object({
              {"version", 2},
              {"assets", nlohmann::json::object()},
              {"maps", nlohmann::json::object()}
          })) {}

    nlohmann::json data;
    bool flushed = false;
};

manifest::ManifestData make_manifest_data_from(const nlohmann::json& snapshot) {
    manifest::ManifestData manifest_data;
    manifest_data.raw = snapshot;
    manifest_data.assets = manifest_data.raw["assets"];
    manifest_data.maps = manifest_data.raw["maps"];
    return manifest_data;
}

std::unique_ptr<Asset> make_world_grid_test_asset(int world_x, int world_z, int grid_resolution = 0) {
    auto info = std::make_shared<AssetInfo>("world_grid_test_asset");
    Area spawn_area("world_grid_test_area", 0);
    return std::make_unique<Asset>(info,
                                   spawn_area,
                                   SDL_Point{world_x, world_z},
                                   0,
                                   std::string{},
                                   std::string{},
                                   grid_resolution);
}

Area make_warped_screen_test_view(const std::string& name, SDL_Point center, int w = 3200, int h = 2400) {
    const int half_w = w / 2;
    const int half_h = h / 2;
    std::vector<Area::Point> corners{
        Area::Point{center.x - half_w, center.y - half_h},
        Area::Point{center.x + half_w, center.y - half_h},
        Area::Point{center.x + half_w, center.y + half_h},
        Area::Point{center.x - half_w, center.y + half_h}
    };
    return Area(name, corners, 0);
}

bool traversal_contains_asset(const WarpedScreenGrid& camera_grid, const Asset* asset) {
    for (const WarpedScreenGrid::VisibleTraversalEntry& entry : camera_grid.visible_traversal_entries()) {
        if (entry.asset == asset) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("GridPoint projection round-trips via camera params") {
    world::WorldGrid grid;
    const axis::WorldPos original_pos{32, 5, 64};
    world::GridPoint original = world::GridPoint::make_virtual(original_pos, 1);

    world::CameraProjectionParams params;
    params.position_x = 0.0;
    params.position_y = 3.5;
    params.position_z = -10.0;
    params.forward_x = 0.0;
    params.forward_y = 0.0;
    params.forward_z = 1.0;
    params.right_x = 1.0;
    params.right_y = 0.0;
    params.right_z = 0.0;
    params.up_x = 0.0;
    params.up_y = 1.0;
    params.up_z = 0.0;
    params.screen_width = 1280;
    params.screen_height = 720;
    params.tan_half_fov_x = 0.7;
    params.tan_half_fov_y = 0.7;
    params.meters_scale = 1.0;
    params.screen_zoom = 1.0;
    params.anchor_world_x = 0.0;
    params.anchor_world_y = 0.0;
    params.anchor_world_z = 0.0;
    params.state_version = 1;

    original.project_to_screen(params);
    SDL_FPoint screen_pos = original.screen_position();
    REQUIRE(std::isfinite(screen_pos.x));
    REQUIRE(std::isfinite(screen_pos.y));

    world::GridPoint* round_trip = world::GridPoint::from_screen(screen_pos,
                                                                 static_cast<float>(original.world_y()),
                                                                 params,
                                                                 grid);
    REQUIRE(round_trip != nullptr);
    CHECK(round_trip->world_x() == original.world_x());
    CHECK(round_trip->world_y() == original.world_y());
    CHECK(round_trip->world_z() == original.world_z());
}

TEST_CASE("WorldGrid projection cache invalidates on topology updates") {
    world::WorldGrid grid;
    world::GridPoint& point = world::GridPoint::from_world(24, 0, 48, 0, grid);

    point.mutable_projection_cache().screen_data_valid = true;
    point.mutable_projection_cache().screen_data_frame_updated = 77;
    point.mutable_projection_cache().perspective_scale = 2.25f;

    grid.set_origin(world::GridPoint::make_virtual(100, 0, 200, 0));
    CHECK_FALSE(point.projection_cache().screen_data_valid);
    CHECK(point.perspective_scale() == doctest::Approx(2.25f));

    point.mutable_projection_cache().screen_data_valid = true;
    grid.set_grid_resolution(1);
    CHECK_FALSE(point.projection_cache().screen_data_valid);
}

TEST_CASE("WorldGrid keys stay stable across repeated requests") {
    world::WorldGrid grid;
    const axis::WorldPos sample_pos{15, 2, 31};
    world::GridPoint sample = world::GridPoint::make_virtual(sample_pos, 3);
    const world::GridKey first_key = grid.grid_key_from_world(sample);
    const world::GridKey second_key = grid.grid_key_from_world(sample);
    CHECK(first_key == second_key);
    CHECK(first_key.x == sample_pos.x);
    CHECK(first_key.y == sample_pos.y);
    CHECK(first_key.z == sample_pos.z);

    world::Chunk* chunk = grid.ensure_chunk_from_world(sample);
    REQUIRE(chunk != nullptr);
    world::GridPoint& stored = grid.find_or_create_grid_point(first_key, chunk, nullptr);
    CHECK(stored.hash_key() == grid.hash_key(first_key));
    CHECK(first_key.layer == stored.resolution_layer());
}


TEST_CASE("WorldGrid preserves canonical Z depth when creating points") {
    world::WorldGrid grid;

    const int world_x = 120;
    const int world_y = 0;   // floor height
    const int world_z = 480; // deep into the room
    const int layer = 0;

    world::GridPoint& point = world::GridPoint::from_world(world_x, world_y, world_z, layer, grid);
    CHECK(point.world_x() == world_x);
    CHECK(point.world_y() == world_y);
    CHECK(point.world_z() == world_z);

    const world::GridKey expected_key{world_x, world_y, world_z, layer};
    world::GridPoint* looked_up = grid.find_grid_point(expected_key);
    REQUIRE(looked_up != nullptr);
    CHECK(looked_up->world_x() == world_x);
    CHECK(looked_up->world_y() == world_y);
    CHECK(looked_up->world_z() == world_z);
}

TEST_CASE("WorldGrid keeps depth distinct from height for sibling points") {
    world::WorldGrid grid;

    const int world_x = 48;
    const int world_y = 5;     // elevated point
    const int near_z  = 96;    // closer to camera
    const int far_z   = 320;   // deeper into the scene
    const int layer   = 1;

    world::GridPoint& near_point = world::GridPoint::from_world(world_x, world_y, near_z, layer, grid);
    world::GridPoint& far_point  = world::GridPoint::from_world(world_x, world_y, far_z, layer, grid);

    CHECK(near_point.world_z() == near_z);
    CHECK(far_point.world_z() == far_z);
    CHECK(near_point.world_y() == world_y);
    CHECK(far_point.world_y() == world_y);
    CHECK(near_point.id != far_point.id);
    CHECK(near_point.hash_key() != far_point.hash_key());
}

TEST_CASE("WorldGrid move_asset preserves unique ownership and mapping") {
    world::WorldGrid grid;
    std::unique_ptr<Asset> owned = make_world_grid_test_asset(64, 96);
    Asset* raw = grid.create_asset_at_point(std::move(owned));
    REQUIRE(raw != nullptr);

    const world::GridPoint* start = grid.point_for_asset(raw);
    REQUIRE(start != nullptr);

    raw->clear_composite_dirty();
    raw->clear_mesh_dirty();
    CHECK_FALSE(raw->is_composite_dirty());
    CHECK_FALSE(raw->is_mesh_dirty());

    const world::GridPoint old_pos =
        world::GridPoint::make_virtual(start->world_x(), start->world_y(), start->world_z(), start->resolution_layer());
    const world::GridPoint new_pos =
        world::GridPoint::make_virtual(start->world_x() + 48, start->world_y(), start->world_z() + 72, start->resolution_layer());

    Asset* moved = grid.move_asset(raw, old_pos, new_pos);
    REQUIRE(moved == raw);

    const world::GridPoint* moved_point = grid.point_for_asset(raw);
    REQUIRE(moved_point != nullptr);
    CHECK(moved_point->world_x() == new_pos.world_x());
    CHECK(moved_point->world_y() == new_pos.world_y());
    CHECK(moved_point->world_z() == new_pos.world_z());
    CHECK(raw->is_composite_dirty());
    CHECK(raw->is_mesh_dirty());

    const std::vector<Asset*> listed = grid.all_assets();
    CHECK(std::count(listed.begin(), listed.end(), raw) == 1);

    std::unique_ptr<Asset> extracted = grid.extract_asset(raw);
    REQUIRE(extracted != nullptr);
    CHECK(extracted.get() == raw);
    CHECK(grid.point_for_asset(raw) == nullptr);
}

TEST_CASE("WorldGrid move_asset aborts when source ownership is missing") {
    world::WorldGrid grid;
    std::unique_ptr<Asset> owned = make_world_grid_test_asset(140, 180);
    Asset* raw = grid.create_asset_at_point(std::move(owned));
    REQUIRE(raw != nullptr);

    const world::GridPoint* start = grid.point_for_asset(raw);
    REQUIRE(start != nullptr);
    const world::GridPoint old_pos =
        world::GridPoint::make_virtual(start->world_x(), start->world_y(), start->world_z(), start->resolution_layer());
    const world::GridPoint new_pos =
        world::GridPoint::make_virtual(start->world_x() + 32, start->world_y(), start->world_z() + 64, start->resolution_layer());

    std::unique_ptr<Asset> extracted = grid.extract_asset(raw);
    REQUIRE(extracted != nullptr);
    CHECK(grid.point_for_asset(raw) == nullptr);

    CHECK(grid.move_asset(raw, old_pos, new_pos) == nullptr);
    CHECK(grid.point_for_asset(raw) == nullptr);

    Asset* reattached = grid.attach_asset(std::move(extracted), old_pos.world_z(), old_pos.resolution_layer());
    REQUIRE(reattached == raw);
    REQUIRE(grid.point_for_asset(raw) != nullptr);
}

TEST_CASE("WorldGrid horizontal transfer marks render invalidation every step") {
    world::WorldGrid grid;
    std::unique_ptr<Asset> owned = make_world_grid_test_asset(32, 64);
    Asset* raw = grid.create_asset_at_point(std::move(owned));
    REQUIRE(raw != nullptr);

    for (int step = 0; step < 6; ++step) {
        const world::GridPoint* start = grid.point_for_asset(raw);
        REQUIRE(start != nullptr);

        raw->clear_composite_dirty();
        raw->clear_mesh_dirty();
        CHECK_FALSE(raw->is_composite_dirty());
        CHECK_FALSE(raw->is_mesh_dirty());

        const world::GridPoint old_pos =
            world::GridPoint::make_virtual(start->world_x(), start->world_y(), start->world_z(), start->resolution_layer());
        const world::GridPoint new_pos =
            world::GridPoint::make_virtual(start->world_x() + 24, start->world_y(), start->world_z(), start->resolution_layer());

        Asset* moved = grid.move_asset(raw, old_pos, new_pos);
        REQUIRE(moved == raw);

        const world::GridPoint* moved_point = grid.point_for_asset(raw);
        REQUIRE(moved_point != nullptr);
        CHECK(moved_point->world_x() == new_pos.world_x());
        CHECK(moved_point->world_z() == new_pos.world_z());
        CHECK(raw->is_composite_dirty());
        CHECK(raw->is_mesh_dirty());
    }
}

TEST_CASE("ManifestStore map entry round-trip honors writes") {
    FakeManifestBackend storage;
    auto loader = [&storage]() {
        return make_manifest_data_from(storage.data);
    };

    auto submit = [&storage](const std::filesystem::path&, const nlohmann::json& json, int) {
        storage.data = json;
    };

    auto flush = [&storage]() {
        storage.flushed = true;
    };

    devmode::core::ManifestStore store("manifest.json", loader, submit, flush, 2);
    const nlohmann::json map_entry = {
        {"schema_version", manifest::kMapSchemaVersion},
        {"rooms_data", nlohmann::json::object()}
    };

    REQUIRE(store.update_map_entry("map_alpha", map_entry));
    CHECK(storage.data["maps"]["map_alpha"] == map_entry);

    store.flush();
    CHECK(storage.flushed);

    store.reload();
    const nlohmann::json* persisted = store.find_map_entry("map_alpha");
    REQUIRE(persisted != nullptr);
    CHECK(*persisted == map_entry);
}

TEST_CASE("WarpedScreenGrid traversal keeps deeper world-z points without depth-window culling") {
    world::WorldGrid grid;
    Asset* near_asset = grid.create_asset_at_point(make_world_grid_test_asset(0, 40));
    Asset* far_asset = grid.create_asset_at_point(make_world_grid_test_asset(0, 180));
    REQUIRE(near_asset != nullptr);
    REQUIRE(far_asset != nullptr);

    WarpedScreenGrid camera_grid(1280, 720, make_warped_screen_test_view("camera_view", SDL_Point{0, 0}));
    camera_grid.rebuild_grid(grid, 0.016f, 1);

    CHECK(camera_grid.grid_point_for_asset(near_asset) != nullptr);
    CHECK(camera_grid.grid_point_for_asset(far_asset) != nullptr);
}

TEST_CASE("WarpedScreenGrid max_cull_depth filters far depth traversal entries") {
    world::WorldGrid grid;
    Asset* near_asset = grid.create_asset_at_point(make_world_grid_test_asset(0, 40));
    Asset* far_asset = grid.create_asset_at_point(make_world_grid_test_asset(0, 180));
    REQUIRE(near_asset != nullptr);
    REQUIRE(far_asset != nullptr);

    WarpedScreenGrid camera_grid(1280, 720, make_warped_screen_test_view("camera_view", SDL_Point{0, 0}));
    auto settings = camera_grid.get_settings();
    settings.max_cull_depth = 80.0f;
    camera_grid.set_realism_settings(settings);
    camera_grid.rebuild_grid(grid, 0.016f, 1);

    CHECK(camera_grid.grid_point_for_asset(near_asset) != nullptr);
    CHECK(camera_grid.grid_point_for_asset(far_asset) == nullptr);
    CHECK(camera_grid.last_depth_culled() > 0);
}

TEST_CASE("WarpedScreenGrid directional overscan expands left right and bottom only") {
    world::WorldGrid grid;
    WarpedScreenGrid camera_grid(1280, 720, make_warped_screen_test_view("camera_view", SDL_Point{0, 0}));
    camera_grid.rebuild_grid(grid, 0.016f, 1);
    const WarpedScreenGrid::GridBounds rebuilt_bounds = camera_grid.get_bounds();

    CHECK(rebuilt_bounds.left == doctest::Approx(-128.0f).epsilon(1e-5));
    CHECK(rebuilt_bounds.right == doctest::Approx(1408.0f).epsilon(1e-5));
    CHECK(rebuilt_bounds.top == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(rebuilt_bounds.bottom == doctest::Approx(828.0f).epsilon(1e-5));
}

TEST_CASE("WarpedScreenGrid camera-derived query area updates with camera movement") {
    world::WorldGrid grid;
    WarpedScreenGrid camera_grid(1280, 720, make_warped_screen_test_view("camera_view", SDL_Point{0, 0}));

    camera_grid.rebuild_grid(grid, 0.016f, 1);
    const Area initial_view = camera_grid.get_current_view();

    camera_grid.set_screen_center(SDL_Point{700, 450});
    camera_grid.rebuild_grid(grid, 0.016f, 2);
    const Area moved_view = camera_grid.get_current_view();

    auto [initial_min_x, initial_min_z, initial_max_x, initial_max_z] = initial_view.get_bounds();
    auto [moved_min_x, moved_min_z, moved_max_x, moved_max_z] = moved_view.get_bounds();
    CHECK((moved_min_x != initial_min_x || moved_max_x != initial_max_x ||
           moved_min_z != initial_min_z || moved_max_z != initial_max_z));
}

TEST_CASE("WarpedScreenGrid min on-screen culling uses largest projected dimension") {
    world::WorldGrid grid;
    Asset* tiny_asset = grid.create_asset_at_point(make_world_grid_test_asset(0, 80));
    Asset* wide_asset = grid.create_asset_at_point(make_world_grid_test_asset(0, 80));
    REQUIRE(tiny_asset != nullptr);
    REQUIRE(wide_asset != nullptr);

    tiny_asset->info->original_canvas_width = 1;
    tiny_asset->info->original_canvas_height = 1;
    wide_asset->info->original_canvas_width = 256;
    wide_asset->info->original_canvas_height = 1;

    WarpedScreenGrid camera_grid(1280, 720, make_warped_screen_test_view("camera_view", SDL_Point{0, 0}));
    WarpedScreenGrid::RealismSettings settings = camera_grid.get_settings();
    settings.min_visible_screen_ratio = 0.03f;
    camera_grid.set_realism_settings(settings);
    camera_grid.rebuild_grid(grid, 0.016f, 1);

    CHECK_FALSE(traversal_contains_asset(camera_grid, tiny_asset));
    CHECK(traversal_contains_asset(camera_grid, wide_asset));
}

TEST_CASE("WarpedScreenGrid min on-screen threshold applies immediately after settings change") {
    world::WorldGrid grid;
    Asset* tiny_asset = grid.create_asset_at_point(make_world_grid_test_asset(0, 80));
    REQUIRE(tiny_asset != nullptr);
    tiny_asset->info->original_canvas_width = 1;
    tiny_asset->info->original_canvas_height = 1;

    WarpedScreenGrid camera_grid(1280, 720, make_warped_screen_test_view("camera_view", SDL_Point{0, 0}));
    WarpedScreenGrid::RealismSettings settings = camera_grid.get_settings();
    settings.min_visible_screen_ratio = 0.0f;
    camera_grid.set_realism_settings(settings);
    camera_grid.rebuild_grid(grid, 0.016f, 1);
    CHECK(traversal_contains_asset(camera_grid, tiny_asset));

    settings.min_visible_screen_ratio = 0.05f;
    camera_grid.set_realism_settings(settings);
    camera_grid.rebuild_grid(grid, 0.016f, 2);
    CHECK_FALSE(traversal_contains_asset(camera_grid, tiny_asset));
}

TEST_CASE("WarpedScreenGrid min on-screen culling exempts tracked player asset") {
    world::WorldGrid grid;
    Asset* player_asset = grid.create_asset_at_point(make_world_grid_test_asset(0, 80));
    Asset* npc_asset = grid.create_asset_at_point(make_world_grid_test_asset(0, 80));
    REQUIRE(player_asset != nullptr);
    REQUIRE(npc_asset != nullptr);

    player_asset->info->original_canvas_width = 1;
    player_asset->info->original_canvas_height = 1;
    npc_asset->info->original_canvas_width = 1;
    npc_asset->info->original_canvas_height = 1;

    WarpedScreenGrid camera_grid(1280, 720, make_warped_screen_test_view("camera_view", SDL_Point{0, 0}));
    WarpedScreenGrid::RealismSettings settings = camera_grid.get_settings();
    settings.min_visible_screen_ratio = 0.05f;
    camera_grid.set_realism_settings(settings);
    camera_grid.update_camera_height(nullptr, nullptr, player_asset, true, 0.016f, false);
    camera_grid.rebuild_grid(grid, 0.016f, 1);

    CHECK(traversal_contains_asset(camera_grid, player_asset));
    CHECK_FALSE(traversal_contains_asset(camera_grid, npc_asset));
}

TEST_CASE("WarpedScreenGrid camera_settings_to_json omits removed legacy keys") {
    WarpedScreenGrid camera_grid(1280, 720, make_warped_screen_test_view("camera_view", SDL_Point{0, 0}));
    const nlohmann::json serialized = camera_grid.camera_settings_to_json();

    CHECK_FALSE(serialized.contains("render_quality_percent"));
    CHECK_FALSE(serialized.contains("meters_per_100_world_px"));
    CHECK_FALSE(serialized.contains("offscreen_fade_amount_px"));
    CHECK_FALSE(serialized.contains("near_camera_max_perspective_scale"));
    CHECK_FALSE(serialized.contains("extra_cull_margin"));
    CHECK_FALSE(serialized.contains("depth_near_world"));
    CHECK_FALSE(serialized.contains("depth_far_world"));
}

TEST_CASE("WarpedScreenGrid apply_camera_settings ignores removed legacy keys") {
    WarpedScreenGrid camera_grid(1280, 720, make_warped_screen_test_view("camera_view", SDL_Point{0, 0}));
    const WarpedScreenGrid::RealismSettings before = camera_grid.get_settings();

    camera_grid.apply_camera_settings(nlohmann::json{
        {"render_quality_percent", 10},
        {"meters_per_100_world_px", 0.5},
        {"offscreen_fade_amount_px", 0.0},
        {"near_camera_max_perspective_scale", 6.0},
        {"extra_cull_margin", 250.0},
        {"depth_near_world", 10.0},
        {"depth_far_world", 100.0}
    });

    const WarpedScreenGrid::RealismSettings after = camera_grid.get_settings();
    CHECK(after.min_visible_screen_ratio == doctest::Approx(before.min_visible_screen_ratio));
    CHECK(after.base_height_px == doctest::Approx(before.base_height_px));
    CHECK(after.max_cull_depth == doctest::Approx(before.max_cull_depth));
    CHECK(after.layer_depth_interval == doctest::Approx(before.layer_depth_interval));
    CHECK(after.layer_depth_curve == doctest::Approx(before.layer_depth_curve));
}

TEST_CASE("WarpedScreenGrid camera settings roundtrip includes aperture and layer controls") {
    WarpedScreenGrid camera_grid(1280, 720, make_warped_screen_test_view("camera_view", SDL_Point{0, 0}));
    camera_grid.apply_camera_settings(nlohmann::json{
        {"max_cull_depth", 2500.0},
        {"layer_depth_interval", 180.0},
        {"layer_depth_curve", 1.75},
        {"aperture_f_stop", 4.0},
        {"focal_length_mm", 35.0},
        {"max_blur_px", 20.0}
    });
    const WarpedScreenGrid::RealismSettings settings = camera_grid.get_settings();
    CHECK(settings.max_cull_depth == doctest::Approx(2500.0f));
    CHECK(settings.layer_depth_interval == doctest::Approx(180.0f));
    CHECK(settings.layer_depth_curve == doctest::Approx(1.75f));
    CHECK(settings.aperture_f_stop == doctest::Approx(4.0f));
    CHECK(settings.focal_length_mm == doctest::Approx(35.0f));
    CHECK(settings.max_blur_px == doctest::Approx(20.0f));

    const nlohmann::json serialized = camera_grid.camera_settings_to_json();
    CHECK(serialized["max_cull_depth"] == doctest::Approx(2500.0));
    CHECK(serialized["layer_depth_interval"] == doctest::Approx(180.0));
    CHECK(serialized["layer_depth_curve"] == doctest::Approx(1.75));
    CHECK_FALSE(serialized.contains("focus_depth"));
}
