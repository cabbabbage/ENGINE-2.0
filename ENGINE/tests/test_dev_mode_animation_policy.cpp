#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <string>

#include "core/dev_mode_animation_policy.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/controller_factory.hpp"
#include "animation/controllers/custom_controllers/fly_controller.hpp"
#include "animation/controllers/custom_controllers/spider_controller.hpp"
#include "animation/controllers/shared/custom_asset_controller.hpp"

namespace {

std::unique_ptr<Asset> make_test_asset(const std::string& asset_name) {
    auto info = std::make_shared<AssetInfo>(asset_name);
    info->type = "object";
    Area spawn_area("dev_mode_animation_policy_test_area", 0);
    return std::make_unique<Asset>(info,
                                   spawn_area,
                                   SDL_Point{0, 0},
                                   0,
                                   std::string{},
                                   std::string{},
                                   0);
}

Assets* fake_assets_handle() {
    // ControllerFactory only checks this pointer for non-null in these tests.
    return reinterpret_cast<Assets*>(static_cast<std::uintptr_t>(1));
}

} // namespace

TEST_CASE("Dev mode animation policy advances runtime-updated assets outside frame editor") {
    CHECK(runtime::dev_mode_policy::should_advance_animation_for_asset(
        false, false, false, false));
    CHECK(runtime::dev_mode_policy::should_advance_animation_for_asset(
        true, true, false, false));
    CHECK_FALSE(runtime::dev_mode_policy::should_advance_animation_for_asset(
        true, false, false, false));
}

TEST_CASE("Dev mode animation policy keeps frame editor target-only") {
    CHECK(runtime::dev_mode_policy::should_advance_animation_for_asset(
        true, true, true, true));
    CHECK_FALSE(runtime::dev_mode_policy::should_advance_animation_for_asset(
        true, true, true, false));
    CHECK_FALSE(runtime::dev_mode_policy::should_advance_animation_for_asset(
        true, false, true, false));
}

TEST_CASE("Dev mode movement policy blocks all movement") {
    CHECK(runtime::dev_mode_policy::should_allow_movement_for_asset(false));
    CHECK_FALSE(runtime::dev_mode_policy::should_allow_movement_for_asset(true));
}

TEST_CASE("ControllerFactory resolves spider controller from asset name") {
    auto asset = make_test_asset("spider");
    ControllerFactory factory(fake_assets_handle());

    std::unique_ptr<AssetController> controller = factory.create_for_asset(asset.get());

    REQUIRE(controller != nullptr);
    CHECK(dynamic_cast<spider_controller*>(controller.get()) != nullptr);
}

TEST_CASE("ControllerFactory keeps unmatched names on base custom controller") {
    auto asset = make_test_asset("not_a_custom_controller_asset");
    ControllerFactory factory(fake_assets_handle());

    std::unique_ptr<AssetController> controller = factory.create_for_asset(asset.get());

    REQUIRE(controller != nullptr);
    CHECK(dynamic_cast<spider_controller*>(controller.get()) == nullptr);
    CHECK(dynamic_cast<CustomAssetController*>(controller.get()) != nullptr);
}

TEST_CASE("ControllerFactory resolves fly controller and arms movement targeting") {
    auto asset = make_test_asset("fly");
    ControllerFactory factory(fake_assets_handle());

    std::unique_ptr<AssetController> controller = factory.create_for_asset(asset.get());

    REQUIRE(controller != nullptr);
    CHECK(dynamic_cast<fly_controller*>(controller.get()) != nullptr);
    CHECK(asset->needs_target);
}
