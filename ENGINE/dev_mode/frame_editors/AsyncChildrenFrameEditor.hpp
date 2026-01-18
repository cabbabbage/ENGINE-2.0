#pragma once

#include "dev_mode/frame_editors/FrameEditorBase.hpp"

namespace devmode::frame_editors {

// Placeholder editor to keep Async Children mode present during the refactor.
class AsyncChildrenFrameEditor final : public FrameEditorBase {
public:
    void begin(const FrameEditorContext& context) override;
    void end() override;
    bool handle_event(const SDL_Event& e) override;
    void update(const Input& input, float dt) override;
    void render_world(SDL_Renderer* renderer) const override;
    void render_overlays(SDL_Renderer* renderer) const override;
};

}  // namespace devmode::frame_editors
