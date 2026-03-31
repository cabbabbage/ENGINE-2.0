#include <doctest/doctest.h>

#include <cmath>
#include <memory>

#include <SDL3/SDL.h>

#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_info.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/animation_frame.hpp"
#include "core/manifest/depth_cue_settings.hpp"
#include "rendering/render/composite_asset_renderer.hpp"

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
        window_ = SDL_CreateWindow("depth_cue_tests", 32, 32, SDL_WINDOW_HIDDEN);
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

Asset make_test_asset(int world_x, int world_y) {
    auto info = std::make_shared<AssetInfo>("depth_cue_merge_refresh_asset");
    Area spawn_area("depth_cue_merge_refresh_area", 0);
    return Asset(info, spawn_area, SDL_Point{world_x, world_y}, 0);
}

void bind_single_variant_frame(Asset& asset,
                               AnimationFrame& frame,
                               SDL_Texture* base_texture,
                               SDL_Texture* foreground_texture,
                               SDL_Texture* background_texture) {
    frame.variants.clear();
    FrameVariant variant{};
    variant.base_texture = base_texture;
    variant.foreground_texture = foreground_texture;
    variant.background_texture = background_texture;
    frame.variants.push_back(variant);

    asset.current_animation = "idle";
    asset.info->animations["idle"] = Animation{};
    asset.current_frame = &frame;
    asset.current_variant_index = 0;
    asset.current_remaining_scale_adjustment = 1.0f;
}

} // namespace

TEST_CASE("depth cue merge signature transitions trigger rebuild at expected thresholds") {
    CompositeAssetRenderer renderer(nullptr, nullptr);
    Asset asset = make_test_asset(0, 0);

    depth_cue::DepthCueSettings settings{};
    settings.center_depth_offset = 0.0f;
    settings.foreground_max_depth_offset = -600.0f;
    settings.background_max_depth_offset = 600.0f;

    SDL_Texture* base_texture = reinterpret_cast<SDL_Texture*>(0x1);
    SDL_Texture* foreground_texture = reinterpret_cast<SDL_Texture*>(0x2);
    SDL_Texture* background_texture = reinterpret_cast<SDL_Texture*>(0x3);

    const DepthCueMergeSignature neutral_signature = renderer.test_build_depth_cue_merge_signature(
        base_texture,
        foreground_texture,
        background_texture,
        0.0f,
        255,
        true,
        settings);
    CHECK(neutral_signature.valid);
    CHECK_FALSE(neutral_signature.overlay_active);

    asset.test_set_depth_cue_merge_applied_signature(neutral_signature);

    const DepthCueMergeSignature foreground_signature = renderer.test_build_depth_cue_merge_signature(
        base_texture,
        foreground_texture,
        background_texture,
        -900.0f,
        255,
        true,
        settings);
    CHECK(foreground_signature.overlay_active);
    CHECK(foreground_signature.overlay_layer == 1);
    CHECK(renderer.test_should_mark_composite_dirty_for_depth_cue_merge(&asset, foreground_signature));

    const DepthCueMergeSignature mid_foreground_signature = renderer.test_build_depth_cue_merge_signature(
        base_texture,
        foreground_texture,
        background_texture,
        -300.0f,
        255,
        true,
        settings);
    REQUIRE(mid_foreground_signature.overlay_active);
    REQUIRE(mid_foreground_signature.overlay_layer == foreground_signature.overlay_layer);

    asset.test_set_depth_cue_merge_applied_signature(mid_foreground_signature);

    const DepthCueMergeSignature small_delta_signature = renderer.test_build_depth_cue_merge_signature(
        base_texture,
        foreground_texture,
        background_texture,
        -285.0f,
        255,
        true,
        settings);
    REQUIRE(small_delta_signature.overlay_active);
    CHECK(small_delta_signature.overlay_layer == mid_foreground_signature.overlay_layer);
    CHECK(std::abs(static_cast<int>(small_delta_signature.overlay_alpha) -
                   static_cast<int>(mid_foreground_signature.overlay_alpha)) >= 1);
    CHECK(renderer.test_should_mark_composite_dirty_for_depth_cue_merge(&asset, small_delta_signature));

    const DepthCueMergeSignature large_delta_signature = renderer.test_build_depth_cue_merge_signature(
        base_texture,
        foreground_texture,
        background_texture,
        -280.0f,
        255,
        true,
        settings);
    REQUIRE(large_delta_signature.overlay_active);
    CHECK(large_delta_signature.overlay_layer == mid_foreground_signature.overlay_layer);
    CHECK(std::abs(static_cast<int>(large_delta_signature.overlay_alpha) -
                   static_cast<int>(mid_foreground_signature.overlay_alpha)) >= 8);
    CHECK(renderer.test_should_mark_composite_dirty_for_depth_cue_merge(&asset, large_delta_signature));

    asset.test_set_depth_cue_merge_applied_signature(large_delta_signature);

    const DepthCueMergeSignature neutral_again_signature = renderer.test_build_depth_cue_merge_signature(
        base_texture,
        foreground_texture,
        background_texture,
        0.0f,
        255,
        true,
        settings);
    CHECK_FALSE(neutral_again_signature.overlay_active);
    CHECK(renderer.test_should_mark_composite_dirty_for_depth_cue_merge(&asset, neutral_again_signature));

    asset.test_set_depth_cue_merge_applied_signature(neutral_again_signature);

    const DepthCueMergeSignature background_signature = renderer.test_build_depth_cue_merge_signature(
        base_texture,
        foreground_texture,
        background_texture,
        900.0f,
        255,
        true,
        settings);
    CHECK(background_signature.overlay_active);
    CHECK(background_signature.overlay_layer == 2);
    CHECK(renderer.test_should_mark_composite_dirty_for_depth_cue_merge(&asset, background_signature));
}

TEST_CASE("depth cue layered package places foreground overlays after base") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());

    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    SDL_Texture* base_texture = create_solid_texture(renderer, 4, 4, SDL_Color{255, 0, 0, 255});
    SDL_Texture* overlay_texture = create_solid_texture(renderer, 8, 8, SDL_Color{0, 0, 255, 255});
    REQUIRE(base_texture != nullptr);
    REQUIRE(overlay_texture != nullptr);

    Asset asset = make_test_asset(120, 240);
    AnimationFrame frame{};
    bind_single_variant_frame(asset, frame, base_texture, overlay_texture, nullptr);

    DepthCueMergeSignature desired_signature{};
    desired_signature.valid = true;
    desired_signature.base_texture = base_texture;
    desired_signature.overlay_active = true;
    desired_signature.overlay_texture = overlay_texture;
    desired_signature.overlay_layer = 1;
    desired_signature.overlay_alpha = 170;

    CompositeAssetRenderer composite_renderer(renderer, nullptr);
    composite_renderer.test_regenerate_package_with_signature(&asset, 1.0f, desired_signature);

    REQUIRE(asset.render_package.size() == 2);
    const RenderObject& base_obj = asset.render_package[0];
    const RenderObject& overlay_obj = asset.render_package[1];

    CHECK(base_obj.texture == base_texture);
    CHECK(base_obj.screen_rect.w == 4);
    CHECK(base_obj.screen_rect.h == 4);
    CHECK(base_obj.projection_anchor_uv.x == doctest::Approx(0.5f));
    CHECK(base_obj.projection_anchor_uv.y == doctest::Approx(1.0f));

    CHECK(overlay_obj.texture == overlay_texture);
    CHECK(overlay_obj.screen_rect.w == 8);
    CHECK(overlay_obj.screen_rect.h == 8);
    CHECK(overlay_obj.projection_anchor_uv.x == doctest::Approx(0.5f));
    CHECK(overlay_obj.projection_anchor_uv.y == doctest::Approx(0.75f));
    CHECK(overlay_obj.color_mod.a == desired_signature.overlay_alpha);
    CHECK(overlay_obj.world_anchor_x == doctest::Approx(base_obj.world_anchor_x));
    CHECK(overlay_obj.world_anchor_y == doctest::Approx(base_obj.world_anchor_y));

    SDL_DestroyTexture(base_texture);
    SDL_DestroyTexture(overlay_texture);
}

TEST_CASE("depth cue layered package places background overlays before base") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());

    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    SDL_Texture* base_texture = create_solid_texture(renderer, 4, 4, SDL_Color{255, 0, 0, 255});
    SDL_Texture* overlay_texture = create_solid_texture(renderer, 8, 8, SDL_Color{0, 0, 255, 255});
    REQUIRE(base_texture != nullptr);
    REQUIRE(overlay_texture != nullptr);

    Asset asset = make_test_asset(100, 220);
    AnimationFrame frame{};
    bind_single_variant_frame(asset, frame, base_texture, nullptr, overlay_texture);

    DepthCueMergeSignature desired_signature{};
    desired_signature.valid = true;
    desired_signature.base_texture = base_texture;
    desired_signature.overlay_active = true;
    desired_signature.overlay_texture = overlay_texture;
    desired_signature.overlay_layer = 2;
    desired_signature.overlay_alpha = 200;

    CompositeAssetRenderer composite_renderer(renderer, nullptr);
    composite_renderer.test_regenerate_package_with_signature(&asset, 1.0f, desired_signature);

    REQUIRE(asset.render_package.size() == 2);
    const RenderObject& overlay_obj = asset.render_package[0];
    const RenderObject& base_obj = asset.render_package[1];

    CHECK(overlay_obj.texture == overlay_texture);
    CHECK(base_obj.texture == base_texture);
    CHECK(overlay_obj.color_mod.a == desired_signature.overlay_alpha);
    CHECK(overlay_obj.screen_rect.w == 8);
    CHECK(overlay_obj.screen_rect.h == 8);
    CHECK(base_obj.screen_rect.w == 4);
    CHECK(base_obj.screen_rect.h == 4);
    CHECK(overlay_obj.projection_anchor_uv.x == doctest::Approx(0.5f));
    CHECK(overlay_obj.projection_anchor_uv.y == doctest::Approx(0.75f));

    SDL_DestroyTexture(base_texture);
    SDL_DestroyTexture(overlay_texture);
}

TEST_CASE("depth cue layered package emits base only when overlay is inactive") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());

    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    SDL_Texture* base_texture = create_solid_texture(renderer, 4, 4, SDL_Color{255, 0, 0, 255});
    REQUIRE(base_texture != nullptr);

    Asset asset = make_test_asset(96, 144);
    AnimationFrame frame{};
    bind_single_variant_frame(asset, frame, base_texture, nullptr, nullptr);

    DepthCueMergeSignature desired_signature{};
    desired_signature.valid = true;
    desired_signature.base_texture = base_texture;
    desired_signature.overlay_active = false;

    CompositeAssetRenderer composite_renderer(renderer, nullptr);
    composite_renderer.test_regenerate_package_with_signature(&asset, 1.0f, desired_signature);

    REQUIRE(asset.render_package.size() == 1);
    const RenderObject& base_obj = asset.render_package[0];
    CHECK(base_obj.texture == base_texture);
    CHECK(base_obj.projection_anchor_uv.x == doctest::Approx(0.5f));
    CHECK(base_obj.projection_anchor_uv.y == doctest::Approx(1.0f));

    SDL_DestroyTexture(base_texture);
}
