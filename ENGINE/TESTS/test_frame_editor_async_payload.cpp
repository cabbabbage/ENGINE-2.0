// Test disabled: FrameChildrenEditor was removed in frame editor refactoring.
// This test needs to be rewritten for the new AsyncChildrenFrameEditor class.
#if 0
#include "doctest/doctest.h"

#include <nlohmann/json.hpp>

#define FRAME_EDITOR_TEST_PUBLIC_ACCESS
#include "devtools/asset_sections/animation_editor_window/frame_editor/children/FrameChildrenEditor.hpp"
#undef FRAME_EDITOR_TEST_PUBLIC_ACCESS

using animation_editor::FrameChildrenEditor;

TEST_CASE("FrameChildrenEditor preserves async timelines in payload") {
    FrameChildrenEditor editor;
    editor.child_ids_ = {"child_async"};
    editor.child_modes_.assign(1, AnimationChildMode::Async);
    editor.frames_.push_back(FrameChildrenEditor::MovementFrame{});
    editor.ensure_async_timeline_size();
    editor.async_child_frames_[0].clear();

    FrameChildrenEditor::ChildFrame sample{};
    sample.child_index = 0;
    sample.dx = 7.0f;
    sample.dy = 1.0f;
    sample.dz = 9.0f;
    sample.rotation = 15.0f;
    sample.visible = true;
    editor.async_child_frames_[0].push_back(sample);

    const nlohmann::json payload = editor.build_child_timelines_payload(nlohmann::json::object());
    REQUIRE(payload.is_array());
    REQUIRE(payload.size() == 1);
    const auto& entry = payload[0];
    CHECK(entry.value("mode", "") == "async");
    REQUIRE(entry.contains("frames"));
    REQUIRE(entry["frames"].is_array());
    REQUIRE(entry["frames"].size() == 1);
    const auto& out_sample = entry["frames"][0];
    CHECK(out_sample.value("dx", 0) == 7);
    CHECK(out_sample.value("dz", 0) == 9);
    CHECK(out_sample.value("visible", false) == true);
}
#endif
