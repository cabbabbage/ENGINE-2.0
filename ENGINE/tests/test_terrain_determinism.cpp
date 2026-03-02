#include <doctest/doctest.h>

#include "rendering/render/terrain_field.hpp"
#include "rendering/render/terrain_runtime_state.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "gameplay/world/world_grid.hpp"
#include "gameplay/world/grid_point.hpp"
#include "assets/Asset.hpp"
#include "assets/asset/asset_info.hpp"
#include "utils/utils/area.hpp"

#include <vector>
#include <string>
#include <utility>

namespace {

TerrainRuntimeState make_state(TerrainSettings settings, const std::string& map_id) {
    settings.clamp();
    return TerrainRuntimeState::from_settings(settings, map_id, false);
}

Area make_rect(const std::string& name, int x0, int y0, int x1, int y1, int resolution = 2) {
    return Area{name, std::vector<SDL_Point>{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}, resolution};
}

// Friend hook (enabled in terrain_field.hpp) so tests can seed regions without full Room objects.
struct TerrainFieldTestHook {
    static void add_region(TerrainField& field, const Area& area, bool is_trail) {
        const Room* dummy_owner = reinterpret_cast<const Room*>(0x1);
        field.add_indexed_area(&area, dummy_owner, is_trail);
    }
};

} // namespace

TEST_SUITE("Terrain deterministic validation") {

TEST_CASE("TerrainRuntimeState keeps deterministic seed when randomize is false") {
    TerrainSettings settings{};
    settings.light.base_seed = 0xABC123u;
    settings.light.direction_world = SDL_FPoint{0.25f, -1.0f};

    const std::string map_id = "map_alpha";
    auto state1 = make_state(settings, map_id);
    auto state2 = make_state(settings, map_id);

    CHECK(state1.session_seed == state2.session_seed);
    CHECK(state1.light_direction_world.x == doctest::Approx(state2.light_direction_world.x));
    CHECK(state1.light_direction_world.y == doctest::Approx(state2.light_direction_world.y));

    TerrainSettings mutated = settings;
    mutated.light.base_seed += 7;
    auto mutated_state = make_state(mutated, map_id);

    CHECK(mutated_state.session_seed != state1.session_seed);
    CHECK(mutated_state.revision > state2.revision);
}

TEST_CASE("TerrainField flattens rooms/trails and blends edges smoothly") {
    TerrainSettings settings{};
    settings.enabled = true;
    settings.max_elevation_world = 120.0f;
    settings.edge_falloff_distance_world = 8.0f;
    settings.blend_strength = 0.25f;

    const auto runtime = make_state(settings, "terrain_flatten");
    std::vector<Room*> rooms;
    world::WorldGrid grid;

    Area room_area = make_rect("room", 0, 0, 20, 20);
    room_area.set_type("room");
    Area trail_area = make_rect("trail", 30, 0, 50, 8);
    trail_area.set_type("trail");

    TerrainField with_regions;
    TerrainField no_regions;

    // Seed caches for both samplers.
    with_regions.begin_frame(1, runtime, rooms);
    no_regions.begin_frame(1, runtime, rooms);
    TerrainFieldTestHook::add_region(with_regions, room_area, false);
    TerrainFieldTestHook::add_region(with_regions, trail_area, true);

    const world::GridKey inside_room{10, 10, 0, 0};
    const world::GridKey inside_trail{35, 4, 0, 0};
    CHECK(with_regions.sample_elevation(inside_room, grid, rooms, runtime, 1) == doctest::Approx(0.0f));
    CHECK(with_regions.sample_elevation(inside_trail, grid, rooms, runtime, 1) == doctest::Approx(0.0f));

    // Edge blend: point just outside the room should be faded relative to the base noise.
    with_regions.begin_frame(2, runtime, rooms);
    no_regions.begin_frame(2, runtime, rooms);
    TerrainFieldTestHook::add_region(with_regions, room_area, false);
    const world::GridKey near_room_edge{21, 10, 0, 0};
    const float height_with_region = with_regions.sample_elevation(near_room_edge, grid, rooms, runtime, 2);
    const float base_height = no_regions.sample_elevation(near_room_edge, grid, rooms, runtime, 2);
    CHECK(height_with_region >= 0.0f);
    CHECK(base_height >= 0.0f);
    if (base_height > 1e-4f) {
        CHECK(height_with_region < base_height);
    }

    // Seam closure: same corner sampled twice in one frame should hit the cache, not grow it.
    with_regions.begin_frame(3, runtime, rooms);
    TerrainFieldTestHook::add_region(with_regions, room_area, false);
    const world::GridKey seam_key{5, 5, 0, 0};
    const float first = with_regions.sample_elevation(seam_key, grid, rooms, runtime, 3);
    const std::size_t cache_after_first = with_regions.frame_cache_size();
    const float second = with_regions.sample_elevation(seam_key, grid, rooms, runtime, 3);
    CHECK(first == doctest::Approx(second));
    CHECK(with_regions.frame_cache_size() == cache_after_first);
}

TEST_CASE("GridPoint caches survive revision reuse across WarpedScreenGrid rebuilds") {
    TerrainSettings settings{};
    settings.enabled = true;
    settings.max_elevation_world = 90.0f;
    settings.edge_falloff_distance_world = 6.0f;
    auto runtime = make_state(settings, "gridpoint-cache");

    Area view = make_rect("view", -64, -64, 64, 64);
    WarpedScreenGrid cam(128, 128, view);
    TerrainField field;
    world::WorldGrid grid;
    std::vector<Room*> rooms;

    auto info = std::make_shared<AssetInfo>("stub_asset");
    info->original_canvas_width = 16;
    info->original_canvas_height = 16;
    Area spawn = make_rect("spawn", 0, 0, 1, 1);
    auto asset = std::make_unique<Asset>(info, spawn, SDL_Point{0, 0}, /*depth*/ 0);

    const int resolved_layer = grid.default_resolution_layer();
    world::GridPoint& gp = world::GridPoint::from_world(0, 0, 0, resolved_layer, grid);
    gp.assets_here().push_back(std::move(asset));

    cam.rebuild_grid(grid, 0.0f, 1, &field, &runtime, &rooms);
    const auto cached_revision = gp.terrain_revision;
    const float cached_slope_x = gp.terrain_slope_x;
    const float cached_slope_y = gp.terrain_slope_y;
    CHECK(cached_revision == runtime.revision);
    CHECK(field.frame_cache_size() > 0);

    // Same revision on the next frame should reuse cached slopes and avoid resampling (cache stays empty).
    cam.rebuild_grid(grid, 0.0f, 2, &field, &runtime, &rooms);
    CHECK(gp.terrain_revision == cached_revision);
    CHECK(gp.terrain_slope_x == doctest::Approx(cached_slope_x));
    CHECK(gp.terrain_slope_y == doctest::Approx(cached_slope_y));
    CHECK(field.frame_cache_size() == 0);

    // Disable terrain: slopes and elevation reset and revision bumps.
    TerrainSettings disabled = settings;
    disabled.enabled = false;
    auto disabled_state = make_state(disabled, "gridpoint-cache");
    cam.rebuild_grid(grid, 0.0f, 3, &field, &disabled_state, &rooms);
    CHECK(gp.terrain_revision == disabled_state.revision);
    CHECK(gp.terrain_elevation == doctest::Approx(0.0f));
    CHECK(gp.terrain_slope_x == doctest::Approx(0.0f));
    CHECK(gp.terrain_slope_y == doctest::Approx(0.0f));

    // Revision bump forces projection invalidation even if frame stamp is stable.
    gp.screen_data_valid = true;
    gp.screen_data_frame_updated = 10;
    gp.last_camera_state_version_ = cam.projection_params().state_version;
    CHECK_FALSE(gp.needs_projection_update(10, gp.last_camera_state_version_, disabled_state.revision));
    CHECK(gp.needs_projection_update(10, gp.last_camera_state_version_, disabled_state.revision + 1));
}

TEST_CASE("GridPoint floor flag propagates through value semantics and reset") {
    using world::GridPoint;

    auto make_virtual = [](int z) {
        return GridPoint::make_virtual(1, 2, z, 0);
    };

    GridPoint floor = make_virtual(0);
    floor.is_floor = true;

    GridPoint copied{floor};
    CHECK(copied.is_floor);

    GridPoint moved = make_virtual(5);
    moved = std::move(copied);
    CHECK(moved.is_floor);
    CHECK_FALSE(copied.is_floor);

    GridPoint other = make_virtual(7);
    other.is_floor = false;
    using std::swap;
    swap(moved, other);
    CHECK_FALSE(moved.is_floor);
    CHECK(other.is_floor);

    moved.is_floor = true;
    moved.reset_frame_state();
    CHECK_FALSE(moved.is_floor);
}

TEST_CASE("Terrain application marks ground points as floor and clears vertical variants") {
    TerrainSettings settings{};
    settings.enabled = true;
    settings.max_elevation_world = 60.0f;
    auto runtime = make_state(settings, "floor-flag");

    Area view = make_rect("view", -32, -32, 32, 32);
    WarpedScreenGrid cam(128, 128, view);
    TerrainField field;
    world::WorldGrid grid;
    std::vector<Room*> rooms;

    auto info = std::make_shared<AssetInfo>("stub_asset");
    info->original_canvas_width = 8;
    info->original_canvas_height = 8;
    Area spawn = make_rect("spawn", 0, 0, 1, 1);

    auto asset_floor = std::make_unique<Asset>(info, spawn, SDL_Point{0, 0}, 0);
    auto asset_upper = std::make_unique<Asset>(info, spawn, SDL_Point{0, 0}, 0);

    const int layer = grid.default_resolution_layer();
    world::GridPoint& gp_floor = world::GridPoint::from_world(0, 0, 0, layer, grid);
    world::GridPoint& gp_upper = world::GridPoint::from_world(0, 0, 5, layer, grid);

    gp_floor.is_floor = false;
    gp_upper.is_floor = true; // Intentionally wrong to verify it gets cleared.

    gp_floor.assets_here().push_back(std::move(asset_floor));
    gp_upper.assets_here().push_back(std::move(asset_upper));

    cam.rebuild_grid(grid, 0.0f, 1, &field, &runtime, &rooms);

    CHECK(gp_floor.is_floor);
    CHECK_FALSE(gp_upper.is_floor);
}

} // TEST_SUITE
