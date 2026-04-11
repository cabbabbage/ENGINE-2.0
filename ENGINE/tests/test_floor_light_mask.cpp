#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include "rendering/render/render.hpp"

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
        window_ = SDL_CreateWindow("floor_light_mask_tests", 64, 64, SDL_WINDOW_HIDDEN);
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

} // namespace

TEST_CASE("Floor light mask clear helper preserves solid background clear") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());

    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    SDL_Texture* target = create_target_texture(renderer, 16, 16);
    REQUIRE(target != nullptr);

    const SDL_Color clear_color{32, 68, 104, 255};
    CHECK(render_internal::clear_gameplay_target_to_color(renderer, target, clear_color));

    SDL_Color pixel{};
    REQUIRE(read_pixel(renderer, target, 8, 8, pixel));
    CHECK(pixel.r == clear_color.r);
    CHECK(pixel.g == clear_color.g);
    CHECK(pixel.b == clear_color.b);

    SDL_DestroyTexture(target);
}

TEST_CASE("Floor light depth weighting reaches zero at half-cull and is monotonic") {
    constexpr float kCullDepth = 100.0f;
    const float near_weight = render_internal::floor_light_depth_weight(0.0f, kCullDepth);
    const float mid_weight = render_internal::floor_light_depth_weight(50.0f, kCullDepth);
    const float edge_weight = render_internal::floor_light_depth_weight(100.0f, kCullDepth);
    const float beyond_weight = render_internal::floor_light_depth_weight(150.0f, kCullDepth);

    CHECK(near_weight >= mid_weight);
    CHECK(mid_weight >= edge_weight);
    CHECK(edge_weight == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(beyond_weight == doctest::Approx(0.0f).epsilon(1e-5));
}

TEST_CASE("Floor light height attenuation and footprint scale realistically") {
    constexpr float kRadius = 120.0f;
    const float low_height_weight = render_internal::floor_light_height_weight(0.0f, kRadius);
    const float mid_height_weight = render_internal::floor_light_height_weight(20.0f, kRadius);
    const float high_height_weight = render_internal::floor_light_height_weight(80.0f, kRadius);

    CHECK(low_height_weight >= mid_height_weight);
    CHECK(mid_height_weight >= high_height_weight);

    const float base_footprint = render_internal::floor_light_footprint_radius(kRadius, 0.0f);
    const float raised_footprint = render_internal::floor_light_footprint_radius(kRadius, 40.0f);
    CHECK(raised_footprint > base_footprint);
}

TEST_CASE("Layer light strength multipliers split front and behind depths independently") {
    constexpr float kBaseIntensity = 2.0f;
    constexpr float kFrontMultiplier = 1.6f;
    constexpr float kBehindMultiplier = 0.45f;

    const float front_side =
        render_internal::apply_layer_light_strength_bias(kBaseIntensity, -16.0, kFrontMultiplier, kBehindMultiplier);
    const float boundary_side =
        render_internal::apply_layer_light_strength_bias(kBaseIntensity, 0.0, kFrontMultiplier, kBehindMultiplier);
    const float behind_side =
        render_internal::apply_layer_light_strength_bias(kBaseIntensity, 16.0, kFrontMultiplier, kBehindMultiplier);

    CHECK(front_side == doctest::Approx(kBaseIntensity * kFrontMultiplier));
    CHECK(boundary_side == doctest::Approx(kBaseIntensity * kFrontMultiplier));
    CHECK(behind_side == doctest::Approx(kBaseIntensity * kBehindMultiplier));
    CHECK(front_side > behind_side);
}
