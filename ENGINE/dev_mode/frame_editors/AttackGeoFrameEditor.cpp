#include "AttackGeoFrameEditor.hpp"

namespace devmode::frame_editors {

void AttackGeoFrameEditor::begin(const FrameEditorContext& context) {
    context_ = context;
    selection_state_ = context.selection_state;
    if (selection_state_) {
        selection_state_->reset();
    }
}

void AttackGeoFrameEditor::end() {
    if (selection_state_) {
        selection_state_->reset();
        selection_state_ = nullptr;
    }
}

bool AttackGeoFrameEditor::handle_event(const SDL_Event& e) {
    (void)e;
    return false;
}

void AttackGeoFrameEditor::update(const Input& input, float dt) {
    (void)input;
    (void)dt;
}

void AttackGeoFrameEditor::render_world(SDL_Renderer* renderer) const {
    (void)renderer;
}

void AttackGeoFrameEditor::render_overlays(SDL_Renderer* renderer) const {
    (void)renderer;
}

}  // namespace devmode::frame_editors
