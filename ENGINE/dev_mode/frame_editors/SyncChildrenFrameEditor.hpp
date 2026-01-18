#pragma once

#include "FrameEditorBase.hpp"
#include "shared/AxisAdjuster.hpp"
#include "shared/FrameEditorContext.hpp"
#include "shared/SelectionState.hpp"

namespace devmode::frame_editors {

class SyncChildrenFrameEditor : public FrameEditorBase {
public:
    SyncChildrenFrameEditor() : axis_adjuster_(&selection_) {}

    void begin(const FrameEditorContext& context) override;
    void end() override;
    bool handle_event(const SDL_Event& e) override;
    void update(const Input& input, float dt) override;
    void render_world(SDL_Renderer* renderer) const override;
    void render_overlays(SDL_Renderer* renderer) const override;

private:
    FrameEditorContext context_{};
    SelectionState selection_{};
    AxisAdjuster axis_adjuster_;
};

}  // namespace devmode::frame_editors
