#include "frame_editor_session.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "core/AssetsManager.hpp"
#include "devtools/frame_editors/AttackGeoFrameEditor.hpp"
#include "devtools/frame_editors/FrameEditorBase.hpp"
#include "devtools/frame_editors/HitGeoFrameEditor.hpp"
#include "devtools/frame_editors/MovementFrameEditor.hpp"
#include "devtools/anchor_editor/AnchorEditor.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "utils/grid.hpp"
#include "utils/input.hpp"
#include "devtools/dev_camera_controls.hpp"

namespace {

FrameEditorSession::Mode mode_for_launch(FrameEditorLaunchMode launch_mode) {
    switch (launch_mode) {
        case FrameEditorLaunchMode::Movement: return FrameEditorSession::Mode::Movement;
        case FrameEditorLaunchMode::AttackGeometry: return FrameEditorSession::Mode::AttackGeometry;
        case FrameEditorLaunchMode::HitGeometry: return FrameEditorSession::Mode::HitGeometry;
        case FrameEditorLaunchMode::AnchorPoints: return FrameEditorSession::Mode::AnchorPoints;
    }
    return FrameEditorSession::Mode::Movement;
}

FrameEditorLaunchMode launch_mode_for_mode(FrameEditorSession::Mode mode) {
    switch (mode) {
        case FrameEditorSession::Mode::Movement: return FrameEditorLaunchMode::Movement;
        case FrameEditorSession::Mode::AttackGeometry: return FrameEditorLaunchMode::AttackGeometry;
        case FrameEditorSession::Mode::HitGeometry: return FrameEditorLaunchMode::HitGeometry;
        case FrameEditorSession::Mode::AnchorPoints: return FrameEditorLaunchMode::AnchorPoints;
    }
    return FrameEditorLaunchMode::Movement;
}

std::unique_ptr<devmode::frame_editors::FrameEditorBase> create_editor(FrameEditorSession::Mode mode) {
    switch (mode) {
        case FrameEditorSession::Mode::Movement:
            return std::make_unique<devmode::frame_editors::MovementFrameEditor>();
        case FrameEditorSession::Mode::AttackGeometry:
            return std::make_unique<devmode::frame_editors::AttackGeoFrameEditor>();
        case FrameEditorSession::Mode::HitGeometry:
            return std::make_unique<devmode::frame_editors::HitGeoFrameEditor>();
        case FrameEditorSession::Mode::AnchorPoints:
            return std::make_unique<devmode::anchor_editor::AnchorEditor>();
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
    editor_context_.selected_animation_ids_provider = [this]() {
        if (document_) {
            const auto ids = document_->animation_ids();
            if (!ids.empty()) {
                return ids;
            }
        }
        if (!animation_id_.empty()) {
            return std::vector<std::string>{animation_id_};
        }
        return std::vector<std::string>{};
    };
    editor_context_.on_undo_checkpoint = [](const std::string&) {};

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

    destroy_editor(true);

    const bool target_alive = assets_ && target_ && assets_->contains_asset(target_);
    if (target_alive) {
        target_->set_hidden(prev_asset_hidden_);
    }
    if (assets_) {
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
    const bool handled = active_editor_->handle_event(e);
    if (active_editor_ && active_editor_->wants_close()) {
        end();
        return true;
    }
    return handled;
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
    destroy_editor(false);
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
    editor_context_.selected_animation_ids_provider = [this]() {
        if (document_) {
            const auto ids = document_->animation_ids();
            if (!ids.empty()) {
                return ids;
            }
        }
        if (!animation_id_.empty()) {
            return std::vector<std::string>{animation_id_};
        }
        return std::vector<std::string>{};
    };
    editor_context_.on_undo_checkpoint = [](const std::string&) {};

    active_editor_ = create_editor(mode_);
    if (active_editor_) {
        frame_camera_for_editor_entry();
        active_editor_->begin(editor_context_);
    }
}

void FrameEditorSession::frame_camera_for_editor_entry() {
    if (!assets_ || !target_) {
        return;
    }

    WarpedScreenGrid& cam = assets_->getView();
    const SDL_Point focus = target_->world_point();

    // Establish a consistent baseline before computing scale.
    cam.set_manual_height_override(true);
    cam.set_manual_zoom_override(true);
    cam.set_zoom_percent(0.0);
    cam.set_focus_override(focus);
    cam.set_screen_center(focus);

    constexpr float kFramingTiltDeg = 1.0f;  // Forward-facing, not top-down.
    cam.set_tilt_override(kFramingTiltDeg);

    SDL_Renderer* renderer = assets_->renderer();
    int screen_w = 0;
    int screen_h = 0;
    if (renderer) {
        SDL_GetCurrentRenderOutputSize(renderer, &screen_w, &screen_h);
    }
    if (screen_w <= 0 || screen_h <= 0) {
        return;
    }

    const int frame_h = target_->height();
    if (frame_h <= 0) {
        return;
    }

    auto sanitize_scale = [](float value) -> double {
        return (std::isfinite(value) && value > 0.0f) ? static_cast<double>(value) : 1.0;
    };

    double sprite_height_world = static_cast<double>(frame_h);
    if (target_->info && std::isfinite(target_->info->scale_factor) && target_->info->scale_factor > 0.0f) {
        sprite_height_world *= static_cast<double>(target_->info->scale_factor);
    }
    sprite_height_world *= sanitize_scale(target_->smoothed_scale());
    if (!(sprite_height_world > 0.0)) {
        return;
    }

    constexpr double kTargetHeightRatio = 0.7;
    const double target_height_px = static_cast<double>(screen_h) * kTargetHeightRatio;

    const double starting_scale = cam.get_scale();
    auto sprite_height_for_scale = [&](double scale) -> std::optional<double> {
        cam.set_scale(scale);
        const auto effects = cam.compute_render_effects(
            focus, 0.0f, 0.0f, WarpedScreenGrid::RenderSmoothingKey{}, target_->world_z());
        if (effects.distance_scale <= 0.0f || effects.vertical_scale <= 0.0f) {
            return std::nullopt;
        }
        const double h_px = sprite_height_world *
                            static_cast<double>(effects.distance_scale) *
                            static_cast<double>(effects.vertical_scale);
        if (!std::isfinite(h_px)) {
            return std::nullopt;
        }
        return h_px;
    };

    constexpr double kMinScale = 1.0;
    constexpr double kMaxScale = 50000.0;
    double best_scale = std::clamp(starting_scale, kMinScale, kMaxScale);
    double best_diff = std::numeric_limits<double>::infinity();

    auto consider = [&](double scale) {
        auto height_opt = sprite_height_for_scale(scale);
        if (!height_opt.has_value()) {
            return;
        }
        const double diff = std::abs(*height_opt - target_height_px);
        if (diff < best_diff) {
            best_diff = diff;
            best_scale = scale;
        }
    };

    consider(best_scale);
    consider(kMinScale);
    consider(kMaxScale);

    double lo = kMinScale;
    double hi = kMaxScale;
    for (int i = 0; i < 26; ++i) {
        const double mid = 0.5 * (lo + hi);
        auto h_opt = sprite_height_for_scale(mid);
        if (h_opt.has_value()) {
            consider(mid);
            if (*h_opt > target_height_px) {
                lo = mid;  // Sprite too large -> move camera farther.
            } else {
                hi = mid;  // Sprite too small -> move camera closer.
            }
        } else {
            break;
        }
    }

    cam.set_scale(best_scale);
    cam.set_focus_override(focus);
    cam.set_screen_center(focus);
}

void FrameEditorSession::destroy_editor(bool persist_changes) {
    if (!active_editor_) {
        return;
    }
    if (persist_changes) {
        active_editor_->persist_pending_changes();
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
    cam.set_zoom_percent(camera_lock_state_.camera_zoom_percent_before);
    cam.set_manual_zoom_override(camera_lock_state_.manual_zoom_override_before);
    camera_lock_state_.valid = false;
}

std::unique_ptr<devmode::frame_editors::FrameEditorBase> FrameEditorSession::create_editor(Mode mode) {
    return ::create_editor(mode);
}

