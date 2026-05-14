#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "animation/animation_runtime.hpp"
#include "animation/animation_update.hpp"
#include "animation/controllers/shared/attack_processing_helper.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/animation.hpp"
#include "core/runtime_world_context.hpp"
#include "gameplay/map_generation/room.hpp"

namespace {

AnimationFrame make_runtime_frame(int frame_index, int frame_count) {
    AnimationFrame frame{};
    frame.frame_index = frame_index;
    frame.is_first = (frame_index == 0);
    frame.is_last = (frame_index + 1 == frame_count);
    return frame;
}

Animation make_runtime_animation(int frame_count,
                                 bool locked = false,
                                 int dx = 0,
                                 int dy = 0,
                                 int dz = 0) {
    Animation animation{};
    auto& path = animation.movement_path(0);
    path.clear();
    path.reserve(static_cast<std::size_t>(frame_count));
    for (int idx = 0; idx < frame_count; ++idx) {
        AnimationFrame frame = make_runtime_frame(idx, frame_count);
        frame.dx = dx;
        frame.dy = dy;
        frame.dz = dz;
        path.push_back(std::move(frame));
    }
    for (int idx = 0; idx < frame_count; ++idx) {
        AnimationFrame& frame = path[static_cast<std::size_t>(idx)];
        frame.prev = (idx > 0) ? &path[static_cast<std::size_t>(idx - 1)] : nullptr;
        frame.next = (idx + 1 < frame_count) ? &path[static_cast<std::size_t>(idx + 1)] : nullptr;
    }
    animation.locked = locked;
    animation.on_end_animation = "default";
    animation.on_end_behavior = Animation::OnEndDirective::Default;
    return animation;
}

std::unique_ptr<Asset> make_attack_runtime_test_asset() {
    auto info = std::make_shared<AssetInfo>("animation_runtime_auto_attack_test_asset");
    info->animations["default"] = make_runtime_animation(1, false);
    info->animations["walk_right"] = make_runtime_animation(1, false, 8, 0, 0);
    info->animations["attack_left"] = make_runtime_animation(2, true);
    info->animations["attack_left"].tags = {"attack", "left"};
    info->animations["attack_right"] = make_runtime_animation(2, true);
    info->animations["attack_right"].tags = {"attack", "right"};
    info->animations["hit"] = make_runtime_animation(1, false);
    info->animations["die"] = make_runtime_animation(1, false);
    info->start_animation = "default";
    info->type = "enemy";
    info->movement_enabled = true;

    Area spawn_area("animation_runtime_auto_attack_area", 0);
    return std::make_unique<Asset>(info,
                                   spawn_area,
                                   SDL_Point{0, 0},
                                   0,
                                   std::string{},
                                   std::string{},
                                   0);
}

std::unique_ptr<Asset> make_runtime_test_asset() {
    auto info = std::make_shared<AssetInfo>("animation_runtime_reverse_test_asset");
    info->animations["default"] = make_runtime_animation(2, false);
    info->animations["left"] = make_runtime_animation(3, true);
    info->animations["up"] = make_runtime_animation(3, false);
    info->start_animation = "left";
    info->type = "object";
    info->movement_enabled = true;

    Area spawn_area("animation_runtime_reverse_area", 0);
    auto asset = std::make_unique<Asset>(info,
                                         spawn_area,
                                         SDL_Point{0, 0},
                                         0,
                                         std::string{},
                                         std::string{},
                                         0);
    return asset;
}

void force_single_advance_tick(Asset& asset) {
    asset.set_frame_progress(1.0f / static_cast<float>(kBaseAnimationFps));
}

} // namespace

TEST_CASE("Animation adopt_prebuilt_frames keeps primary path synchronized") {
    Animation animation;

    std::vector<Animation::FrameCache> caches(3);
    animation.adopt_prebuilt_frames(std::move(caches), {1.0f, 0.5f});

    REQUIRE(animation.movement_path_count() == 1);
    REQUIRE(animation.has_frames());
    CHECK(animation.frame_count() == 3);

    auto& primary = animation.primary_frames();
    REQUIRE(primary.size() == 3);
    CHECK(animation.primary_frame_at(0) == &primary[0]);
    CHECK(animation.primary_frame_at(1) == &primary[1]);
    CHECK(animation.primary_frame_at(2) == &primary[2]);
    CHECK(animation.primary_frame_at(3) == nullptr);

    CHECK(primary[0].is_first);
    CHECK_FALSE(primary[0].is_last);
    CHECK(primary[0].prev == nullptr);
    CHECK(primary[0].next == &primary[1]);

    CHECK(primary[1].frame_index == 1);
    CHECK(primary[1].prev == &primary[0]);
    CHECK(primary[1].next == &primary[2]);

    CHECK(primary[2].is_last);
    CHECK(primary[2].prev == &primary[1]);
    CHECK(primary[2].next == nullptr);
}

TEST_CASE("Animation primary_frames aliases movement_path zero") {
    Animation animation;

    std::vector<Animation::FrameCache> caches(2);
    animation.adopt_prebuilt_frames(std::move(caches), {1.0f});

    auto& primary = animation.primary_frames();
    auto& path0 = animation.movement_path(0);
    REQUIRE(primary.size() == 2);
    REQUIRE(path0.size() == 2);

    path0[1].dx = 42;
    CHECK(primary[1].dx == 42);
    CHECK(animation.frame_count(0) == primary.size());
}

TEST_CASE("Animation classify_on_end maps loop and reserved directives") {
    CHECK(Animation::classify_on_end("default") == Animation::OnEndDirective::Default);
    CHECK(Animation::classify_on_end("loop") == Animation::OnEndDirective::Loop);
    CHECK(Animation::classify_on_end("kill") == Animation::OnEndDirective::Kill);
    CHECK(Animation::classify_on_end("lock") == Animation::OnEndDirective::Lock);
    CHECK(Animation::classify_on_end("reverse") == Animation::OnEndDirective::Reverse);
    CHECK(Animation::classify_on_end("vibble_attack_2") == Animation::OnEndDirective::Animation);
}

TEST_CASE("AnimationRuntime reverse-until-stop loops backward at frame zero") {
    auto asset = make_runtime_test_asset();
    REQUIRE(asset != nullptr);
    AnimationRuntime runtime(asset.get(), nullptr);

    REQUIRE(asset->info != nullptr);
    REQUIRE(asset->current_frame != nullptr);
    CHECK(asset->current_animation == "left");
    auto& left_path = asset->info->animations["left"].movement_path(0);
    REQUIRE(left_path.size() == 3);

    asset->current_frame = &left_path[2];
    runtime.begin_reverse_current_animation_until_stop();
    CHECK(runtime.reverse_playback_mode() ==
          AnimationRuntime::ReversePlaybackMode::ReverseUntilStopCurrentAnimation);

    force_single_advance_tick(*asset);
    CHECK(runtime.advance(asset->current_frame));
    CHECK(asset->current_frame == &left_path[1]);

    force_single_advance_tick(*asset);
    CHECK(runtime.advance(asset->current_frame));
    CHECK(asset->current_frame == &left_path[0]);

    force_single_advance_tick(*asset);
    CHECK(runtime.advance(asset->current_frame));
    CHECK(asset->current_frame == &left_path[2]);
}

TEST_CASE("AnimationRuntime reverse-to-default switches at first frame") {
    auto asset = make_runtime_test_asset();
    REQUIRE(asset != nullptr);
    AnimationRuntime runtime(asset.get(), nullptr);

    REQUIRE(asset->info != nullptr);
    auto& left_path = asset->info->animations["left"].movement_path(0);
    REQUIRE(left_path.size() == 3);
    asset->current_animation = "left";
    asset->current_frame = &left_path[0];

    runtime.begin_reverse_current_animation_to_default();
    force_single_advance_tick(*asset);
    CHECK(runtime.advance(asset->current_frame));
    CHECK(asset->current_animation == "default");
    CHECK(runtime.reverse_playback_mode() == AnimationRuntime::ReversePlaybackMode::None);
    REQUIRE(asset->current_frame != nullptr);
    CHECK(asset->current_frame->is_first);
}

TEST_CASE("AnimationRuntime clears reverse mode on animation change") {
    auto asset = make_runtime_test_asset();
    REQUIRE(asset != nullptr);
    AnimationRuntime runtime(asset.get(), nullptr);

    runtime.begin_reverse_current_animation_until_stop();
    CHECK(runtime.reverse_playback_mode() ==
          AnimationRuntime::ReversePlaybackMode::ReverseUntilStopCurrentAnimation);

    runtime.switch_to("up");
    CHECK(asset->current_animation == "up");
    CHECK(runtime.reverse_playback_mode() == AnimationRuntime::ReversePlaybackMode::None);
}

TEST_CASE("AnimationRuntime applies queued reverse command after move-triggered animation switch") {
    auto asset = make_runtime_test_asset();
    REQUIRE(asset != nullptr);
    REQUIRE(asset->info != nullptr);
    asset->info->movement_enabled = true;
    AnimationRuntime runtime(asset.get(), nullptr);
    AnimationUpdate updater(asset.get(), nullptr);
    runtime.set_planner(&updater);

    REQUIRE(asset->info != nullptr);
    auto& up_path = asset->info->animations["up"].movement_path(0);
    REQUIRE(up_path.size() == 3);

    updater.move(SDL_Point{0, 0}, "up", true, true);
    updater.begin_reverse_current_animation_until_stop();
    runtime.update();

    CHECK(asset->current_animation == "up");
    CHECK(runtime.reverse_playback_mode() ==
          AnimationRuntime::ReversePlaybackMode::ReverseUntilStopCurrentAnimation);

    force_single_advance_tick(*asset);
    CHECK(runtime.advance(asset->current_frame));
    CHECK(asset->current_frame == &up_path.back());
}

TEST_CASE("attack facing scoring prefers right animation tags for right-side target") {
    const int right_score = animation_runtime::test_hooks::attack_facing_match_score(
        {"attack", "right"},
        "attack_right",
        24);
    const int left_score = animation_runtime::test_hooks::attack_facing_match_score(
        {"attack", "left"},
        "attack_left",
        24);

    CHECK(right_score > left_score);
    CHECK(right_score == 1);
    CHECK(left_score == -1);
}

TEST_CASE("AnimationRuntime advances locked attack frames while commitment is active") {
    auto asset = make_attack_runtime_test_asset();
    REQUIRE(asset != nullptr);

    AnimationRuntime runtime(asset.get(), nullptr);
    AnimationUpdate updater(asset.get(), nullptr);
    runtime.set_planner(&updater);

    runtime.switch_to("attack_right");
    REQUIRE(asset->current_frame != nullptr);
    CHECK(asset->current_frame->frame_index == 0);
    CHECK(runtime.auto_attack_commitment_active());

    force_single_advance_tick(*asset);
    CHECK(runtime.advance(asset->current_frame));
    REQUIRE(asset->current_frame != nullptr);
    CHECK(asset->current_frame->frame_index == 1);
}

TEST_CASE("AnimationRuntime advances locked attack frames when playing tagged attack clip") {
    auto asset = make_attack_runtime_test_asset();
    REQUIRE(asset != nullptr);
    REQUIRE(asset->info != nullptr);

    AnimationRuntime runtime(asset.get(), nullptr);
    AnimationUpdate updater(asset.get(), nullptr);
    runtime.set_planner(&updater);

    asset->set_current_animation("attack_right");
    REQUIRE(asset->current_frame != nullptr);
    CHECK(asset->current_frame->frame_index == 0);
    CHECK_FALSE(runtime.auto_attack_commitment_active());

    force_single_advance_tick(*asset);
    CHECK(runtime.advance(asset->current_frame));
    REQUIRE(asset->current_frame != nullptr);
    CHECK(asset->current_frame->frame_index == 1);
}

TEST_CASE("committed auto attack loop finishes one cycle and releases follow-through") {
    auto asset = make_attack_runtime_test_asset();
    REQUIRE(asset != nullptr);
    REQUIRE(asset->info != nullptr);

    asset->info->animations["attack_right"].on_end_behavior = Animation::OnEndDirective::Loop;

    AnimationRuntime runtime(asset.get(), nullptr);
    AnimationUpdate updater(asset.get(), nullptr);
    runtime.set_planner(&updater);

    runtime.switch_to("attack_right");
    animation_runtime::test_hooks::force_committed_attack_target(runtime, "player");
    REQUIRE(asset->current_frame != nullptr);
    CHECK(asset->current_frame->frame_index == 0);
    CHECK(runtime.auto_attack_commitment_active());

    force_single_advance_tick(*asset);
    CHECK(runtime.advance(asset->current_frame));
    REQUIRE(asset->current_frame != nullptr);
    CHECK(asset->current_animation == "attack_right");
    CHECK(asset->current_frame->frame_index == 1);
    CHECK(runtime.auto_attack_commitment_active());

    force_single_advance_tick(*asset);
    CHECK(runtime.advance(asset->current_frame));
    REQUIRE(asset->current_frame != nullptr);
    CHECK(asset->current_animation == "default");
    CHECK(asset->needs_target);
    CHECK_FALSE(runtime.auto_attack_commitment_active());
}

TEST_CASE("AnimationUpdate defers auto_move planning during committed attack follow-through") {
    auto asset = make_attack_runtime_test_asset();
    REQUIRE(asset != nullptr);

    AnimationRuntime runtime(asset.get(), nullptr);
    AnimationUpdate updater(asset.get(), nullptr);
    runtime.set_planner(&updater);

    runtime.switch_to("attack_right");
    CHECK(runtime.auto_attack_commitment_active());

    updater.auto_move(SDL_Point{asset->world_x() + 64, asset->world_z()});
    CHECK(updater.current_plan_mode() == AnimationUpdate::ActivePlanMode::None);
    REQUIRE(updater.current_plan() != nullptr);
    CHECK(updater.current_plan()->strides.empty());
}

TEST_CASE("hit and death processing interrupt committed attack follow-through") {
    auto asset = make_attack_runtime_test_asset();
    REQUIRE(asset != nullptr);
    if (!asset->anim_) {
        asset->anim_ = std::make_unique<AnimationUpdate>(asset.get(), nullptr);
    }
    REQUIRE(asset->anim_ != nullptr);

    AnimationRuntime runtime(asset.get(), nullptr);
    runtime.set_planner(asset->anim_.get());

    runtime.switch_to("attack_right");
    CHECK(runtime.auto_attack_commitment_active());

    asset->runtime_health = 10;
    animation_update::Attack hit_attack{};
    hit_attack.damage_amount = 2;
    hit_attack.payload.damage_amount = 2;
    asset->send_attack(hit_attack);
    animation_update::custom_controllers::AttackProcessingHelper::process_pending_attacks(*asset);
    CHECK(asset->current_animation == "hit");
    CHECK_FALSE(runtime.auto_attack_commitment_active());

    runtime.switch_to("attack_right");
    CHECK(runtime.auto_attack_commitment_active());
    asset->runtime_health = 1;
    animation_update::Attack lethal_attack{};
    lethal_attack.damage_amount = 2;
    lethal_attack.payload.damage_amount = 2;
    asset->send_attack(lethal_attack);
    animation_update::custom_controllers::AttackProcessingHelper::process_pending_attacks(*asset);
    CHECK(asset->current_animation == "die");
    CHECK_FALSE(runtime.auto_attack_commitment_active());
}

TEST_CASE("AnimationUpdate resolve_animation_by_tags matches required tags") {
    auto asset = make_runtime_test_asset();
    REQUIRE(asset != nullptr);
    REQUIRE(asset->info != nullptr);

    asset->info->animations["left"].tags = {"break", "heavy"};
    asset->info->animations["up"].tags = {"break", "air"};

    AnimationUpdate updater(asset.get(), nullptr);
    const std::optional<std::string> resolved =
        updater.resolve_animation_by_tags({"  BREAK  "}, {});
    REQUIRE(resolved.has_value());
    CHECK((*resolved == "left" || *resolved == "up"));
}

TEST_CASE("AnimationUpdate resolve_animation_by_tags excludes blocked tags") {
    auto asset = make_runtime_test_asset();
    REQUIRE(asset != nullptr);
    REQUIRE(asset->info != nullptr);

    asset->info->animations["left"].tags = {"break", "heavy"};
    asset->info->animations["up"].tags = {"break", "air"};

    AnimationUpdate updater(asset.get(), nullptr);
    const std::optional<std::string> resolved =
        updater.resolve_animation_by_tags({"break"}, {"HEAVY"});
    REQUIRE(resolved.has_value());
    CHECK(*resolved == "up");
}

TEST_CASE("AnimationUpdate set_animation_by_tags keeps animation unchanged when no match") {
    auto asset = make_runtime_test_asset();
    REQUIRE(asset != nullptr);
    REQUIRE(asset->info != nullptr);
    asset->info->animations["left"].tags = {"idle"};
    asset->info->animations["up"].tags = {"jump"};

    AnimationRuntime runtime(asset.get(), nullptr);
    AnimationUpdate updater(asset.get(), nullptr);
    runtime.set_planner(&updater);
    const std::string before = asset->current_animation;

    CHECK_FALSE(updater.set_animation_by_tags({"break"}, {}));
    CHECK(asset->current_animation == before);
}

TEST_CASE("AnimationUpdate resolve_animation_by_tags multiple matches stay in valid set") {
    auto asset = make_runtime_test_asset();
    REQUIRE(asset != nullptr);
    REQUIRE(asset->info != nullptr);

    asset->info->animations["left"].tags = {"break"};
    asset->info->animations["up"].tags = {"break"};

    AnimationUpdate updater(asset.get(), nullptr);
    const std::optional<std::string> resolved =
        updater.resolve_animation_by_tags({"break"}, {});
    REQUIRE(resolved.has_value());
    CHECK((*resolved == "left" || *resolved == "up"));
}

TEST_CASE("RuntimeWorldContext rebuilds room view and tracks topology generation") {
    RuntimeWorldContext context;
    CHECK(context.topology_generation() == 0);

    std::vector<std::unique_ptr<Room>> owned_rooms;
    owned_rooms.emplace_back();
    owned_rooms.emplace_back();
    context.adopt_rooms(std::move(owned_rooms));

    REQUIRE(context.owned_rooms().size() == 2);
    REQUIRE(context.rooms().size() == 2);
    CHECK(context.rooms()[0] == nullptr);
    CHECK(context.rooms()[1] == nullptr);
    CHECK(context.topology_generation() == 1);

    context.owned_rooms().push_back(std::unique_ptr<Room>{});
    context.rebuild_room_view();
    REQUIRE(context.rooms().size() == 3);
    CHECK(context.rooms()[2] == nullptr);
    CHECK(context.topology_generation() == 1);

    context.notify_topology_changed();
    CHECK(context.topology_generation() == 2);
}
