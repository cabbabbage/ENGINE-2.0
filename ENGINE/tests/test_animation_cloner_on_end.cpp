#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

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
