#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

#include "assets/asset/animation.hpp"
#include "assets/asset/animation_loader.hpp"
#include "assets/asset/asset_info.hpp"

namespace {

Animation::FrameCache make_cache_frame(std::size_t variant_count = 1) {
    Animation::FrameCache cache;
    cache.resize(variant_count);
    return cache;
}

}  // namespace

TEST_CASE("AnimationLoader loads and normalizes per-animation tags from payload") {
    AssetInfo info("animation_loader_tags_asset");
    Animation animation;
    animation.tags = {"stale"};

    nlohmann::json payload = {
        {"source", {{"kind", "folder"}, {"path", "default"}, {"name", ""}}},
        {"number_of_frames", 1},
        {"tags", nlohmann::json::array({" Run ", "run", "combat"})},
    };

    PrebuiltAnimationFrames prebuilt;
    prebuilt.variant_steps = {1.0f};
    prebuilt.frames.push_back(make_cache_frame());
    prebuilt.canvas_width = 16;
    prebuilt.canvas_height = 16;

    SDL_Texture* base_sprite = nullptr;
    int scaled_w = 0;
    int scaled_h = 0;
    int canvas_w = 0;
    int canvas_h = 0;
    AnimationLoader::load(animation,
                          "default",
                          payload,
                          info,
                          ".",
                          "cache",
                          1.0f,
                          nullptr,
                          base_sprite,
                          scaled_w,
                          scaled_h,
                          canvas_w,
                          canvas_h,
                          false,
                          nullptr,
                          &prebuilt);

    CHECK(animation.tags == std::vector<std::string>{"run", "combat"});
}

TEST_CASE("AnimationLoader does not inherit tags from source animations") {
    AssetInfo info("animation_loader_no_inherit_tags_asset");
    Animation source;
    source.source.kind = "folder";
    source.tags = {"source_tag"};
    info.animations["base"] = source;

    Animation animation;
    nlohmann::json payload = {
        {"source", {{"kind", "animation"}, {"path", "base"}, {"name", "base"}}},
        {"number_of_frames", 1},
        {"inherit_data", true},
        {"tags", nlohmann::json::array({"derived_tag"})},
    };

    SDL_Texture* base_sprite = nullptr;
    int scaled_w = 0;
    int scaled_h = 0;
    int canvas_w = 0;
    int canvas_h = 0;
    AnimationLoader::load(animation,
                          "derived",
                          payload,
                          info,
                          ".",
                          "cache",
                          1.0f,
                          nullptr,
                          base_sprite,
                          scaled_w,
                          scaled_h,
                          canvas_w,
                          canvas_h,
                          false);

    CHECK(animation.tags == std::vector<std::string>{"derived_tag"});
    CHECK(std::find(animation.tags.begin(), animation.tags.end(), "source_tag") == animation.tags.end());
}

TEST_CASE("AnimationLoader clears stale runtime tags when payload omits tags") {
    AssetInfo info("animation_loader_tag_reset_asset");
    Animation animation;
    animation.tags = {"old_tag"};

    nlohmann::json payload = {
        {"source", {{"kind", "folder"}, {"path", "default"}, {"name", ""}}},
        {"number_of_frames", 1},
    };

    PrebuiltAnimationFrames prebuilt;
    prebuilt.variant_steps = {1.0f};
    prebuilt.frames.push_back(make_cache_frame());
    prebuilt.canvas_width = 8;
    prebuilt.canvas_height = 8;

    SDL_Texture* base_sprite = nullptr;
    int scaled_w = 0;
    int scaled_h = 0;
    int canvas_w = 0;
    int canvas_h = 0;
    AnimationLoader::load(animation,
                          "default",
                          payload,
                          info,
                          ".",
                          "cache",
                          1.0f,
                          nullptr,
                          base_sprite,
                          scaled_w,
                          scaled_h,
                          canvas_w,
                          canvas_h,
                          false,
                          nullptr,
                          &prebuilt);

    CHECK(animation.tags.empty());
}
