#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "animation/get_best_path.hpp"
#include "animation/path_sanitizer.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/animation.hpp"
#include "utils/grid.hpp"

namespace {

AnimationFrame make_motion_frame(int dx, int dz) {
    AnimationFrame frame;
    frame.dx = dx;
    frame.dz = dz;
    return frame;
}

Animation make_single_path_animation(int dx, int dz) {
    Animation animation;
    std::vector<AnimationFrame> path;
    path.push_back(make_motion_frame(dx, dz));
    std::vector<std::vector<AnimationFrame>> paths;
    paths.push_back(std::move(path));
    animation.replace_movement_paths(std::move(paths));
    return animation;
}

std::unique_ptr<Asset> make_pathing_test_asset() {
    auto info = std::make_shared<AssetInfo>("auto_move_zero_stride_recovery_asset");
    info->animations["a_left"] = make_single_path_animation(-10, 0);
    info->animations["b_right"] = make_single_path_animation(10, 0);
    info->start_animation = "a_left";
    info->movement_enabled = true;

    Area spawn_area("auto_move_zero_stride_recovery_area", 0);
    auto asset = std::make_unique<Asset>(info,
                                         spawn_area,
                                         SDL_Point{0, 0},
                                         0,
                                         std::string{},
                                         std::string{},
                                         0);
    asset->move_to_world_position(0, 0, 0, 0);
    return asset;
}

} // namespace

TEST_CASE("GetBestPath selects a progress stride toward checkpoint") {
    const auto asset = make_pathing_test_asset();
    REQUIRE(asset != nullptr);
    REQUIRE(asset->info != nullptr);

    const SDL_Point target{30, 0};
    GetBestPath planner;
    const Plan plan = planner(*asset, {target}, 0, vibble::grid::global_grid());

    REQUIRE_FALSE(plan.strides.empty());
    CHECK(plan.strides.front().animation_id == "b_right");
    CHECK(plan.final_dest.x > asset->world_x());
}

#if defined(ENGINE_WORLD_TESTS)
TEST_CASE("PathSanitizer marks anchor-equal checkpoints as collapsed") {
    CHECK(path_sanitizer::test_hooks::checkpoint_collapses_to_anchor(SDL_Point{10, 20},
                                                                      SDL_Point{10, 20}));
    CHECK_FALSE(path_sanitizer::test_hooks::checkpoint_collapses_to_anchor(SDL_Point{10, 20},
                                                                            SDL_Point{11, 20}));
}
#endif
