#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>

#include "rendering/render/sink_clip.hpp"

namespace {

SDL_Vertex make_vertex(float x, float y, float u, float v) {
    SDL_Vertex out{};
    out.position = SDL_FPoint{x, y};
    out.tex_coord = SDL_FPoint{u, v};
    out.color = SDL_FColor{1.0f, 1.0f, 1.0f, 1.0f};
    return out;
}

float polygon_area(const render_sink::ClipResult& clip) {
    if (!clip.valid || clip.vertex_count < 3) {
        return 0.0f;
    }
    float area2 = 0.0f;
    for (int i = 0; i < clip.vertex_count; ++i) {
        const SDL_FPoint a = clip.vertices[i].position;
        const SDL_FPoint b = clip.vertices[(i + 1) % clip.vertex_count].position;
        area2 += a.x * b.y - b.x * a.y;
    }
    return std::fabs(area2) * 0.5f;
}

} // namespace

TEST_CASE("sink clip clips rotated quad against a horizontal screen-space line") {
    const SDL_Vertex quad[4]{
        make_vertex(0.0f, -20.0f, 0.5f, 0.0f),
        make_vertex(20.0f, 0.0f, 1.0f, 0.5f),
        make_vertex(0.0f, 20.0f, 0.5f, 1.0f),
        make_vertex(-20.0f, 0.0f, 0.0f, 0.5f)};

    const render_sink::ClipResult clip =
        render_sink::clip_quad_against_horizontal_sink_line(quad, 0.0f);

    REQUIRE(clip.valid);
    CHECK(clip.clipped);
    CHECK_FALSE(clip.fully_clipped);
    for (int i = 0; i < clip.vertex_count; ++i) {
        CHECK(clip.vertices[i].position.y <= doctest::Approx(0.0f).epsilon(1e-4));
    }
}

TEST_CASE("sink clip applies no clipping when object is far above the sink line") {
    const SDL_Vertex quad[4]{
        make_vertex(-10.0f, -10.0f, 0.0f, 0.0f),
        make_vertex(10.0f, -10.0f, 1.0f, 0.0f),
        make_vertex(10.0f, 10.0f, 1.0f, 1.0f),
        make_vertex(-10.0f, 10.0f, 0.0f, 1.0f)};

    const render_sink::ClipResult clip =
        render_sink::clip_quad_against_horizontal_sink_line(quad, 100.0f);

    REQUIRE(clip.valid);
    CHECK_FALSE(clip.clipped);
    CHECK(clip.vertex_count == 4);
    CHECK(clip.index_count == 6);
}

TEST_CASE("sink clip increases burial as sink line moves upward") {
    const SDL_Vertex quad[4]{
        make_vertex(-10.0f, -10.0f, 0.0f, 0.0f),
        make_vertex(10.0f, -10.0f, 1.0f, 0.0f),
        make_vertex(10.0f, 10.0f, 1.0f, 1.0f),
        make_vertex(-10.0f, 10.0f, 0.0f, 1.0f)};

    const render_sink::ClipResult baseline =
        render_sink::clip_quad_against_horizontal_sink_line(quad, 0.0f);
    const render_sink::ClipResult more_buried =
        render_sink::clip_quad_against_horizontal_sink_line(quad, -5.0f);

    REQUIRE(baseline.valid);
    REQUIRE(more_buried.valid);
    CHECK(more_buried.clipped);
    CHECK(polygon_area(more_buried) < polygon_area(baseline));
}

TEST_CASE("sink clip yields multi-triangle geometry when a single corner is cut") {
    const SDL_Vertex quad[4]{
        make_vertex(0.0f, -20.0f, 0.5f, 0.0f),
        make_vertex(20.0f, 0.0f, 1.0f, 0.5f),
        make_vertex(0.0f, 20.0f, 0.5f, 1.0f),
        make_vertex(-20.0f, 0.0f, 0.0f, 0.5f)};

    const render_sink::ClipResult clip =
        render_sink::clip_quad_against_horizontal_sink_line(quad, 5.0f);

    REQUIRE(clip.valid);
    CHECK(clip.clipped);
    CHECK(clip.vertex_count == 5);
    CHECK(clip.index_count == 9);
}

TEST_CASE("sink clip keeps full quad for no-rotation and non-negative burial") {
    const SDL_Vertex quad[4]{
        make_vertex(-16.0f, -12.0f, 0.0f, 0.0f),
        make_vertex(16.0f, -12.0f, 1.0f, 0.0f),
        make_vertex(16.0f, 12.0f, 1.0f, 1.0f),
        make_vertex(-16.0f, 12.0f, 0.0f, 1.0f)};

    const render_sink::ClipResult clip =
        render_sink::clip_quad_against_horizontal_sink_line(quad, 16.0f);

    REQUIRE(clip.valid);
    CHECK_FALSE(clip.clipped);
    CHECK(clip.vertex_count == 4);
    CHECK(clip.index_count == 6);
}
