#pragma once

#include <memory>

#include <SDL.h>

class Input;
struct SDL_Renderer;

namespace devmode::frame_editors {

struct FrameEditorContext;

// Base interface implemented by each frame editor mode.
class FrameEditorBase {
public:
    virtual ~FrameEditorBase() = default;

    virtual void begin(const FrameEditorContext& context) = 0;
    virtual void end() = 0;
    virtual bool handle_event(const SDL_Event& e) = 0;
    virtual void update(const Input& input, float dt) = 0;
    virtual void render_world(SDL_Renderer* renderer) const = 0;
    virtual void render_overlays(SDL_Renderer* renderer) const = 0;
    virtual bool wants_close() const { return false; }
};

}  // namespace devmode::frame_editors
