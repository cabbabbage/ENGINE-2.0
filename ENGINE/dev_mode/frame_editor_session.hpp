#pragma once

#include <SDL.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "dev_mode/frame_editors/shared/FrameEditorContext.hpp"

#include "dev_mode/frame_editors/FrameEditorBase.hpp"
#include "dev_mode/dev_camera_controls.hpp"

class Assets;
class Asset;

class Input;
class WarpedScreenGrid;

enum class FrameEditorLaunchMode;

struct SDL_Renderer;

namespace animation_editor {
class AnimationDocument;
class PreviewProvider;
}

namespace devmode::frame_editors {
class FrameEditorBase;
}

class FrameEditorSession {
public:
    enum class Mode { Movement, SyncChildren, AsyncChildren, AttackGeometry, HitGeometry };

    FrameEditorSession();
    ~FrameEditorSession();

    void begin(Assets* assets,
               Asset* asset,
               std::shared_ptr<animation_editor::AnimationDocument> document,
               std::shared_ptr<animation_editor::PreviewProvider> preview,
               const std::string& animation_id,
               FrameEditorLaunchMode launch_mode,
               std::function<void(const std::string&)> on_host_closed,
               std::function<void()> on_end_callback = {});
    void end();

    bool is_active() const { return active_; }
    Mode mode() const { return mode_; }
    void set_mode(Mode mode);

    void update(const Input& input);
    bool handle_event(const SDL_Event& e);
    void render(SDL_Renderer* renderer) const;

    void set_snap_resolution(int r);

private:
    void create_and_begin_editor();
    void destroy_editor(bool persist_changes);
    std::unique_ptr<devmode::frame_editors::FrameEditorBase> create_editor(Mode mode);

    void capture_camera_state();
    void lock_camera_state();
    void restore_camera_state();
    void capture_edit_camera_state();
    void restore_edit_camera_state();
    void enforce_camera_locks(WarpedScreenGrid& cam);
    bool should_render_asset(const Asset* asset) const;

    Assets* assets_ = nullptr;
    Asset* target_ = nullptr;
    std::shared_ptr<animation_editor::AnimationDocument> document_;
    std::shared_ptr<animation_editor::PreviewProvider> preview_;
    std::string animation_id_;
    FrameEditorLaunchMode launch_mode_ = FrameEditorLaunchMode::Movement;
    std::function<void(const std::string&)> on_host_closed_{};
    std::function<void()> on_end_{};

    bool active_ = false;
    Mode mode_ = Mode::Movement;
    int snap_resolution_r_ = 0;
    bool snap_resolution_override_ = false;
    bool prev_asset_hidden_ = false;

    std::unique_ptr<devmode::frame_editors::FrameEditorBase> active_editor_;
    devmode::frame_editors::FrameEditorContext editor_context_{};
    devmode::frame_editors::SelectionState selection_state_{};

    struct CameraLockState {
        bool valid = false;
        bool manual_override_before = false;
        bool focus_override_before = false;
        SDL_Point focus_point_before{0, 0};
        SDL_Point screen_center_before{0, 0};
        std::optional<float> tilt_override_before;
        bool manual_zoom_override_before = false;
        double camera_zoom_percent_before = 0.0;
    };
    CameraLockState camera_lock_state_{};
    CameraLockState edit_camera_state_{};
    bool tilt_locked_ = false;
    float locked_tilt_deg_ = 0.0f;
    bool edit_camera_locked_ = false;

    DevCameraControls camera_controls_;
};
