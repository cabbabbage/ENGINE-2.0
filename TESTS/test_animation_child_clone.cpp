#include "doctest/doctest.h"

#include <vector>

#include "asset/animation.hpp"

namespace {
AnimationChildFrameData make_child(int idx, bool visible = false) {
    AnimationChildFrameData child;
    child.child_index = idx;
    child.visible = visible;
    return child;
}

void seed_frames(Animation& animation, std::size_t count) {
    auto& path = animation.movement_path(0);
    path.clear();
    path.resize(count);
    animation.frames.clear();
    for (std::size_t i = 0; i < count; ++i) {
        animation.frames.push_back(&path[i]);
    }
}
}

TEST_CASE("rebuild_frames_from_child_timelines populates derived children") {
    Animation animation;
    animation.child_assets() = {"childA"};
    seed_frames(animation, 2);

    AnimationChildData descriptor;
    descriptor.asset_name = "childA";
    descriptor.mode = AnimationChildMode::Static;
    descriptor.frames = {make_child(0, true), make_child(0, false)};
    animation.child_timelines().push_back(descriptor);

    animation.rebuild_frames_from_child_timelines();
    animation.refresh_child_start_events();

    REQUIRE(animation.frames.size() == 2);
    REQUIRE(animation.frames[0]->children.size() == 1);
    CHECK(animation.frames[0]->children[0].visible);
    REQUIRE(animation.frames[1]->children.size() == 1);
    CHECK_FALSE(animation.frames[1]->children[0].visible);
    REQUIRE(animation.frames[0]->child_start_events.size() == 1);
    CHECK(animation.frames[0]->child_start_events[0] == 0);
}

TEST_CASE("rebuild_frames_from_child_timelines overwrites stale frame caches") {
    Animation animation;
    animation.child_assets() = {"childA"};
    seed_frames(animation, 1);

    animation.frames[0]->children.push_back(make_child(0, false));

    AnimationChildData descriptor;
    descriptor.asset_name = "childA";
    descriptor.mode = AnimationChildMode::Static;
    descriptor.frames = {make_child(0, true)};
    animation.child_timelines().push_back(descriptor);

    animation.rebuild_frames_from_child_timelines();

    REQUIRE(animation.frames[0]->children.size() == 1);
    CHECK(animation.frames[0]->children[0].visible);
}
