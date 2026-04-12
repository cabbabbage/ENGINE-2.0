#include <doctest/doctest.h>

#include <SDL3/SDL.h>
#include <cmath>
#include <limits>
#include <vector>

#include "rendering/render/render.hpp"
#include "rendering/render/warped_screen_grid.hpp"

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

Area make_starting_area() {
    std::vector<SDL_Point> corners{
        SDL_Point{-100, -100},
        SDL_Point{100, -100},
        SDL_Point{100, 100},
        SDL_Point{-100, 100}};
    return Area("floor_light_mask_start", corners, 0);
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
    constexpr float kRadiusWorld = 120.0f;
    const float low_height_weight = render_internal::floor_light_height_weight(0.0f, kRadiusWorld);
    const float mid_height_weight = render_internal::floor_light_height_weight(20.0f, kRadiusWorld);
    const float high_height_weight = render_internal::floor_light_height_weight(80.0f, kRadiusWorld);

    CHECK(low_height_weight >= mid_height_weight);
    CHECK(mid_height_weight >= high_height_weight);
    CHECK(low_height_weight == doctest::Approx(1.0f));

    const float base_spread = render_internal::floor_light_height_spread_scale(0.0f, kRadiusWorld);
    const float raised_spread = render_internal::floor_light_height_spread_scale(40.0f, kRadiusWorld);
    CHECK(raised_spread > base_spread);

    const float base_footprint = render_internal::floor_light_footprint_radius(120.0f, 0.0f);
    const float raised_footprint = render_internal::floor_light_footprint_radius(120.0f, 40.0f);
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

TEST_CASE("Floor light contact resolution locks to flat world point") {
    constexpr float kFlatX = 100.0f;
    constexpr float kFlatZ = 260.0f;
    constexpr float kDisplacedX = 180.0f;
    constexpr float kDisplacedZ = 340.0f;
    constexpr float kHeight = 42.0f;

    const render_internal::FloorLightContact contact = render_internal::resolve_floor_light_contact(
        kFlatX, kFlatZ, kDisplacedX, kDisplacedZ, kHeight);
    REQUIRE(contact.valid);
    CHECK(contact.world_x == doctest::Approx(kFlatX));
    CHECK(contact.world_z == doctest::Approx(kFlatZ));
    CHECK(contact.world_height == doctest::Approx(kHeight));
}

TEST_CASE("Floor projection center is invariant with light height for fixed floor XZ") {
    WarpedScreenGrid grid(1280, 720, make_starting_area());

    const auto params = grid.projection_params();
    const SDL_FPoint map_center =
        grid.screen_to_map(SDL_Point{params.screen_width / 2, params.screen_height / 2});

    const render_internal::FloorLightContact low_contact =
        render_internal::resolve_floor_light_contact(map_center.x, map_center.y, map_center.x, map_center.y, 5.0f);
    const render_internal::FloorLightContact high_contact =
        render_internal::resolve_floor_light_contact(map_center.x, map_center.y, map_center.x + 120.0f, map_center.y + 120.0f, 180.0f);

    SDL_FPoint low_screen{};
    SDL_FPoint high_screen{};
    REQUIRE(render_internal::project_floor_contact_to_screen(grid, low_contact, low_screen));
    REQUIRE(render_internal::project_floor_contact_to_screen(grid, high_contact, high_screen));

    CHECK(low_screen.x == doctest::Approx(high_screen.x).epsilon(1e-5));
    CHECK(low_screen.y == doctest::Approx(high_screen.y).epsilon(1e-5));
}

TEST_CASE("Floor footprint axes widen while intensity decays with increasing height") {
    WarpedScreenGrid grid(1280, 720, make_starting_area());

    const auto params = grid.projection_params();
    const SDL_FPoint map_center =
        grid.screen_to_map(SDL_Point{params.screen_width / 2, params.screen_height / 2});

    const render_internal::FloorLightContact contact =
        render_internal::resolve_floor_light_contact(map_center.x, map_center.y, map_center.x, map_center.y, 0.0f);
    SDL_FPoint floor_screen{};
    REQUIRE(render_internal::project_floor_contact_to_screen(grid, contact, floor_screen));

    constexpr float kRadiusWorld = 120.0f;

    float rx_low = 0.0f;
    float ry_low = 0.0f;
    REQUIRE(render_internal::sample_floor_light_footprint_axes_px(grid,
                                                                  contact,
                                                                  floor_screen,
                                                                  kRadiusWorld,
                                                                  render_internal::floor_light_height_spread_scale(0.0f, kRadiusWorld),
                                                                  rx_low,
                                                                  ry_low));

    float rx_high = 0.0f;
    float ry_high = 0.0f;
    REQUIRE(render_internal::sample_floor_light_footprint_axes_px(grid,
                                                                  contact,
                                                                  floor_screen,
                                                                  kRadiusWorld,
                                                                  render_internal::floor_light_height_spread_scale(180.0f, kRadiusWorld),
                                                                  rx_high,
                                                                  ry_high));

    CHECK(rx_high > rx_low);
    CHECK(ry_high > ry_low);

    const float low_weight = render_internal::floor_light_height_weight(0.0f, kRadiusWorld);
    const float high_weight = render_internal::floor_light_height_weight(180.0f, kRadiusWorld);
    CHECK(low_weight > high_weight);
}

TEST_CASE("Near-camera floor footprint sampling remains finite and stable") {
    WarpedScreenGrid grid(1280, 720, make_starting_area());

    const auto params = grid.projection_params();
    const SDL_FPoint near_floor_world =
        grid.screen_to_map(SDL_Point{static_cast<int>(params.screen_width * 0.35f),
                                     static_cast<int>(params.screen_height * 0.92f)});

    const render_internal::FloorLightContact low_contact =
        render_internal::resolve_floor_light_contact(near_floor_world.x, near_floor_world.y, near_floor_world.x, near_floor_world.y, 5.0f);
    const render_internal::FloorLightContact high_contact =
        render_internal::resolve_floor_light_contact(near_floor_world.x, near_floor_world.y, near_floor_world.x + 100.0f, near_floor_world.y + 150.0f, 220.0f);

    SDL_FPoint low_screen{};
    SDL_FPoint high_screen{};
    REQUIRE(render_internal::project_floor_contact_to_screen(grid, low_contact, low_screen));
    REQUIRE(render_internal::project_floor_contact_to_screen(grid, high_contact, high_screen));

    CHECK(std::isfinite(low_screen.x));
    CHECK(std::isfinite(high_screen.x));
    CHECK(low_screen.x == doctest::Approx(high_screen.x).epsilon(1e-5));

    float rx_low = 0.0f;
    float ry_low = 0.0f;
    float rx_high = 0.0f;
    float ry_high = 0.0f;
    REQUIRE(render_internal::sample_floor_light_footprint_axes_px(grid,
                                                                  low_contact,
                                                                  low_screen,
                                                                  80.0f,
                                                                  render_internal::floor_light_height_spread_scale(5.0f, 80.0f),
                                                                  rx_low,
                                                                  ry_low));
    REQUIRE(render_internal::sample_floor_light_footprint_axes_px(grid,
                                                                  high_contact,
                                                                  high_screen,
                                                                  80.0f,
                                                                  render_internal::floor_light_height_spread_scale(220.0f, 80.0f),
                                                                  rx_high,
                                                                  ry_high));
    CHECK(std::isfinite(rx_low));
    CHECK(std::isfinite(ry_low));
    CHECK(std::isfinite(rx_high));
    CHECK(std::isfinite(ry_high));
    CHECK(rx_high > rx_low);
    CHECK(ry_high > ry_low);
}

TEST_CASE("Layer light overlap requires depth overlap even when screen footprint overlaps") {
    LayerEffectProcessor::RuntimeLight light{};
    light.screen_center = SDL_FPoint{50.0f, 50.0f};
    light.radius_px = 32.0f;
    light.radius_world = 16.0f;
    light.world_z = 200.0f;

    const bool overlaps = render_internal::light_overlaps_layer_slice(
        light,
        -10.0,
        10.0,
        0.0f,
        0.0f,
        100.0f,
        100.0f);
    CHECK_FALSE(overlaps);
}

TEST_CASE("Layer light overlap requires coverage overlap even when depth slice overlaps") {
    LayerEffectProcessor::RuntimeLight light{};
    light.screen_center = SDL_FPoint{400.0f, 400.0f};
    light.radius_px = 8.0f;
    light.radius_world = 30.0f;
    light.world_z = 4.0f;

    const bool overlaps = render_internal::light_overlaps_layer_slice(
        light,
        -10.0,
        10.0,
        0.0f,
        0.0f,
        100.0f,
        100.0f);
    CHECK_FALSE(overlaps);
}

TEST_CASE("Layer light overlap accepts depth and coverage intersection") {
    LayerEffectProcessor::RuntimeLight light{};
    light.screen_center = SDL_FPoint{70.0f, 60.0f};
    light.radius_px = 24.0f;
    light.radius_world = 20.0f;
    light.world_z = 5.0f;

    const bool overlaps = render_internal::light_overlaps_layer_slice(
        light,
        -10.0,
        10.0,
        40.0f,
        40.0f,
        120.0f,
        120.0f);
    CHECK(overlaps);
}

TEST_CASE("Layer light overlap accepts inside-layer center when depth overlaps") {
    LayerEffectProcessor::RuntimeLight light{};
    light.screen_center = SDL_FPoint{64.0f, 64.0f};
    light.radius_px = 12.0f;
    light.radius_world = 18.0f;
    light.world_z = -3.0f;

    const bool overlaps = render_internal::light_overlaps_layer_slice(
        light,
        -20.0,
        10.0,
        32.0f,
        32.0f,
        96.0f,
        96.0f);
    CHECK(overlaps);
}

TEST_CASE("Layer light overlap rejects invalid geometry inputs") {
    LayerEffectProcessor::RuntimeLight light{};
    light.screen_center = SDL_FPoint{50.0f, 50.0f};
    light.radius_px = 20.0f;
    light.radius_world = 20.0f;
    light.world_z = 0.0f;

    CHECK_FALSE(render_internal::light_overlaps_layer_slice(
        light,
        std::numeric_limits<double>::quiet_NaN(),
        10.0,
        0.0f,
        0.0f,
        100.0f,
        100.0f));

    CHECK_FALSE(render_internal::light_overlaps_layer_slice(
        light,
        -10.0,
        10.0,
        100.0f,
        0.0f,
        0.0f,
        100.0f));

    light.screen_center.x = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(render_internal::light_overlaps_layer_slice(
        light,
        -10.0,
        10.0,
        0.0f,
        0.0f,
        100.0f,
        100.0f));
}
