#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "SelectionState.hpp"
#include "core/AssetsManager.hpp"
#include "devtools/asset_editor/animation_editor_window/PreviewProvider.hpp"

class Assets;
class Asset;
class WarpedScreenGrid;
class Input;
struct SDL_Renderer;

namespace animation_editor {
class AnimationDocument;
class PreviewProvider;
}

namespace devmode::frame_editors {

struct FrameEditorContext {
    Assets* assets = nullptr;
    Asset* target = nullptr;
    std::shared_ptr<animation_editor::AnimationDocument> document;
    std::shared_ptr<animation_editor::PreviewProvider> preview;
    std::string animation_id;
    FrameEditorLaunchMode launch_mode = FrameEditorLaunchMode::Movement;
    std::function<void(const std::string&)> on_host_closed;
    std::function<void()> on_end;
    WarpedScreenGrid* camera = nullptr;
    int snap_resolution = 0;
    bool snap_override = false;
    SelectionState* selection_state = nullptr;
    std::function<std::vector<std::string>()> selected_animation_ids_provider;
    std::function<void(const std::string&)> on_undo_checkpoint;
};

}  // namespace devmode::frame_editors
