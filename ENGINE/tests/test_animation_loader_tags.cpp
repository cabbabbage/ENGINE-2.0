#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

void load_animation_with_prebuilt(Animation& animation,
                                  AssetInfo& info,
                                  const nlohmann::json& payload,
                                  const char* trigger = "default") {
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
                          trigger,
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

TEST_CASE("AnimationLoader keeps local tags when derived animation clones frames from source animation") {
    AssetInfo info("animation_loader_derived_clone_tags_asset");
    Animation source;
    source.source.kind = "folder";
    source.tags = {"source_tag"};
    source.adopt_prebuilt_frames(std::vector<Animation::FrameCache>{make_cache_frame()}, {1.0f});
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
    SDL_Renderer* renderer = reinterpret_cast<SDL_Renderer*>(static_cast<std::uintptr_t>(1));
    AnimationLoader::load(animation,
                          "derived",
                          payload,
                          info,
                          ".",
                          "cache",
                          1.0f,
                          renderer,
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

TEST_CASE("AnimationLoader parses hit and attack box forward/backward extrusion fields") {
    AssetInfo info("animation_loader_box_extrusion_fields_asset");
    info.hitbox_enabled = true;
    info.attack_box_enabled = true;
    Animation animation;

    nlohmann::json payload = {
        {"source", {{"kind", "folder"}, {"path", "default"}, {"name", ""}}},
        {"number_of_frames", 1},
        {"hit_boxes",
         nlohmann::json::array({
             nlohmann::json::array({
                 {{"id", "hb_1"},
                  {"name", "hb_1"},
                  {"position", {{"x", 2}, {"y", 3}}},
                  {"size", {{"w", 4}, {"h", 5}}},
                  {"extrusion_forward", 7},
                  {"extrusion_backward", 2}},
             }),
         })},
        {"attack_boxes",
         nlohmann::json::array({
             nlohmann::json::array({
                 {{"id", "ab_1"},
                  {"name", "ab_1"},
                  {"position", {{"x", 6}, {"y", 7}}},
                  {"size", {{"w", 8}, {"h", 9}}},
                  {"extrusion_forward", 11},
                  {"extrusion_backward", 4},
                  {"damage_amount", 12}},
             }),
         })},
    };

    load_animation_with_prebuilt(animation, info, payload);

    const AnimationFrame* frame = animation.primary_frame_at(0);
    REQUIRE(frame != nullptr);
    REQUIRE(frame->get_hit_boxes().boxes.size() == 1);
    REQUIRE(frame->get_attack_boxes().boxes.size() == 1);
    CHECK(frame->get_hit_boxes().boxes[0].extrusion_forward == 7);
    CHECK(frame->get_hit_boxes().boxes[0].extrusion_backward == 2);
    CHECK(frame->get_attack_boxes().boxes[0].extrusion_forward == 11);
    CHECK(frame->get_attack_boxes().boxes[0].extrusion_backward == 4);
}

TEST_CASE("AnimationLoader ignores legacy flatten_bottom_to_floor fields") {
    AssetInfo info("animation_loader_legacy_flatten_ignored_asset");
    info.hitbox_enabled = true;
    info.attack_box_enabled = true;
    Animation animation;

    nlohmann::json payload = {
        {"source", {{"kind", "folder"}, {"path", "default"}, {"name", ""}}},
        {"number_of_frames", 1},
        {"hit_boxes",
         nlohmann::json::array({
             nlohmann::json::array({
                 {{"id", "hb_flatten"},
                  {"name", "hb_flatten"},
                  {"position", {{"x", 2}, {"y", 3}}},
                  {"size", {{"w", 4}, {"h", 5}}},
                  {"rotation_degrees", 37.0},
                  {"flatten_bottom_to_floor", true},
                  {"extrusion_forward", 6},
                  {"extrusion_backward", 2}},
             }),
         })},
        {"attack_boxes",
         nlohmann::json::array({
             nlohmann::json::array({
                 {{"id", "ab_flatten"},
                  {"name", "ab_flatten"},
                  {"position", {{"x", 6}, {"y", 7}}},
                  {"size", {{"w", 8}, {"h", 9}}},
                  {"rotation_degrees", -22.0},
                  {"flatten_bottom_to_floor", false},
                  {"damage_amount", 9},
                  {"extrusion_forward", 10},
                  {"extrusion_backward", 3}},
             }),
         })},
    };

    load_animation_with_prebuilt(animation, info, payload);

    const AnimationFrame* frame = animation.primary_frame_at(0);
    REQUIRE(frame != nullptr);
    REQUIRE(frame->get_hit_boxes().boxes.size() == 1);
    REQUIRE(frame->get_attack_boxes().boxes.size() == 1);
    CHECK(frame->get_hit_boxes().boxes[0].extrusion_forward == 6);
    CHECK(frame->get_hit_boxes().boxes[0].extrusion_backward == 2);
    CHECK(frame->get_attack_boxes().boxes[0].extrusion_forward == 10);
    CHECK(frame->get_attack_boxes().boxes[0].extrusion_backward == 3);
    CHECK(frame->get_hit_boxes().boxes[0].rotation_degrees == doctest::Approx(37.0f));
    CHECK(frame->get_attack_boxes().boxes[0].rotation_degrees == doctest::Approx(-22.0f));
}

TEST_CASE("AnimationLoader ignores legacy extrusion_amount when new extrusion fields are absent") {
    AssetInfo info("animation_loader_legacy_extrusion_ignored_asset");
    info.hitbox_enabled = true;
    info.attack_box_enabled = true;
    Animation animation;

    nlohmann::json payload = {
        {"source", {{"kind", "folder"}, {"path", "default"}, {"name", ""}}},
        {"number_of_frames", 1},
        {"hit_boxes",
         nlohmann::json::array({
             nlohmann::json::array({
                 {{"id", "hb_legacy"},
                  {"name", "hb_legacy"},
                  {"position", {{"x", 1}, {"y", 1}}},
                  {"size", {{"w", 3}, {"h", 3}}},
                  {"extrusion_amount", 99}},
             }),
         })},
        {"attack_boxes",
         nlohmann::json::array({
             nlohmann::json::array({
                 {{"id", "ab_legacy"},
                  {"name", "ab_legacy"},
                  {"position", {{"x", 4}, {"y", 4}}},
                  {"size", {{"w", 5}, {"h", 5}}},
                  {"extrusion_amount", 77},
                  {"damage_amount", 1}},
             }),
         })},
    };

    load_animation_with_prebuilt(animation, info, payload);

    const AnimationFrame* frame = animation.primary_frame_at(0);
    REQUIRE(frame != nullptr);
    REQUIRE(frame->get_hit_boxes().boxes.size() == 1);
    REQUIRE(frame->get_attack_boxes().boxes.size() == 1);
    CHECK(frame->get_hit_boxes().boxes[0].extrusion_forward == 1);
    CHECK(frame->get_hit_boxes().boxes[0].extrusion_backward == 1);
    CHECK(frame->get_attack_boxes().boxes[0].extrusion_forward == 1);
    CHECK(frame->get_attack_boxes().boxes[0].extrusion_backward == 1);
}

TEST_CASE("AnimationLoader defaults and clamps extrusion_forward/backward to minimum one") {
    AssetInfo info("animation_loader_extrusion_defaults_clamps_asset");
    info.hitbox_enabled = true;
    info.attack_box_enabled = true;
    Animation animation;

    nlohmann::json payload = {
        {"source", {{"kind", "folder"}, {"path", "default"}, {"name", ""}}},
        {"number_of_frames", 1},
        {"hit_boxes",
         nlohmann::json::array({
             nlohmann::json::array({
                 {{"id", "hb_clamp"},
                  {"name", "hb_clamp"},
                  {"position", {{"x", 10}, {"y", 11}}},
                  {"size", {{"w", 2}, {"h", 2}}},
                  {"extrusion_forward", 0},
                  {"extrusion_backward", -5}},
                 {{"id", "hb_default"},
                  {"name", "hb_default"},
                  {"position", {{"x", 12}, {"y", 13}}},
                  {"size", {{"w", 2}, {"h", 2}}}},
             }),
         })},
        {"attack_boxes",
         nlohmann::json::array({
             nlohmann::json::array({
                 {{"id", "ab_default"},
                  {"name", "ab_default"},
                  {"position", {{"x", 14}, {"y", 15}}},
                  {"size", {{"w", 2}, {"h", 2}}},
                  {"damage_amount", 2}},
             }),
         })},
    };

    load_animation_with_prebuilt(animation, info, payload);

    const AnimationFrame* frame = animation.primary_frame_at(0);
    REQUIRE(frame != nullptr);
    REQUIRE(frame->get_hit_boxes().boxes.size() == 2);
    REQUIRE(frame->get_attack_boxes().boxes.size() == 1);

    CHECK(frame->get_hit_boxes().boxes[0].extrusion_forward == 1);
    CHECK(frame->get_hit_boxes().boxes[0].extrusion_backward == 1);
    CHECK(frame->get_hit_boxes().boxes[1].extrusion_forward == 1);
    CHECK(frame->get_hit_boxes().boxes[1].extrusion_backward == 1);
    CHECK(frame->get_attack_boxes().boxes[0].extrusion_forward == 1);
    CHECK(frame->get_attack_boxes().boxes[0].extrusion_backward == 1);
}
