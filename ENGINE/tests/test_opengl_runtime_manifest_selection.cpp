#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <filesystem>

#include "rendering/render/render.hpp"

TEST_CASE("OpenGL runtime manifest path uses single authoritative default when env is unset") {
    SDL_Environment* env = SDL_GetEnvironment();
    REQUIRE(env != nullptr);

    CHECK(SDL_SetEnvironmentVariable(env, "VIBBLE_OPENGL_SHADER_MANIFEST", "", true));

    const std::filesystem::path manifest = render_internal::opengl_runtime_shader_manifest_path();
    CHECK(manifest == std::filesystem::path("ENGINE/runtime/rendering/shaders/runtime_shaders.json"));
}

TEST_CASE("OpenGL runtime manifest path uses configured override directly without fallback chain") {
    SDL_Environment* env = SDL_GetEnvironment();
    REQUIRE(env != nullptr);

    const char* configured_path = "/tmp/custom_runtime_manifest.json";
    CHECK(SDL_SetEnvironmentVariable(env, "VIBBLE_OPENGL_SHADER_MANIFEST", configured_path, true));

    const std::filesystem::path manifest = render_internal::opengl_runtime_shader_manifest_path();
    CHECK(manifest == std::filesystem::path(configured_path));
}