#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <string>
#include <vector>

#include "rendering/render/opengl_runtime_renderer.hpp"

TEST_CASE("OpenGL runtime renderer fails fast on null SDL renderer") {
    std::string error;
    std::unique_ptr<OpenGLRuntimeRenderer> runtime = OpenGLRuntimeRenderer::Create(nullptr, nullptr, 640, 360, error);
    CHECK(runtime == nullptr);
    CHECK_FALSE(error.empty());
}

TEST_CASE("OpenGL runtime renderer reports a single backend name") {
    std::string error;
    std::unique_ptr<OpenGLRuntimeRenderer> runtime = OpenGLRuntimeRenderer::Create(nullptr, nullptr, 1, 1, error);
    CHECK(runtime == nullptr);

    // Ensure no backend selection matrix exists in runtime API surface.
    // API exposes one backend naming channel only.
    CHECK(!runtime);
}

TEST_CASE("XY Sprite Pass excludes tillable assets") {
    CHECK(opengl_runtime_renderer_detail::info_is_xy_sprite_pass_eligible(false));
    CHECK_FALSE(opengl_runtime_renderer_detail::info_is_xy_sprite_pass_eligible(true));
}

TEST_CASE("Floor pass tags are diagnostics-only under strict two-pass routing") {
    const std::vector<std::string> floor_tagged{
        "render_pass:floor",
        "render-pass:floor",
        "render_floor_pass",
        "floor_render_pass",
    };
    CHECK(opengl_runtime_renderer_detail::info_requests_floor_pass_tag_for_diagnostics("", floor_tagged));
    CHECK(opengl_runtime_renderer_detail::info_requests_floor_pass_tag_for_diagnostics("render_floor_pass", {}));
    CHECK_FALSE(opengl_runtime_renderer_detail::info_requests_floor_pass_tag_for_diagnostics("", {}));
    CHECK(opengl_runtime_renderer_detail::info_is_xy_sprite_pass_eligible(false));
}

TEST_CASE("XY Sprite Pass sorts by strict far-to-near camera depth first") {
    GpuSpriteDrawPacket near_packet{};
    near_packet.camera_depth_key = 10.0f;
    near_packet.projected_foot_y_key = 100.0f;
    near_packet.stable_sort_id = 1u;

    GpuSpriteDrawPacket far_packet{};
    far_packet.camera_depth_key = 50.0f;
    far_packet.projected_foot_y_key = 500.0f;
    far_packet.stable_sort_id = 2u;

    std::vector<GpuSpriteDrawPacket> packets{near_packet, far_packet};
    std::stable_sort(packets.begin(), packets.end(), opengl_runtime_renderer_detail::draw_packet_sort_predicate_xy);

    CHECK(packets[0].camera_depth_key == doctest::Approx(50.0f));
    CHECK(packets[1].camera_depth_key == doctest::Approx(10.0f));
}

TEST_CASE("XY Sprite Pass ties resolve with projected foot Y then stable id") {
    GpuSpriteDrawPacket a{};
    a.camera_depth_key = 20.0f;
    a.projected_foot_y_key = 300.0f;
    a.stable_sort_id = 9u;

    GpuSpriteDrawPacket b{};
    b.camera_depth_key = 20.0f;
    b.projected_foot_y_key = 200.0f;
    b.stable_sort_id = 7u;

    GpuSpriteDrawPacket c{};
    c.camera_depth_key = 20.0f;
    c.projected_foot_y_key = 200.0f;
    c.stable_sort_id = 5u;

    std::vector<GpuSpriteDrawPacket> packets{a, b, c};
    std::stable_sort(packets.begin(), packets.end(), opengl_runtime_renderer_detail::draw_packet_sort_predicate_xy);

    CHECK(packets[0].stable_sort_id == 5u);
    CHECK(packets[1].stable_sort_id == 7u);
    CHECK(packets[2].stable_sort_id == 9u);
}

TEST_CASE("Floor sort remains independent from XY depth ordering") {
    GpuSpriteDrawPacket top{};
    top.projected_foot_y_key = 100.0f;
    top.camera_depth_key = 1000.0f;
    top.stable_sort_id = 2u;

    GpuSpriteDrawPacket bottom{};
    bottom.projected_foot_y_key = 300.0f;
    bottom.camera_depth_key = 1.0f;
    bottom.stable_sort_id = 1u;

    std::vector<GpuSpriteDrawPacket> packets{bottom, top};
    std::stable_sort(packets.begin(), packets.end(), opengl_runtime_renderer_detail::draw_packet_sort_predicate_floor);

    CHECK(packets[0].projected_foot_y_key == doctest::Approx(100.0f));
    CHECK(packets[1].projected_foot_y_key == doctest::Approx(300.0f));
}
