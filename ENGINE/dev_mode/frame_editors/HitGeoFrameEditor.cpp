#include "HitGeoFrameEditor.hpp"

namespace devmode::frame_editors {

void HitGeoFrameEditor::begin(const FrameEditorContext& context) {
    context_ = context;
}

void HitGeoFrameEditor::end() {
    selection_.reset();
}

bool HitGeoFrameEditor::handle_event(const SDL_Event& e) {
    (void)e;
    return false;
}

void HitGeoFrameEditor::update(const Input& input, float dt) {
    (void)input;
    (void)dt;
}

void HitGeoFrameEditor::render_world(SDL_Renderer* renderer) const {
    (void)renderer;
}

void HitGeoFrameEditor::render_overlays(SDL_Renderer* renderer) const {
    (void)renderer;
}

}  // namespace devmode::frame_editors
