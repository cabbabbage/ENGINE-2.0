#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <vector>

#include "rendering/render/dof_blur_chain.hpp"

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
        window_ = SDL_CreateWindow("dof_blur_chain_tests", 64, 64, SDL_WINDOW_HIDDEN);
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

        SDL_Texture* probe = create_target_texture(renderer_, 4, 4);
        target_texture_supported_ = probe != nullptr;
        if (probe) {
            SDL_DestroyTexture(probe);
        }
    }

    ~ScopedRenderer() {
        if (renderer_) {
            SDL_DestroyRenderer(renderer_);
        }
        if (window_) {
            SDL_DestroyWindow(window_);
        }
    }

    SDL_Renderer* get() const { return renderer_; }
    bool ready() const { return renderer_ && target_texture_supported_; }

    static SDL_Texture* create_target_texture(SDL_Renderer* renderer, int width, int height) {
        if (!renderer || width <= 0 || height <= 0) {
            return nullptr;
        }
        SDL_Texture* texture = SDL_CreateTexture(renderer,
                                                 SDL_PIXELFORMAT_RGBA32,
                                                 SDL_TEXTUREACCESS_TARGET,
                                                 width,
                                                 height);
        if (texture) {
            SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        }
        return texture;
    }

private:
    ScopedSdlVideo video_{};
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    bool target_texture_supported_ = false;
};

SDL_Texture* create_target_texture(SDL_Renderer* renderer, int width, int height) {
    return ScopedRenderer::create_target_texture(renderer, width, height);
}

bool fill_texture(SDL_Renderer* renderer, SDL_Texture* texture, SDL_Color color) {
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

TEST_CASE("DoF blur-chain gate requires DoF enabled and non-zero blur") {
    CHECK_FALSE(dof_blur_chain::enabled(false, 32.0f, 0.0f));
    CHECK_FALSE(dof_blur_chain::enabled(false, 0.0f, 16.0f));
    CHECK_FALSE(dof_blur_chain::enabled(false, 16.0f, 16.0f));

    CHECK_FALSE(dof_blur_chain::enabled(true, 0.0f, 0.0f));
    CHECK_FALSE(dof_blur_chain::enabled(true, 1.0e-6f, 1.0e-6f));

    CHECK(dof_blur_chain::enabled(true, 0.25f, 0.0f));
    CHECK(dof_blur_chain::enabled(true, 0.0f, 0.25f));
}

TEST_CASE("DoF signed depth chains split around focus layer") {
    const std::vector<int> depth_layers{3, 1, 0, -1, -3, -2};

    CHECK(dof_blur_chain::background_chain_layers(depth_layers) == std::vector<int>({3, 1, 0}));
    CHECK(dof_blur_chain::foreground_chain_layers(depth_layers) == std::vector<int>({-3, -2, -1}));
}

TEST_CASE("DoF blur-chain compose restores target and reports blur passes") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());

    SDL_Renderer* renderer = renderer_scope.get();
    SDL_Texture* floor = create_target_texture(renderer, 16, 16);
    SDL_Texture* layer_far = create_target_texture(renderer, 16, 16);
    SDL_Texture* layer_focus = create_target_texture(renderer, 16, 16);
    SDL_Texture* wrong_target = create_target_texture(renderer, 16, 16);
    REQUIRE(floor);
    REQUIRE(layer_far);
    REQUIRE(layer_focus);
    REQUIRE(wrong_target);

    REQUIRE(fill_texture(renderer, floor, SDL_Color{32, 0, 0, 255}));
    REQUIRE(fill_texture(renderer, layer_far, SDL_Color{0, 96, 0, 192}));
    REQUIRE(fill_texture(renderer, layer_focus, SDL_Color{0, 0, 128, 192}));
    REQUIRE(fill_texture(renderer, wrong_target, SDL_Color{250, 1, 2, 255}));

    SDL_SetRenderTarget(renderer, wrong_target);

    dof_blur_chain::Renderer dof(renderer);
    dof.set_output_dimensions(16, 16);
    const dof_blur_chain::CompositeResult result =
        dof.compose({dof_blur_chain::LayerTexture{2, layer_far},
                     dof_blur_chain::LayerTexture{0, layer_focus}},
                    floor,
                    true,
                    4.0f,
                    0.0f,
                    SDL_FPoint{8.0f, 8.0f});

    CHECK(result.valid);
    CHECK(result.background_mid != nullptr);
    CHECK(result.blur_pass_count > 0);
    CHECK(SDL_GetRenderTarget(renderer) == wrong_target);

    SDL_Color wrong_pixel{};
    REQUIRE(read_pixel(renderer, wrong_target, 8, 8, wrong_pixel));
    CHECK(wrong_pixel.r == 250);
    CHECK(wrong_pixel.g == 1);
    CHECK(wrong_pixel.b == 2);

    SDL_DestroyTexture(wrong_target);
    SDL_DestroyTexture(layer_focus);
    SDL_DestroyTexture(layer_far);
    SDL_DestroyTexture(floor);
}
