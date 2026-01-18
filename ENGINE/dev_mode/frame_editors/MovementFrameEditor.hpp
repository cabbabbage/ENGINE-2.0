#pragma once

#include "FrameEditorBase.hpp"
#include "shared/AxisAdjuster.hpp"
#include "shared/FrameEditorContext.hpp"
#include "shared/SelectionState.hpp"

namespace devmode::frame_editors {

class MovementFrameEditor : public FrameEditorBase {
public:
    MovementFrameEditor() : axis_adjuster_(&selection_) {}

    void begin(const FrameEditorContext& context) override;
    void end() override;
    bool handle_event(const SDL_Event& e) override;
    void update(const Input& input, float dt) override;
    void render_world(SDL_Renderer* renderer) const override;
    void render_overlays(SDL_Renderer* renderer) const override;
    bool wants_close() const override { return wants_close_; }

private:
    FrameEditorContext context_{};
    SelectionState* selection_state_ = nullptr;
    AxisAdjuster* axis_adjuster_ = nullptr;
    bool wants_close_ = false;
};

}  // namespace devmode::frame_editors
