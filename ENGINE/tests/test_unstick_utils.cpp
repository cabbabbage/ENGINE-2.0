#include <doctest/doctest.h>

#include <memory>
#include <vector>

#include "animation/unstick_utils.hpp"
#include "animation/animation_update.hpp"
#include "assets/asset/Asset.hpp"
#include "utils/area.hpp"

namespace {

std::unique_ptr<Asset> make_actor(int x, int z) {
    auto info = std::make_shared<AssetInfo>("unstick_actor");
    info->movement_enabled = true;
    Area spawn("unstick_spawn", 0);
    auto asset = std::make_unique<Asset>(info, spawn, SDL_Point{x, z}, 0, std::string{}, std::string{}, 0);
    asset->move_to_world_position(x, 0, z, 0);
    return asset;
}

Assets::FrameCollisionEntry make_rect_entry(const Asset& owner, int min_x, int min_z, int max_x, int max_z) {
    Assets::FrameCollisionEntry entry;
    entry.asset = &owner;
    entry.area = Area("impassable", std::vector<Area::Point>{
        SDL_Point{min_x, min_z}, SDL_Point{max_x, min_z}, SDL_Point{max_x, max_z}, SDL_Point{min_x, max_z}}, 0);
    entry.world_center = entry.area.get_center();
    entry.bottom_middle = world::GridPoint::make_virtual(entry.world_center.x, owner.world_y(), entry.world_center.y, 0);
    return entry;
}

} // namespace

TEST_CASE("unstick utility pushes trapped actor outside impassable immediately") {
    auto actor = make_actor(5, 0);
    REQUIRE(actor != nullptr);

    auto blocker_info = std::make_shared<AssetInfo>("unstick_blocker");
    Area blocker_spawn("blocker_spawn", 0);
    Asset blocker(blocker_info, blocker_spawn, SDL_Point{0, 0}, 0, std::string{}, std::string{}, 0);

    const Assets::FrameCollisionEntry entry = make_rect_entry(blocker, 0, -2, 10, 2);
    std::vector<const Assets::FrameCollisionEntry*> entries{&entry};

    const world::GridPoint start = world::GridPoint::make_virtual(actor->world_x(), actor->world_y(), actor->world_z(), actor->grid_resolution);
    world::GridPoint destination = start;

    const bool resolved = animation::unstick::resolve_destination(*actor, nullptr, entries, start, destination, 12);
    CHECK(resolved);

    const world::GridPoint bottom = animation_update::detail::bottom_middle_for(*actor, destination);
    CHECK_FALSE(entry.area.contains_point(bottom.to_sdl_point()));
}
