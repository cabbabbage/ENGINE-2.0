#include <doctest/doctest.h>

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
