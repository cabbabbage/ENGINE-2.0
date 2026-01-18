#include "SyncChildrenFrameEditor.hpp"

namespace devmode::frame_editors {

void SyncChildrenFrameEditor::begin(const FrameEditorContext& context) {
    context_ = context;
}

void SyncChildrenFrameEditor::end() {
    selection_.reset();
}

bool SyncChildrenFrameEditor::handle_event(const SDL_Event& e) {
    (void)e;
    return false;
}

void SyncChildrenFrameEditor::update(const Input& input, float dt) {
    (void)input;
    (void)dt;
}

void SyncChildrenFrameEditor::render_world(SDL_Renderer* renderer) const {
    (void)renderer;
}

void SyncChildrenFrameEditor::render_overlays(SDL_Renderer* renderer) const {
    (void)renderer;
}

}  // namespace devmode::frame_editors
