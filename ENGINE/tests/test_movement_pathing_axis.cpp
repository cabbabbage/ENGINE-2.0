#include <doctest/doctest.h>

#include <memory>
#include <string>

#include "animation/collision_query_context.hpp"
#include "animation/movement_target_utils.hpp"
#include "assets/asset/Asset.hpp"
#include "utils/range_util.hpp"

namespace {

std::unique_ptr<Asset> make_test_asset(int world_x, int world_y, int world_z, int grid_resolution = 0) {
    auto info = std::make_shared<AssetInfo>("movement_pathing_axis_test_asset");
    Area spawn_area("movement_pathing_axis_test_area", 0);
    auto asset = std::make_unique<Asset>(info,
                                         spawn_area,
                                         SDL_Point{world_x, world_z},
                                         0,
                                         std::string{},
                                         std::string{},
                                         grid_resolution);
    asset->move_to_world_position(world_x, world_y, world_z, grid_resolution);
    return asset;
}

} // namespace

TEST_CASE("Movement target helper uses world X/Z checkpoint for target assets") {
    const auto target = make_test_asset(120, 7, 480, 2);
    const SDL_Point checkpoint = animation_update::movement_targets::world_checkpoint(*target);

    CHECK(checkpoint.x == 120);
    CHECK(checkpoint.y == 480);
}

TEST_CASE("Movement target helper computes X/Z delta from self to target checkpoint") {
    const auto self = make_test_asset(35, 22, 140, 1);
    const auto target = make_test_asset(120, 7, 480, 1);
    const SDL_Point checkpoint = animation_update::movement_targets::world_checkpoint(*target);
    const SDL_Point delta = animation_update::movement_targets::world_delta_to_checkpoint(*self, checkpoint);

    CHECK(delta.x == 85);
    CHECK(delta.y == 340);
    CHECK_FALSE((delta.x == checkpoint.x && delta.y == checkpoint.y));
}

TEST_CASE("Range radius checks use pixel radius on the X/Z map plane") {
    const SDL_Point origin{0, 0};
    CHECK(Range::is_in_range(origin, SDL_Point{96, 0}, 96));
    CHECK_FALSE(Range::is_in_range(origin, SDL_Point{97, 0}, 96));

    CHECK(Range::distance_sq(origin, SDL_Point{96, 0}) == 96LL * 96LL);
    CHECK(Range::distance_sq(origin, SDL_Point{0, -96}) == 96LL * 96LL);
}

TEST_CASE("Asset world X/Z plane keeps depth separate from world height") {
    const auto asset = make_test_asset(42, 11, 333, 1);

    CHECK(asset->world_xy_point().x == 42);
    CHECK(asset->world_xy_point().y == 11);
    CHECK(asset->world_xz_point().x == 42);
    CHECK(asset->world_xz_point().y == 333);
}

TEST_CASE("Collision query context stays reusable for repeated lookup calls") {
    const auto asset = make_test_asset(42, 11, 333, 1);
    CollisionQueryContext context;

    const auto first = context.collisions_for(*asset);
    const auto second = context.collisions_for(*asset);

    CHECK(first.size() == second.size());
}


TEST_CASE("Collision query radius expands with checkpoint extent and respects caps") {
    const int expanded = CollisionQueryContext::resolve_search_radius(64, 220, 320);
    CHECK(expanded == 252);

    const int capped = CollisionQueryContext::resolve_search_radius(64, 1000, 320);
    CHECK(capped == 320);

    const int neighbor_only = CollisionQueryContext::resolve_search_radius(96, 0, 320);
    CHECK(neighbor_only == 96);
}
