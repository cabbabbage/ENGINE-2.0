#include "dev_mode/frame_editors/AsyncChildrenFrameEditor.hpp"

namespace devmode::frame_editors {

void AsyncChildrenFrameEditor::begin(const FrameEditorContext& /*context*/) {}

void AsyncChildrenFrameEditor::end() {}

bool AsyncChildrenFrameEditor::handle_event(const SDL_Event& /*e*/) {
    return false;
}

void AsyncChildrenFrameEditor::update(const Input& /*input*/, float /*dt*/) {}

void AsyncChildrenFrameEditor::render_world(SDL_Renderer* /*renderer*/) const {}

void AsyncChildrenFrameEditor::render_overlays(SDL_Renderer* /*renderer*/) const {}

}  // namespace devmode::frame_editors
