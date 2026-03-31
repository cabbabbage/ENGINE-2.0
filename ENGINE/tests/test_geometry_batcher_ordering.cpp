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
        window_ = SDL_CreateWindow("geometry_batcher_tests", 32, 32, SDL_WINDOW_HIDDEN);
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
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderClear(renderer);
    SDL_SetRenderTarget(renderer, previous_target);
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

    SDL_Rect pixel_rect{x, y, 1, 1};
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

TEST_CASE("GeometryBatcher preserves insertion order within a depth bucket") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());

    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    SDL_Texture* red_texture = create_solid_texture(renderer, 2, 2, SDL_Color{255, 0, 0, 255});
    SDL_Texture* blue_texture = create_solid_texture(renderer, 2, 2, SDL_Color{0, 0, 255, 255});
    SDL_Texture* target = create_solid_texture(renderer, 8, 8, SDL_Color{0, 0, 0, 0});
    REQUIRE(red_texture != nullptr);
    REQUIRE(blue_texture != nullptr);
    REQUIRE(target != nullptr);

    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    REQUIRE(SDL_SetRenderTarget(renderer, target));
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
    SDL_RenderClear(renderer);

    GeometryBatcher batcher(renderer);
    SDL_Vertex quad_vertices[4]{};
    quad_vertices[0].position = SDL_FPoint{0.0f, 0.0f};
    quad_vertices[1].position = SDL_FPoint{8.0f, 0.0f};
    quad_vertices[2].position = SDL_FPoint{8.0f, 8.0f};
    quad_vertices[3].position = SDL_FPoint{0.0f, 8.0f};
    const SDL_FColor white{1.0f, 1.0f, 1.0f, 1.0f};
    quad_vertices[0].color = quad_vertices[1].color = quad_vertices[2].color = quad_vertices[3].color = white;
    quad_vertices[0].tex_coord = SDL_FPoint{0.0f, 0.0f};
    quad_vertices[1].tex_coord = SDL_FPoint{1.0f, 0.0f};
    quad_vertices[2].tex_coord = SDL_FPoint{1.0f, 1.0f};
    quad_vertices[3].tex_coord = SDL_FPoint{0.0f, 1.0f};
    static constexpr int kQuadIndices[6] = {0, 1, 2, 0, 2, 3};

    batcher.addQuad(red_texture, quad_vertices, kQuadIndices, SDL_BLENDMODE_BLEND, 0.0);
    batcher.addQuad(blue_texture, quad_vertices, kQuadIndices, SDL_BLENDMODE_BLEND, 0.0);
    batcher.addQuad(red_texture, quad_vertices, kQuadIndices, SDL_BLENDMODE_BLEND, 0.0);
    batcher.flush();

    SDL_Color center_color{};
    REQUIRE(read_pixel(renderer, target, 4, 4, center_color));
    CHECK(center_color.r > 200);
    CHECK(center_color.b < 40);
    CHECK(center_color.a > 200);

    SDL_SetRenderTarget(renderer, previous_target);
    SDL_DestroyTexture(target);
    SDL_DestroyTexture(red_texture);
    SDL_DestroyTexture(blue_texture);
}
