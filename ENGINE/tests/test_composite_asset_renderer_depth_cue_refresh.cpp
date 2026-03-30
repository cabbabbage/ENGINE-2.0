#include <doctest/doctest.h>

#include <cmath>
#include <memory>

#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_info.hpp"
#include "core/manifest/depth_cue_settings.hpp"
#include "rendering/render/composite_asset_renderer.hpp"

namespace {

Asset make_test_asset(int world_x, int world_y) {
    auto info = std::make_shared<AssetInfo>("depth_cue_merge_refresh_asset");
    Area spawn_area("depth_cue_merge_refresh_area", 0);
    return Asset(info, spawn_area, SDL_Point{world_x, world_y}, 0);
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
                   static_cast<int>(mid_foreground_signature.overlay_alpha)) < 8);
    CHECK_FALSE(renderer.test_should_mark_composite_dirty_for_depth_cue_merge(&asset, small_delta_signature));

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
