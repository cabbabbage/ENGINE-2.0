#include "doctest/doctest.h"

#include <vector>

#define FRAME_EDITOR_ACCESS public
#include "dev_mode/frame_editor_session.hpp"
#undef FRAME_EDITOR_ACCESS

#include "animation_update/child_attachment_controller.hpp"
#include "asset/animation.hpp"
#include "render/warped_screen_grid.hpp"
#include "utils/area.hpp"

TEST_CASE("Hidden child attachments restart from frame zero when revealed") {
    Animation child_anim;
    child_anim.loop = false;
    auto& child_path = child_anim.movement_path(0);
    child_path.resize(2);
    child_path[0].frame_index = 0;
    child_path[0].is_first = true;
    child_path[0].next = &child_path[1];
    child_path[1].frame_index = 1;
    child_path[1].is_last = true;
    child_path[1].prev = &child_path[0];
    child_path[1].next = nullptr;

    Asset::AnimationChildAttachment slot;
    slot.child_index = 0;
    slot.animation = &child_anim;
    slot.current_frame = child_anim.get_first_frame();

    std::vector<Asset::AnimationChildAttachment> slots;
    slots.push_back(slot);

    animation_update::child_attachments::ParentState parent_state;
    parent_state.position = SDL_Point{10, 20};
    parent_state.base_position = SDL_Point{10, 20}; // For tests, assume base is same as position
    parent_state.flipped = false;
    parent_state.animation_id = "custom_anim";

    AnimationChildFrameData visible{};
    visible.child_index = 0;
    visible.dx = 3;
    visible.dy = -2;
    visible.degree = 15.0f;
    visible.visible = true;

    AnimationChildFrameData hidden = visible;
    hidden.visible = false;

    AnimationFrame frame_visible;
    frame_visible.frame_index = 0;
    frame_visible.children.push_back(visible);

    AnimationFrame frame_hidden;
    frame_hidden.frame_index = 1;
    frame_hidden.children.push_back(hidden);

    AnimationFrame frame_visible_again;
    frame_visible_again.frame_index = 2;
    frame_visible_again.children.push_back(visible);

    const float frame_dt = 1.0f / static_cast<float>(kBaseAnimationFps);

    auto step = [&](AnimationFrame* frame) {
        animation_update::child_attachments::advance_frames(slots, parent_state, frame_dt);
        animation_update::child_attachments::apply_frame_data(slots, parent_state, frame);
    };

    auto& slot_ref = slots.front();

    step(&frame_visible);
    REQUIRE(slot_ref.current_frame != nullptr);
    CHECK(slot_ref.current_frame->frame_index == 0);

    // Hide for multiple frames to let the attachment advance.
    for (int i = 0; i < 3; ++i) {
        step(&frame_hidden);
    }
    REQUIRE(slot_ref.current_frame != nullptr);
    CHECK(slot_ref.current_frame->frame_index == 1);

    step(&frame_visible_again);
    REQUIRE(slot_ref.current_frame != nullptr);
    CHECK(slot_ref.current_frame->frame_index == 0);
    CHECK(slot_ref.frame_progress == doctest::Approx(0.0f));
}

TEST_CASE("Shift adjustment scrolls child axis values") {
    FrameEditorSession session;
    session.mode_ = FrameEditorSession::Mode::StaticChildren;
    FrameEditorSession::MovementFrame frame;
    FrameEditorSession::ChildFrame child;
    child.child_index = 0;
    child.dx = 2.0f;
    child.dy = -1.0f;
    child.dz = 4.0f;
    child.has_data = true;
    frame.children.push_back(child);
    session.frames_.push_back(frame);
    session.selected_child_index_ = 0;
    session.selected_index_ = 0;

    Area area("test", SDL_Point{0, 0}, 10, 10, "square", 0, 10, 10);
    WarpedScreenGrid cam(800, 600, area);

    session.select_adjustment_target(FrameEditorSession::AdjustmentTarget::ChildPoint, 0, SDL_FPoint{}, SDL_Point{});
    session.adjust_selection_axis(1, cam);
    CHECK(session.frames_[0].children[0].dx == doctest::Approx(3.0f));

    session.cycle_adjustment_axis();
    session.adjust_selection_axis(1, cam);
    CHECK(session.frames_[0].children[0].dy == doctest::Approx(0.0f));

    session.cycle_adjustment_axis();
    session.adjust_selection_axis(-2, cam);
    CHECK(session.frames_[0].children[0].dz == doctest::Approx(2.0f));
}
