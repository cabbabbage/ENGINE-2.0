#include <doctest/doctest.h>

#include "animation/controllers/shared/controller_game_context.hpp"
#include "animation/controllers/shared/custom_controller_update_utils.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "stubs/asset/child_asset_runtime_test_support.hpp"

namespace {

struct ControllerAssets {
    ControllerAssets() {
        assets = test_child_asset_runtime::create_assets_stub();
        self = test_child_asset_runtime::attach_owned_asset(
            assets,
            test_child_asset_runtime::make_test_asset("self"));
        player = test_child_asset_runtime::attach_owned_asset(
            assets,
            test_child_asset_runtime::make_test_asset("player"));
    }

    ~ControllerAssets() {
        if (assets) {
            test_child_asset_runtime::destroy_assets_stub(assets);
            assets = nullptr;
        }
    }

    Assets* assets = nullptr;
    Asset* self = nullptr;
    Asset* player = nullptr;
};

} // namespace

namespace custom_controllers = animation_update::custom_controllers;

TEST_CASE("resolve_valid_player_target rejects missing assets or player") {
    ControllerAssets ctx;

    CHECK(custom_controllers::resolve_valid_player_target(ctx.self, nullptr) == nullptr);

    ctx.assets->player = nullptr;
    CHECK(custom_controllers::resolve_valid_player_target(ctx.self, ctx.assets) == nullptr);
}

TEST_CASE("resolve_valid_player_target rejects self as target") {
    ControllerAssets ctx;
    ctx.assets->player = ctx.self;
    CHECK(custom_controllers::resolve_valid_player_target(ctx.self, ctx.assets) == nullptr);
}

TEST_CASE("resolve_valid_player_target rejects dead or inactive players") {
    ControllerAssets ctx;
    ctx.assets->player = ctx.player;

    ctx.player->dead = true;
    CHECK(custom_controllers::resolve_valid_player_target(ctx.self, ctx.assets) == nullptr);

    ctx.player->dead = false;
    ctx.player->active = false;
    CHECK(custom_controllers::resolve_valid_player_target(ctx.self, ctx.assets) == nullptr);
}

TEST_CASE("resolve_valid_player_target returns valid player") {
    ControllerAssets ctx;
    ctx.assets->player = ctx.player;

    ctx.player->dead = false;
    ctx.player->active = true;
    CHECK(custom_controllers::resolve_valid_player_target(ctx.self, ctx.assets) == ctx.player);
}

TEST_CASE("dispatch_contact_attack tolerates invalid target arguments") {
    ControllerAssets ctx;
    CHECK_NOTHROW(custom_controllers::dispatch_contact_attack(ctx.self, nullptr));
}

TEST_CASE("build_controller_game_context handles null inputs safely") {
    const custom_controllers::ControllerGameContext context =
        custom_controllers::build_controller_game_context(nullptr, nullptr);

    CHECK_FALSE(context.has_self());
    CHECK_FALSE(context.has_assets());
    CHECK_FALSE(context.has_player());
    CHECK_FALSE(context.player_is_valid());
    CHECK_FALSE(context.has_current_room());
    CHECK_FALSE(context.self_in_current_room());
    CHECK_FALSE(context.player_in_current_room());
    CHECK_FALSE(context.self_and_player_share_room());
    CHECK(context.resolved_player == nullptr);
    CHECK(context.frame_id == 0);
    CHECK(context.delta_seconds == doctest::Approx(1.0f / 60.0f));
    CHECK_FALSE(context.fly_orbit_point.valid);
    CHECK(context.fly_orbit_point.world_xz.x == 0);
    CHECK(context.fly_orbit_point.world_xz.y == 0);
    CHECK(context.fly_orbit_point.world_y == 0);
    CHECK(context.fly_orbit_point.grid_resolution == 0);
    CHECK_FALSE(context.fly_orbit_target.valid);
    CHECK(context.fly_orbit_target.last_update_frame == 0);
    CHECK(context.fly_orbit_target.next_refresh_frame == 0);
    CHECK(context.fly_orbit_target.target_id == 0);
    CHECK(context.fly_orbit_target.target_version == 0);
    CHECK_FALSE(context.fly_orbit_target_changed);
}

TEST_CASE("build_controller_game_context snapshots self and resolves valid player") {
    ControllerAssets ctx;
    ctx.assets->player = ctx.player;
    ctx.player->active = true;
    ctx.player->dead = false;
    ctx.self->set_owning_room_name("room_a");
    ctx.player->set_owning_room_name("room_a");
    ctx.self->move_to_world_position(101, 202, 303, 4);
    ctx.self->grid_resolution = 4;

    const custom_controllers::ControllerGameContext context =
        custom_controllers::build_controller_game_context(ctx.self, ctx.assets);

    CHECK(context.has_self());
    CHECK(context.has_assets());
    CHECK(context.has_player());
    CHECK(context.player_is_valid());
    CHECK(context.player == ctx.player);
    CHECK(context.resolved_player == ctx.player);
    CHECK(context.self_world_xz.x == 101);
    CHECK(context.self_world_xz.y == 303);
    CHECK(context.self_world_y == 202);
    CHECK(context.self_grid_resolution == 4);
    CHECK(context.self_has_room_assignment());
    CHECK(context.player_has_room_assignment());
    CHECK(context.self_and_player_share_room());
}

TEST_CASE("context-based helpers preserve null safety") {
    ControllerAssets ctx;
    ctx.assets->player = ctx.player;
    ctx.player->active = false;
    ctx.player->dead = false;

    const custom_controllers::ControllerGameContext context =
        custom_controllers::build_controller_game_context(ctx.self, ctx.assets);
    CHECK(custom_controllers::resolve_valid_player_target(context) == nullptr);
    CHECK_NOTHROW(custom_controllers::dispatch_contact_attack(context));
    CHECK_NOTHROW(custom_controllers::dispatch_interact(context, ctx.player));
}

TEST_CASE("reverse control helpers preserve null safety") {
    CHECK_NOTHROW(custom_controllers::begin_reverse_current_animation_until_stop(static_cast<Asset*>(nullptr)));
    CHECK_NOTHROW(custom_controllers::begin_reverse_current_animation_to_default(static_cast<Asset*>(nullptr)));
    CHECK_NOTHROW(custom_controllers::stop_reverse_current_animation(static_cast<Asset*>(nullptr)));

    const custom_controllers::ControllerGameContext null_context =
        custom_controllers::build_controller_game_context(nullptr, nullptr);
    CHECK_NOTHROW(custom_controllers::begin_reverse_current_animation_until_stop(null_context));
    CHECK_NOTHROW(custom_controllers::begin_reverse_current_animation_to_default(null_context));
    CHECK_NOTHROW(custom_controllers::stop_reverse_current_animation(null_context));
}

TEST_CASE("interact helpers preserve null safety") {
    ControllerAssets ctx;
    CHECK_NOTHROW(custom_controllers::dispatch_interact(static_cast<Asset*>(nullptr), ctx.player));
    CHECK_NOTHROW(custom_controllers::dispatch_interact(ctx.self, static_cast<Asset*>(nullptr)));
    CHECK_NOTHROW(custom_controllers::dispatch_interact(ctx.self, ctx.self));

    const custom_controllers::ControllerGameContext null_context =
        custom_controllers::build_controller_game_context(nullptr, nullptr);
    CHECK_NOTHROW(custom_controllers::dispatch_interact(null_context, ctx.player));
}

TEST_CASE("build_controller_game_context preserves prior orbit lifecycle snapshot when runtime data is unavailable") {
    custom_controllers::FlyOrbitTargetSnapshot previous{};
    previous.valid = true;
    previous.world_xz = SDL_Point{33, 44};
    previous.world_y = 120;
    previous.grid_resolution = 2;
    previous.last_update_frame = 99;
    previous.next_refresh_frame = 555;
    previous.target_id = 123456u;
    previous.target_version = 7;

    const custom_controllers::ControllerGameContext context =
        custom_controllers::build_controller_game_context(nullptr, nullptr, &previous);

    CHECK(context.fly_orbit_target.valid);
    CHECK(context.fly_orbit_point.valid);
    CHECK(context.fly_orbit_point.world_xz.x == 33);
    CHECK(context.fly_orbit_point.world_xz.y == 44);
    CHECK(context.fly_orbit_point.world_y == 120);
    CHECK(context.fly_orbit_point.grid_resolution == 2);
    CHECK(context.fly_orbit_target.world_xz.x == 33);
    CHECK(context.fly_orbit_target.world_xz.y == 44);
    CHECK(context.fly_orbit_target.world_y == 120);
    CHECK(context.fly_orbit_target.grid_resolution == 2);
    CHECK(context.fly_orbit_target.last_update_frame == 99);
    CHECK(context.fly_orbit_target.next_refresh_frame == 555);
    CHECK(context.fly_orbit_target.target_id == 123456u);
    CHECK(context.fly_orbit_target.target_version == 7);
    CHECK_FALSE(context.fly_orbit_target_changed);
}
