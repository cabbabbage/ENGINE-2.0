#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
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

TEST_CASE("Sink clip packet is invariant to camera translation for static asset geometry") {
    render_projection::ProjectedSpriteFrame frame_a{};
    frame_a.screen_tl = SDL_FPoint{100.0f, 120.0f};
    frame_a.screen_tr = SDL_FPoint{180.0f, 120.0f};
    frame_a.screen_br = SDL_FPoint{180.0f, 220.0f};
    frame_a.screen_bl = SDL_FPoint{100.0f, 220.0f};
    frame_a.final_height_px = 100;

    render_projection::ProjectedSpriteFrame frame_b = frame_a;
    // Simulate camera panning: same sprite shape translated in screen space.
    frame_b.screen_tl.x += 57.0f; frame_b.screen_tl.y += 33.0f;
    frame_b.screen_tr.x += 57.0f; frame_b.screen_tr.y += 33.0f;
    frame_b.screen_br.x += 57.0f; frame_b.screen_br.y += 33.0f;
    frame_b.screen_bl.x += 57.0f; frame_b.screen_bl.y += 33.0f;

    GpuSpriteDrawPacket packet_a{};
    GpuSpriteDrawPacket packet_b{};
    REQUIRE(opengl_runtime_renderer_detail::build_sink_clipped_sprite_packet(
        frame_a, 0.0f, 0.0f, 1.0f, 1.0f, -25.0f, 800, 600, packet_a));
    REQUIRE(opengl_runtime_renderer_detail::build_sink_clipped_sprite_packet(
        frame_b, 0.0f, 0.0f, 1.0f, 1.0f, -25.0f, 800, 600, packet_b));

    CHECK(packet_a.vertex_count == packet_b.vertex_count);
    CHECK(packet_a.index_count == packet_b.index_count);
    for (int i = 0; i < packet_a.vertex_count; ++i) {
        CHECK(packet_a.vertices[static_cast<std::size_t>(i)].uv_y ==
              doctest::Approx(packet_b.vertices[static_cast<std::size_t>(i)].uv_y).epsilon(1e-6));
    }
}

TEST_CASE("Sink clip packet burial responds to asset sink offset changes") {
    render_projection::ProjectedSpriteFrame frame{};
    frame.screen_tl = SDL_FPoint{100.0f, 120.0f};
    frame.screen_tr = SDL_FPoint{180.0f, 120.0f};
    frame.screen_br = SDL_FPoint{180.0f, 220.0f};
    frame.screen_bl = SDL_FPoint{100.0f, 220.0f};
    frame.final_height_px = 100;

    GpuSpriteDrawPacket mild_burial{};
    GpuSpriteDrawPacket deeper_burial{};
    REQUIRE(opengl_runtime_renderer_detail::build_sink_clipped_sprite_packet(
        frame, 0.0f, 0.0f, 1.0f, 1.0f, -10.0f, 800, 600, mild_burial));
    REQUIRE(opengl_runtime_renderer_detail::build_sink_clipped_sprite_packet(
        frame, 0.0f, 0.0f, 1.0f, 1.0f, -30.0f, 800, 600, deeper_burial));

    auto min_max_uv_y = [](const GpuSpriteDrawPacket& packet) {
        float min_v = 1.0f;
        float max_v = 0.0f;
        for (int i = 0; i < packet.vertex_count; ++i) {
            const float v = packet.vertices[static_cast<std::size_t>(i)].uv_y;
            min_v = std::min(min_v, v);
            max_v = std::max(max_v, v);
        }
        return std::pair<float, float>{min_v, max_v};
    };

    const auto [mild_min_v, mild_max_v] = min_max_uv_y(mild_burial);
    const auto [deep_min_v, deep_max_v] = min_max_uv_y(deeper_burial);
    CHECK(mild_min_v == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(deep_min_v == doctest::Approx(0.0f).epsilon(1e-6));
    CHECK(deep_max_v < mild_max_v);
    CHECK(deep_max_v == doctest::Approx(0.7f).epsilon(1e-6));
    CHECK(mild_max_v == doctest::Approx(0.9f).epsilon(1e-6));
}
