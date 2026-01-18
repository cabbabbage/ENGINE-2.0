#include "MovementFrameEditor.hpp"

namespace devmode::frame_editors {

void MovementFrameEditor::begin(const FrameEditorContext& context) {
    context_ = context;
    selection_state_ = context.selection_state;
    axis_adjuster_ = context.axis_adjuster;
    if (selection_state_) {
        selection_state_->reset();
    }
    if (axis_adjuster_) {
        axis_adjuster_->reset_axis(AdjustmentAxis::X);
    }
}

void MovementFrameEditor::end() {
    wants_close_ = false;
    if (selection_state_) {
        selection_state_->reset();
        selection_state_ = nullptr;
    }
    axis_adjuster_ = nullptr;
}

bool MovementFrameEditor::handle_event(const SDL_Event& e) {
    (void)e;
    return false;
}

void MovementFrameEditor::update(const Input& input, float dt) {
    (void)input;
    (void)dt;
}

void MovementFrameEditor::render_world(SDL_Renderer* renderer) const {
    (void)renderer;
}

void MovementFrameEditor::render_overlays(SDL_Renderer* renderer) const {
    (void)renderer;
}

}  // namespace devmode::frame_editors
