#include "doctest/doctest.h"

#include <nlohmann/json.hpp>

#define FRAME_EDITOR_ACCESS public
#include "dev_mode/frame_editor_session.hpp"
#undef FRAME_EDITOR_ACCESS

#include "asset/animation_frame.hpp"
#include "animation_update/child_attachment_math.hpp"
#include "render/warped_screen_grid.hpp"
#include "utils/area.hpp"

TEST_CASE("AnimationChildFrameData defaults to visible when flag is omitted") {
    AnimationChildFrameData child;
    CHECK(child.visible);
}

TEST_CASE("Frame editor keeps children visible when payload omits boolean") {
    nlohmann::json payload = nlohmann::json::object();
    payload["movement"] = nlohmann::json::array();
    payload["movement"].push_back(nlohmann::json::array({
        0,                                      // dx
        0,                                      // dy
        false,                                  // z resort
        nlohmann::json::array({255, 255, 255}), // rgb
        nlohmann::json::array({
            nlohmann::json::array({
                0,      // child index
                12,     // dx
                -3,     // dy
                15.0    // degree
                // visible flag intentionally omitted to exercise default
            })
        })
    }));

    const auto frames = FrameEditorSession::parse_movement_frames_json(payload.dump());
    REQUIRE(frames.size() == 1);
    REQUIRE(frames.front().children.size() == 1);

    const auto& parsed_child = frames.front().children.front();
    CHECK(parsed_child.visible);
}

TEST_CASE("Frame editor hydrates frames from child timelines") {
    FrameEditorSession session;
    session.child_assets_ = {"childA"};
    session.frames_.resize(1);
    session.sync_child_frames();

    nlohmann::json payload = nlohmann::json::object({
        {"child_timelines", nlohmann::json::array({
            nlohmann::json::object({
                {"child", 0},
                {"asset", "childA"},
                {"mode", "static"},
                {"frames", nlohmann::json::array({
                    nlohmann::json::object({
                        {"dx", 7},
                        {"dy", -2},
                        {"degree", 45.0},
                        {"visible", true}
                    })
                })}
            })
        })}
    });

    session.apply_child_timelines_from_payload(payload);
    REQUIRE_FALSE(session.frames_.empty());
    REQUIRE(session.frames_.front().children.size() == 1);
    const auto& child = session.frames_.front().children.front();
    CHECK(child.has_data);
    CHECK(child.dx == doctest::Approx(7.0f));
    CHECK(child.dy == doctest::Approx(-2.0f));
    CHECK(child.degree == doctest::Approx(45.0f));
    CHECK(child.visible);
}

TEST_CASE("Frame editor serializes child timelines from frames") {
    FrameEditorSession session;
    session.child_assets_ = {"childA"};
    FrameEditorSession::MovementFrame frame;
    FrameEditorSession::ChildFrame child;
    child.child_index = 0;
    child.dx = 5.0f;
    child.dy = -3.0f;
    child.degree = 10.0f;
    child.visible = true;
    child.has_data = true;
    frame.children.push_back(child);
    session.frames_.push_back(frame);

    const nlohmann::json payload = nlohmann::json::object();
    const nlohmann::json timelines = session.build_child_timelines_payload(payload);
    REQUIRE(timelines.is_array());
    REQUIRE(timelines.size() == 1);
    const auto& entry = timelines.front();
    CHECK(entry.value("mode", "") == "static");
    REQUIRE(entry.contains("frames"));
    REQUIRE(entry["frames"].is_array());
    REQUIRE(entry["frames"].size() == 1);
    const auto& sample = entry["frames"][0];
    CHECK(sample.value("dx", 0) == 5);
    CHECK(sample.value("dy", 0) == -3);
    CHECK(sample.value("visible", false));
    CHECK(sample.value("degree", 0.0) == doctest::Approx(10.0));
}

TEST_CASE("Child rotation mirrors when parent flips horizontally") {
    const float original = 20.0f;
    CHECK(mirrored_child_rotation(false, original) == doctest::Approx(original));
    CHECK(mirrored_child_rotation(true, original) == doctest::Approx(-original));
}

TEST_CASE("Frame editor parses six-element child arrays without render-in-front") {
    nlohmann::json payload = nlohmann::json::object();
    payload["movement"] = nlohmann::json::array({
        nlohmann::json::array({
            0,
            0,
            false,
            nlohmann::json::array({255, 255, 255}),
            nlohmann::json::array({
                nlohmann::json::array({0, 4, -1, 2, 30.0, true})
            })
        })
    });

    const auto frames = FrameEditorSession::parse_movement_frames_json(payload.dump());
    REQUIRE(frames.size() == 1);
    REQUIRE(frames.front().children.size() == 1);
    const auto& child = frames.front().children.front();
    CHECK(child.child_index == 0);
    CHECK(child.dx == doctest::Approx(4.0f));
    CHECK(child.dy == doctest::Approx(-1.0f));
    CHECK(child.dz == doctest::Approx(2.0f));
    CHECK(child.degree == doctest::Approx(30.0f));
    CHECK(child.visible);
}

TEST_CASE("Shift adjustment cycles axes and scrolls movement points") {
    FrameEditorSession session;
    session.mode_ = FrameEditorSession::Mode::Movement;
    session.frames_.resize(2);
    session.frames_[1].dx = 10.0f;
    session.frames_[1].dy = 5.0f;
    session.frames_[1].dz = 3.0f;
    session.selected_index_ = 1;

    Area area("test", SDL_Point{0, 0}, 10, 10, "square", 0, 10, 10);
    WarpedScreenGrid cam(800, 600, area);

    session.select_adjustment_target(FrameEditorSession::AdjustmentTarget::MovementPoint, -1, SDL_FPoint{}, SDL_Point{});
    session.adjust_selection_axis(1, cam);
    CHECK(session.frames_[1].dx == doctest::Approx(11.0f));

    session.cycle_adjustment_axis();
    session.adjust_selection_axis(-1, cam);
    CHECK(session.frames_[1].dy == doctest::Approx(4.0f));

    session.cycle_adjustment_axis();
    session.adjust_selection_axis(2, cam);
    CHECK(session.frames_[1].dz == doctest::Approx(5.0f));
}

TEST_CASE("Shift adjustment deselect resets state on exit") {
    FrameEditorSession session;
    session.mode_ = FrameEditorSession::Mode::Movement;
    session.frames_.resize(2);
    session.selected_index_ = 1;
    session.select_adjustment_target(FrameEditorSession::AdjustmentTarget::MovementPoint, -1, SDL_FPoint{}, SDL_Point{});
    session.adjustment_mode_active_ = true;
    session.adjustment_dirty_ = true;

    session.exit_adjustment_mode(true);
    CHECK_FALSE(session.adjustment_mode_active_);
    CHECK_FALSE(session.adjustment_selection_.has_value());
    CHECK_FALSE(session.adjustment_dirty_);
}
