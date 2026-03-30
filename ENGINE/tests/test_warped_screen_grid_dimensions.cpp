#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <vector>

#include "core/manifest/depth_cue_settings.hpp"
#include "rendering/render/projected_sprite_frame.hpp"
#include "rendering/render/warped_screen_grid.hpp"

namespace {
Area make_starting_area() {
    std::vector<SDL_Point> corners{
        SDL_Point{-100, -100},
        SDL_Point{100, -100},
        SDL_Point{100, 100},
        SDL_Point{-100, 100}};
    return Area("starting", corners, 0);
}

float projected_bottom_edge_length(const render_projection::ProjectedSpriteFrame& frame) {
    return std::hypot(frame.screen_br.x - frame.screen_bl.x,
                      frame.screen_br.y - frame.screen_bl.y);
}

SDL_FPoint legacy_anchor_screen_point(const WarpedScreenGrid& grid,
                                      const render_projection::SpriteProjectionInput& input) {
    const float safe_perspective =
        (std::isfinite(input.perspective_scale) && input.perspective_scale > 0.0f)
            ? input.perspective_scale
            : 1.0f;
    const float legacy_half_width =
        static_cast<float>(input.final_width_px) / safe_perspective * 0.5f;
    SDL_FPoint left{};
    SDL_FPoint right{};
    const bool projected =
        grid.project_world_point(SDL_FPoint{input.world_x - legacy_half_width, input.world_y},
                                 input.world_z,
                                 left) &&
        grid.project_world_point(SDL_FPoint{input.world_x + legacy_half_width, input.world_y},
                                 input.world_z,
                                 right);
    REQUIRE(projected);
    return SDL_FPoint{
        0.5f * (left.x + right.x),
        0.5f * (left.y + right.y)};
}
}  // namespace

TEST_CASE("WarpedScreenGrid updates projection dimensions immediately on resize") {
    WarpedScreenGrid grid(1280, 720, make_starting_area());

    const world::CameraProjectionParams initial = grid.projection_params();
    CHECK(initial.screen_width == 1280);
    CHECK(initial.screen_height == 720);

    grid.set_screen_dimensions(1600, 900);
    const world::CameraProjectionParams resized = grid.projection_params();
    CHECK(resized.screen_width == 1600);
    CHECK(resized.screen_height == 900);
    CHECK(resized.state_version != initial.state_version);
}

TEST_CASE("WarpedScreenGrid updates visible bounds immediately on resize") {
    WarpedScreenGrid grid(1280, 720, make_starting_area());

    const WarpedScreenGrid::GridBounds initial_bounds = grid.get_bounds();
    const float initial_width = initial_bounds.right - initial_bounds.left;
    const float initial_height = initial_bounds.bottom - initial_bounds.top;
    REQUIRE(initial_width > 0.0f);
    REQUIRE(initial_height > 0.0f);

    grid.set_screen_dimensions(1600, 900);
    const WarpedScreenGrid::GridBounds resized_bounds = grid.get_bounds();
    const float resized_width = resized_bounds.right - resized_bounds.left;
    const float resized_height = resized_bounds.bottom - resized_bounds.top;

    CHECK(resized_width == doctest::Approx(initial_width * (1600.0f / 1280.0f)).epsilon(1e-5));
    CHECK(resized_height == doctest::Approx(initial_height * (900.0f / 720.0f)).epsilon(1e-5));
}

TEST_CASE("WarpedScreenGrid screen-to-world depth plane roundtrip is stable") {
    WarpedScreenGrid grid(1280, 720, make_starting_area());

    const world::CameraProjectionParams params = grid.projection_params();
    const SDL_FPoint source_screen{
        static_cast<float>(params.screen_width) * 0.35f,
        static_cast<float>(params.screen_height) * 0.72f
    };
    const float depth_sign = (params.forward_z >= 0.0) ? 1.0f : -1.0f;
    const float target_world_z = static_cast<float>(params.anchor_world_z) + depth_sign * 300.0f;

    render_projection::WorldPoint3 world_point{};
    REQUIRE(grid.screen_to_world_on_depth_plane(source_screen, target_world_z, world_point));
    REQUIRE(world_point.valid);

    SDL_FPoint reprojected_screen{};
    REQUIRE(grid.project_world_point(SDL_FPoint{world_point.x, world_point.y}, world_point.z, reprojected_screen));

    CHECK(reprojected_screen.x == doctest::Approx(source_screen.x).epsilon(1e-4));
    CHECK(reprojected_screen.y == doctest::Approx(source_screen.y).epsilon(1e-4));
}

TEST_CASE("WarpedScreenGrid depth guides remain world-anchored and ordered across tilt changes") {
    WarpedScreenGrid grid(1280, 720, make_starting_area());

    const std::array<float, 2> tilt_values{35.0f, 75.0f};

    constexpr float kForegroundDepth = -220.0f;
    constexpr float kCenterDepth = 0.0f;
    constexpr float kBackgroundDepth = 220.0f;

    for (float tilt_deg : tilt_values) {
        grid.set_tilt_override(tilt_deg);
        grid.update();

        const world::CameraProjectionParams params = grid.projection_params();
        const float depth_axis_sign = depth_cue::depth_axis_sign_from_forward_z(params.forward_z);
        const float anchor_world_z = static_cast<float>(params.anchor_world_z);
        const SDL_FPoint sample_screen{
            static_cast<float>(params.screen_width) * 0.35f,
            static_cast<float>(params.screen_height) * 0.72f
        };

        const float world_z_fg =
            depth_cue::world_z_from_depth_offset(kForegroundDepth, anchor_world_z, depth_axis_sign);
        const float world_z_center =
            depth_cue::world_z_from_depth_offset(kCenterDepth, anchor_world_z, depth_axis_sign);
        const float world_z_bg =
            depth_cue::world_z_from_depth_offset(kBackgroundDepth, anchor_world_z, depth_axis_sign);

        const float depth_sign = (params.forward_z >= 0.0) ? 1.0f : -1.0f;
        const float roundtrip_world_z = anchor_world_z + depth_sign * 300.0f;

        render_projection::WorldPoint3 roundtrip_world_point{};
        REQUIRE(grid.screen_to_world_on_depth_plane(sample_screen, roundtrip_world_z, roundtrip_world_point));
        REQUIRE(roundtrip_world_point.valid);

        SDL_FPoint roundtrip_reprojected{};
        REQUIRE(grid.project_world_point(
            SDL_FPoint{roundtrip_world_point.x, roundtrip_world_point.y},
            roundtrip_world_point.z,
            roundtrip_reprojected));
        CHECK(roundtrip_reprojected.x == doctest::Approx(sample_screen.x).epsilon(1e-4));
        CHECK(roundtrip_reprojected.y == doctest::Approx(sample_screen.y).epsilon(1e-4));

        const SDL_FPoint ground_center = grid.screen_to_map(SDL_Point{params.screen_width / 2, params.screen_height / 2});
        const SDL_FPoint sample_world{ground_center.x, 0.0f};

        SDL_FPoint fg_screen{};
        SDL_FPoint center_screen{};
        SDL_FPoint bg_screen{};
        REQUIRE(grid.project_world_point(sample_world, world_z_fg, fg_screen));
        REQUIRE(grid.project_world_point(sample_world, world_z_center, center_screen));
        REQUIRE(grid.project_world_point(sample_world, world_z_bg, bg_screen));

        CHECK(bg_screen.y < center_screen.y);
        CHECK(center_screen.y < fg_screen.y);
    }
}

TEST_CASE("Projected sprite frame keeps final width stable across depth with fixed perspective source") {
    WarpedScreenGrid grid(1280, 720, make_starting_area());

    const world::CameraProjectionParams params = grid.projection_params();
    const float depth_sign = (params.forward_z >= 0.0) ? 1.0f : -1.0f;
    const float anchor_world_z = static_cast<float>(params.anchor_world_z);
    const float farther_world_z = anchor_world_z + depth_sign * 520.0f;
    const float nearer_world_z = anchor_world_z + depth_sign * 140.0f;

    const SDL_FPoint ground_center =
        grid.screen_to_map(SDL_Point{params.screen_width / 2, params.screen_height / 2});

    render_projection::SpriteProjectionInput projection_input{};
    projection_input.world_x = ground_center.x;
    projection_input.world_y = 0.0f;
    projection_input.perspective_scale = 2.4f; // Emulates parent-forced perspective source.
    projection_input.frame_width_px = 64;
    projection_input.frame_height_px = 64;
    projection_input.final_width_px = 96;
    projection_input.final_height_px = 96;

    render_projection::ProjectedSpriteFrame farther_frame{};
    projection_input.world_z = farther_world_z;
    REQUIRE(render_projection::build_projected_sprite_frame(grid, projection_input, farther_frame));
    REQUIRE(farther_frame.valid);

    render_projection::ProjectedSpriteFrame nearer_frame{};
    projection_input.world_z = nearer_world_z;
    REQUIRE(render_projection::build_projected_sprite_frame(grid, projection_input, nearer_frame));
    REQUIRE(nearer_frame.valid);

    const float farther_width = projected_bottom_edge_length(farther_frame);
    const float nearer_width = projected_bottom_edge_length(nearer_frame);
    REQUIRE(std::isfinite(farther_width));
    REQUIRE(std::isfinite(nearer_width));

    CHECK(farther_width == doctest::Approx(96.0f).epsilon(0.08));
    CHECK(nearer_width == doctest::Approx(96.0f).epsilon(0.08));
    CHECK(nearer_width == doctest::Approx(farther_width).epsilon(0.06));
}

TEST_CASE("Projected sprite frame preserves legacy anchor placement while applying local width calibration") {
    WarpedScreenGrid grid(1280, 720, make_starting_area());

    const world::CameraProjectionParams params = grid.projection_params();
    const float depth_sign = (params.forward_z >= 0.0) ? 1.0f : -1.0f;
    const float anchor_world_z = static_cast<float>(params.anchor_world_z);
    const float farther_world_z = anchor_world_z + depth_sign * 520.0f;
    const float nearer_world_z = anchor_world_z + depth_sign * 140.0f;

    const SDL_FPoint ground_center =
        grid.screen_to_map(SDL_Point{params.screen_width / 2, params.screen_height / 2});

    render_projection::SpriteProjectionInput projection_input{};
    projection_input.world_x = ground_center.x;
    projection_input.world_y = 0.0f;
    projection_input.perspective_scale = 2.4f;
    projection_input.frame_width_px = 64;
    projection_input.frame_height_px = 64;
    projection_input.final_width_px = 96;
    projection_input.final_height_px = 96;
    projection_input.anchor_uv = SDL_FPoint{0.2f, 0.35f};

    render_projection::ProjectedSpriteFrame farther_frame{};
    projection_input.world_z = farther_world_z;
    REQUIRE(render_projection::build_projected_sprite_frame(grid, projection_input, farther_frame));
    REQUIRE(farther_frame.valid);
    const SDL_FPoint farther_anchor = farther_frame.sample_screen_from_uv(projection_input.anchor_uv);
    const SDL_FPoint farther_legacy_anchor = legacy_anchor_screen_point(grid, projection_input);
    CHECK(farther_anchor.x == doctest::Approx(farther_legacy_anchor.x).epsilon(1e-4));
    CHECK(farther_anchor.y == doctest::Approx(farther_legacy_anchor.y).epsilon(1e-4));

    render_projection::ProjectedSpriteFrame nearer_frame{};
    projection_input.world_z = nearer_world_z;
    REQUIRE(render_projection::build_projected_sprite_frame(grid, projection_input, nearer_frame));
    REQUIRE(nearer_frame.valid);
    const SDL_FPoint nearer_anchor = nearer_frame.sample_screen_from_uv(projection_input.anchor_uv);
    const SDL_FPoint nearer_legacy_anchor = legacy_anchor_screen_point(grid, projection_input);
    CHECK(nearer_anchor.x == doctest::Approx(nearer_legacy_anchor.x).epsilon(1e-4));
    CHECK(nearer_anchor.y == doctest::Approx(nearer_legacy_anchor.y).epsilon(1e-4));
}
