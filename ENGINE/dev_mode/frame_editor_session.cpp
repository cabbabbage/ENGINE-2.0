#include "frame_editor_session.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include "core/AssetsManager.hpp"
#include "dev_mode/frame_editors/AsyncChildrenFrameEditor.hpp"
#include "dev_mode/frame_editors/AttackGeoFrameEditor.hpp"
#include "dev_mode/frame_editors/FrameEditorBase.hpp"
#include "dev_mode/frame_editors/HitGeoFrameEditor.hpp"
#include "dev_mode/frame_editors/MovementFrameEditor.hpp"
#include "dev_mode/frame_editors/SyncChildrenFrameEditor.hpp"
#include "render/warped_screen_grid.hpp"
#include "utils/grid.hpp"
#include "utils/input.hpp"
#include "dev_mode/dev_camera_controls.hpp"

namespace {

FrameEditorSession::Mode mode_for_launch(FrameEditorLaunchMode launch_mode) {
    switch (launch_mode) {
        case FrameEditorLaunchMode::Movement: return FrameEditorSession::Mode::Movement;
        case FrameEditorLaunchMode::SyncChildren: return FrameEditorSession::Mode::SyncChildren;
        case FrameEditorLaunchMode::AsyncChildren: return FrameEditorSession::Mode::AsyncChildren;
        case FrameEditorLaunchMode::AttackGeometry: return FrameEditorSession::Mode::AttackGeometry;
        case FrameEditorLaunchMode::HitGeometry: return FrameEditorSession::Mode::HitGeometry;
    }
    return FrameEditorSession::Mode::Movement;
}

FrameEditorLaunchMode launch_mode_for_mode(FrameEditorSession::Mode mode) {
    switch (mode) {
        case FrameEditorSession::Mode::Movement: return FrameEditorLaunchMode::Movement;
        case FrameEditorSession::Mode::SyncChildren: return FrameEditorLaunchMode::SyncChildren;
        case FrameEditorSession::Mode::AsyncChildren: return FrameEditorLaunchMode::AsyncChildren;
        case FrameEditorSession::Mode::AttackGeometry: return FrameEditorLaunchMode::AttackGeometry;
        case FrameEditorSession::Mode::HitGeometry: return FrameEditorLaunchMode::HitGeometry;
    }
    return FrameEditorLaunchMode::Movement;
}

std::unique_ptr<devmode::frame_editors::FrameEditorBase> create_editor(FrameEditorSession::Mode mode) {
    switch (mode) {
        case FrameEditorSession::Mode::Movement:
            return std::make_unique<devmode::frame_editors::MovementFrameEditor>();
        case FrameEditorSession::Mode::SyncChildren:
            return std::make_unique<devmode::frame_editors::SyncChildrenFrameEditor>();
        case FrameEditorSession::Mode::AsyncChildren:
            return std::make_unique<devmode::frame_editors::AsyncChildrenFrameEditor>();
        case FrameEditorSession::Mode::AttackGeometry:
            return std::make_unique<devmode::frame_editors::AttackGeoFrameEditor>();
        case FrameEditorSession::Mode::HitGeometry:
            return std::make_unique<devmode::frame_editors::HitGeoFrameEditor>();
    }
    return nullptr;
}

}

FrameEditorSession::FrameEditorSession() = default;

FrameEditorSession::~FrameEditorSession() {
    end();
}

void FrameEditorSession::begin(Assets* assets,
                               Asset* asset,
                               std::shared_ptr<animation_editor::AnimationDocument> document,
                               std::shared_ptr<animation_editor::PreviewProvider> preview,
                               const std::string& animation_id,
                               FrameEditorLaunchMode launch_mode,
                               std::function<void(const std::string&)> on_host_closed,
                               std::function<void()> on_end_callback) {
    if (active_) {
        end();
    }
    if (!assets || !asset || !document || animation_id.empty()) {
        return;
    }
    if (!assets->contains_asset(asset)) {
        return;
    }

    assets_ = assets;
    target_ = asset;
    document_ = std::move(document);
    preview_ = std::move(preview);
    animation_id_ = animation_id;
    launch_mode_ = launch_mode;
    on_host_closed_ = std::move(on_host_closed);
    on_end_ = std::move(on_end_callback);
    prev_asset_hidden_ = target_->is_hidden();
    target_->set_hidden(false);

    editor_context_.assets = assets_;
    editor_context_.target = target_;
    editor_context_.document = document_;
    editor_context_.preview = preview_;
    editor_context_.animation_id = animation_id_;
    editor_context_.launch_mode = launch_mode_;
    editor_context_.on_host_closed = on_host_closed_;
    editor_context_.on_end = on_end_;
    editor_context_.camera = assets_ ? &assets_->getView() : nullptr;
    editor_context_.snap_resolution = snap_resolution_r_;
    editor_context_.snap_override = snap_resolution_override_;
    editor_context_.selection_state = &selection_state_;

    mode_ = mode_for_launch(launch_mode_);
    camera_controls_.set_height_scale_factor(1.1);
    capture_camera_state();
    active_ = true;
    create_and_begin_editor();
}

void FrameEditorSession::end() {
    if (!active_) {
        return;
    }

    destroy_editor();

    const bool target_alive = assets_ && target_ && assets_->contains_asset(target_);
    if (target_alive) {
        target_->set_hidden(prev_asset_hidden_);
    }
    if (assets_) {
        if (edit_camera_locked_) {
            restore_edit_camera_state();
        }
        restore_camera_state();
    }

    auto saved_host_callback = std::move(on_host_closed_);
    const std::string saved_animation_id = animation_id_;

    active_ = false;
    assets_ = nullptr;
    target_ = nullptr;
    document_.reset();
    preview_.reset();
    animation_id_.clear();
    launch_mode_ = FrameEditorLaunchMode::Movement;
    prev_asset_hidden_ = false;
    snap_resolution_override_ = false;
    snap_resolution_r_ = 0;
    selection_state_.reset();
    editor_context_ = {};
    camera_lock_state_.valid = false;
    edit_camera_state_.valid = false;
    camera_y_distance_locked_ = false;
    tilt_locked_ = false;
    edit_camera_locked_ = false;

    if (saved_host_callback) {
        saved_host_callback(saved_animation_id);
    }
    if (on_end_) {
        auto cb = std::move(on_end_);
        cb();
    }
}

void FrameEditorSession::set_mode(Mode mode) {
    if (mode_ == mode) {
        return;
    }
    mode_ = mode;
    if (!active_) {
        return;
    }
    create_and_begin_editor();
}

void FrameEditorSession::update(const Input& input) {
    if (!active_editor_) {
        return;
    }
    if (assets_) {
        WarpedScreenGrid& cam = assets_->getView();
        // Standard camera controls: left-click drag = pan, scroll = height, Ctrl + drag for tilt, Ctrl + scroll for zoom
        camera_controls_.handle_input(cam, input, false);
    }
    active_editor_->update(input, 0.0f);
    if (active_editor_ && active_editor_->wants_close()) {
        end();
        return;
    }
}

bool FrameEditorSession::handle_event(const SDL_Event& e) {
    if (!active_editor_) {
        return false;
    }
    return active_editor_->handle_event(e);
}

void FrameEditorSession::render(SDL_Renderer* renderer) const {
    if (!active_editor_ || !renderer) {
        return;
    }
    active_editor_->render_world(renderer);
    active_editor_->render_overlays(renderer);
}

void FrameEditorSession::set_snap_resolution(int r) {
    snap_resolution_r_ = vibble::grid::clamp_resolution(std::max(0, r));
    snap_resolution_override_ = true;
    editor_context_.snap_resolution = snap_resolution_r_;
    editor_context_.snap_override = snap_resolution_override_;
}

bool FrameEditorSession::should_render_asset(const Asset* asset) const {
    if (!active_ || !asset || !target_) {
        return false;
    }
    if (asset == target_) {
        return true;
    }
    const Asset* current = asset->parent;
    while (current) {
        if (current == target_) {
            return true;
        }
        current = current->parent;
    }
    return false;
}

void FrameEditorSession::create_and_begin_editor() {
    destroy_editor();
    editor_context_.launch_mode = launch_mode_for_mode(mode_);
    editor_context_.animation_id = animation_id_;
    editor_context_.camera = assets_ ? &assets_->getView() : nullptr;
    editor_context_.on_host_closed = on_host_closed_;
    editor_context_.on_end = on_end_;
    editor_context_.assets = assets_;
    editor_context_.target = target_;
    editor_context_.document = document_;
    editor_context_.preview = preview_;
    editor_context_.snap_resolution = snap_resolution_r_;
    editor_context_.snap_override = snap_resolution_override_;

    selection_state_.reset();
    editor_context_.selection_state = &selection_state_;

    active_editor_ = create_editor(mode_);
    if (active_editor_) {
        active_editor_->begin(editor_context_);
    }
}

void FrameEditorSession::destroy_editor() {
    if (!active_editor_) {
        return;
    }
    active_editor_->end();
    active_editor_.reset();
}

void FrameEditorSession::capture_camera_state() {
    if (!assets_) {
        return;
    }
    WarpedScreenGrid& cam = assets_->getView();
    camera_lock_state_.manual_override_before = cam.is_manual_height_override();
    camera_lock_state_.focus_override_before = cam.has_focus_override();
    camera_lock_state_.focus_point_before = cam.get_focus_override_point();
    camera_lock_state_.screen_center_before = cam.get_screen_center();
    camera_lock_state_.tilt_override_before = cam.tilt_override();
    camera_lock_state_.camera_y_distance_before = cam.camera_y_distance();
    camera_lock_state_.manual_zoom_override_before = cam.is_manual_zoom_override();
    camera_lock_state_.camera_zoom_percent_before = cam.get_zoom_percent();
    camera_lock_state_.valid = true;
}

void FrameEditorSession::restore_camera_state() {
    if (!assets_ || !camera_lock_state_.valid) {
        return;
    }
    WarpedScreenGrid& cam = assets_->getView();
    cam.set_manual_height_override(camera_lock_state_.manual_override_before);
    if (camera_lock_state_.focus_override_before) {
        cam.set_focus_override(camera_lock_state_.focus_point_before);
    } else {
        cam.clear_focus_override();
    }
    cam.set_screen_center(camera_lock_state_.screen_center_before);
    if (camera_lock_state_.tilt_override_before.has_value()) {
        cam.set_tilt_override(*camera_lock_state_.tilt_override_before);
    } else {
        cam.clear_tilt_override();
    }
    cam.set_camera_y_distance(camera_lock_state_.camera_y_distance_before);
    cam.set_zoom_percent(camera_lock_state_.camera_zoom_percent_before);
    cam.set_manual_zoom_override(camera_lock_state_.manual_zoom_override_before);
    camera_lock_state_.valid = false;
    camera_y_distance_locked_ = false;
    tilt_locked_ = false;
}

void FrameEditorSession::capture_edit_camera_state() {
    if (!assets_) {
        return;
    }
    WarpedScreenGrid& cam = assets_->getView();
    edit_camera_state_.manual_override_before = cam.is_manual_height_override();
    edit_camera_state_.focus_override_before = cam.has_focus_override();
    edit_camera_state_.focus_point_before = cam.get_focus_override_point();
    edit_camera_state_.screen_center_before = cam.get_screen_center();
    edit_camera_state_.tilt_override_before = cam.tilt_override();
    edit_camera_state_.camera_y_distance_before = cam.camera_y_distance();
    edit_camera_state_.manual_zoom_override_before = cam.is_manual_zoom_override();
    edit_camera_state_.camera_zoom_percent_before = cam.get_zoom_percent();
    edit_camera_state_.valid = true;
}

void FrameEditorSession::restore_edit_camera_state() {
    if (!assets_ || !edit_camera_state_.valid) {
        edit_camera_locked_ = false;
        return;
    }
    WarpedScreenGrid& cam = assets_->getView();
    cam.set_manual_height_override(edit_camera_state_.manual_override_before);
    if (edit_camera_state_.focus_override_before) {
        cam.set_focus_override(edit_camera_state_.focus_point_before);
    } else {
        cam.clear_focus_override();
    }
    cam.set_screen_center(edit_camera_state_.screen_center_before);
    if (edit_camera_state_.tilt_override_before.has_value()) {
        cam.set_tilt_override(*edit_camera_state_.tilt_override_before);
    } else {
        cam.clear_tilt_override();
    }
    cam.set_camera_y_distance(edit_camera_state_.camera_y_distance_before);
    cam.set_zoom_percent(edit_camera_state_.camera_zoom_percent_before);
    cam.set_manual_zoom_override(edit_camera_state_.manual_zoom_override_before);
    edit_camera_state_.valid = false;
    camera_y_distance_locked_ = false;
    tilt_locked_ = false;
    edit_camera_locked_ = false;
}


std::unique_ptr<devmode::frame_editors::FrameEditorBase> FrameEditorSession::create_editor(Mode mode) {
    return ::create_editor(mode);
}
