#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "assets/asset/animation.hpp"
#include "assets/asset/asset_info.hpp"
#include "devtools/animation_source_navigation.hpp"

namespace {

struct AnimationSourceFixture {
    std::shared_ptr<AssetInfo> info = std::make_shared<AssetInfo>("animation_source_navigation_test");

    Animation& add_file_animation(const std::string& id, int frame_count = 1) {
        Animation animation;
        animation.source.kind = "file";
        auto& frames = animation.primary_frames();
        frames.resize(frame_count);
        for (int i = 0; i < frame_count; ++i) {
            frames[static_cast<std::size_t>(i)].frame_index = i;
        }
        auto [it, inserted] = info->animations.emplace(id, std::move(animation));
        if (!inserted) {
            it->second = Animation{};
            it->second.source.kind = "file";
            auto& existing_frames = it->second.primary_frames();
            existing_frames.clear();
            existing_frames.resize(frame_count);
            for (int i = 0; i < frame_count; ++i) {
                existing_frames[static_cast<std::size_t>(i)].frame_index = i;
            }
        }
        return it->second;
    }

    Animation& add_derived_animation(const std::string& id,
                                     const std::string& source_id,
                                     bool inherit_geometry = true,
                                     int frame_count = 1) {
        Animation animation;
        animation.source.kind = "animation";
        animation.source.name = source_id;
        animation.inherit_source_geometry = inherit_geometry;
        auto& frames = animation.primary_frames();
        frames.resize(frame_count);
        for (int i = 0; i < frame_count; ++i) {
            frames[static_cast<std::size_t>(i)].frame_index = i;
        }
        auto [it, inserted] = info->animations.emplace(id, std::move(animation));
        if (!inserted) {
            it->second = Animation{};
            it->second.source.kind = "animation";
            it->second.source.name = source_id;
            it->second.inherit_source_geometry = inherit_geometry;
            auto& existing_frames = it->second.primary_frames();
            existing_frames.clear();
            existing_frames.resize(frame_count);
            for (int i = 0; i < frame_count; ++i) {
                existing_frames[static_cast<std::size_t>(i)].frame_index = i;
            }
        }
        return it->second;
    }
};

}  // namespace

TEST_CASE("File sourced animation resolves to itself") {
    AnimationSourceFixture fixture;
    fixture.add_file_animation("idle", 2);
    fixture.add_derived_animation("idle_copy", "idle");

    const auto selection = devmode::resolve_file_sourced_animation_selection(fixture.info.get(), "idle");

    CHECK(selection.has_selection());
    CHECK(selection.requested_animation_id == "idle");
    CHECK(selection.resolved_animation_id == "idle");
    CHECK(!selection.requested_was_derived);
    CHECK(!selection.used_fallback);
    CHECK(selection.navigable_animation_ids == std::vector<std::string>{"idle"});
}

TEST_CASE("Derived animation resolves to top-level file sourced animation") {
    AnimationSourceFixture fixture;
    fixture.add_file_animation("idle", 3);
    fixture.add_derived_animation("idle_variant", "idle");
    fixture.add_derived_animation("idle_variant_flip", "idle_variant");

    const auto selection =
        devmode::resolve_file_sourced_animation_selection(fixture.info.get(), "idle_variant_flip");

    CHECK(selection.has_selection());
    CHECK(selection.resolved_animation_id == "idle");
    CHECK(selection.requested_was_derived);
    CHECK(!selection.used_fallback);
    CHECK(selection.navigable_animation_ids == std::vector<std::string>{"idle"});
}

TEST_CASE("Derived animation with local geometry is directly navigable") {
    AnimationSourceFixture fixture;
    fixture.add_file_animation("idle", 3);
    fixture.add_derived_animation("idle_variant", "idle", false, 3);
    fixture.add_derived_animation("idle_variant_flip", "idle_variant");

    const auto selection =
        devmode::resolve_file_sourced_animation_selection(fixture.info.get(), "idle_variant");

    CHECK(selection.has_selection());
    CHECK(selection.resolved_animation_id == "idle_variant");
    CHECK(!selection.requested_was_derived);
    CHECK(!selection.used_fallback);
    CHECK(selection.navigable_animation_ids == std::vector<std::string>{"idle", "idle_variant"});
}

TEST_CASE("Broken and cyclic chains fall back to the first file sourced animation") {
    AnimationSourceFixture fixture;
    fixture.add_file_animation("attack", 2);
    fixture.add_file_animation("idle", 1);
    fixture.add_derived_animation("broken", "missing");
    fixture.add_derived_animation("loop_a", "loop_b");
    fixture.add_derived_animation("loop_b", "loop_a");

    const auto broken = devmode::resolve_file_sourced_animation_selection(fixture.info.get(), "broken");
    CHECK(broken.has_selection());
    CHECK(broken.resolved_animation_id == "attack");
    CHECK(broken.used_fallback);
    CHECK(broken.navigable_animation_ids == std::vector<std::string>{"attack", "idle"});

    const auto cyclic = devmode::resolve_file_sourced_animation_selection(fixture.info.get(), "loop_a");
    CHECK(cyclic.has_selection());
    CHECK(cyclic.resolved_animation_id == "attack");
    CHECK(cyclic.used_fallback);
    CHECK(cyclic.requested_was_derived);
    CHECK(cyclic.navigable_animation_ids == std::vector<std::string>{"attack", "idle"});
}
