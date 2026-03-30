#include <doctest/doctest.h>

#include <memory>
#include <vector>

#include "assets/asset/animation.hpp"
#include "core/runtime_world_context.hpp"
#include "gameplay/map_generation/room.hpp"

TEST_CASE("Animation adopt_prebuilt_frames keeps primary path synchronized") {
    Animation animation;

    std::vector<Animation::FrameCache> caches(3);
    animation.adopt_prebuilt_frames(std::move(caches), {1.0f, 0.5f});

    REQUIRE(animation.movement_path_count() == 1);
    REQUIRE(animation.has_frames());
    CHECK(animation.frame_count() == 3);

    auto& primary = animation.primary_frames();
    REQUIRE(primary.size() == 3);
    CHECK(animation.primary_frame_at(0) == &primary[0]);
    CHECK(animation.primary_frame_at(1) == &primary[1]);
    CHECK(animation.primary_frame_at(2) == &primary[2]);
    CHECK(animation.primary_frame_at(3) == nullptr);

    CHECK(primary[0].is_first);
    CHECK_FALSE(primary[0].is_last);
    CHECK(primary[0].prev == nullptr);
    CHECK(primary[0].next == &primary[1]);

    CHECK(primary[1].frame_index == 1);
    CHECK(primary[1].prev == &primary[0]);
    CHECK(primary[1].next == &primary[2]);

    CHECK(primary[2].is_last);
    CHECK(primary[2].prev == &primary[1]);
    CHECK(primary[2].next == nullptr);
}

TEST_CASE("Animation primary_frames aliases movement_path zero") {
    Animation animation;

    std::vector<Animation::FrameCache> caches(2);
    animation.adopt_prebuilt_frames(std::move(caches), {1.0f});

    auto& primary = animation.primary_frames();
    auto& path0 = animation.movement_path(0);
    REQUIRE(primary.size() == 2);
    REQUIRE(path0.size() == 2);

    path0[1].dx = 42;
    CHECK(primary[1].dx == 42);
    CHECK(animation.frame_count(0) == primary.size());
}

TEST_CASE("Animation classify_on_end maps loop and reserved directives") {
    CHECK(Animation::classify_on_end("default") == Animation::OnEndDirective::Default);
    CHECK(Animation::classify_on_end("loop") == Animation::OnEndDirective::Loop);
    CHECK(Animation::classify_on_end("kill") == Animation::OnEndDirective::Kill);
    CHECK(Animation::classify_on_end("lock") == Animation::OnEndDirective::Lock);
    CHECK(Animation::classify_on_end("reverse") == Animation::OnEndDirective::Reverse);
    CHECK(Animation::classify_on_end("vibble_attack_2") == Animation::OnEndDirective::Animation);
}

TEST_CASE("RuntimeWorldContext rebuilds room view and tracks topology generation") {
    RuntimeWorldContext context;
    CHECK(context.topology_generation() == 0);

    std::vector<std::unique_ptr<Room>> owned_rooms;
    owned_rooms.emplace_back();
    owned_rooms.emplace_back();
    context.adopt_rooms(std::move(owned_rooms));

    REQUIRE(context.owned_rooms().size() == 2);
    REQUIRE(context.rooms().size() == 2);
    CHECK(context.rooms()[0] == nullptr);
    CHECK(context.rooms()[1] == nullptr);
    CHECK(context.topology_generation() == 1);

    context.owned_rooms().push_back(std::unique_ptr<Room>{});
    context.rebuild_room_view();
    REQUIRE(context.rooms().size() == 3);
    CHECK(context.rooms()[2] == nullptr);
    CHECK(context.topology_generation() == 1);

    context.notify_topology_changed();
    CHECK(context.topology_generation() == 2);
}
