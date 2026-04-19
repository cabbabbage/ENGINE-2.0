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
    info->impassable_box_enabled = false;
    return info;
}

animation_update::FrameHitBox make_impassable_box(const std::string& id,
                                                   const std::string& name,
                                                   bool enabled = true) {
    animation_update::FrameHitBox box{};
    box.id = id;
    box.name = name;
    box.type = "impassable_box";
    box.enabled = enabled;
    box.set_rect(animation_update::FrameBoxRect{0, 0, 64, 64});
    return box;
}

}  // namespace

TEST_CASE("affects_collision_context excludes players even with authored impassable boxes") {
    auto player_info = make_info("collision_policy_player");
    player_info->type = "player";
    player_info->impassable_box_enabled = true;
    player_info->impassable_boxes = {make_impassable_box("player_box", "Player Box")};
    const auto player = make_asset_for_collision_policy(player_info);
    REQUIRE(player != nullptr);
    CHECK_FALSE(player->affects_collision_context());
}

TEST_CASE("affects_collision_context depends only on impassable box system data") {
    auto disabled_info = make_info("collision_policy_impassable_disabled");
    disabled_info->impassable_box_enabled = false;
    disabled_info->impassable_boxes = {make_impassable_box("disabled_box", "Disabled Box")};
    const auto disabled_asset = make_asset_for_collision_policy(disabled_info);
    REQUIRE(disabled_asset != nullptr);
    CHECK_FALSE(disabled_asset->affects_collision_context());

    auto empty_info = make_info("collision_policy_impassable_empty");
    empty_info->impassable_box_enabled = true;
    empty_info->impassable_boxes.clear();
    const auto empty_asset = make_asset_for_collision_policy(empty_info);
    REQUIRE(empty_asset != nullptr);
    CHECK_FALSE(empty_asset->affects_collision_context());

    auto valid_info = make_info("collision_policy_impassable_valid");
    valid_info->impassable_box_enabled = true;
    valid_info->impassable_boxes = {make_impassable_box("imp_box_1", "Impassable Box 1")};
    const auto valid_asset = make_asset_for_collision_policy(valid_info);
    REQUIRE(valid_asset != nullptr);
    CHECK(valid_asset->affects_collision_context());
}

TEST_CASE("affects_collision_context ignores boundary type and floor-box boundary tags") {
    auto boundary_info = make_info("collision_policy_boundary");
    boundary_info->type = "boundary";
    const auto boundary_asset = make_asset_for_collision_policy(boundary_info);
    REQUIRE(boundary_asset != nullptr);
    CHECK_FALSE(boundary_asset->affects_collision_context());

    auto floor_boundary_info = make_info("collision_policy_floor_boundary");
    floor_boundary_info->floor_boxes_enabled = true;
    floor_boundary_info->floor_boxes = {
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
    const auto floor_boundary_asset = make_asset_for_collision_policy(floor_boundary_info);
    REQUIRE(floor_boundary_asset != nullptr);
    CHECK_FALSE(floor_boundary_asset->affects_collision_context());
}
