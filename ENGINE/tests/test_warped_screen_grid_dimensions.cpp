#include <doctest/doctest.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <vector>

#include "rendering/render/projected_sprite_frame.hpp"
#include "rendering/render/render_depth_policy.hpp"
#include "rendering/render/screen_space_math.hpp"
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

float normalized_depth_axis_sign(float sign) {
    return render_depth::normalize_depth_axis_sign(sign);
}

float world_z_from_depth_offset(float depth_offset, float anchor_world_z, float depth_axis_sign) {
    return render_depth::world_z_from_depth_offset(depth_offset, anchor_world_z, depth_axis_sign);
}

float projected_bottom_edge_length(const render_projection::ProjectedSpriteFrame& frame) {
    return std::hypot(frame.screen_br.x - frame.screen_bl.x,
                      frame.screen_br.y - frame.screen_bl.y);
}

render_projection::WorldPoint3 camera_space_to_world(
    const world::CameraProjectionParams& params,
    double cam_x,
    double cam_y,
    double depth) {
    const double safe_scale = std::max(1e-6, params.meters_scale);
    const double world_m_x = params.position_x +
        params.forward_x * depth + params.right_x * cam_x + params.up_x * cam_y;
    const double world_m_y = params.position_y +
        params.forward_y * depth + params.right_y * cam_x + params.up_y * cam_y;
    const double world_m_z = params.position_z +
        params.forward_z * depth + params.right_z * cam_x + params.up_z * cam_y;
    return render_projection::WorldPoint3{
        static_cast<float>(world_m_x / safe_scale + params.anchor_world_x),
        static_cast<float>(world_m_y / safe_scale + params.anchor_world_y),
        static_cast<float>(world_m_z / safe_scale + params.anchor_world_z),
        std::isfinite(world_m_x) && std::isfinite(world_m_y) && std::isfinite(world_m_z)};
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

    constexpr float kNearDepth = -220.0f;
    constexpr float kCenterDepth = 0.0f;
    constexpr float kFarDepth = 220.0f;

    for (float tilt_deg : tilt_values) {
        grid.set_tilt_override(tilt_deg);
        grid.update();

        const world::CameraProjectionParams params = grid.projection_params();
        const float depth_axis_sign = normalized_depth_axis_sign(static_cast<float>(params.forward_z));
        const float anchor_world_z = static_cast<float>(params.anchor_world_z);
        const SDL_FPoint sample_screen{
            static_cast<float>(params.screen_width) * 0.35f,
            static_cast<float>(params.screen_height) * 0.72f
        };

        const float world_z_fg =
            world_z_from_depth_offset(kNearDepth, anchor_world_z, depth_axis_sign);
        const float world_z_center =
            world_z_from_depth_offset(kCenterDepth, anchor_world_z, depth_axis_sign);
        const float world_z_bg =
            world_z_from_depth_offset(kFarDepth, anchor_world_z, depth_axis_sign);

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

TEST_CASE("Depth guide world-z conversion maps larger depth toward horizon") {
    WarpedScreenGrid grid(1280, 720, make_starting_area());

    const world::CameraProjectionParams params = grid.projection_params();
    const float depth_axis_sign = normalized_depth_axis_sign(static_cast<float>(params.forward_z));
    const float anchor_world_z = static_cast<float>(params.anchor_world_z);
    const SDL_FPoint ground_center = grid.screen_to_map(SDL_Point{params.screen_width / 2, params.screen_height / 2});
    const SDL_FPoint sample_world{ground_center.x, 0.0f};

    const float near_world_z = world_z_from_depth_offset(150.0f, anchor_world_z, depth_axis_sign);
    const float far_world_z = world_z_from_depth_offset(650.0f, anchor_world_z, depth_axis_sign);

    CHECK(render_depth::depth_from_anchor(anchor_world_z, near_world_z) > 0.0);
    CHECK(render_depth::depth_from_anchor(anchor_world_z, far_world_z) > 0.0);

    SDL_FPoint near_screen{};
    SDL_FPoint far_screen{};
    REQUIRE(grid.project_world_point(sample_world, near_world_z, near_screen));
    REQUIRE(grid.project_world_point(sample_world, far_world_z, far_screen));
    CHECK(far_screen.y < near_screen.y);
}

TEST_CASE("WarpedScreenGrid bounds perspective scale before the near camera plane") {
    WarpedScreenGrid grid(1280, 720, make_starting_area());

    const world::CameraProjectionParams params = grid.projection_params();
    REQUIRE(params.min_perspective_depth > params.near_plane);
    REQUIRE(std::isfinite(params.max_perspective_scale));
    REQUIRE(params.max_perspective_scale > 0.0f);

    const double close_depth =
        std::max(params.near_plane * 2.0, params.min_perspective_depth * 0.25);
    REQUIRE(close_depth < params.min_perspective_depth);
    const render_projection::WorldPoint3 close_world =
        camera_space_to_world(params, 0.0, 0.0, close_depth);
    REQUIRE(close_world.valid);

    float close_scale = 0.0f;
    REQUIRE(grid.sample_perspective_scale(
        SDL_FPoint{close_world.x, close_world.y},
        close_world.z,
        close_scale));
    CHECK(close_scale == doctest::Approx(params.max_perspective_scale).epsilon(1e-5));

    const render_projection::WorldPoint3 behind_near =
        camera_space_to_world(params, 0.0, 0.0, params.near_plane * 0.5);
    REQUIRE(behind_near.valid);
    float invalid_scale = 0.0f;
    CHECK_FALSE(grid.sample_perspective_scale(
        SDL_FPoint{behind_near.x, behind_near.y},
        behind_near.z,
        invalid_scale));
}

TEST_CASE("Projected sprite frame stays finite below the lower camera edge") {
    WarpedScreenGrid grid(1280, 720, make_starting_area());

    const world::CameraProjectionParams params = grid.projection_params();
    const double lower_edge_y = static_cast<double>(params.screen_height) * 1.4;
    const auto [ndc_x, ndc_y] = render::screen_space::screen_to_ndc(
        static_cast<double>(params.screen_width) * 0.5,
        lower_edge_y,
        params.screen_width,
        params.screen_height,
        params.screen_zoom,
        params.screen_pan_y_px);
    REQUIRE(std::isfinite(ndc_x));
    REQUIRE(std::isfinite(ndc_y));

    const double close_depth =
        std::max(params.near_plane * 2.0, params.min_perspective_depth * 0.45);
    const double cam_x = ndc_x * params.tan_half_fov_x * close_depth;
    const double cam_y = ndc_y * params.tan_half_fov_y * close_depth;
    const render_projection::WorldPoint3 lower_edge_world =
        camera_space_to_world(params, cam_x, cam_y, close_depth);
    REQUIRE(lower_edge_world.valid);

    float perspective_scale = 0.0f;
    REQUIRE(grid.sample_perspective_scale(
        SDL_FPoint{lower_edge_world.x, lower_edge_world.y},
        lower_edge_world.z,
        perspective_scale));
    CHECK(perspective_scale <= params.max_perspective_scale * 1.001f);

    render_projection::SpriteProjectionInput projection_input{};
    projection_input.world_x = lower_edge_world.x;
    projection_input.world_y = lower_edge_world.y;
    projection_input.world_z = lower_edge_world.z;
    projection_input.perspective_scale = perspective_scale;
    projection_input.frame_width_px = 64;
    projection_input.frame_height_px = 64;
    projection_input.final_width_px = 96;
    projection_input.final_height_px = 96;

    render_projection::ProjectedSpriteFrame frame{};
    REQUIRE(render_projection::build_projected_sprite_frame(grid, projection_input, frame));
    REQUIRE(frame.valid);

    const float width = projected_bottom_edge_length(frame);
    REQUIRE(std::isfinite(width));
    CHECK(width < static_cast<float>(params.screen_width) * 0.25f);
    CHECK(width == doctest::Approx(96.0f).epsilon(0.15));
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
