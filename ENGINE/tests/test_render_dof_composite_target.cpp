#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <vector>

#include "rendering/render/blur_chain_renderer.hpp"
#include "rendering/render/render.hpp"
#include "rendering/render/scene_composite_pass.hpp"

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
        window_ = SDL_CreateWindow("dof_composite_target_tests", 64, 64, SDL_WINDOW_HIDDEN);
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

TEST_CASE("Scene composite pass writes layer stack to gameplay target even when current target differs") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());

    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    constexpr int kW = 16;
    constexpr int kH = 16;
    SDL_Texture* gameplay_target = create_target_texture(renderer, kW, kH);
    SDL_Texture* wrong_target = create_target_texture(renderer, kW, kH);
    SDL_Texture* layer0 = create_target_texture(renderer, kW, kH);
    SDL_Texture* layer1 = create_target_texture(renderer, kW, kH);
    REQUIRE(gameplay_target != nullptr);
    REQUIRE(wrong_target != nullptr);
    REQUIRE(layer0 != nullptr);
    REQUIRE(layer1 != nullptr);

    REQUIRE(clear_texture(renderer, gameplay_target, SDL_Color{0, 0, 0, 0}));
    REQUIRE(clear_texture(renderer, wrong_target, SDL_Color{0, 0, 0, 0}));
    REQUIRE(fill_texture(renderer, layer0, SDL_Color{255, 40, 40, 255}));
    REQUIRE(fill_texture(renderer, layer1, SDL_Color{40, 255, 40, 255}));

    REQUIRE(SDL_SetRenderTarget(renderer, wrong_target));

    const std::vector<SDL_Texture*> final_layer_textures{layer0, layer1};
    const std::vector<int> non_empty_layers{0, 1};
    SceneCompositePass composite_pass(renderer);
    render_pipeline::LayerRenderResult layer_render{};
    layer_render.valid = true;
    layer_render.layer_count = 2;
    layer_render.non_empty_layers = non_empty_layers;
    layer_render.final_layer_textures = final_layer_textures;
    render_pipeline::BlurCompositeResult blur_result{};
    const bool ok = composite_pass.compose(gameplay_target, layer_render, blur_result);
    CHECK(ok);
    CHECK(SDL_GetRenderTarget(renderer) == gameplay_target);

    SDL_Color gameplay_pixel{};
    SDL_Color wrong_pixel{};
    REQUIRE(read_pixel(renderer, gameplay_target, 8, 8, gameplay_pixel));
    REQUIRE(read_pixel(renderer, wrong_target, 8, 8, wrong_pixel));

    CHECK(gameplay_pixel.a > 0);
    CHECK(wrong_pixel.a == 0);

    SDL_DestroyTexture(layer1);
    SDL_DestroyTexture(layer0);
    SDL_DestroyTexture(wrong_target);
    SDL_DestroyTexture(gameplay_target);
}

TEST_CASE("Scene mid-layer composite draws foreground over background") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());

    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    constexpr int kW = 16;
    constexpr int kH = 16;
    SDL_Texture* gameplay_target = create_target_texture(renderer, kW, kH);
    SDL_Texture* background_mid = create_target_texture(renderer, kW, kH);
    SDL_Texture* foreground_mid = create_target_texture(renderer, kW, kH);
    REQUIRE(gameplay_target != nullptr);
    REQUIRE(background_mid != nullptr);
    REQUIRE(foreground_mid != nullptr);

    REQUIRE(fill_texture(renderer, background_mid, SDL_Color{25, 80, 210, 255}));
    REQUIRE(fill_texture(renderer, foreground_mid, SDL_Color{220, 35, 35, 255}));

    SceneCompositePass composite_pass(renderer);
    render_pipeline::LayerRenderResult layer_render{};
    render_pipeline::BlurCompositeResult blur_result{};
    blur_result.valid = true;
    blur_result.background_mid = background_mid;
    blur_result.foreground_mid = foreground_mid;
    const bool ok = composite_pass.compose(gameplay_target, layer_render, blur_result);
    CHECK(ok);

    SDL_Color gameplay_pixel{};
    REQUIRE(read_pixel(renderer, gameplay_target, 8, 8, gameplay_pixel));
    CHECK(gameplay_pixel.r == 220);
    CHECK(gameplay_pixel.g == 35);
    CHECK(gameplay_pixel.b == 35);
    CHECK(gameplay_pixel.a == 255);

    SDL_DestroyTexture(foreground_mid);
    SDL_DestroyTexture(background_mid);
    SDL_DestroyTexture(gameplay_target);
}

TEST_CASE("DoF blur-chain gate requires DoF enabled and non-zero blur") {
    CHECK_FALSE(render_internal::dof_blur_chain_enabled(false, 32.0f, 0.0f));
    CHECK_FALSE(render_internal::dof_blur_chain_enabled(false, 0.0f, 16.0f));
    CHECK_FALSE(render_internal::dof_blur_chain_enabled(false, 16.0f, 16.0f));

    CHECK_FALSE(render_internal::dof_blur_chain_enabled(true, 0.0f, 0.0f));
    CHECK_FALSE(render_internal::dof_blur_chain_enabled(true, 1.0e-6f, 1.0e-6f));

    CHECK(render_internal::dof_blur_chain_enabled(true, 0.25f, 0.0f));
    CHECK(render_internal::dof_blur_chain_enabled(true, 0.0f, 0.25f));
}

TEST_CASE("DoF background chain keeps the player split layer in the blur path") {
    const std::vector<int> non_empty_layers{0, 1, 2, 3};

    const std::vector<int> background_chain = render_internal::background_chain_layers(non_empty_layers, 2);
    const std::vector<int> foreground_chain = render_internal::foreground_chain_layers(non_empty_layers, 2);

    CHECK(background_chain == std::vector<int>({3, 2}));
    CHECK(foreground_chain == std::vector<int>({0, 1}));
}

TEST_CASE("Blur chain applies floor dark mask to floor seed when only player layer exists") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());

    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    constexpr int kW = 16;
    constexpr int kH = 16;
    SDL_Texture* floor_seed = create_target_texture(renderer, kW, kH);
    SDL_Texture* floor_dark_mask = create_target_texture(renderer, kW, kH);
    SDL_Texture* transparent_player_layer = create_target_texture(renderer, kW, kH);
    REQUIRE(floor_seed != nullptr);
    REQUIRE(floor_dark_mask != nullptr);
    REQUIRE(transparent_player_layer != nullptr);

    REQUIRE(fill_texture(renderer, floor_seed, SDL_Color{200, 120, 80, 255}));
    REQUIRE(fill_texture(renderer, floor_dark_mask, SDL_Color{128, 128, 128, 255}));
    REQUIRE(fill_texture(renderer, transparent_player_layer, SDL_Color{0, 0, 0, 0}));

    render_pipeline::LayerRenderResult layer_render{};
    layer_render.valid = true;
    layer_render.layer_count = 1;
    layer_render.player_layer_index = 0;
    layer_render.non_empty_layers = {0};
    layer_render.final_layer_textures = {transparent_player_layer};

    BlurChainRenderer blur_chain(renderer);
    blur_chain.set_output_dimensions(kW, kH);
    const render_pipeline::BlurCompositeResult result = blur_chain.compose(
        layer_render,
        floor_seed,
        floor_dark_mask,
        false,
        0.0f,
        0.0f,
        SDL_FPoint{8.0f, 8.0f});
    REQUIRE(result.valid);
    REQUIRE(result.background_mid != nullptr);

    SDL_Color pixel{};
    REQUIRE(read_pixel(renderer, result.background_mid, 8, 8, pixel));
    CHECK(pixel.r <= 102);
    CHECK(pixel.r >= 98);
    CHECK(pixel.g <= 62);
    CHECK(pixel.g >= 58);
    CHECK(pixel.b <= 42);
    CHECK(pixel.b >= 38);

    SDL_DestroyTexture(transparent_player_layer);
    SDL_DestroyTexture(floor_dark_mask);
    SDL_DestroyTexture(floor_seed);
}

TEST_CASE("Blur chain floor dark mask does not darken non-floor background layer stack") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());

    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    constexpr int kW = 16;
    constexpr int kH = 16;
    SDL_Texture* floor_seed = create_target_texture(renderer, kW, kH);
    SDL_Texture* floor_dark_mask = create_target_texture(renderer, kW, kH);
    SDL_Texture* background_layer = create_target_texture(renderer, kW, kH);
    SDL_Texture* transparent_player_layer = create_target_texture(renderer, kW, kH);
    REQUIRE(floor_seed != nullptr);
    REQUIRE(floor_dark_mask != nullptr);
    REQUIRE(background_layer != nullptr);
    REQUIRE(transparent_player_layer != nullptr);

    REQUIRE(fill_texture(renderer, floor_seed, SDL_Color{200, 200, 200, 255}));
    REQUIRE(fill_texture(renderer, floor_dark_mask, SDL_Color{128, 128, 128, 255}));
    REQUIRE(fill_texture(renderer, background_layer, SDL_Color{200, 0, 0, 128}));
    REQUIRE(fill_texture(renderer, transparent_player_layer, SDL_Color{0, 0, 0, 0}));

    render_pipeline::LayerRenderResult layer_render{};
    layer_render.valid = true;
    layer_render.layer_count = 2;
    layer_render.player_layer_index = 0;
    layer_render.non_empty_layers = {0, 1};
    layer_render.final_layer_textures = {transparent_player_layer, background_layer};

    BlurChainRenderer blur_chain(renderer);
    blur_chain.set_output_dimensions(kW, kH);
    const render_pipeline::BlurCompositeResult result = blur_chain.compose(
        layer_render,
        floor_seed,
        floor_dark_mask,
        false,
        0.0f,
        0.0f,
        SDL_FPoint{8.0f, 8.0f});
    REQUIRE(result.valid);
    REQUIRE(result.background_mid != nullptr);

    SDL_Color pixel{};
    REQUIRE(read_pixel(renderer, result.background_mid, 8, 8, pixel));
    CHECK(pixel.r >= 140);
    CHECK(pixel.g <= 60);
    CHECK(pixel.b <= 60);

    SDL_DestroyTexture(transparent_player_layer);
    SDL_DestroyTexture(background_layer);
    SDL_DestroyTexture(floor_dark_mask);
    SDL_DestroyTexture(floor_seed);
}
