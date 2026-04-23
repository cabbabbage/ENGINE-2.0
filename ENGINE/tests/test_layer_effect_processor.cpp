#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "rendering/render/layer_effect_processor.hpp"
#include "rendering/render/layer_stack_renderer.hpp"
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
        window_ = SDL_CreateWindow("layer_effect_processor_tests", 64, 64, SDL_WINDOW_HIDDEN);
        if (!window_) {
            return;
        }
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (!renderer_) {
            renderer_ = SDL_CreateRenderer(window_, SDL_SOFTWARE_RENDERER);
        }
        if (!renderer_) {
            return;
        }

        SDL_Texture* probe = SDL_CreateTexture(renderer_,
                                               SDL_PIXELFORMAT_RGBA8888,
                                               SDL_TEXTUREACCESS_TARGET,
                                               4,
                                               4);
        target_texture_supported_ = (probe != nullptr);
        if (probe) {
            SDL_DestroyTexture(probe);
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
    bool ready() const { return renderer_ != nullptr && target_texture_supported_; }

private:
    ScopedSdlVideo video_{};
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    bool target_texture_supported_ = false;
};

SDL_Texture* create_target_texture(SDL_Renderer* renderer, int width, int height) {
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
    return texture;
}

bool clear_texture(SDL_Renderer* renderer, SDL_Texture* texture, SDL_Color color) {
    if (!renderer || !texture) {
        return false;
    }
    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    if (!SDL_SetRenderTarget(renderer, texture)) {
        return false;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderClear(renderer);
    SDL_SetRenderTarget(renderer, previous_target);
    return true;
}

bool fill_texture_rect(SDL_Renderer* renderer,
                       SDL_Texture* texture,
                       int x,
                       int y,
                       int w,
                       int h,
                       SDL_Color color) {
    if (!renderer || !texture || w <= 0 || h <= 0) {
        return false;
    }
    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    if (!SDL_SetRenderTarget(renderer, texture)) {
        return false;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    const SDL_FRect rect{
        static_cast<float>(x),
        static_cast<float>(y),
        static_cast<float>(w),
        static_cast<float>(h)
    };
    const bool ok = SDL_RenderFillRect(renderer, &rect);
    SDL_SetRenderTarget(renderer, previous_target);
    return ok;
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

    const SDL_Rect pixel_rect{x, y, 1, 1};
    SDL_Surface* captured = SDL_RenderReadPixels(renderer, &pixel_rect);
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

bool capture_texture_pixels(SDL_Renderer* renderer,
                            SDL_Texture* texture,
                            int expected_w,
                            int expected_h,
                            std::vector<SDL_Color>& out_pixels) {
    out_pixels.clear();
    if (!renderer || !texture || expected_w <= 0 || expected_h <= 0) {
        return false;
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    if (!SDL_SetRenderTarget(renderer, texture)) {
        return false;
    }

    SDL_Surface* captured = SDL_RenderReadPixels(renderer, nullptr);
    SDL_SetRenderTarget(renderer, previous_target);
    if (!captured || !captured->pixels) {
        if (captured) {
            SDL_DestroySurface(captured);
        }
        return false;
    }
    if (captured->w != expected_w || captured->h != expected_h) {
        SDL_DestroySurface(captured);
        return false;
    }

    const SDL_PixelFormatDetails* format = SDL_GetPixelFormatDetails(captured->format);
    if (!format || format->bytes_per_pixel <= 0) {
        SDL_DestroySurface(captured);
        return false;
    }
    const int bpp = format->bytes_per_pixel;

    out_pixels.resize(static_cast<std::size_t>(expected_w * expected_h));
    const SDL_Palette* palette = SDL_GetSurfacePalette(captured);
    for (int y = 0; y < expected_h; ++y) {
        const std::uint8_t* row = static_cast<const std::uint8_t*>(captured->pixels) + (captured->pitch * y);
        for (int x = 0; x < expected_w; ++x) {
            Uint32 pixel = 0;
            std::memcpy(&pixel, row + (x * bpp), static_cast<std::size_t>(bpp));
            SDL_Color color{};
            SDL_GetRGBA(pixel, format, palette, &color.r, &color.g, &color.b, &color.a);
            out_pixels[static_cast<std::size_t>(y * expected_w + x)] = color;
        }
    }
    SDL_DestroySurface(captured);
    return true;
}

render_pipeline::GeometryLayerDrawItem make_fullscreen_draw(SDL_Texture* texture, float width, float height) {
    render_pipeline::GeometryLayerDrawItem draw{};
    draw.texture = texture;
    draw.blend_mode = SDL_BLENDMODE_BLEND;
    const SDL_FColor white{1.0f, 1.0f, 1.0f, 1.0f};
    draw.vertices[0] = SDL_Vertex{SDL_FPoint{0.0f, 0.0f}, white, SDL_FPoint{0.0f, 0.0f}};
    draw.vertices[1] = SDL_Vertex{SDL_FPoint{width, 0.0f}, white, SDL_FPoint{1.0f, 0.0f}};
    draw.vertices[2] = SDL_Vertex{SDL_FPoint{width, height}, white, SDL_FPoint{1.0f, 1.0f}};
    draw.vertices[3] = SDL_Vertex{SDL_FPoint{0.0f, height}, white, SDL_FPoint{0.0f, 1.0f}};
    return draw;
}

bool supports_sum_blend() {
    const SDL_BlendMode mode = SDL_ComposeCustomBlendMode(SDL_BLENDFACTOR_ONE,
                                                           SDL_BLENDFACTOR_ONE,
                                                           SDL_BLENDOPERATION_ADD,
                                                           SDL_BLENDFACTOR_ONE,
                                                           SDL_BLENDFACTOR_ONE,
                                                           SDL_BLENDOPERATION_ADD);
    return mode != SDL_BLENDMODE_INVALID;
}

} // namespace

TEST_CASE("LayerEffectProcessor process_layer copies source into output exactly") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());

    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    constexpr int kW = 8;
    constexpr int kH = 8;
    SDL_Texture* base = create_target_texture(renderer, kW, kH);
    SDL_Texture* output = create_target_texture(renderer, kW, kH);
    REQUIRE(base != nullptr);
    REQUIRE(output != nullptr);

    REQUIRE(clear_texture(renderer, base, SDL_Color{0, 0, 0, 0}));
    REQUIRE(fill_texture_rect(renderer, base, 0, 0, 4, 4, SDL_Color{255, 0, 0, 255}));
    REQUIRE(fill_texture_rect(renderer, base, 4, 0, 4, 4, SDL_Color{0, 255, 0, 255}));
    REQUIRE(fill_texture_rect(renderer, base, 0, 4, 4, 4, SDL_Color{0, 0, 255, 255}));
    REQUIRE(fill_texture_rect(renderer, base, 4, 4, 4, 4, SDL_Color{255, 255, 255, 255}));

    LayerEffectProcessor processor(renderer);
    const LayerEffectProcessor::LayerProcessResult result = processor.process_layer(base, output);
    CHECK(result.final_texture == output);

    std::vector<SDL_Color> base_pixels{};
    std::vector<SDL_Color> output_pixels{};
    REQUIRE(capture_texture_pixels(renderer, base, kW, kH, base_pixels));
    REQUIRE(capture_texture_pixels(renderer, output, kW, kH, output_pixels));
    REQUIRE(base_pixels.size() == output_pixels.size());
    for (std::size_t i = 0; i < base_pixels.size(); ++i) {
        CHECK(base_pixels[i].r == output_pixels[i].r);
        CHECK(base_pixels[i].g == output_pixels[i].g);
        CHECK(base_pixels[i].b == output_pixels[i].b);
        CHECK(base_pixels[i].a == output_pixels[i].a);
    }

    SDL_DestroyTexture(output);
    SDL_DestroyTexture(base);
}

TEST_CASE("LayerEffectProcessor tiny blur values are applied and accumulate across passes") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());
    if (!supports_sum_blend()) {
        return;
    }

    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    constexpr int kW = 48;
    constexpr int kH = 48;
    SDL_Texture* source = create_target_texture(renderer, kW, kH);
    SDL_Texture* blurred_once = create_target_texture(renderer, kW, kH);
    SDL_Texture* blurred_twice = create_target_texture(renderer, kW, kH);
    SDL_Texture* scratch = create_target_texture(renderer, kW, kH);
    REQUIRE(source != nullptr);
    REQUIRE(blurred_once != nullptr);
    REQUIRE(blurred_twice != nullptr);
    REQUIRE(scratch != nullptr);

    REQUIRE(clear_texture(renderer, source, SDL_Color{0, 0, 0, 0}));
    REQUIRE(fill_texture_rect(renderer, source, 12, 12, 24, 24, SDL_Color{255, 255, 255, 255}));

    LayerEffectProcessor processor(renderer);
    const SDL_FPoint optical_center{24.0f, 24.0f};
    constexpr float kTinyBlurPx = 0.05f;
    constexpr float kTinyRadialBlurPx = 0.03f;

    REQUIRE(processor.apply_lens_blur(source,
                                      blurred_once,
                                      scratch,
                                      kW,
                                      kH,
                                      kTinyBlurPx,
                                      optical_center,
                                      kTinyRadialBlurPx,
                                      1.0f));

    REQUIRE(processor.apply_lens_blur(blurred_once,
                                      blurred_twice,
                                      scratch,
                                      kW,
                                      kH,
                                      kTinyBlurPx,
                                      optical_center,
                                      kTinyRadialBlurPx,
                                      1.0f));

    std::vector<SDL_Color> source_pixels{};
    std::vector<SDL_Color> once_pixels{};
    std::vector<SDL_Color> twice_pixels{};
    REQUIRE(capture_texture_pixels(renderer, source, kW, kH, source_pixels));
    REQUIRE(capture_texture_pixels(renderer, blurred_once, kW, kH, once_pixels));
    REQUIRE(capture_texture_pixels(renderer, blurred_twice, kW, kH, twice_pixels));
    REQUIRE(source_pixels.size() == once_pixels.size());
    REQUIRE(source_pixels.size() == twice_pixels.size());

    auto l1_diff = [](const std::vector<SDL_Color>& a, const std::vector<SDL_Color>& b) {
        std::uint64_t accum = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            accum += static_cast<std::uint64_t>(std::abs(static_cast<int>(a[i].r) - static_cast<int>(b[i].r)));
            accum += static_cast<std::uint64_t>(std::abs(static_cast<int>(a[i].g) - static_cast<int>(b[i].g)));
            accum += static_cast<std::uint64_t>(std::abs(static_cast<int>(a[i].b) - static_cast<int>(b[i].b)));
        }
        return accum;
    };

    const std::uint64_t single_pass_diff = l1_diff(source_pixels, once_pixels);
    const std::uint64_t repeated_pass_diff = l1_diff(source_pixels, twice_pixels);
    CHECK(single_pass_diff > 0);
    CHECK(repeated_pass_diff > single_pass_diff);

    SDL_DestroyTexture(scratch);
    SDL_DestroyTexture(blurred_twice);
    SDL_DestroyTexture(blurred_once);
    SDL_DestroyTexture(source);
}

TEST_CASE("LayerEffectProcessor zero blur radii copy the source exactly") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());
    if (!supports_sum_blend()) {
        return;
    }

    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    constexpr int kW = 32;
    constexpr int kH = 32;
    SDL_Texture* source = create_target_texture(renderer, kW, kH);
    SDL_Texture* output = create_target_texture(renderer, kW, kH);
    SDL_Texture* scratch = create_target_texture(renderer, kW, kH);
    REQUIRE(source != nullptr);
    REQUIRE(output != nullptr);
    REQUIRE(scratch != nullptr);

    REQUIRE(clear_texture(renderer, source, SDL_Color{0, 0, 0, 0}));
    REQUIRE(fill_texture_rect(renderer, source, 0, 0, 16, 16, SDL_Color{255, 40, 20, 64}));
    REQUIRE(fill_texture_rect(renderer, source, 16, 0, 16, 16, SDL_Color{20, 255, 40, 128}));
    REQUIRE(fill_texture_rect(renderer, source, 0, 16, 16, 16, SDL_Color{40, 20, 255, 200}));
    REQUIRE(fill_texture_rect(renderer, source, 16, 16, 16, 16, SDL_Color{250, 250, 250, 255}));

    LayerEffectProcessor processor(renderer);
    const SDL_FPoint optical_center{16.0f, 16.0f};
    REQUIRE(processor.apply_lens_blur(source,
                                      output,
                                      scratch,
                                      kW,
                                      kH,
                                      0.0f,
                                      optical_center,
                                      0.0f,
                                      1.0f));

    std::vector<SDL_Color> source_pixels{};
    std::vector<SDL_Color> output_pixels{};
    REQUIRE(capture_texture_pixels(renderer, source, kW, kH, source_pixels));
    REQUIRE(capture_texture_pixels(renderer, output, kW, kH, output_pixels));
    REQUIRE(source_pixels.size() == output_pixels.size());

    auto expected_premul = [](Uint8 channel, Uint8 alpha) -> int {
        return static_cast<int>(std::lround((static_cast<float>(channel) * static_cast<float>(alpha)) / 255.0f));
    };
    for (std::size_t i = 0; i < source_pixels.size(); ++i) {
        CHECK(source_pixels[i].a == output_pixels[i].a);
        if (source_pixels[i].a == 255) {
            CHECK(source_pixels[i].r == output_pixels[i].r);
            CHECK(source_pixels[i].g == output_pixels[i].g);
            CHECK(source_pixels[i].b == output_pixels[i].b);
            continue;
        }
        if (source_pixels[i].a == 0) {
            CHECK(output_pixels[i].r == 0);
            CHECK(output_pixels[i].g == 0);
            CHECK(output_pixels[i].b == 0);
            continue;
        }
        CHECK(std::abs(static_cast<int>(output_pixels[i].r) - expected_premul(source_pixels[i].r, source_pixels[i].a)) <= 1);
        CHECK(std::abs(static_cast<int>(output_pixels[i].g) - expected_premul(source_pixels[i].g, source_pixels[i].a)) <= 1);
        CHECK(std::abs(static_cast<int>(output_pixels[i].b) - expected_premul(source_pixels[i].b, source_pixels[i].a)) <= 1);
    }

    SDL_DestroyTexture(scratch);
    SDL_DestroyTexture(output);
    SDL_DestroyTexture(source);
}

TEST_CASE("LayerStackRenderer rasterizes prelit sprites without relighting") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());

    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    constexpr int kW = 32;
    constexpr int kH = 32;
    SDL_Texture* sprite = create_target_texture(renderer, kW, kH);
    REQUIRE(sprite != nullptr);
    REQUIRE(clear_texture(renderer, sprite, SDL_Color{164, 196, 228, 255}));

    LayerStackRenderer stack_renderer(renderer);
    stack_renderer.set_output_dimensions(kW, kH);

    render_pipeline::LayerBuildResult build{};
    build.valid = true;
    build.layer_count = 1;
    build.player_layer_index = 0;
    build.non_empty_layers = {0};
    build.layers.resize(1);
    build.layers[0].depth_min = -32.0;
    build.layers[0].depth_max = 32.0;
    build.layers[0].representative_depth = 0.0;
    build.layers[0].bounds_min_x = 0.0f;
    build.layers[0].bounds_min_y = 0.0f;
    build.layers[0].bounds_max_x = static_cast<float>(kW);
    build.layers[0].bounds_max_y = static_cast<float>(kH);
    build.layers[0].draws.push_back(make_fullscreen_draw(sprite, static_cast<float>(kW), static_cast<float>(kH)));

    const render_pipeline::LayerRenderResult rendered = stack_renderer.render(build);
    REQUIRE(rendered.valid);
    REQUIRE(rendered.final_layer_textures.size() == 1);
    REQUIRE(rendered.final_layer_textures[0] != nullptr);

    SDL_Color center{};
    REQUIRE(read_pixel(renderer, rendered.final_layer_textures[0], 16, 16, center));
    CHECK(center.r == 164);
    CHECK(center.g == 196);
    CHECK(center.b == 228);
    CHECK(center.a == 255);

    SDL_DestroyTexture(sprite);
}

TEST_CASE("LayerStackRenderer output is invariant to layer depth metadata after asset prelighting") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());

    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    constexpr int kW = 32;
    constexpr int kH = 32;
    SDL_Texture* sprite = create_target_texture(renderer, kW, kH);
    REQUIRE(sprite != nullptr);
    REQUIRE(clear_texture(renderer, sprite, SDL_Color{210, 150, 90, 255}));

    LayerStackRenderer stack_renderer(renderer);
    stack_renderer.set_output_dimensions(kW, kH);

    auto build_layer = [&](double depth_min, double depth_max, int player_layer_index) {
        render_pipeline::LayerBuildResult build{};
        build.valid = true;
        build.layer_count = 1;
        build.player_layer_index = player_layer_index;
        build.non_empty_layers = {0};
        build.layers.resize(1);
        build.layers[0].depth_min = depth_min;
        build.layers[0].depth_max = depth_max;
        build.layers[0].representative_depth = 0.5 * (depth_min + depth_max);
        build.layers[0].bounds_min_x = 0.0f;
        build.layers[0].bounds_min_y = 0.0f;
        build.layers[0].bounds_max_x = static_cast<float>(kW);
        build.layers[0].bounds_max_y = static_cast<float>(kH);
        build.layers[0].draws.push_back(make_fullscreen_draw(sprite, static_cast<float>(kW), static_cast<float>(kH)));
        return build;
    };

    const render_pipeline::LayerRenderResult near_render = stack_renderer.render(build_layer(-20.0, 20.0, 0));
    const render_pipeline::LayerRenderResult far_render = stack_renderer.render(build_layer(300.0, 500.0, 3));
    REQUIRE(near_render.valid);
    REQUIRE(far_render.valid);
    REQUIRE(near_render.final_layer_textures.size() == 1);
    REQUIRE(far_render.final_layer_textures.size() == 1);

    SDL_Color near_pixel{};
    SDL_Color far_pixel{};
    REQUIRE(read_pixel(renderer, near_render.final_layer_textures[0], 16, 16, near_pixel));
    REQUIRE(read_pixel(renderer, far_render.final_layer_textures[0], 16, 16, far_pixel));
    CHECK(near_pixel.r == far_pixel.r);
    CHECK(near_pixel.g == far_pixel.g);
    CHECK(near_pixel.b == far_pixel.b);
    CHECK(near_pixel.a == far_pixel.a);

    SDL_DestroyTexture(sprite);
}
