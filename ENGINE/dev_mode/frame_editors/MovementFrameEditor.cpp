#include "MovementFrameEditor.hpp"

namespace devmode::frame_editors {

void MovementFrameEditor::begin(const FrameEditorContext& context) {
    context_ = context;
}

void MovementFrameEditor::end() {
    wants_close_ = false;
    selection_.reset();
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
