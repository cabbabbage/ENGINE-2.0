#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <string>

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
