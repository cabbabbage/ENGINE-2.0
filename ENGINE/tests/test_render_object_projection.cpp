#include <doctest/doctest.h>

#include <limits>

#include "rendering/render/projected_sprite_frame.hpp"
#include "rendering/render/render_object.hpp"
#include "rendering/render/render_object_projection.hpp"

namespace {
RenderObject make_base_object() {
    RenderObject obj{};
    obj.texture = reinterpret_cast<SDL_Texture*>(0x1);
    obj.screen_rect = SDL_Rect{320, 180, 96, 64};
    obj.texture_w = 48;
    obj.texture_h = 32;
    obj.has_texture_size = true;
    obj.flip = SDL_FLIP_NONE;
    return obj;
}

void expect_common(const render_projection::SpriteProjectionInput& input,
                   int world_x,
                   int world_y,
                   float world_z,
                   float perspective,
                   int final_w,
                   int final_h) {
    CHECK(input.world_x == doctest::Approx(static_cast<float>(world_x)));
    CHECK(input.world_y == doctest::Approx(static_cast<float>(world_y)));
    CHECK(input.world_z == doctest::Approx(world_z));
    CHECK(input.perspective_scale == doctest::Approx(perspective));
    CHECK(input.final_width_px == final_w);
    CHECK(input.final_height_px == final_h);
}
}

TEST_CASE("render object projection input: normal asset") {
    RenderObject obj = make_base_object();

    render_projection::SpriteProjectionInput input{};
    const bool ok = render_projection::assemble_render_object_projection_input(
        obj, 2.5f, 12.0f, input);

    CHECK(ok);
    expect_common(input, 320, 180, 12.0f, 2.5f, 96, 64);
    CHECK(input.frame_width_px == 48);
    CHECK(input.frame_height_px == 32);
}

TEST_CASE("render object projection input: anchored/src-rect variant") {
    RenderObject obj = make_base_object();
    obj.has_src_rect = true;
    obj.src_rect = SDL_Rect{16, 24, 20, 10};
    obj.flip = SDL_FLIP_HORIZONTAL;

    render_projection::SpriteProjectionInput input{};
    const bool ok = render_projection::assemble_render_object_projection_input(
        obj, 1.0f, -8.0f, input);

    CHECK(ok);
    expect_common(input, 320, 180, -8.0f, 1.0f, 96, 64);
    CHECK(input.frame_width_px == 20);
    CHECK(input.frame_height_px == 10);
    CHECK(input.flip == SDL_FLIP_HORIZONTAL);
}

TEST_CASE("render object projection input: preserves float world anchor values") {
    RenderObject obj = make_base_object();
    obj.world_anchor_x = 320.375f;
    obj.world_anchor_y = 180.625f;

    render_projection::SpriteProjectionInput input{};
    const bool ok = render_projection::assemble_render_object_projection_input(
        obj, 1.25f, 3.5f, input);

    CHECK(ok);
    CHECK(input.world_x == doctest::Approx(320.375f).epsilon(1e-6));
    CHECK(input.world_y == doctest::Approx(180.625f).epsilon(1e-6));
    CHECK(input.world_z == doctest::Approx(3.5f).epsilon(1e-6));
}

TEST_CASE("render object projection input: tiled/map-wide scale sanitization") {
    RenderObject obj = make_base_object();
    obj.screen_rect = SDL_Rect{-2048, -1024, 8192, 4096};
    obj.texture_w = 512;
    obj.texture_h = 256;

    render_projection::SpriteProjectionInput input{};
    const bool ok = render_projection::assemble_render_object_projection_input(
        obj, -std::numeric_limits<float>::infinity(), 0.0f, input);

    CHECK(ok);
    expect_common(input, -2048, -1024, 0.0f, 1.0f, 8192, 4096);
    CHECK(input.frame_width_px == 512);
    CHECK(input.frame_height_px == 256);
}

TEST_CASE("render object projection input: boundary exclusion / invalid dimensions") {
    RenderObject obj = make_base_object();
    obj.screen_rect.w = 0;

    render_projection::SpriteProjectionInput input{};
    const bool ok = render_projection::assemble_render_object_projection_input(
        obj, 1.0f, 0.0f, input);

    CHECK_FALSE(ok);
}

TEST_CASE("projected sprite frame UV sampling follows render triangle split") {
    render_projection::ProjectedSpriteFrame frame{};
    frame.screen_tl = SDL_FPoint{10.0f, 20.0f};
    frame.screen_tr = SDL_FPoint{70.0f, 25.0f};
    frame.screen_br = SDL_FPoint{90.0f, 90.0f};
    frame.screen_bl = SDL_FPoint{5.0f, 80.0f};

    const SDL_FPoint upper = frame.sample_screen_from_uv(SDL_FPoint{0.8f, 0.3f}); // u >= v => TL/TR/BR
    CHECK(upper.x == doctest::Approx(64.0f).epsilon(1e-6));
    CHECK(upper.y == doctest::Approx(43.5f).epsilon(1e-6));

    const SDL_FPoint lower = frame.sample_screen_from_uv(SDL_FPoint{0.25f, 0.7f}); // v > u => TL/BR/BL
    CHECK(lower.x == doctest::Approx(27.75f).epsilon(1e-6));
    CHECK(lower.y == doctest::Approx(64.5f).epsilon(1e-6));
}

TEST_CASE("projected sprite frame UV sampling stays continuous on diagonal") {
    render_projection::ProjectedSpriteFrame frame{};
    frame.screen_tl = SDL_FPoint{10.0f, 20.0f};
    frame.screen_tr = SDL_FPoint{70.0f, 25.0f};
    frame.screen_br = SDL_FPoint{90.0f, 90.0f};
    frame.screen_bl = SDL_FPoint{5.0f, 80.0f};

    const SDL_FPoint diagonal = frame.sample_screen_from_uv(SDL_FPoint{0.4f, 0.4f});
    CHECK(diagonal.x == doctest::Approx(42.0f).epsilon(1e-6));
    CHECK(diagonal.y == doctest::Approx(48.0f).epsilon(1e-6));
}
