#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "rendering/render/layer_stack_renderer.hpp"
#include "rendering/render/render_diagnostics.hpp"
#include "rendering/render/render_pipeline_types.hpp"

namespace {

class ScopedSdlVideo {
public:
    ScopedSdlVideo() : initialized_(SDL_InitSubSystem(SDL_INIT_VIDEO)) {}
    ~ScopedSdlVideo() {
        if (initialized_) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }

    bool initialized() const { return initialized_; }

private:
    bool initialized_ = false;
};

class ScopedRenderer {
public:
    ScopedRenderer() {
        if (!video_.initialized()) {
            return;
        }
        window_ = SDL_CreateWindow("gpu_compact_pass_graph_tests", 64, 64, SDL_WINDOW_HIDDEN);
        if (!window_) {
            return;
        }
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (!renderer_) {
            renderer_ = SDL_CreateRenderer(window_, SDL_SOFTWARE_RENDERER);
        }
    }

    ~ScopedRenderer() {
        if (renderer_) {
            SDL_DestroyRenderer(renderer_);
            renderer_ = nullptr;
        }
        if (window_) {
            SDL_DestroyWindow(window_);
            window_ = nullptr;
        }
    }

    SDL_Renderer* get() const { return renderer_; }
    bool ready() const { return renderer_ != nullptr; }

private:
    ScopedSdlVideo video_{};
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
};

SDL_Texture* create_solid_texture(SDL_Renderer* renderer,
                                  int width,
                                  int height,
                                  SDL_Color color) {
    if (!renderer || width <= 0 || height <= 0) {
        return nullptr;
    }
    SDL_Texture* texture = SDL_CreateTexture(renderer,
                                             SDL_PIXELFORMAT_RGBA8888,
                                             SDL_TEXTUREACCESS_TARGET,
                                             width,
                                             height);
    if (!texture) {
        return nullptr;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    if (!SDL_SetRenderTarget(renderer, texture)) {
        SDL_DestroyTexture(texture);
        return nullptr;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderClear(renderer);
    SDL_SetRenderTarget(renderer, previous_target);
    return texture;
}

render_pipeline::GeometryLayerDrawItem make_fullscreen_draw(SDL_Texture* texture,
                                                            int width,
                                                            int height,
                                                            SDL_BlendMode blend_mode = SDL_BLENDMODE_BLEND) {
    render_pipeline::GeometryLayerDrawItem draw{};
    draw.texture = texture;
    draw.blend_mode = blend_mode;
    const SDL_FColor white{1.0f, 1.0f, 1.0f, 1.0f};
    draw.vertices[0] = SDL_Vertex{SDL_FPoint{0.0f, 0.0f}, white, SDL_FPoint{0.0f, 0.0f}};
    draw.vertices[1] = SDL_Vertex{SDL_FPoint{static_cast<float>(width), 0.0f}, white, SDL_FPoint{1.0f, 0.0f}};
    draw.vertices[2] = SDL_Vertex{SDL_FPoint{static_cast<float>(width), static_cast<float>(height)}, white, SDL_FPoint{1.0f, 1.0f}};
    draw.vertices[3] = SDL_Vertex{SDL_FPoint{0.0f, static_cast<float>(height)}, white, SDL_FPoint{0.0f, 1.0f}};
    return draw;
}

render_pipeline::LayerBuildResult make_multilayer_build(const std::array<SDL_Texture*, 4>& textures) {
    render_pipeline::LayerBuildResult build{};
    build.valid = true;
    build.layer_count = 4;
    build.player_layer_index = 1;
    build.non_empty_layers = {0, 1, 2, 3};
    build.layers.resize(4);

    const std::array<double, 4> depth_min{-80.0, -20.0, 10.0, 40.0};
    const std::array<double, 4> depth_max{-40.0, 5.0, 35.0, 90.0};
    const std::array<SDL_FRect, 4> bounds{
        SDL_FRect{0.0f, 0.0f, 31.0f, 31.0f},
        SDL_FRect{32.0f, 0.0f, 31.0f, 31.0f},
        SDL_FRect{0.0f, 32.0f, 31.0f, 31.0f},
        SDL_FRect{32.0f, 32.0f, 31.0f, 31.0f}};
    for (int i = 0; i < 4; ++i) {
        auto& layer = build.layers[static_cast<std::size_t>(i)];
        layer.representative_depth = 0.5 * (depth_min[static_cast<std::size_t>(i)] + depth_max[static_cast<std::size_t>(i)]);
        layer.depth_min = depth_min[static_cast<std::size_t>(i)];
        layer.depth_max = depth_max[static_cast<std::size_t>(i)];
        layer.bounds_min_x = bounds[static_cast<std::size_t>(i)].x;
        layer.bounds_min_y = bounds[static_cast<std::size_t>(i)].y;
        layer.bounds_max_x = bounds[static_cast<std::size_t>(i)].x + bounds[static_cast<std::size_t>(i)].w;
        layer.bounds_max_y = bounds[static_cast<std::size_t>(i)].y + bounds[static_cast<std::size_t>(i)].h;
        layer.draws.push_back(make_fullscreen_draw(textures[static_cast<std::size_t>(i)], 64, 64));
    }

    return build;
}

std::vector<LayerEffectProcessor::RuntimeLight> make_dense_lights() {
    std::vector<LayerEffectProcessor::RuntimeLight> lights;
    lights.reserve(96);
    for (int i = 0; i < 96; ++i) {
        const int x = (i % 12) * 6 + 2;
        const int y = (i / 12) * 6 + 2;
        LayerEffectProcessor::RuntimeLight light{};
        light.stable_light_id = static_cast<std::uint64_t>(1000 + i);
        light.screen_center = SDL_FPoint{static_cast<float>(x), static_cast<float>(y)};
        light.color = SDL_Color{255, static_cast<Uint8>(100 + (i % 120)), static_cast<Uint8>(60 + (i % 150)), 255};
        light.intensity = 0.45f + static_cast<float>(i % 5) * 0.1f;
        light.opacity = 0.85f;
        light.radius_px = 10.0f + static_cast<float>((i % 4) * 3);
        light.radius_world = 8.0f + static_cast<float>(i % 6);
        light.falloff = 1.1f + static_cast<float>(i % 4) * 0.35f;
        light.world_z = -90.0f + static_cast<float>((i % 16) * 12);
        light.has_floor_projection = true;
        light.floor_world_x = static_cast<float>(x);
        light.floor_world_z = static_cast<float>(y);
        light.world_height = static_cast<float>((i % 5) * 8);
        light.floor_screen_center = light.screen_center;
        lights.push_back(light);
    }
    return lights;
}

bool read_pixel(SDL_Renderer* renderer, SDL_Texture* texture, int x, int y, SDL_Color& out_color) {
    out_color = SDL_Color{0, 0, 0, 0};
    if (!renderer || !texture || x < 0 || y < 0) {
        return false;
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    if (!SDL_SetRenderTarget(renderer, texture)) {
        return false;
    }
    const SDL_Rect rect{x, y, 1, 1};
    SDL_Surface* captured = SDL_RenderReadPixels(renderer, &rect);
    SDL_SetRenderTarget(renderer, previous_target);
    if (!captured || !captured->pixels) {
        if (captured) {
            SDL_DestroySurface(captured);
        }
        return false;
    }

    const SDL_PixelFormatDetails* format = SDL_GetPixelFormatDetails(captured->format);
    if (!format) {
        SDL_DestroySurface(captured);
        return false;
    }

    const Uint32 pixel = *static_cast<const Uint32*>(captured->pixels);
    SDL_GetRGBA(pixel,
                format,
                SDL_GetSurfacePalette(captured),
                &out_color.r,
                &out_color.g,
                &out_color.b,
                &out_color.a);
    SDL_DestroySurface(captured);
    return true;
}

bool supports_alpha_preserving_pipeline_blends() {
    const SDL_BlendMode alpha_copy = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ZERO,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDFACTOR_ZERO,
        SDL_BLENDOPERATION_ADD);

    const SDL_BlendMode add_rgb_preserve_alpha = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_SRC_ALPHA,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ZERO,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD);

    const SDL_BlendMode alpha_masked_mul = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_DST_COLOR,
        SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ZERO,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDOPERATION_ADD);

    return alpha_copy != SDL_BLENDMODE_INVALID &&
           add_rgb_preserve_alpha != SDL_BLENDMODE_INVALID &&
           alpha_masked_mul != SDL_BLENDMODE_INVALID;
}

} // namespace

TEST_CASE("GPU compact render path reduces fanout passes and CPU light-mask work") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());
    if (!supports_alpha_preserving_pipeline_blends()) {
        return;
    }

    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    std::array<SDL_Texture*, 4> textures{
        create_solid_texture(renderer, 64, 64, SDL_Color{180, 120, 100, 180}),
        create_solid_texture(renderer, 64, 64, SDL_Color{120, 180, 100, 190}),
        create_solid_texture(renderer, 64, 64, SDL_Color{100, 120, 220, 160}),
        create_solid_texture(renderer, 64, 64, SDL_Color{220, 140, 120, 150}),
    };
    for (SDL_Texture* texture : textures) {
        REQUIRE(texture != nullptr);
    }

    const render_pipeline::LayerBuildResult build = make_multilayer_build(textures);
    const std::vector<LayerEffectProcessor::RuntimeLight> lights = make_dense_lights();

    LayerStackRenderer stack(renderer);
    stack.set_output_dimensions(64, 64);

    render_diagnostics::begin_frame();
    const render_pipeline::LayerRenderResult legacy = stack.render(build, lights, true, true);
    render_diagnostics::end_frame();
    const RenderFrameStats legacy_stats = render_diagnostics::current_frame_stats();
    REQUIRE(legacy.valid);

    render_diagnostics::begin_frame();
    stack.build_gpu_tiled_light_bins(build, lights);
    const render_pipeline::CompactLayerRenderResult compact =
        stack.render_gpu_compact(build, lights, true, true);
    render_diagnostics::end_frame();
    const RenderFrameStats compact_stats = render_diagnostics::current_frame_stats();
    REQUIRE(compact.valid);
    REQUIRE(compact.final_texture != nullptr);

    const std::uint32_t expected_legacy_fanout_min =
        static_cast<std::uint32_t>(build.non_empty_layers.size() * 3u);
    CHECK(compact_stats.render_target_switch_count < expected_legacy_fanout_min);
    CHECK(compact_stats.render_pass_count < expected_legacy_fanout_min);
    CHECK(compact_stats.cpu_light_mask_generation_ms <= 0.10);
    CHECK(compact_stats.cpu_light_mask_generation_ms <= legacy_stats.cpu_light_mask_generation_ms);
    CHECK(compact.compact_stats.tiled_light_evaluations < compact.compact_stats.naive_light_evaluations);
    CHECK(compact.compact_stats.aggregated_light_count > 0);

    for (SDL_Texture* texture : textures) {
        SDL_DestroyTexture(texture);
    }
}

TEST_CASE("GPU compact high-light scenes stay deterministic and tile-culling beats naive baseline") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());

    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    std::array<SDL_Texture*, 4> textures{
        create_solid_texture(renderer, 64, 64, SDL_Color{210, 140, 120, 200}),
        create_solid_texture(renderer, 64, 64, SDL_Color{110, 210, 140, 180}),
        create_solid_texture(renderer, 64, 64, SDL_Color{140, 150, 240, 170}),
        create_solid_texture(renderer, 64, 64, SDL_Color{220, 180, 140, 160}),
    };
    for (SDL_Texture* texture : textures) {
        REQUIRE(texture != nullptr);
    }

    const render_pipeline::LayerBuildResult build = make_multilayer_build(textures);
    const std::vector<LayerEffectProcessor::RuntimeLight> lights = make_dense_lights();
    LayerStackRenderer stack(renderer);
    stack.set_output_dimensions(64, 64);

    stack.build_gpu_tiled_light_bins(build, lights);
    const render_pipeline::CompactLayerRenderResult frame_one =
        stack.render_gpu_compact(build, lights, true, true);
    REQUIRE(frame_one.valid);
    REQUIRE(frame_one.final_texture != nullptr);
    CHECK(frame_one.compact_stats.tiled_light_evaluations <
          frame_one.compact_stats.naive_light_evaluations);

    stack.build_gpu_tiled_light_bins(build, lights);
    const render_pipeline::CompactLayerRenderResult frame_two =
        stack.render_gpu_compact(build, lights, true, true);
    REQUIRE(frame_two.valid);
    REQUIRE(frame_two.final_texture != nullptr);

    SDL_Color a{};
    SDL_Color b{};
    REQUIRE(read_pixel(renderer, frame_one.final_texture, 32, 32, a));
    REQUIRE(read_pixel(renderer, frame_two.final_texture, 32, 32, b));
    CHECK(a.r == b.r);
    CHECK(a.g == b.g);
    CHECK(a.b == b.b);
    CHECK(a.a == b.a);

    for (SDL_Texture* texture : textures) {
        SDL_DestroyTexture(texture);
    }
}
