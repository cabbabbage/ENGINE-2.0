#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include "assets/asset/animation.hpp"
#include "assets/asset/animation_cloner.hpp"
#include "assets/asset/animation_loader.hpp"
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

Animation make_animation_with_movement(const std::vector<SDL_Point>& horizontal_vertical,
                                       const std::vector<int>& z_values) {
    Animation animation;
    if (horizontal_vertical.size() != z_values.size()) {
        return animation;
    }
    std::vector<Animation::FrameCache> caches(horizontal_vertical.size());
    for (auto& cache : caches) {
        cache.resize(1);
    }
    animation.adopt_prebuilt_frames(std::move(caches), {1.0f});

    std::vector<AnimationFrame> path(horizontal_vertical.size());
    int total_dx = 0;
    int total_dy = 0;
    int total_dz = 0;
    bool any_motion = false;
    for (std::size_t i = 0; i < horizontal_vertical.size(); ++i) {
        path[i].dx = horizontal_vertical[i].x;
        path[i].dy = horizontal_vertical[i].y;
        path[i].dz = z_values[i];
        total_dx += path[i].dx;
        total_dy += path[i].dy;
        total_dz += path[i].dz;
        any_motion = any_motion || path[i].dx != 0 || path[i].dy != 0 || path[i].dz != 0;
    }
    animation.replace_movement_paths({path});
    animation.total_dx = total_dx;
    animation.total_dy = total_dy;
    animation.total_dz = total_dz;
    animation.movment = any_motion;
    animation.synchronize_runtime_frames();
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

TEST_CASE("AnimationCloner preserves destination tags and does not inherit source tags") {
    Animation source = make_single_frame_animation();
    Animation destination = make_single_frame_animation();

    source.tags = {"source_only", "shared"};
    destination.tags = {"derived_only", "shared"};

    AssetInfo info("test_asset");
    AnimationCloner::Options options{};

    REQUIRE(AnimationCloner::Clone(source, destination, options, sentinel_renderer(), info));
    CHECK(destination.tags == std::vector<std::string>{"derived_only", "shared"});
    CHECK(std::find(destination.tags.begin(), destination.tags.end(), "source_only") == destination.tags.end());
}

TEST_CASE("AnimationCloner preserves source base texture dimensions") {
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
    REQUIRE(cache.textures[0] != nullptr);

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
    REQUIRE(cloned.widths.size() >= 1);
    REQUIRE(cloned.heights.size() >= 1);

    CHECK(cloned.widths[0] == 4);
    CHECK(cloned.heights[0] == 4);

    int base_w = 0;
    int base_h = 0;
    REQUIRE(query_texture_size(cloned.textures[0], base_w, base_h));
    CHECK(base_w == 4);
    CHECK(base_h == 4);
}

TEST_CASE("AnimationCloner applies movement reverse and inversion options to cloned movement and totals") {
    Animation source = make_animation_with_movement({SDL_Point{1, 2}, SDL_Point{-4, 5}, SDL_Point{7, -8}},
                                                    {3, -6, 9});
    Animation destination = make_single_frame_animation();

    AssetInfo info("test_asset");
    AnimationCloner::Options options{};
    options.reverse_frames = true;
    options.invert_movement_x = true;
    options.invert_movement_z = true;

    REQUIRE(AnimationCloner::Clone(source, destination, options, sentinel_renderer(), info));
    REQUIRE(destination.movement_path_count() == 1);
    REQUIRE(destination.frame_count() == 3);

    const auto& frames = destination.movement_path(0);
    REQUIRE(frames.size() == 3);
    CHECK(frames[0].dx == -7);
    CHECK(frames[0].dy == -8);
    CHECK(frames[0].dz == -9);
    CHECK(frames[1].dx == 4);
    CHECK(frames[1].dy == 5);
    CHECK(frames[1].dz == 6);
    CHECK(frames[2].dx == -1);
    CHECK(frames[2].dy == 2);
    CHECK(frames[2].dz == -3);

    CHECK(destination.total_dx == -4);
    CHECK(destination.total_dy == -1);
    CHECK(destination.total_dz == -6);
    CHECK(destination.movment);
}

TEST_CASE("AnimationLoader applies sourced movement inversion only when inherit_data is enabled") {
    AssetInfo info("loader_inherit_policy_asset");
    info.movement_enabled = true;
    info.animations["base"] = make_animation_with_movement({SDL_Point{1, 2}, SDL_Point{-4, 5}, SDL_Point{7, -8}},
                                                           {3, -6, 9});

    nlohmann::json inherit_payload = {
        {"source", {{"kind", "animation"}, {"path", "base"}, {"name", "base"}}},
        {"number_of_frames", 3},
        {"inherit_data", true},
        {"reverse_source", true},
        {"invert_x", true},
        {"invert_y", false},
        {"invert_z", true},
    };

    SDL_Texture* base_sprite = nullptr;
    int scaled_w = 0;
    int scaled_h = 0;
    int canvas_w = 0;
    int canvas_h = 0;
    AnimationLoader::load(info.animations["derived_inherit"],
                          "derived_inherit",
                          inherit_payload,
                          info,
                          ".",
                          "cache",
                          1.0f,
                          sentinel_renderer(),
                          base_sprite,
                          scaled_w,
                          scaled_h,
                          canvas_w,
                          canvas_h,
                          false);

    const auto& inherited_frames = info.animations["derived_inherit"].movement_path(0);
    REQUIRE(inherited_frames.size() == 3);
    CHECK(inherited_frames[0].dx == -7);
    CHECK(inherited_frames[0].dy == -8);
    CHECK(inherited_frames[0].dz == -9);
    CHECK(inherited_frames[1].dx == 4);
    CHECK(inherited_frames[1].dy == 5);
    CHECK(inherited_frames[1].dz == 6);
    CHECK(inherited_frames[2].dx == -1);
    CHECK(inherited_frames[2].dy == 2);
    CHECK(inherited_frames[2].dz == -3);
    CHECK(info.animations["derived_inherit"].total_dx == -4);
    CHECK(info.animations["derived_inherit"].total_dy == -1);
    CHECK(info.animations["derived_inherit"].total_dz == -6);

    nlohmann::json local_payload = {
        {"source", {{"kind", "animation"}, {"path", "base"}, {"name", "base"}}},
        {"number_of_frames", 3},
        {"inherit_data", false},
        {"reverse_source", true},
        {"invert_x", true},
        {"invert_y", true},
        {"invert_z", true},
        {"movement",
         nlohmann::json::array({
             nlohmann::json::array({5, 6, 7}),
             nlohmann::json::array({8, 9, 10}),
             nlohmann::json::array({11, 12, 13}),
         })},
    };

    AnimationLoader::load(info.animations["derived_local"],
                          "derived_local",
                          local_payload,
                          info,
                          ".",
                          "cache",
                          1.0f,
                          sentinel_renderer(),
                          base_sprite,
                          scaled_w,
                          scaled_h,
                          canvas_w,
                          canvas_h,
                          false);

    const auto& local_frames = info.animations["derived_local"].movement_path(0);
    REQUIRE(local_frames.size() == 3);
    CHECK(local_frames[0].dx == 5);
    CHECK(local_frames[0].dy == 6);
    CHECK(local_frames[0].dz == 7);
    CHECK(local_frames[1].dx == 8);
    CHECK(local_frames[1].dy == 9);
    CHECK(local_frames[1].dz == 10);
    CHECK(local_frames[2].dx == 11);
    CHECK(local_frames[2].dy == 12);
    CHECK(local_frames[2].dz == 13);
    CHECK(info.animations["derived_local"].total_dx == 24);
    CHECK(info.animations["derived_local"].total_dy == 27);
    CHECK(info.animations["derived_local"].total_dz == 30);
}
