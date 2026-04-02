#include <doctest/doctest.h>
#include "assets/asset/Asset.hpp"

#include "animation/attack_validation.hpp"
#include "animation/controllers/custom_controllers/davey_controller.hpp"
#include "animation/controllers/shared/attack_detection_helper.hpp"
#include "animation/controllers/shared/attack_processing_helper.hpp"
#include "assets/asset/animation_frame.hpp"
#include "stubs/asset/child_asset_runtime_test_support.hpp"

namespace {

using animation_update::custom_controllers::AttackDetectionHelper;
using animation_update::custom_controllers::AttackProcessingHelper;

struct AssetsScope {
    AssetsScope()
        : assets(test_child_asset_runtime::create_assets_stub()) {}
    ~AssetsScope() {
        test_child_asset_runtime::destroy_assets_stub(assets);
    }
    Assets* assets = nullptr;
};

Asset::RuntimeBoxVolume make_box(float center_x,
                                 float center_y,
                                 float center_z,
                                 float half_extent,
                                 int damage_amount,
                                 std::string type = "attack_box") {
    Asset::RuntimeBoxVolume out{};
    out.valid = true;
    out.type = std::move(type);
    out.damage_amount = damage_amount;
    out.centroid = Asset::RuntimeBoxPoint3{center_x, center_y, center_z};
    out.world_points = {{
        {center_x - half_extent, center_y - half_extent, center_z - half_extent},
        {center_x + half_extent, center_y - half_extent, center_z - half_extent},
        {center_x + half_extent, center_y + half_extent, center_z - half_extent},
        {center_x - half_extent, center_y + half_extent, center_z - half_extent},
        {center_x - half_extent, center_y - half_extent, center_z + half_extent},
        {center_x + half_extent, center_y - half_extent, center_z + half_extent},
        {center_x + half_extent, center_y + half_extent, center_z + half_extent},
        {center_x - half_extent, center_y + half_extent, center_z + half_extent},
    }};
    return out;
}

} // namespace

TEST_CASE("attack dispatch enqueues attacker metadata, damage, and attack type") {
    AssetsScope scope;
    auto attacker = test_child_asset_runtime::make_test_asset("attacker", 0, 0, 0, 0);
    auto target = test_child_asset_runtime::make_test_asset("target", 0, 0, 0, 0);
    Asset* attacker_ptr = test_child_asset_runtime::attach_owned_asset(scope.assets, std::move(attacker));
    Asset* target_ptr = test_child_asset_runtime::attach_owned_asset(scope.assets, std::move(target));
    REQUIRE(attacker_ptr != nullptr);
    REQUIRE(target_ptr != nullptr);

    attacker_ptr->spawn_id = "attacker_spawn";
    target_ptr->spawn_id = "target_spawn";
    AnimationFrame attacker_frame{};
    AnimationFrame target_frame{};
    attacker_frame.frame_index = 7;
    target_frame.frame_index = 3;
    attacker_ptr->current_frame = &attacker_frame;
    target_ptr->current_frame = &target_frame;

    attacker_ptr->test_set_current_attack_box_volumes(
        {make_box(0.0f, 0.0f, 0.0f, 2.0f, 12, "heavy_slash")});
    target_ptr->test_set_current_hit_box_volumes(
        {make_box(0.5f, 0.0f, 0.0f, 1.5f, 0, "hit")});

    AttackDetectionHelper::send_attack_if_hit(attacker_ptr, target_ptr);

    const auto queued = target_ptr->process_pending_attacks();
    REQUIRE(queued.size() == 1);
    CHECK(queued[0].attacker_asset_id == "attacker_spawn");
    CHECK(queued[0].attacker_asset_name == "attacker");
    CHECK(queued[0].target_asset_id == "target_spawn");
    CHECK(queued[0].target_asset_name == "target");
    CHECK(queued[0].damage_amount == 12);
    CHECK(queued[0].attack_type == "heavy_slash");
}

TEST_CASE("attack dispatch skips non-overlapping attack and hit volumes") {
    AssetsScope scope;
    auto attacker = test_child_asset_runtime::make_test_asset("attacker", 0, 0, 0, 0);
    auto target = test_child_asset_runtime::make_test_asset("target", 0, 0, 0, 0);
    Asset* attacker_ptr = test_child_asset_runtime::attach_owned_asset(scope.assets, std::move(attacker));
    Asset* target_ptr = test_child_asset_runtime::attach_owned_asset(scope.assets, std::move(target));
    REQUIRE(attacker_ptr != nullptr);
    REQUIRE(target_ptr != nullptr);

    AnimationFrame attacker_frame{};
    AnimationFrame target_frame{};
    attacker_ptr->current_frame = &attacker_frame;
    target_ptr->current_frame = &target_frame;

    attacker_ptr->test_set_current_attack_box_volumes({make_box(0.0f, 0.0f, 0.0f, 1.0f, 6)});
    target_ptr->test_set_current_hit_box_volumes(
        {make_box(10.0f, 0.0f, 0.0f, 1.0f, 0, "hit")});

    AttackDetectionHelper::send_attack_if_hit(attacker_ptr, target_ptr);
    CHECK(target_ptr->process_pending_attacks().empty());
}

TEST_CASE("attack type falls back to attack_box when source box type is empty") {
    AssetsScope scope;
    auto attacker = test_child_asset_runtime::make_test_asset("attacker", 0, 0, 0, 0);
    auto target = test_child_asset_runtime::make_test_asset("target", 0, 0, 0, 0);
    Asset* attacker_ptr = test_child_asset_runtime::attach_owned_asset(scope.assets, std::move(attacker));
    Asset* target_ptr = test_child_asset_runtime::attach_owned_asset(scope.assets, std::move(target));
    REQUIRE(attacker_ptr != nullptr);
    REQUIRE(target_ptr != nullptr);

    AnimationFrame attacker_frame{};
    AnimationFrame target_frame{};
    attacker_ptr->current_frame = &attacker_frame;
    target_ptr->current_frame = &target_frame;

    attacker_ptr->test_set_current_attack_box_volumes(
        {make_box(0.0f, 0.0f, 0.0f, 2.0f, 9, "")});
    target_ptr->test_set_current_hit_box_volumes(
        {make_box(0.0f, 0.0f, 0.0f, 1.0f, 0, "hit")});

    const auto attack = animation_update::AttackValidation::compute_attack_if_hit(*attacker_ptr, *target_ptr);
    REQUIRE(attack.has_value());
    CHECK(attack->attack_type == "attack_box");
}

TEST_CASE("default pending attack processor applies cumulative damage and deletes below zero") {
    auto self = test_child_asset_runtime::make_test_asset("target", 0, 0, 0, 0);
    Asset* self_ptr = self.get();
    REQUIRE(self_ptr != nullptr);

    self_ptr->runtime_health = 5;
    animation_update::Attack attack_a{};
    attack_a.damage_amount = 3;
    animation_update::Attack attack_b{};
    attack_b.damage_amount = 4;
    self_ptr->send_attack(attack_a);
    self_ptr->send_attack(attack_b);

    AttackProcessingHelper::process_pending_attacks(*self_ptr);
    CHECK(self_ptr->runtime_health == -2);
    CHECK(self_ptr->dead);
}

TEST_CASE("child assets cannot attack their parent assets") {
    AssetsScope scope;
    auto parent = test_child_asset_runtime::make_test_asset("parent", 0, 0, 0, 0);
    auto child = test_child_asset_runtime::make_test_asset("child", 0, 0, 0, 0);
    Asset* parent_ptr = test_child_asset_runtime::attach_owned_asset(scope.assets, std::move(parent));
    Asset* child_ptr = test_child_asset_runtime::attach_owned_asset(scope.assets, std::move(child));
    REQUIRE(parent_ptr != nullptr);
    REQUIRE(child_ptr != nullptr);

    parent_ptr->add_child(child_ptr);

    AnimationFrame parent_frame{};
    AnimationFrame child_frame{};
    parent_ptr->current_frame = &parent_frame;
    child_ptr->current_frame = &child_frame;
    child_ptr->test_set_current_attack_box_volumes({make_box(0.0f, 0.0f, 0.0f, 2.0f, 5)});
    parent_ptr->test_set_current_hit_box_volumes(
        {make_box(0.0f, 0.0f, 0.0f, 1.0f, 0, "hit")});

    AttackDetectionHelper::send_attack_if_hit(child_ptr, parent_ptr);
    CHECK(parent_ptr->process_pending_attacks().empty());
}

TEST_CASE("controllers delegating to base pending processing receive damage") {
    auto target = test_child_asset_runtime::make_test_asset("davey", 0, 0, 0, 0);
    Asset* target_ptr = target.get();
    REQUIRE(target_ptr != nullptr);
    target_ptr->runtime_health = 10;

    animation_update::Attack incoming{};
    incoming.damage_amount = 6;
    target_ptr->send_attack(incoming);

    davey_controller controller(target_ptr);
    controller.process_pending_attacks(*target_ptr);

    CHECK(target_ptr->runtime_health == 4);
    CHECK_FALSE(target_ptr->dead);
}
