#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <string>

#include "assets/asset/Asset.hpp"
#include "gameplay/world/world_grid.hpp"

namespace {

std::unique_ptr<Asset> make_runtime_ownership_asset(int world_x, int world_z, int grid_resolution = 0) {
    auto info = std::make_shared<AssetInfo>("runtime_ownership_asset");
    Area spawn_area("runtime_ownership_area", 0);
    return std::make_unique<Asset>(info,
                                   spawn_area,
                                   SDL_Point{world_x, world_z},
                                   0,
                                   std::string{},
                                   std::string{},
                                   grid_resolution);
}

std::size_t count_asset_in_chunks(const world::WorldGrid& grid, const Asset* asset) {
    std::size_t count = 0;
    for (const world::Chunk* chunk : grid.all_chunks()) {
        if (!chunk) {
            continue;
        }
        count += static_cast<std::size_t>(std::count(chunk->assets.begin(), chunk->assets.end(), asset));
    }
    return count;
}

}  // namespace

TEST_CASE("WorldGrid attach and extract preserve unique asset ownership") {
    world::WorldGrid grid;
    Asset* raw = grid.create_asset_at_point(make_runtime_ownership_asset(96, 144));
    REQUIRE(raw != nullptr);
    REQUIRE(grid.point_for_asset(raw) != nullptr);
    const std::vector<Asset*> initially_listed = grid.all_assets();
    CHECK(std::count(initially_listed.begin(), initially_listed.end(), raw) == 1);
    CHECK(count_asset_in_chunks(grid, raw) == 1);

    std::unique_ptr<Asset> extracted = grid.extract_asset(raw);
    REQUIRE(extracted != nullptr);
    CHECK(extracted.get() == raw);
    CHECK(grid.point_for_asset(raw) == nullptr);
    CHECK(grid.all_assets().empty());
    CHECK(count_asset_in_chunks(grid, raw) == 0);

    Asset* reattached = grid.attach_asset(std::move(extracted));
    REQUIRE(reattached == raw);
    REQUIRE(grid.point_for_asset(raw) != nullptr);
    const std::vector<Asset*> reattached_listed = grid.all_assets();
    CHECK(std::count(reattached_listed.begin(), reattached_listed.end(), raw) == 1);
    CHECK(count_asset_in_chunks(grid, raw) == 1);
}

TEST_CASE("WorldGrid move_asset failure leaves destination state untouched when ownership is missing") {
    world::WorldGrid grid;
    Asset* raw = grid.create_asset_at_point(make_runtime_ownership_asset(140, 180));
    REQUIRE(raw != nullptr);

    const world::GridPoint* start = grid.point_for_asset(raw);
    REQUIRE(start != nullptr);
    const world::GridPoint old_pos =
        world::GridPoint::make_virtual(start->world_x(), start->world_y(), start->world_z(), start->resolution_layer());
    const world::GridPoint new_pos =
        world::GridPoint::make_virtual(start->world_x() + 512, start->world_y(), start->world_z() + 640, start->resolution_layer());
    const world::GridKey destination_key = grid.grid_key_from_world(new_pos, new_pos.resolution_layer());
    const std::size_t chunk_count_before = grid.all_chunks().size();

    std::unique_ptr<Asset> extracted = grid.extract_asset(raw);
    REQUIRE(extracted != nullptr);
    CHECK(grid.point_for_asset(raw) == nullptr);
    CHECK(grid.find_grid_point(destination_key) == nullptr);

    CHECK(grid.move_asset(raw, old_pos, new_pos) == nullptr);
    CHECK(grid.point_for_asset(raw) == nullptr);
    CHECK(grid.find_grid_point(destination_key) == nullptr);
    CHECK(grid.all_chunks().size() == chunk_count_before);
    CHECK(extracted.get() == raw);
}

TEST_CASE("WorldGrid remove_asset refuses detached raw pointers") {
    world::WorldGrid grid;
    Asset* raw = grid.create_asset_at_point(make_runtime_ownership_asset(32, 48));
    REQUIRE(raw != nullptr);

    std::unique_ptr<Asset> extracted = grid.extract_asset(raw);
    REQUIRE(extracted != nullptr);
    const std::size_t chunk_count_before = grid.all_chunks().size();

    CHECK(grid.remove_asset(raw) == nullptr);
    CHECK(extracted.get() == raw);
    CHECK(grid.all_assets().empty());
    CHECK(grid.all_chunks().size() == chunk_count_before);
}
