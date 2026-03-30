#include <doctest/doctest.h>

#include <memory>

#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_info.hpp"

#define private public
#include "rendering/render/composite_asset_renderer.hpp"
#undef private

namespace {

Asset make_test_asset(int world_x, int world_y) {
    auto info = std::make_shared<AssetInfo>("depth_cue_overlay_refresh_asset");
    Area spawn_area("depth_cue_overlay_refresh_area", 0);
    return Asset(info, spawn_area, SDL_Point{world_x, world_y}, 0);
}

RenderObject make_base_object() {
    RenderObject base{};
    base.texture = reinterpret_cast<SDL_Texture*>(0x1);
    base.screen_rect = SDL_Rect{320, 180, 96, 64};
    base.world_anchor_x = 320.0f;
    base.world_anchor_y = 180.0f;
    base.color_mod = SDL_Color{255, 255, 255, 255};
    base.blend_mode = SDL_BLENDMODE_BLEND;
    base.angle = 0.0;
    base.flip = SDL_FLIP_NONE;
    base.world_z_offset = 0.0f;
    base.texture_w = 96;
    base.texture_h = 64;
    base.has_texture_size = true;
    base.projection_anchor_uv = SDL_FPoint{0.5f, 1.0f};
    base.is_depth_cue_overlay = false;
    return base;
}

RenderObject make_overlay_object(SDL_Texture* texture, Uint8 alpha, int width, int height) {
    RenderObject overlay{};
    overlay.texture = texture;
    overlay.screen_rect = SDL_Rect{320, 180, width, height};
    overlay.world_anchor_x = 320.0f;
    overlay.world_anchor_y = 180.0f;
    overlay.color_mod = SDL_Color{255, 255, 255, alpha};
    overlay.blend_mode = SDL_BLENDMODE_BLEND;
    overlay.angle = 0.0;
    overlay.flip = SDL_FLIP_NONE;
    overlay.world_z_offset = 0.0f;
    overlay.texture_w = width;
    overlay.texture_h = height;
    overlay.has_texture_size = true;
    overlay.atlas_w = width;
    overlay.atlas_h = height;
    overlay.has_atlas_size = true;
    overlay.projection_anchor_uv = SDL_FPoint{0.5f, 0.75f};
    overlay.is_depth_cue_overlay = true;
    return overlay;
}

} // namespace

TEST_CASE("depth cue overlay refresh updates render package without composite rebuild") {
    Asset asset = make_test_asset(0, 0);
    asset.render_package.clear();
    asset.render_package.push_back(make_base_object());

    CompositeAssetRenderer renderer(nullptr, nullptr);
    SDL_Texture* overlay_texture = reinterpret_cast<SDL_Texture*>(0x2);
    const std::size_t base_index = 0;

    const RenderObject first_overlay = make_overlay_object(overlay_texture, 120, 192, 128);
    CHECK(renderer.upsert_depth_cue_overlay_object(&asset, base_index, first_overlay));
    REQUIRE(asset.render_package.size() == 2);
    CHECK(asset.render_package[1].is_depth_cue_overlay);
    CHECK(asset.render_package[1].color_mod.a == 120);
    CHECK(asset.render_package[1].screen_rect.w == 192);
    CHECK(asset.render_package[1].screen_rect.h == 128);

    CHECK_FALSE(renderer.upsert_depth_cue_overlay_object(&asset, base_index, first_overlay));
    REQUIRE(asset.render_package.size() == 2);
    CHECK(asset.render_package[1].color_mod.a == 120);

    const RenderObject updated_alpha_overlay = make_overlay_object(overlay_texture, 180, 192, 128);
    CHECK_FALSE(renderer.upsert_depth_cue_overlay_object(&asset, base_index, updated_alpha_overlay));
    REQUIRE(asset.render_package.size() == 2);
    CHECK(asset.render_package[1].color_mod.a == 180);
    CHECK(asset.render_package[1].screen_rect.w == 192);
    CHECK(asset.render_package[1].screen_rect.h == 128);

    const RenderObject resized_overlay = make_overlay_object(overlay_texture, 180, 256, 160);
    CHECK(renderer.upsert_depth_cue_overlay_object(&asset, base_index, resized_overlay));
    REQUIRE(asset.render_package.size() == 2);
    CHECK(asset.render_package[1].screen_rect.w == 256);
    CHECK(asset.render_package[1].screen_rect.h == 160);

    CHECK(renderer.remove_depth_cue_overlay_objects(&asset));
    REQUIRE(asset.render_package.size() == 1);
    CHECK_FALSE(asset.render_package[0].is_depth_cue_overlay);

    CHECK_FALSE(renderer.remove_depth_cue_overlay_objects(&asset));
}
