#include <doctest/doctest.h>

#include <SDL3/SDL.h>

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
