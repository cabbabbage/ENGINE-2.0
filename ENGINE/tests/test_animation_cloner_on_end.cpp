#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include <SDL3/SDL.h>

#include "assets/asset/animation.hpp"
#include "assets/asset/animation_cloner.hpp"
#include "assets/asset/asset_info.hpp"

namespace {

Animation make_single_frame_animation() {
    Animation animation;
    Animation::FrameCache cache;
    cache.resize(1);
    std::vector<Animation::FrameCache> caches;
    caches.push_back(std::move(cache));
    animation.adopt_prebuilt_frames(std::move(caches), {1.0f});
    return animation;
}

SDL_Renderer* sentinel_renderer() {
    // Clone only needs a non-null renderer when source textures are null.
    return reinterpret_cast<SDL_Renderer*>(static_cast<std::uintptr_t>(1));
}

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
        window_ = SDL_CreateWindow("animation_cloner_tests", 32, 32, SDL_WINDOW_HIDDEN);
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

SDL_Texture* create_solid_texture(SDL_Renderer* renderer, int width, int height, SDL_Color color) {
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

bool query_texture_size(SDL_Texture* texture, int& width, int& height) {
    width = 0;
    height = 0;
    if (!texture) {
        return false;
    }
    float wf = 0.0f;
    float hf = 0.0f;
    if (!SDL_GetTextureSize(texture, &wf, &hf)) {
        return false;
    }
    width = static_cast<int>(std::lround(wf));
    height = static_cast<int>(std::lround(hf));
    return width > 0 && height > 0;
}

} // namespace

TEST_CASE("AnimationCloner keeps local on_end when inherit flag is disabled") {
    Animation source = make_single_frame_animation();
    Animation destination = make_single_frame_animation();

    source.on_end_animation = "kill";
    source.on_end_behavior = Animation::classify_on_end(source.on_end_animation);
    destination.on_end_animation = "reverse";
    destination.on_end_behavior = Animation::classify_on_end(destination.on_end_animation);

    AssetInfo info("test_asset");
    AnimationCloner::Options options{};
    options.inherit_on_end_from_source = false;

    REQUIRE(AnimationCloner::Clone(source, destination, options, sentinel_renderer(), info));
    CHECK(destination.on_end_animation == "reverse");
    CHECK(destination.on_end_behavior == Animation::OnEndDirective::Reverse);
}

TEST_CASE("AnimationCloner inherits source on_end and directive when enabled") {
    Animation source = make_single_frame_animation();
    Animation destination = make_single_frame_animation();

    source.on_end_animation = "next_anim";
    source.on_end_behavior = Animation::classify_on_end(source.on_end_animation);
    destination.on_end_animation = "loop";
    destination.on_end_behavior = Animation::classify_on_end(destination.on_end_animation);

    AssetInfo info("test_asset");
    AnimationCloner::Options options{};
    options.inherit_on_end_from_source = true;

    REQUIRE(AnimationCloner::Clone(source, destination, options, sentinel_renderer(), info));
    CHECK(destination.on_end_animation == "next_anim");
    CHECK(destination.on_end_behavior == Animation::OnEndDirective::Animation);
}

TEST_CASE("AnimationCloner preserves source overlay texture dimensions") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());
    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    Animation source;
    Animation::FrameCache cache;
    cache.resize(1);
    cache.textures[0] = create_solid_texture(renderer, 4, 4, SDL_Color{255, 0, 0, 255});
    cache.widths[0] = 4;
    cache.heights[0] = 4;
    cache.foreground_textures[0] = create_solid_texture(renderer, 8, 8, SDL_Color{0, 255, 0, 255});
    cache.background_textures[0] = create_solid_texture(renderer, 8, 8, SDL_Color{0, 0, 255, 255});
    REQUIRE(cache.textures[0] != nullptr);
    REQUIRE(cache.foreground_textures[0] != nullptr);
    REQUIRE(cache.background_textures[0] != nullptr);

    std::vector<Animation::FrameCache> source_caches;
    source_caches.push_back(std::move(cache));
    source.adopt_prebuilt_frames(std::move(source_caches), {1.0f});

    Animation destination;
    AssetInfo info("test_asset");
    AnimationCloner::Options options{};
    REQUIRE(AnimationCloner::Clone(source, destination, options, renderer, info));

    REQUIRE(destination.cached_frame_count() == 1);
    const Animation::FrameCache& cloned = destination.cached_frames()[0];
    REQUIRE(cloned.textures.size() >= 1);
    REQUIRE(cloned.foreground_textures.size() >= 1);
    REQUIRE(cloned.background_textures.size() >= 1);
    REQUIRE(cloned.widths.size() >= 1);
    REQUIRE(cloned.heights.size() >= 1);

    CHECK(cloned.widths[0] == 4);
    CHECK(cloned.heights[0] == 4);

    int fg_w = 0;
    int fg_h = 0;
    REQUIRE(query_texture_size(cloned.foreground_textures[0], fg_w, fg_h));
    CHECK(fg_w == 8);
    CHECK(fg_h == 8);

    int bg_w = 0;
    int bg_h = 0;
    REQUIRE(query_texture_size(cloned.background_textures[0], bg_w, bg_h));
    CHECK(bg_w == 8);
    CHECK(bg_h == 8);
}
