#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "assets/asset/Asset.hpp"

namespace {

std::unique_ptr<Asset> make_asset_for_collision_policy(const std::shared_ptr<AssetInfo>& info) {
    Area spawn_area("collision_policy_test_area", 0);
    return std::make_unique<Asset>(info,
                                   spawn_area,
                                   SDL_Point{0, 0},
                                   0,
                                   std::string{},
                                   std::string{},
                                   0);
}

std::shared_ptr<AssetInfo> make_info(const std::string& name) {
    auto info = std::make_shared<AssetInfo>(name);
    info->type = "object";
    info->passable = true;
    info->movement_enabled = false;
    info->floor_boxes_enabled = false;
    return info;
}

} // namespace

TEST_CASE("affects_collision_context excludes player and passable static objects") {
    auto player_info = make_info("collision_policy_player");
    player_info->type = "player";
    const auto player = make_asset_for_collision_policy(player_info);
    REQUIRE(player != nullptr);
    CHECK_FALSE(player->affects_collision_context());

    auto decorative_info = make_info("collision_policy_decorative");
    decorative_info->type = "decorative";
    decorative_info->passable = true;
    decorative_info->movement_enabled = false;
    const auto decorative = make_asset_for_collision_policy(decorative_info);
    REQUIRE(decorative != nullptr);
    CHECK_FALSE(decorative->affects_collision_context());
}

TEST_CASE("affects_collision_context includes explicit blocking types and solid blockers") {
    auto boundary_info = make_info("collision_policy_boundary");
    boundary_info->type = "boundary";
    const auto boundary = make_asset_for_collision_policy(boundary_info);
    REQUIRE(boundary != nullptr);
    CHECK(boundary->affects_collision_context());

    auto mover_info = make_info("collision_policy_mover");
    mover_info->type = "object";
    mover_info->movement_enabled = true;
    mover_info->passable = true;
    const auto mover = make_asset_for_collision_policy(mover_info);
    REQUIRE(mover != nullptr);
    CHECK_FALSE(mover->affects_collision_context());

    auto solid_info = make_info("collision_policy_solid");
    solid_info->type = "object";
    solid_info->passable = false;
    const auto solid = make_asset_for_collision_policy(solid_info);
    REQUIRE(solid != nullptr);
    CHECK(solid->affects_collision_context());
}

TEST_CASE("affects_collision_context includes only boundary-tagged floor boxes") {
    auto boundary_floor_info = make_info("collision_policy_floor_boundary");
    boundary_floor_info->type = "object";
    boundary_floor_info->floor_boxes_enabled = true;
    boundary_floor_info->floor_boxes = {
        AssetInfo::FloorBox{
            "floor_box_boundary",
            "floor_box_boundary",
            0.0f,
            0.0f,
            64.0f,
            64.0f,
            true,
            std::vector<std::string>{"boundary"}
        }
    };
    const auto boundary_floor_asset = make_asset_for_collision_policy(boundary_floor_info);
    REQUIRE(boundary_floor_asset != nullptr);
    CHECK(boundary_floor_asset->affects_collision_context());

    auto non_boundary_floor_info = make_info("collision_policy_floor_non_boundary");
    non_boundary_floor_info->type = "object";
    non_boundary_floor_info->floor_boxes_enabled = true;
    non_boundary_floor_info->floor_boxes = {
        AssetInfo::FloorBox{
            "floor_box_non_boundary",
            "floor_box_non_boundary",
            0.0f,
            0.0f,
            64.0f,
            64.0f,
            true,
            std::vector<std::string>{"walkable_hint"}
        }
    };
    const auto non_boundary_floor_asset = make_asset_for_collision_policy(non_boundary_floor_info);
    REQUIRE(non_boundary_floor_asset != nullptr);
    CHECK_FALSE(non_boundary_floor_asset->affects_collision_context());
}
