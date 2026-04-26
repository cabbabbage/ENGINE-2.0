#include <doctest/doctest.h>

#include "rendering/render/shader_pipeline_cache.hpp"

TEST_CASE("ShaderPipelineCache returns stable graphics/compute handles and improves hit rate after warmup") {
    ShaderPipelineCache cache;

    ShaderPipelineKey graphics_key{};
    graphics_key.shader_id = "final_compose";
    graphics_key.variant = "dxil";
    graphics_key.color_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    graphics_key.depth_format = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    graphics_key.sample_count = SDL_GPU_SAMPLECOUNT_1;
    graphics_key.render_state_key = 0x1001u;

    auto* graphics_first = cache.get_or_create_graphics_pipeline(
        graphics_key,
        []() { return reinterpret_cast<SDL_GPUGraphicsPipeline*>(0x1ull); });
    REQUIRE(graphics_first != nullptr);

    auto* graphics_second = cache.get_or_create_graphics_pipeline(
        graphics_key,
        []() { return reinterpret_cast<SDL_GPUGraphicsPipeline*>(0x2ull); });
    CHECK(graphics_second == graphics_first);

    ShaderPipelineKey compute_key{};
    compute_key.shader_id = "compute_light_binning";
    compute_key.variant = "dxil";
    compute_key.color_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    compute_key.depth_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    compute_key.sample_count = SDL_GPU_SAMPLECOUNT_1;
    compute_key.render_state_key = 0xC011u;

    auto* compute_first = cache.get_or_create_compute_pipeline(
        compute_key,
        []() { return reinterpret_cast<SDL_GPUComputePipeline*>(0x3ull); });
    REQUIRE(compute_first != nullptr);

    auto* compute_second = cache.get_or_create_compute_pipeline(
        compute_key,
        []() { return reinterpret_cast<SDL_GPUComputePipeline*>(0x4ull); });
    CHECK(compute_second == compute_first);

    CHECK(cache.graphics_pipeline_count() == 1);
    CHECK(cache.compute_pipeline_count() == 1);
    CHECK(cache.hit_rate() > 0.0);

    cache.clear(nullptr);
    CHECK(cache.graphics_pipeline_count() == 0);
    CHECK(cache.compute_pipeline_count() == 0);
}
