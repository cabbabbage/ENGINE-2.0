#pragma once

#include <atomic>
#include <string>

#include "devtools/frame_editors/FrameEditorBase.hpp"
#include "devtools/frame_editors/shared/FrameEditorContext.hpp"

namespace devmode::anchor_editor {

class AnchorEditor : public frame_editors::FrameEditorBase {
public:
    ~AnchorEditor() override;

    void begin(const frame_editors::FrameEditorContext& context) override;
    void end() override;
    bool handle_event(const SDL_Event& e) override;
    void update(const Input& input, float dt) override;
    void render_world(SDL_Renderer* renderer) const override;
    void render_overlays(SDL_Renderer* renderer) const override;
    void persist_pending_changes() override;
    bool wants_close() const override { return wants_close_; }

private:
    void launch_python_editor();
    static std::string quote_arg(const std::string& value);

    frame_editors::FrameEditorContext context_{};
    std::atomic<bool> editor_finished_{false};
    bool rebuild_triggered_ = false;
    bool wants_close_ = false;
};

}  // namespace devmode::anchor_editor
