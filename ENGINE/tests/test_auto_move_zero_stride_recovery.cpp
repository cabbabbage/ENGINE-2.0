#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "animation/get_best_path.hpp"
#include "animation/path_sanitizer.hpp"
#include "animation/collision_query_context.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/animation.hpp"
#include "utils/grid.hpp"
#include "utils/area.hpp"

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


Assets::FrameCollisionEntry make_collision_entry_rect(const Asset& owner,
                                                      int min_x,
                                                      int min_z,
                                                      int max_x,
                                                      int max_z) {
    std::vector<Area::Point> points{
        SDL_Point{min_x, min_z},
        SDL_Point{max_x, min_z},
        SDL_Point{max_x, max_z},
        SDL_Point{min_x, max_z}
    };
    Assets::FrameCollisionEntry entry;
    entry.asset = &owner;
    entry.area = Area("collision_entry", points, owner.grid_resolution);
    entry.world_center = entry.area.get_center();
    entry.bottom_middle = world::GridPoint::make_virtual(entry.world_center.x,
                                                         owner.world_y(),
                                                         entry.world_center.y,
                                                         owner.grid_resolution);
    entry.canonical_type = "impassable_shape";
    return entry;
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

TEST_CASE("Path planner and sanitizer produce identical results with shared collision context") {
    const auto asset = make_pathing_test_asset();
    REQUIRE(asset != nullptr);
    REQUIRE(asset->info != nullptr);

    const std::vector<SDL_Point> absolute_checkpoints{ SDL_Point{30, 0}, SDL_Point{40, 0} };
    PathSanitizer sanitizer;
    GetBestPath planner;

    const std::vector<SDL_Point> baseline_sanitized = sanitizer.sanitize(*asset, absolute_checkpoints, 0);
    const Plan baseline_plan = planner(*asset, baseline_sanitized, 0, vibble::grid::global_grid());

    CollisionQueryContext collision_context;
    const std::vector<SDL_Point> shared_sanitized =
        sanitizer.sanitize(*asset, absolute_checkpoints, 0, &collision_context);
    const Plan shared_plan =
        planner(*asset, shared_sanitized, 0, vibble::grid::global_grid(), &collision_context);

    CHECK(shared_sanitized.size() == baseline_sanitized.size());
    REQUIRE(shared_sanitized.size() == baseline_sanitized.size());
    for (std::size_t i = 0; i < shared_sanitized.size(); ++i) {
        CHECK(shared_sanitized[i].x == baseline_sanitized[i].x);
        CHECK(shared_sanitized[i].y == baseline_sanitized[i].y);
    }
    CHECK(shared_plan.final_dest.x == baseline_plan.final_dest.x);
    CHECK(shared_plan.final_dest.y == baseline_plan.final_dest.y);
    CHECK(shared_plan.strides.size() == baseline_plan.strides.size());
    REQUIRE(shared_plan.strides.size() == baseline_plan.strides.size());
    for (std::size_t i = 0; i < shared_plan.strides.size(); ++i) {
        CHECK(shared_plan.strides[i].animation_id == baseline_plan.strides[i].animation_id);
        CHECK(shared_plan.strides[i].frames == baseline_plan.strides[i].frames);
        CHECK(shared_plan.strides[i].path_index == baseline_plan.strides[i].path_index);
    }
}

#if defined(ENGINE_WORLD_TESTS)
TEST_CASE("PathSanitizer marks anchor-equal checkpoints as collapsed") {
    CHECK(path_sanitizer::test_hooks::checkpoint_collapses_to_anchor(SDL_Point{10, 20},
                                                                      SDL_Point{10, 20}));
    CHECK_FALSE(path_sanitizer::test_hooks::checkpoint_collapses_to_anchor(SDL_Point{10, 20},
                                                                            SDL_Point{11, 20}));
}
#endif


TEST_CASE("GetBestPath fallback rejects collision-blocked stride candidates") {
    auto asset = make_pathing_test_asset();
    REQUIRE(asset != nullptr);

    auto blocker_info = std::make_shared<AssetInfo>("blocker_asset");
    Area blocker_area("blocker_spawn", 0);
    Asset blocker(blocker_info,
                  blocker_area,
                  SDL_Point{8, 0},
                  0,
                  std::string{},
                  std::string{},
                  0);
    blocker.move_to_world_position(8, 0, 0, 0);

    const Assets::FrameCollisionEntry blocked_entry = make_collision_entry_rect(blocker, 4, -4, 14, 4);
    CollisionQueryContext context;
    context.loaded = true;
    context.entries.push_back(&blocked_entry);

    GetBestPath planner;
    const Plan plan = planner(*asset, {SDL_Point{20, 0}}, 0, vibble::grid::global_grid(), &context);

    CHECK(plan.strides.empty());
}
