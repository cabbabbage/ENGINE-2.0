#include <doctest/doctest.h>
#include <initializer_list>
#include <cmath>
#include "assets/asset/Asset.hpp"

#include "animation/attack_validation.hpp"
#include "animation/animation_update.hpp"
#include "animation/animation_runtime.hpp"
#include "animation/controllers/custom_controllers/davey_controller.hpp"
#include "animation/controllers/custom_controllers/spider_controller.hpp"
#include "animation/controllers/shared/attack_detection_helper.hpp"
#include "animation/controllers/shared/attack_processing_helper.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/animation_frame.hpp"
#include "utils/input.hpp"
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
    out.id = type + "_id";
    out.payload_id = out.id;
    out.valid = true;
    out.type = std::move(type);
    out.damage_amount = damage_amount;
    out.meta_json = "{}";
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

Animation make_single_frame_animation(std::initializer_list<std::string> tags) {
    Animation animation{};
    auto& path = animation.movement_path(0);
    path.clear();
    path.push_back(AnimationFrame{});
    path.front().is_first = true;
    path.front().is_last = true;
    animation.tags.assign(tags.begin(), tags.end());
    return animation;
}

Animation make_single_frame_attack_animation(std::initializer_list<std::string> tags,
                                             int box_x,
                                             int box_y,
                                             int box_w,
                                             int box_h) {
    Animation animation{};
    auto& path = animation.movement_path(0);
    path.clear();
    path.push_back(AnimationFrame{});
    path.front().is_first = true;
    path.front().is_last = true;

    animation_update::FrameAttackBox box{};
    box.id = "attack_box_id";
    box.name = "attack_box";
    box.type = "attack_box";
    box.enabled = true;
    box.payload.damage_amount = 5;
    box.payload.payload_id = "attack_box_id";
    box.set_position_and_size(box_x, box_y, box_w, box_h);
    path.front().set_attack_boxes({box});

    animation.tags.assign(tags.begin(), tags.end());
    return animation;
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
    CHECK(queued[0].attack_payload_id == "heavy_slash_id");
    CHECK(queued[0].payload.damage_amount == 12);
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

TEST_CASE("death processing falls back to break-tagged animation when die is missing") {
    auto self = test_child_asset_runtime::make_test_asset("target", 0, 0, 0, 0);
    Asset* self_ptr = self.get();
    REQUIRE(self_ptr != nullptr);
    REQUIRE(self_ptr->info != nullptr);

    self_ptr->info->animations["default"] = make_single_frame_animation({});
    self_ptr->info->animations["shatter_a"] = make_single_frame_animation({"break"});
    self_ptr->info->animations["shatter_b"] = make_single_frame_animation({"break", "heavy"});
    self_ptr->info->start_animation = "default";

    AnimationRuntime runtime(self_ptr, nullptr);
    REQUIRE(self_ptr->anim_ != nullptr);
    runtime.set_planner(self_ptr->anim_.get());

    self_ptr->runtime_health = 0;
    animation_update::Attack incoming{};
    incoming.damage_amount = 1;
    self_ptr->send_attack(incoming);

    AttackProcessingHelper::process_pending_attacks(*self_ptr);

    CHECK_FALSE(self_ptr->dead);
    CHECK((self_ptr->current_animation == "shatter_a" || self_ptr->current_animation == "shatter_b"));
}

TEST_CASE("death processing prefers die animation over break-tagged fallback") {
    auto self = test_child_asset_runtime::make_test_asset("target", 0, 0, 0, 0);
    Asset* self_ptr = self.get();
    REQUIRE(self_ptr != nullptr);
    REQUIRE(self_ptr->info != nullptr);

    self_ptr->info->animations["default"] = make_single_frame_animation({});
    self_ptr->info->animations["die"] = make_single_frame_animation({});
    self_ptr->info->animations["shatter_a"] = make_single_frame_animation({"break"});
    self_ptr->info->start_animation = "default";

    AnimationRuntime runtime(self_ptr, nullptr);
    REQUIRE(self_ptr->anim_ != nullptr);
    runtime.set_planner(self_ptr->anim_.get());

    self_ptr->runtime_health = 0;
    animation_update::Attack incoming{};
    incoming.damage_amount = 1;
    self_ptr->send_attack(incoming);

    AttackProcessingHelper::process_pending_attacks(*self_ptr);

    CHECK_FALSE(self_ptr->dead);
    CHECK(self_ptr->current_animation == "die");
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

TEST_CASE("knockback uses payload hitback distance when configured") {
    auto self = test_child_asset_runtime::make_test_asset("target", 30, 0, 0, 0);
    Asset* self_ptr = self.get();
    REQUIRE(self_ptr != nullptr);

    animation_update::Attack incoming{};
    incoming.damage_amount = 1;
    incoming.hit_x = 0.0f;
    incoming.hit_z = 0.0f;
    incoming.payload.hitback_enabled = true;
    incoming.payload.hitback_distance = 30.0f;

    SDL_Point delta{};
    REQUIRE(AttackProcessingHelper::compute_knockback_delta(*self_ptr, incoming, delta, 50.0f, 100));
    CHECK(delta.x == 30);
    CHECK(delta.y == 0);
}

TEST_CASE("evaluate_attack_window uses candidate animation metadata for non-current attacks") {
    auto attacker = test_child_asset_runtime::make_test_asset("attacker", 0, 0, 0, 0);
    auto target = test_child_asset_runtime::make_test_asset("target", 0, 0, 0, 0);
    Asset* attacker_ptr = attacker.get();
    Asset* target_ptr = target.get();
    REQUIRE(attacker_ptr != nullptr);
    REQUIRE(target_ptr != nullptr);
    REQUIRE(attacker_ptr->info != nullptr);

    attacker_ptr->info->attack_box_enabled = true;
    target_ptr->info->hitbox_enabled = true;
    attacker_ptr->info->animations["attack_left"] =
        make_single_frame_attack_animation({"attack", "left"}, 8, 4, 12, 12);
    attacker_ptr->info->animations["attack_right"] =
        make_single_frame_attack_animation({"attack", "right"}, 500, 500, 8, 8);
    attacker_ptr->current_animation = "attack_left";
    attacker_ptr->current_frame = attacker_ptr->info->animations["attack_left"].get_first_frame(0);

    // Simulate current-frame runtime volume overlap so pre-fix behavior would falsely score
    // "attack_right" as a hit by reusing current attack volumes.
    attacker_ptr->test_set_current_attack_box_volumes(
        {make_box(12.0f, 0.0f, 10.0f, 4.0f, 5, "attack_box")});
    target_ptr->test_set_current_hit_box_volumes(
        {make_box(12.0f, 0.0f, 10.0f, 3.0f, 0, "hit")});

    const auto right_eval =
        animation_update::AttackValidation::evaluate_attack_window(
            *attacker_ptr,
            *target_ptr,
            "attack_right",
            8);

    CHECK(right_eval.score == animation_update::AttackValidation::AttackWindowScore::Miss);
}

TEST_CASE("spider controller relies on committed auto-attack dispatch only") {
    AssetsScope scope;
    auto spider = test_child_asset_runtime::make_test_asset("spider", 0, 0, 0, 0);
    auto player = test_child_asset_runtime::make_test_asset("player", 300, 0, 300, 0);
    auto target = test_child_asset_runtime::make_test_asset("target", 0, 0, 0, 0);
    Asset* spider_ptr = test_child_asset_runtime::attach_owned_asset(scope.assets, std::move(spider));
    Asset* player_ptr = test_child_asset_runtime::attach_owned_asset(scope.assets, std::move(player));
    Asset* target_ptr = test_child_asset_runtime::attach_owned_asset(scope.assets, std::move(target));
    REQUIRE(spider_ptr != nullptr);
    REQUIRE(player_ptr != nullptr);
    REQUIRE(target_ptr != nullptr);
    REQUIRE(spider_ptr->info != nullptr);
    REQUIRE(target_ptr->info != nullptr);

    scope.assets->player = player_ptr;
    spider_ptr->info->type = "object";
    spider_ptr->info->attack_box_enabled = true;
    target_ptr->info->hitbox_enabled = true;

    AnimationFrame spider_frame{};
    AnimationFrame target_frame{};
    spider_ptr->current_frame = &spider_frame;
    target_ptr->current_frame = &target_frame;

    spider_ptr->test_set_current_attack_box_volumes({make_box(0.0f, 0.0f, 0.0f, 4.0f, 25)});
    target_ptr->test_set_current_hit_box_volumes({make_box(0.0f, 0.0f, 0.0f, 3.0f, 0, "hit")});

    spider_controller controller(spider_ptr);
    Input input{};
    controller.update(input);

    CHECK(target_ptr->process_pending_attacks().empty());
}

TEST_CASE("knockback remains finite and capped with invalid weight and extreme payload distance") {
    auto self = test_child_asset_runtime::make_test_asset("target", 60, 0, 0, 0);
    Asset* self_ptr = self.get();
    REQUIRE(self_ptr != nullptr);
    REQUIRE(self_ptr->info != nullptr);

    self_ptr->info->weight_kg = 0.0f;

    animation_update::Attack incoming{};
    incoming.payload.damage_amount = 30;
    incoming.payload.hitback_enabled = true;
    incoming.payload.hitback_distance = 1000000.0f;
    incoming.hit_x = 0.0f;
    incoming.hit_z = 0.0f;

    SDL_Point delta{};
    REQUIRE(AttackProcessingHelper::compute_knockback_delta(*self_ptr, incoming, delta, 120.0f, 150));
    CHECK(delta.x > 0);
    CHECK(std::abs(delta.x) <= 120);
    CHECK(std::abs(delta.y) <= 120);
}

TEST_CASE("attack processing applies damage when knockback payload is present") {
    auto self = test_child_asset_runtime::make_test_asset("target", 60, 0, 0, 0);
    Asset* self_ptr = self.get();
    REQUIRE(self_ptr != nullptr);
    REQUIRE(self_ptr->info != nullptr);

    self_ptr->info->weight_kg = 0.0f;
    const int start_health = self_ptr->runtime_health;

    animation_update::Attack incoming{};
    incoming.payload.damage_amount = 20;
    incoming.payload.hitback_enabled = true;
    incoming.payload.hitback_distance = 5000.0f;
    incoming.hit_x = 0.0f;
    incoming.hit_z = 0.0f;

    animation_update::custom_controllers::AttackProcessingConfig config{};
    config.max_knockback_distance = 120.0f;
    config.max_damage_for_knockback = 150;

    const auto summary =
        AttackProcessingHelper::process_attacks(*self_ptr, std::vector<animation_update::Attack>{incoming}, config);
    CHECK(summary.took_damage);
    CHECK_FALSE(summary.died);
    CHECK(self_ptr->runtime_health == start_health - 20);
}
