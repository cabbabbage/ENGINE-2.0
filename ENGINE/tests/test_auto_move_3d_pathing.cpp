#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "animation/animation_update.hpp"
#include "animation/get_best_path.hpp"
#include "animation/get_best_path_3d.hpp"
#include "animation/movement_target_utils.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/animation.hpp"
#include "core/axis_convention.hpp"
#include "utils/grid.hpp"

namespace {

AnimationFrame make_motion_frame(int dx, int dy, int dz) {
    AnimationFrame frame;
    frame.dx = dx;
    frame.dy = dy;
    frame.dz = dz;
    return frame;
}

Animation make_single_path_animation(int dx, int dy, int dz) {
    Animation animation;
    std::vector<AnimationFrame> path;
    path.push_back(make_motion_frame(dx, dy, dz));
    std::vector<std::vector<AnimationFrame>> paths;
    paths.push_back(std::move(path));
    animation.replace_movement_paths(std::move(paths));
    return animation;
}

std::unique_ptr<Asset> make_test_asset(
    const std::vector<std::pair<std::string, Animation>>& animations,
    const std::string&                                    start_animation,
    int                                                   world_x = 0,
    int                                                   world_y = 0,
    int                                                   world_z = 0,
    int                                                   grid_resolution = 0) {
    auto info = std::make_shared<AssetInfo>("auto_move_3d_test_asset");
    for (const auto& [id, anim] : animations) {
        info->animations[id] = anim;
    }
    info->start_animation = start_animation;
    info->type = "object";
    info->movement_enabled = true;

    Area spawn_area("auto_move_3d_test_area", 0);
    auto asset = std::make_unique<Asset>(info,
                                         spawn_area,
                                         SDL_Point{world_x, world_z},
                                         0,
                                         std::string{},
                                         std::string{},
                                         grid_resolution);
    asset->move_to_world_position(world_x, world_y, world_z, grid_resolution);
    return asset;
}

bool same_world_pos(const axis::WorldPos& lhs, const axis::WorldPos& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}

} // namespace

TEST_CASE("GetBestPath3D selects upward stride toward absolute 3D target") {
    const auto asset = make_test_asset(
        {
            {"default", make_single_path_animation(0, 0, 0)},
            {"down", make_single_path_animation(0, -10, 0)},
            {"up", make_single_path_animation(0, 10, 0)},
        },
        "default");

    REQUIRE(asset != nullptr);
    REQUIRE(asset->info != nullptr);

    GetBestPath3D planner;
    const Plan3D plan = planner(*asset, {axis::WorldPos{0, 30, 0}}, 0, vibble::grid::global_grid());

    REQUIRE_FALSE(plan.strides.empty());
    CHECK(plan.strides.front().animation_id == "up");
    CHECK(plan.final_dest.y > asset->world_y());
    CHECK(same_world_pos(plan.final_dest, axis::WorldPos{0, 30, 0}));
}

TEST_CASE("3D movement target helpers map checkpoint and self delta in XYZ") {
    const auto self = make_test_asset(
        {{"default", make_single_path_animation(0, 0, 0)}},
        "default",
        10,
        5,
        20);
    const auto target = make_test_asset(
        {{"default", make_single_path_animation(0, 0, 0)}},
        "default",
        13,
        7,
        16);

    REQUIRE(self != nullptr);
    REQUIRE(target != nullptr);

    const axis::WorldPos checkpoint = animation_update::movement_targets::world_checkpoint_3d(*target);
    const axis::WorldPos delta = animation_update::movement_targets::world_delta_to_checkpoint_3d(*self, checkpoint);

    const axis::WorldPos expected_checkpoint{ target->world_x(), target->world_y(), target->world_z() };
    const axis::WorldPos expected_delta{
        target->world_x() - self->world_x(),
        target->world_y() - self->world_y(),
        target->world_z() - self->world_z()
    };

    CHECK(same_world_pos(checkpoint, expected_checkpoint));
    CHECK(same_world_pos(delta, expected_delta));
}

TEST_CASE("auto_move_3d_relative keeps 3D plan isolated and triggers fallback mode") {
    auto asset = make_test_asset(
        {
            {"default", make_single_path_animation(0, 0, 0)},
            {"away", make_single_path_animation(-10, 0, 0)},
        },
        "default");
    REQUIRE(asset != nullptr);

    AnimationUpdate updater(asset.get(), nullptr);
    asset->needs_target = true;

    updater.auto_move_3d_relative(axis::WorldPos{5, -3, 7});

    CHECK(updater.current_plan_mode() == AnimationUpdate::ActivePlanMode::None);
    CHECK(updater.current_plan_3d() != nullptr);
    CHECK(updater.current_plan_3d()->strides.empty());
    CHECK_FALSE(asset->needs_target);
    CHECK_FALSE(asset->target_reached);
}

TEST_CASE("GetBestPath3D preserves ordered multi-checkpoint progress") {
    const auto asset = make_test_asset(
        {
            {"default", make_single_path_animation(0, 0, 0)},
            {"step", make_single_path_animation(4, 2, 0)},
        },
        "default");

    REQUIRE(asset != nullptr);

    const std::vector<axis::WorldPos> checkpoints{
        axis::WorldPos{4, 2, 0},
        axis::WorldPos{8, 4, 0}
    };

    GetBestPath3D planner;
    const Plan3D plan = planner(*asset, checkpoints, 0, vibble::grid::global_grid());

    CHECK(plan.sanitized_checkpoints.size() == checkpoints.size());
    REQUIRE(plan.strides.size() >= 2);
    CHECK(plan.strides[0].animation_id == "step");
    CHECK(same_world_pos(plan.final_dest, checkpoints.back()));
}

TEST_CASE("GetBestPath3D no-progress plan returns empty strides") {
    const auto asset = make_test_asset(
        {
            {"default", make_single_path_animation(0, 0, 0)},
            {"away", make_single_path_animation(-10, 0, 0)},
        },
        "default");

    REQUIRE(asset != nullptr);

    GetBestPath3D planner;
    const Plan3D plan = planner(*asset, {axis::WorldPos{20, 0, 0}}, 0, vibble::grid::global_grid());

    CHECK(plan.strides.empty());
}

TEST_CASE("GetBestPath3D reached threshold uses full XYZ distance") {
    const auto asset = make_test_asset(
        {
            {"default", make_single_path_animation(0, 0, 0)},
            {"up", make_single_path_animation(0, 2, 0)},
        },
        "default");

    REQUIRE(asset != nullptr);

    GetBestPath3D planner;

    const Plan3D within_threshold = planner(*asset, {axis::WorldPos{0, 3, 0}}, 4, vibble::grid::global_grid());
    CHECK(within_threshold.strides.empty());

    const Plan3D outside_threshold = planner(*asset, {axis::WorldPos{0, 6, 0}}, 4, vibble::grid::global_grid());
    CHECK_FALSE(outside_threshold.strides.empty());
}

TEST_CASE("2D GetBestPath behavior remains unchanged") {
    const auto asset = make_test_asset(
        {
            {"default", make_single_path_animation(0, 0, 0)},
            {"a_left", make_single_path_animation(-10, 0, 0)},
            {"b_right", make_single_path_animation(10, 0, 0)},
        },
        "default");

    REQUIRE(asset != nullptr);

    GetBestPath planner_2d;
    const Plan plan = planner_2d(*asset, {SDL_Point{30, 0}}, 0, vibble::grid::global_grid());

    REQUIRE_FALSE(plan.strides.empty());
    CHECK(plan.strides.front().animation_id == "b_right");
    CHECK(plan.final_dest.x > asset->world_x());
}

TEST_CASE("auto_move applies global retry cooldown after zero-target no-stride planning") {
    auto asset = make_test_asset(
        {
            {"default", make_single_path_animation(0, 0, 0)},
            {"a_left", make_single_path_animation(-8, 0, 0)},
            {"b_right", make_single_path_animation(8, 0, 0)},
        },
        "default");
    REQUIRE(asset != nullptr);

    AnimationUpdate updater(asset.get(), nullptr);
    asset->needs_target = true;

    updater.auto_move(std::vector<SDL_Point>{}, 0, std::nullopt, true);
    CHECK(updater.current_plan_mode() == AnimationUpdate::ActivePlanMode::None);
    CHECK(asset->needs_target);

    updater.auto_move(std::vector<SDL_Point>{SDL_Point{32, 0}}, 0, std::nullopt, true);
    CHECK(updater.current_plan_mode() == AnimationUpdate::ActivePlanMode::None);
    CHECK(asset->needs_target);

    updater.auto_move(std::vector<SDL_Point>{SDL_Point{32, 0}}, 0, std::nullopt, true);
    updater.auto_move(std::vector<SDL_Point>{SDL_Point{32, 0}}, 0, std::nullopt, true);
    updater.auto_move(std::vector<SDL_Point>{SDL_Point{32, 0}}, 0, std::nullopt, true);

    updater.auto_move(std::vector<SDL_Point>{SDL_Point{32, 0}}, 0, std::nullopt, true);
    const bool resumed_with_plan_or_fallback =
        updater.current_plan_mode() == AnimationUpdate::ActivePlanMode::Plan2D ||
        updater.current_plan_mode() == AnimationUpdate::ActivePlanMode::None;
    CHECK(resumed_with_plan_or_fallback);
    CHECK_FALSE(asset->needs_target);
}


TEST_CASE("GetBestPath3D fallback rejects collision-blocked options") {
    const auto asset = make_test_asset(
        {
            {"default", make_single_path_animation(0, 0, 0)},
            {"right", make_single_path_animation(10, 0, 0)},
        },
        "default");

    REQUIRE(asset != nullptr);

    auto blocker_info = std::make_shared<AssetInfo>("blocker_3d_asset");
    Area blocker_area("blocker_3d_area", 0);
    Asset blocker(blocker_info,
                  blocker_area,
                  SDL_Point{8, 0},
                  0,
                  std::string{},
                  std::string{},
                  0);
    blocker.move_to_world_position(8, 0, 0, 0);

    std::vector<Area::Point> points{
        SDL_Point{4, -4}, SDL_Point{14, -4}, SDL_Point{14, 4}, SDL_Point{4, 4}
    };
    Assets::FrameCollisionEntry blocked_entry;
    blocked_entry.asset = &blocker;
    blocked_entry.area = Area("blocked_3d", points, 0);
    blocked_entry.world_center = blocked_entry.area.get_center();
    blocked_entry.bottom_middle = world::GridPoint::make_virtual(blocked_entry.world_center.x,
                                                                  0,
                                                                  blocked_entry.world_center.y,
                                                                  0);

    CollisionQueryContext context;
    context.loaded = true;
    context.entries.push_back(&blocked_entry);

    GetBestPath3D planner;
    const Plan3D plan = planner(*asset, {axis::WorldPos{20, 0, 0}}, 0, vibble::grid::global_grid(), &context);

    CHECK(plan.strides.empty());
}
