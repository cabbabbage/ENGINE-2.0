#include "SyncChildrenFrameEditor.hpp"
#include "utils/sdl_mouse_utils.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "assets/Asset.hpp"
#include "assets/animation.hpp"
#include "assets/animation_frame.hpp"
#include "devtools/asset_editor/animation_editor_window/AnimationDocument.hpp"
#include "devtools/dm_styles.hpp"
#include "devtools/widgets.hpp"
#include "devtools/frame_editors/shared/SnapUtils.hpp"
#include "utils/FramePointResolver.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "rendering/render/composite_asset_renderer.hpp"
#include <SDL3_image/SDL_image.h>

namespace devmode::frame_editors {

namespace {
SDL_Point round_point(const SDL_FPoint& pt) {
    return SDL_Point{static_cast<int>(std::lround(pt.x)), static_cast<int>(std::lround(pt.y))};
}

}  // namespace

void SyncChildrenFrameEditor::begin(const FrameEditorContext& context) {
    context_ = context;
    selection_state_ = context.selection_state;
    point_3d_editor_ = std::make_unique<Point3DEditor>(selection_state_);
    if (selection_state_) {
        selection_state_->reset();
    }

    cache_parent_pose();

    // Cache the perspective scale from the camera at session start
    // This prevents camera movement from affecting child position calculations
    cached_perspective_scale_ = 1.0f;
    if (context_.assets && context_.target) {
        const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
        if (const auto* gp = cam.grid_point_for_asset(context_.target)) {
            cached_perspective_scale_ = std::max(0.0001f, gp->perspective_scale);
        }
    }
    cached_attachment_scale_ = 1.0f;
    if (context_.target) {
        float remainder = context_.target->current_remaining_scale_adjustment;
        if (!std::isfinite(remainder) || remainder <= 0.0f) {
            remainder = 1.0f;
        }
        cached_attachment_scale_ = remainder / std::max(0.0001f, cached_perspective_scale_);
        if (!std::isfinite(cached_attachment_scale_) || cached_attachment_scale_ <= 0.0f) {
            cached_attachment_scale_ = 1.0f;
        }
    }

    if (point_3d_editor_) {
        // Depth axis is locked in sync child mode
        point_3d_editor_->set_axis_enabled(AdjustmentAxis::Y, false);
        point_3d_editor_->reset_axis(AdjustmentAxis::X);
        point_3d_editor_->set_grid_resolution(context_.snap_resolution);
        // Children use Z as percentage of parent height
        point_3d_editor_->set_z_display_mode(ZDisplayMode::Percentage);
        FramePointResolver resolver(context_.target);
        point_3d_editor_->set_parent_height(resolver.parent_height_px());

        point_3d_editor_->set_on_point_selected([this](int index) {
            if (index < 0) {
                // Deselecting - persist changes before clearing selection state
                if (data_dirty_ && !static_frames_by_child_.empty()) {
                    apply_live_changes();
                }
                if (selection_state_) {
                    selection_state_->reset();
                }
                selected_child_index_ = -1;
            } else {
                // Selecting a child point - guard against invalid state
                if (static_frames_by_child_.empty() || child_assets_.empty()) {
                    return;  // Data not ready
                }
                const int child_index = child_index_from_point_index(index);
                if (child_index < 0) {
                    return;
                }
                selected_child_index_ = std::clamp(child_index, 0, static_cast<int>(child_assets_.size()) - 1);
                if (dd_child_selector_) {
                    dd_child_selector_->set_selected(selected_child_index_);
                }
                if (selection_state_) {
                    selection_state_->target = SelectionTarget::ChildPoint;
                    selection_state_->child_index = selected_child_index_;
                }
                refresh_selection_state();
                sync_visibility_checkbox();
            }
        });

        point_3d_editor_->set_on_position_changed([this](const SDL_FPoint& new_world_pos, float new_world_z) {
            if (selected_child_index_ < 0 || selected_child_index_ >= static_cast<int>(static_frames_by_child_.size()) ||
                selected_frame_index_ < 0 || selected_frame_index_ >= frame_count_ || static_frames_by_child_.empty()) {
                return;
            }

            // Guard: verify the selected child has a frame at this index
            const auto& child_frames = static_frames_by_child_[selected_child_index_];
            if (selected_frame_index_ >= static_cast<int>(child_frames.size())) {
                return;
            }

            float scale = attachment_scale();
            if (scale <= 0.0f) scale = 1.0f;

            SDL_FPoint snapped_world = snap_world_point_to_grid(new_world_pos, context_.snap_resolution);
            float snapped_world_z = snap_world_z_to_grid(new_world_z, context_.snap_resolution);
            SDL_Point anchor = asset_anchor_world();
            const bool flipped = context_.target && context_.target->flipped;
            FramePointResolver resolver(context_.target);

            const float parent_height = (cached_parent_pose_valid_ && cached_parent_height_ > 0.0f)
                                            ? cached_parent_height_
                                            : std::max(1.0f, resolver.parent_height_px());
            const float base_world_z = cached_parent_pose_valid_ ? cached_base_world_z_ : resolver.base_world_z();

            float dx_world = (snapped_world.x - static_cast<float>(anchor.x)) / scale;
            float dy_world = (snapped_world.y - static_cast<float>(anchor.y)) / scale;
            float px_percent = dx_world / parent_height;
            float py_percent = dy_world / parent_height;

            auto& sample = const_cast<std::vector<child_timelines::ChildFrameSample>&>(child_frames)[selected_frame_index_];
            const float dz_percent = (snapped_world_z - base_world_z) / parent_height;

            sample.px = flipped ? -px_percent : px_percent;
            sample.py = py_percent;
            sample.pz = dz_percent;
            sample.has_data = true;
            sample.child_index = selected_child_index_;
            sample.visible = true;

            data_dirty_ = true;
            refresh_selection_state();
        });

        point_3d_editor_->set_on_coordinates_changed([this]() {
            if (selected_child_index_ < 0 || selected_child_index_ >= static_cast<int>(static_frames_by_child_.size()) ||
                selected_frame_index_ < 0 || selected_frame_index_ >= frame_count_ || !selection_state_ || static_frames_by_child_.empty()) {
                return;
            }

            // Guard: verify the selected child has a frame at this index
            const auto& child_frames = static_frames_by_child_[selected_child_index_];
            if (selected_frame_index_ >= static_cast<int>(child_frames.size())) {
                return;
            }

            float scale = attachment_scale();
            if (scale <= 0.0f) scale = 1.0f;

            SDL_FPoint snapped_world = snap_world_point_to_grid(selection_state_->world_pos, context_.snap_resolution);
            float snapped_world_z = snap_world_z_to_grid(selection_state_->world_z, context_.snap_resolution);
            selection_state_->world_pos = snapped_world;
            selection_state_->world_z = snapped_world_z;
            SDL_Point anchor = asset_anchor_world();
            const bool flipped = context_.target && context_.target->flipped;
            FramePointResolver resolver(context_.target);

            const float parent_height = (cached_parent_pose_valid_ && cached_parent_height_ > 0.0f)
                                            ? cached_parent_height_
                                            : std::max(1.0f, resolver.parent_height_px());
            const float base_world_z = cached_parent_pose_valid_ ? cached_base_world_z_ : resolver.base_world_z();

            float dx_world = (snapped_world.x - static_cast<float>(anchor.x)) / scale;
            float dy_world = (snapped_world.y - static_cast<float>(anchor.y)) / scale;
            float px_percent = dx_world / parent_height;
            float py_percent = dy_world / parent_height;

            auto& sample = const_cast<std::vector<child_timelines::ChildFrameSample>&>(child_frames)[selected_frame_index_];
            const float dz_percent = (snapped_world_z - base_world_z) / parent_height;

            sample.px = flipped ? -px_percent : px_percent;
            sample.py = py_percent;
            sample.pz = dz_percent;
            sample.has_data = true;
            sample.child_index = selected_child_index_;
            sample.visible = true;

            data_dirty_ = true;
        });
    }
    wants_close_ = false;
    selected_frame_index_ = 0;
    selected_child_index_ = 0;
    btn_back_ = std::make_unique<DMButton>("Back", &DMStyles::HeaderButton(), 80, DMButton::height());
    back_rect_ = SDL_Rect{DMSpacing::small_gap(), DMSpacing::small_gap(), 80, DMButton::height()};
    if (btn_back_) {
        btn_back_->set_rect(back_rect_);
    }
    if (!context_.target || !context_.document) {
        return;
    }
    populate_child_data();

    // Initialize manifest transaction AFTER loading data
    ensure_manifest_transaction();

    frame_navigator_ = std::make_unique<FrameNavigator>();
    frame_navigator_->set_frame_count(frame_count_);
    frame_navigator_->set_current_frame(selected_frame_index_);
    frame_navigator_->set_on_frame_changed([this](int frame) {
        // Save changes before changing frames
        persist_pending_changes();
        selected_frame_index_ = frame;
        data_dirty_ = false;  // Don't re-apply just from frame change
        // Deselect current point when changing frames
        if (selection_state_) {
            selection_state_->reset();
        }
        if (point_3d_editor_) {
            point_3d_editor_->set_selected_point_index(-1);
        }
        sync_visibility_checkbox();
        apply_current_frame_to_children();
        refresh_selection_state();
    });

    // Initialize child selector dropdown
    std::vector<std::string> child_labels;
    for (const auto& child : child_assets_) {
        child_labels.push_back(child);
    }
    if (child_labels.empty()) {
        child_labels.push_back("No children");
    }
    dd_child_selector_ = std::make_unique<DMDropdown>("Child", child_labels, std::min(selected_child_index_, static_cast<int>(child_labels.size()) - 1));
    dd_child_selector_->set_on_selection_changed([this](int index) {
        // Save changes before switching children
        if (data_dirty_) {
            persist_pending_changes();
        }

        selected_child_index_ = index;

        if (selection_state_) {
            selection_state_->target = SelectionTarget::ChildPoint;
            selection_state_->child_index = selected_child_index_;
        }
        if (point_3d_editor_) {
            const int point_index = point_index_for_child(selected_child_index_);
            if (point_index >= 0) {
                point_3d_editor_->set_selected_point_index(point_index);
            }
        }
        data_dirty_ = true;
        sync_visibility_checkbox();
        refresh_selection_state();
    });

    // Initialize visibility checkbox
    auto visibility_checkbox = std::make_unique<DMCheckbox>("Visible", true);
    cb_child_visible_ = std::make_unique<CallbackCheckboxWidget>(
        std::move(visibility_checkbox),
        [this](bool visible) {
            if (selected_child_index_ >= 0 && selected_child_index_ < static_cast<int>(static_frames_by_child_.size()) &&
                selected_frame_index_ >= 0 && selected_frame_index_ < frame_count_ && !static_frames_by_child_.empty()) {
                // Verify the selected child has a frame at this index
                const auto& child_frames = static_frames_by_child_[selected_child_index_];
                if (selected_frame_index_ < static_cast<int>(child_frames.size())) {
                    auto& sample = const_cast<std::vector<child_timelines::ChildFrameSample>&>(child_frames)[selected_frame_index_];
                    sample.visible = visible;
                    sample.has_data = true;
                    data_dirty_ = true;
                }
            }
    });

    // Initialize reset frame button
    btn_reset_frame_ = std::make_unique<DMButton>("Reset Frame", &DMStyles::PrimaryButton(), 120, DMButton::height());
    btn_apply_next_ = std::make_unique<DMButton>("Apply Data To Next Frame", &DMStyles::PrimaryButton(), 200, DMButton::height());
    btn_apply_all_ = std::make_unique<DMButton>("Apply Data To All Frames", &DMStyles::PrimaryButton(), 200, DMButton::height());

    // Initial sync of visibility checkbox with current frame/child state
    sync_visibility_checkbox();

    // If we didn't initialize anything, mark as clean
    // If we did initialize, data_dirty_ is already true
}

void SyncChildrenFrameEditor::end() {
    // Persist any pending changes before exiting
    persist_pending_changes();

    if (selection_state_) {
        selection_state_->reset();
        selection_state_ = nullptr;
    }
    point_3d_editor_ = nullptr;
    btn_back_.reset();
    btn_reset_frame_.reset();
    btn_apply_next_.reset();
    btn_apply_all_.reset();
    frame_navigator_.reset();
    dd_child_selector_.reset();
    cb_child_visible_.reset();
    wants_close_ = false;
    cached_parent_pose_valid_ = false;
    cached_anchor_world_ = SDL_Point{0, 0};
    cached_world_point_ = SDL_Point{0, 0};
    cached_base_world_z_ = 0.0f;
    cached_parent_height_ = 0.0f;
}

bool SyncChildrenFrameEditor::handle_event(const SDL_Event& e) {
    // Handle Point3DEditor events first
    SDL_Rect overlay_rect{0, 0, 0, 0};
    bool overlay_valid = false;
    if (point_3d_editor_) {
        // Use the cached container from Point3DEditor (set during render_overlays)
        // This avoids issues with SDL_GetCurrentRenderOutputSize(nullptr, ...) failing
        overlay_rect = point_3d_editor_->get_cached_container();
        overlay_valid = (overlay_rect.w > 0 && overlay_rect.h > 0);
        if (point_3d_editor_->handle_event(e, overlay_rect)) {
            return true;
        }
    }

    if (!context_.assets || !context_.target) {
        return false;
    }
    if (frame_navigator_ && frame_navigator_->handle_event(e)) {
        if (selection_state_) selection_state_->reset();
        if (point_3d_editor_) point_3d_editor_->set_selected_point_index(-1);
        return true;
    }
    if (btn_back_ && btn_back_->handle_event(e)) {
        wants_close_ = true;
        return true;
    }
    if (dd_child_selector_ && dd_child_selector_->handle_event(e)) {
        return true;
    }
    if (cb_child_visible_ && cb_child_visible_->handle_event(e)) {
        return true;
    }
    if (btn_reset_frame_ && btn_reset_frame_->handle_event(e)) {
        reset_current_frame();
        return true;
    }
    if (btn_apply_next_ && btn_apply_next_->handle_event(e)) {
        apply_current_sample_to_next_frame();
        return true;
    }
    if (btn_apply_all_ && btn_apply_all_->handle_event(e)) {
        apply_current_sample_to_all_frames();
        return true;
    }

    SDL_Point mouse_pos = {0, 0};
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        mouse_pos = {static_cast<int>(std::lround(e.button.x)), static_cast<int>(std::lround(e.button.y))};
    } else if (e.type == SDL_EVENT_MOUSE_MOTION) {
        mouse_pos = {static_cast<int>(std::lround(e.motion.x)), static_cast<int>(std::lround(e.motion.y))};
    } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        sdl_mouse_util::GetMouseState(&mouse_pos.x, &mouse_pos.y);
    }

    const bool pointer_in_overlay = overlay_valid && SDL_PointInRect(&mouse_pos, &overlay_rect);
    if (!SDL_PointInRect(&mouse_pos, &back_rect_) &&
        !SDL_PointInRect(&mouse_pos, &ui_rect_) &&
        !pointer_in_overlay) {
        const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
        SDL_Point anchor = asset_anchor_world();
        std::vector<SDL_FPoint> point_screens;
        std::vector<bool> point_selectable;
        const std::vector<int> point_indices = static_child_point_indices();
        point_screens.reserve(point_indices.size());
        point_selectable.reserve(point_indices.size());

        for (int child_index : point_indices) {
            const ChildWorldPose pose = child_world_pose(child_index);
            SDL_FPoint world_pos{anchor.x + pose.pos.x, anchor.y + pose.pos.y};
            SDL_FPoint screen{};
            if (!cam.project_world_point(world_pos, pose.z, screen)) {
                screen = cam.map_to_screen_f(world_pos);
            }
            point_screens.push_back(screen);
            point_selectable.push_back(true);
        }

        // Only consume event if point editor actually handled it
        if (point_3d_editor_->handle_mouse_event(e, point_screens, point_selectable)) {
            return true;
        }
    }

    if (e.type == SDL_EVENT_KEY_DOWN) {
        // No LEFT/RIGHT arrow keys - use frame navigator buttons only
        if (e.key.key == SDLK_TAB) {
            selected_child_index_ = (selected_child_index_ + 1) % std::max(1, static_cast<int>(child_assets_.size()));
            if (selection_state_) {
                selection_state_->child_index = selected_child_index_;
            }
            data_dirty_ = true;
            return true;
        }
    }
    return false;
}

void SyncChildrenFrameEditor::update(const Input& /*input*/, float /*dt*/) {
    if (data_dirty_ && context_.target) {
        apply_current_frame_to_children();
        apply_live_changes();
        data_dirty_ = false;
    }
}

void SyncChildrenFrameEditor::render_world(SDL_Renderer* renderer) const {
    if (!renderer || !context_.target || !context_.assets || !point_3d_editor_) return;
    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();

    // Render child assets - sync their positions from selected frame data first
    SDL_Point anchor = asset_anchor_world();
    for (std::size_t i = 0; i < child_assets_.size(); ++i) {
        if (i >= static_frames_by_child_.size()) {
            continue;
        }
        if (selected_frame_index_ < 0 || selected_frame_index_ >= frame_count_) {
            continue;
        }
        const auto& timeline = static_frames_by_child_[i];
        if (selected_frame_index_ >= static_cast<int>(timeline.size())) {
            continue;
        }
        const auto& sample = timeline[static_cast<std::size_t>(selected_frame_index_)];
        if (!sample.visible) {
            continue;
        }
        Asset* child = context_.assets->find_child_timeline_asset(context_.target, static_cast<int>(i));
        if (!child) continue;
        const ChildWorldPose pose = child_world_pose(static_cast<int>(i));
        child->move_to_world_position(static_cast<int>(std::lround(anchor.x + pose.pos.x)),
                               static_cast<int>(std::lround(anchor.y + pose.pos.y)));
        child->set_world_z_offset(pose.z);
        child->mark_composite_dirty();
        CompositeAssetRenderer composite_renderer(renderer, context_.assets);
        composite_renderer.update(child, 0.0f);
    }

    // Render axis points - always render them so user can see and edit them
    for (std::size_t idx = 0; idx < child_assets_.size(); ++idx) {
        if (idx >= static_frames_by_child_.size()) continue;
        const ChildWorldPose pose = child_world_pose(static_cast<int>(idx));
        SDL_FPoint child_pos = pose.pos;
        SDL_FPoint world{anchor.x + child_pos.x, anchor.y + child_pos.y};
        SDL_FPoint screen{};
        if (!cam.project_world_point(world, pose.z, screen)) {
            screen = cam.map_to_screen_f(world);
        }

        const bool is_current_child = (static_cast<int>(idx) == selected_child_index_);
        const bool is_selected = (is_current_child &&
                                 selection_state_ &&
                                 selection_state_->target == SelectionTarget::ChildPoint);
        const int hovered_child = child_index_from_point_index(point_3d_editor_->get_hovered_point_index());
        const bool is_hovered = (static_cast<int>(idx) == hovered_child);

        // Check if this child is marked as invisible in the frame data
        bool is_visible_in_frame = false;
        if (selected_frame_index_ >= 0 && selected_frame_index_ < static_cast<int>(static_frames_by_child_[idx].size())) {
            is_visible_in_frame = static_frames_by_child_[idx][selected_frame_index_].visible;
        }

        // Always render the edit point, but use different appearance for invisible children
        if (is_current_child) {
            point_3d_editor_->render_selectable_point(renderer, screen, is_selected, is_hovered);
            // If invisible, draw an X through the point to indicate it won't render
            if (!is_visible_in_frame) {
                SDL_SetRenderDrawColor(renderer, 255, 0, 0, 180);
                constexpr int x_size = 12;
                SDL_RenderLine(renderer,
                                 static_cast<int>(screen.x) - x_size, static_cast<int>(screen.y) - x_size,
                                 static_cast<int>(screen.x) + x_size, static_cast<int>(screen.y) + x_size);
                SDL_RenderLine(renderer,
                                 static_cast<int>(screen.x) + x_size, static_cast<int>(screen.y) - x_size,
                                 static_cast<int>(screen.x) - x_size, static_cast<int>(screen.y) + x_size);
            }
        } else {
            point_3d_editor_->render_non_selectable_point(renderer, screen);
            // If invisible, draw a smaller X to indicate it won't render
            if (!is_visible_in_frame) {
                SDL_SetRenderDrawColor(renderer, 255, 0, 0, 120);
                constexpr int x_size = 8;
                SDL_RenderLine(renderer,
                                 static_cast<int>(screen.x) - x_size, static_cast<int>(screen.y) - x_size,
                                 static_cast<int>(screen.x) + x_size, static_cast<int>(screen.y) + x_size);
                SDL_RenderLine(renderer,
                                 static_cast<int>(screen.x) + x_size, static_cast<int>(screen.y) - x_size,
                                 static_cast<int>(screen.x) - x_size, static_cast<int>(screen.y) + x_size);
            }
        }
    }
}

void SyncChildrenFrameEditor::render_overlays(SDL_Renderer* renderer) const {
    if (!renderer) return;

    // Layout the UI elements
    int sw = 0, sh = 0;
    SDL_GetCurrentRenderOutputSize(renderer, &sw, &sh);
    const int padding = DMSpacing::small_gap();
    const int width = 280;
    const int x = padding;
    const int y = padding;
    ui_rect_ = SDL_Rect{x, y, width, 0};
    int cursor_y = y + padding;

    // Back button
    if (btn_back_) {
        SDL_Rect back_rect{x + padding, cursor_y, 80, DMButton::height()};
        btn_back_->set_rect(back_rect);
        cursor_y += DMButton::height() + DMSpacing::small_gap();
    }

    // Frame navigator
    if (frame_navigator_) {
        SDL_Rect nav_rect{x + padding, cursor_y, width - (padding * 2), frame_navigator_->get_preferred_rect().h};
        frame_navigator_->set_rect(nav_rect);
        cursor_y += nav_rect.h + DMSpacing::small_gap();
    }

    // Child selector
    if (dd_child_selector_) {
        SDL_Rect dd_rect{x + padding, cursor_y, width - (padding * 2), DMDropdown::height()};
        dd_child_selector_->set_rect(dd_rect);
        cursor_y += DMDropdown::height() + DMSpacing::small_gap();
    }

    // Visibility checkbox
    if (cb_child_visible_) {
        SDL_Rect cb_rect{x + padding, cursor_y, width - (padding * 2), DMCheckbox::height()};
        cb_child_visible_->set_rect(cb_rect);
        cursor_y += DMCheckbox::height() + DMSpacing::small_gap();
    }

    // Reset frame button
    if (btn_reset_frame_) {
        SDL_Rect reset_rect{x + padding, cursor_y, 120, DMButton::height()};
        btn_reset_frame_->set_rect(reset_rect);
        cursor_y += DMButton::height() + DMSpacing::small_gap();
    }
    if (btn_apply_next_) {
        SDL_Rect next_rect{x + padding, cursor_y, width - (padding * 2), DMButton::height()};
        btn_apply_next_->set_rect(next_rect);
        cursor_y += DMButton::height() + DMSpacing::small_gap();
    }
    if (btn_apply_all_) {
        SDL_Rect all_rect{x + padding, cursor_y, width - (padding * 2), DMButton::height()};
        btn_apply_all_->set_rect(all_rect);
        cursor_y += DMButton::height() + DMSpacing::small_gap();
    }

    ui_rect_.h = cursor_y - y;

    // Render elements
    if (btn_back_) btn_back_->render(renderer);
    if (frame_navigator_) frame_navigator_->render(renderer);
    if (dd_child_selector_) dd_child_selector_->render(renderer);
    if (cb_child_visible_) cb_child_visible_->render(renderer);
    if (btn_reset_frame_) btn_reset_frame_->render(renderer);
    if (btn_apply_next_) btn_apply_next_->render(renderer);
    if (btn_apply_all_) btn_apply_all_->render(renderer);

    // Render Point3DEditor overlays at the bottom
    if (point_3d_editor_) {
        int sw = 0, sh = 0;
        SDL_GetCurrentRenderOutputSize(renderer, &sw, &sh);
        int height = point_3d_editor_->get_overlay_height(sw);
        SDL_Rect bottom_container{0, sh - height, sw, height};
        point_3d_editor_->render_overlays(renderer, bottom_container);
    }
}

void SyncChildrenFrameEditor::populate_child_data() {
    child_assets_.clear();
    static_frames_by_child_.clear();
    frame_count_ = 0;
    if (!context_.target || !context_.document) {
        return;
    }
    child_assets_ = context_.document->animation_children();

    if (!context_.target->info) {
        return;
    }
    auto it = context_.target->info->animations.find(context_.animation_id);
    if (it == context_.target->info->animations.end()) {
        return;
    }
    const Animation& animation = it->second;
    frame_count_ = std::max(1, animation.number_of_frames);

    static_frames_by_child_.assign(child_assets_.size(),
                                   std::vector<child_timelines::ChildFrameSample>(static_cast<std::size_t>(frame_count_)));

    // Default samples
    for (std::size_t child_idx = 0; child_idx < child_assets_.size(); ++child_idx) {
        for (int frame_idx = 0; frame_idx < frame_count_; ++frame_idx) {
            auto& sample = static_frames_by_child_[child_idx][static_cast<std::size_t>(frame_idx)];
            sample.child_index = static_cast<int>(child_idx);
            sample.px = 0.0f;
            sample.py = 0.0f;
            sample.pz = 0.0f;
            sample.degree = 0.0f;
            sample.visible = false;
            sample.has_data = true;
        }
    }

    // Load child timeline data if present
    const auto& timelines = animation.child_timelines();
    for (const auto& descriptor : timelines) {
        std::size_t idx = std::distance(child_assets_.begin(),
                                        std::find(child_assets_.begin(), child_assets_.end(), descriptor.asset_name));
        if (idx >= child_assets_.size()) {
            continue;
        }
        auto& timeline = static_frames_by_child_[idx];
        const std::size_t frame_count = std::min<std::size_t>(timeline.size(), descriptor.frames.size());
        for (std::size_t frame_idx = 0; frame_idx < frame_count; ++frame_idx) {
            const auto& frame = descriptor.frames[frame_idx];
            auto& sample = timeline[frame_idx];
            sample.child_index = static_cast<int>(idx);
            sample.px = frame.offset.px;
            sample.py = frame.offset.py;
            sample.pz = frame.offset.pz;
            sample.degree = frame.degree;
            sample.visible = frame.visible;
            sample.has_data = true;
        }
    }
    data_dirty_ = true;
}

void SyncChildrenFrameEditor::apply_current_frame_to_children() {
    if (!context_.target || !context_.assets) {
        return;
    }
    if (selected_frame_index_ < 0 || selected_frame_index_ >= frame_count_) {
        return;
    }
    SDL_Point anchor = asset_anchor_world();
    FramePointResolver resolver(context_.target);
    const float parent_height = (cached_parent_pose_valid_ && cached_parent_height_ > 0.0f)
                                    ? cached_parent_height_
                                    : resolver.parent_height_px();
    const float base_world_z = cached_parent_pose_valid_ ? cached_base_world_z_ : resolver.base_world_z();
    const float scale = attachment_scale();
    if (!(parent_height > 0.0f)) {
        // Without a valid parent height we cannot place children meaningfully; hide them.
        for (std::size_t idx = 0; idx < child_assets_.size(); ++idx) {
            if (auto* child_asset = context_.assets->find_child_timeline_asset(context_.target, static_cast<int>(idx))) {
                child_asset->set_hidden(true);
            }
        }
        return;
    }

    for (std::size_t idx = 0; idx < child_assets_.size(); ++idx) {
        if (idx >= static_frames_by_child_.size()) continue;
        auto* child_asset = context_.assets->find_child_timeline_asset(context_.target, static_cast<int>(idx));
        if (!child_asset) continue;

        const auto& timeline = static_frames_by_child_[idx];
        if (timeline.empty()) {
            child_asset->set_hidden(true);
            continue;
        }
        const std::size_t sample_idx = std::min<std::size_t>(timeline.size() - 1, static_cast<std::size_t>(selected_frame_index_));
        const auto& sample = timeline[sample_idx];
        if (!sample.visible) {
            child_asset->set_hidden(true);
            continue;
        }

        float offset_x = sample.px * parent_height * scale;
        if (context_.target && context_.target->flipped) {
            offset_x = -offset_x;
        }
        const float world_x = static_cast<float>(anchor.x) + offset_x;
        const float world_y = static_cast<float>(anchor.y) + sample.py * parent_height * scale;
        const float world_z = base_world_z + sample.pz * parent_height;
        child_asset->set_hidden(false);
        child_asset->move_to_world_position(static_cast<int>(std::lround(world_x)),
                                            static_cast<int>(std::lround(world_y)),
                                            static_cast<int>(std::lround(world_z)));
    }
    context_.target->mark_composite_dirty();
}

void SyncChildrenFrameEditor::cache_parent_pose() {
    cached_anchor_world_ = SDL_Point{0, 0};
    cached_world_point_ = SDL_Point{0, 0};
    cached_base_world_z_ = 0.0f;
    cached_parent_height_ = 0.0f;
    cached_parent_pose_valid_ = false;
    if (!context_.target) {
        return;
    }

    FramePointResolver resolver(context_.target);
    cached_anchor_world_ = resolver.anchor_world();
    cached_world_point_ = context_.target->world_point();
    cached_base_world_z_ = resolver.base_world_z();
    cached_parent_height_ = resolver.parent_height_px();
    cached_parent_pose_valid_ = true;
}

float SyncChildrenFrameEditor::attachment_scale() const {
    if (!std::isfinite(cached_attachment_scale_) || cached_attachment_scale_ <= 0.0f) {
        return 1.0f;
    }
    return cached_attachment_scale_;
}

SDL_Point SyncChildrenFrameEditor::asset_anchor_world() const {
    if (cached_parent_pose_valid_) {
        return cached_anchor_world_;
    }
    if (!context_.target) {
        return SDL_Point{0, 0};
    }
    FramePointResolver resolver(context_.target);
    return resolver.anchor_world();
}

SyncChildrenFrameEditor::ChildWorldPose SyncChildrenFrameEditor::child_world_pose(int child_index) const {
    ChildWorldPose pose{};
    if (child_index < 0 || child_index >= static_cast<int>(static_frames_by_child_.size())) {
        return pose;
    }
    if (frame_count_ <= 0) {
        return pose;
    }
    if (!context_.target) {
        return pose;
    }

    int frame_idx = std::clamp(selected_frame_index_, 0, frame_count_ - 1);
    const auto& timeline = static_frames_by_child_[static_cast<std::size_t>(child_index)];
    if (frame_idx >= static_cast<int>(timeline.size())) {
        return pose;
    }
    const auto& sample = timeline[static_cast<std::size_t>(frame_idx)];

    FramePointResolver resolver(context_.target);
    const float scale = attachment_scale();
    float parent_height = (cached_parent_pose_valid_ && cached_parent_height_ > 0.0f)
                              ? cached_parent_height_
                              : resolver.parent_height_px();
    if (!(parent_height > 0.0f)) {
        parent_height = 1.0f;
    }
    const float dx_world = sample.px * parent_height * scale;
    const float dy_world = sample.py * parent_height * scale;
    float offset_x = dx_world;
    if (context_.target && context_.target->flipped) {
        offset_x = -offset_x;
    }
    pose.pos = SDL_FPoint{offset_x, dy_world};
    const float base_world_z = cached_parent_pose_valid_ ? cached_base_world_z_ : resolver.base_world_z();
    pose.z = base_world_z + (sample.pz * parent_height);
    return pose;
}

std::vector<int> SyncChildrenFrameEditor::static_child_point_indices() const {
    std::vector<int> indices;
    indices.reserve(child_assets_.size());
    for (std::size_t idx = 0; idx < child_assets_.size(); ++idx) {
        indices.push_back(static_cast<int>(idx));
    }
    return indices;
}

int SyncChildrenFrameEditor::child_index_from_point_index(int point_index) const {
    if (point_index < 0) {
        return -1;
    }
    const std::vector<int> indices = static_child_point_indices();
    if (point_index >= static_cast<int>(indices.size())) {
        return -1;
    }
    return indices[static_cast<std::size_t>(point_index)];
}

int SyncChildrenFrameEditor::point_index_for_child(int child_index) const {
    const std::vector<int> indices = static_child_point_indices();
    auto it = std::find(indices.begin(), indices.end(), child_index);
    if (it == indices.end()) {
        return -1;
    }
    return static_cast<int>(std::distance(indices.begin(), it));
}


void SyncChildrenFrameEditor::ensure_manifest_transaction() {
    if (!context_.document) {
        return;
    }
    // Only begin if not already active to avoid re-entrant calls
    if (!manifest_txn_.active()) {
        manifest_txn_.begin(context_);
        manifest_txn_.set_immediate_persist(true);
        manifest_txn_.set_deferred_persist(false);
    }
    // Always update the callback to use current data
    manifest_txn_.set_apply_callback([this]() -> bool {
        if (!context_.document) {
            return false;
        }

        auto payload_opt = context_.document->animation_payload_json(context_.animation_id);
        nlohmann::json payload = payload_opt.value_or(nlohmann::json::object());

        payload["child_timelines"] = child_timelines::build_child_timelines_payload(
            payload,
            static_frames_by_child_,
            child_assets_);
        return context_.document->update_animation_payload(context_.animation_id, payload);
    });
}

void SyncChildrenFrameEditor::apply_live_changes() {
    force_save_to_disk();
}

void SyncChildrenFrameEditor::persist_pending_changes() {
    force_save_to_disk();
}

void SyncChildrenFrameEditor::force_save_to_disk() {
    ensure_manifest_transaction();
    if (manifest_txn_.active()) {
        manifest_txn_.commit(true);
        invalidate_preview();
    }
}

void SyncChildrenFrameEditor::invalidate_preview() const {
    if (context_.preview && !context_.animation_id.empty()) {
        context_.preview->invalidate(context_.animation_id);
    }
}

void SyncChildrenFrameEditor::refresh_selection_state() {
    if (!selection_state_ || !context_.target || !context_.assets ||
        selection_state_->target != SelectionTarget::ChildPoint) {
        return;
    }
    FramePointResolver resolver(context_.target);
    // Update parent height for Z percent display (in case scale changed)
    if (point_3d_editor_) {
        float parent_height = cached_parent_pose_valid_ ? cached_parent_height_ : resolver.parent_height_px();
        if (!(parent_height > 0.0f)) {
            parent_height = resolver.parent_height_px();
        }
        point_3d_editor_->set_parent_height(parent_height);
    }
    selection_state_->child_index = selected_child_index_;
    const ChildWorldPose pose = child_world_pose(selected_child_index_);
    SDL_Point anchor = asset_anchor_world();
    const float base_z = cached_parent_pose_valid_ ? cached_base_world_z_ : resolver.base_world_z();
    // Store ABSOLUTE world position (anchor + offset) so callbacks can correctly convert back to offsets
    selection_state_->world_pos = SDL_FPoint{
        static_cast<float>(anchor.x) + pose.pos.x,
        static_cast<float>(anchor.y) + pose.pos.y
    };
    selection_state_->world_z = pose.z;
    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    SDL_FPoint screen{};
    if (!cam.project_world_point(selection_state_->world_pos, selection_state_->world_z, screen)) {
        screen = cam.map_to_screen_f(selection_state_->world_pos);
    }
    selection_state_->screen_pos = round_point(screen);
    selection_state_->set_anchor_world(anchor, base_z);
}

void SyncChildrenFrameEditor::sync_visibility_checkbox() {
    if (!cb_child_visible_) {
        return;
    }
    // Get the visibility state for the currently selected child and frame
    bool visible = false;
    if (selected_child_index_ >= 0 &&
        selected_child_index_ < static_cast<int>(static_frames_by_child_.size()) &&
        selected_frame_index_ >= 0 && selected_frame_index_ < frame_count_) {
        const auto& timeline = static_frames_by_child_[static_cast<std::size_t>(selected_child_index_)];
        if (selected_frame_index_ < static_cast<int>(timeline.size())) {
            visible = timeline[static_cast<std::size_t>(selected_frame_index_)].visible;
        }
    }
    cb_child_visible_->set_value(visible);
}

bool SyncChildrenFrameEditor::has_static_child_selection() const {
    if (selected_child_index_ < 0 || selected_child_index_ >= static_cast<int>(child_assets_.size())) {
        return false;
    }
    return true;
}

child_timelines::ChildFrameSample* SyncChildrenFrameEditor::selected_sample() {
    if (!has_static_child_selection()) {
        return nullptr;
    }
    if (selected_frame_index_ < 0 || selected_frame_index_ >= frame_count_) {
        return nullptr;
    }
    auto& timeline = static_frames_by_child_[static_cast<std::size_t>(selected_child_index_)];
    if (selected_frame_index_ >= static_cast<int>(timeline.size())) {
        return nullptr;
    }
    return &timeline[static_cast<std::size_t>(selected_frame_index_)];
}

const child_timelines::ChildFrameSample* SyncChildrenFrameEditor::selected_sample() const {
    return const_cast<SyncChildrenFrameEditor*>(this)->selected_sample();
}

void SyncChildrenFrameEditor::copy_current_sample_to_frame(int frame_index) {
    const auto* source = selected_sample();
    if (!source) {
        return;
    }
    if (frame_index < 0 || frame_index >= frame_count_) {
        return;
    }
    auto& timeline = static_frames_by_child_[static_cast<std::size_t>(selected_child_index_)];
    if (frame_index >= static_cast<int>(timeline.size())) {
        return;
    }
    auto& dest = timeline[static_cast<std::size_t>(frame_index)];
    dest = *source;
    dest.child_index = selected_child_index_;
    data_dirty_ = true;
}

void SyncChildrenFrameEditor::apply_current_sample_to_next_frame() {
    if (!has_static_child_selection()) {
        return;
    }
    const int next_frame = selected_frame_index_ + 1;
    if (next_frame >= frame_count_) {
        return;
    }
    copy_current_sample_to_frame(next_frame);
    if (data_dirty_) {
        apply_current_frame_to_children();
        apply_live_changes();
        data_dirty_ = false;
    }
}

void SyncChildrenFrameEditor::apply_current_sample_to_all_frames() {
    if (!has_static_child_selection()) {
        return;
    }
    for (int frame_idx = 0; frame_idx < frame_count_; ++frame_idx) {
        copy_current_sample_to_frame(frame_idx);
    }
    if (data_dirty_) {
        apply_current_frame_to_children();
        apply_live_changes();
        data_dirty_ = false;
    }
}

void SyncChildrenFrameEditor::reset_current_frame() {
    if (selected_frame_index_ < 0 || selected_frame_index_ >= frame_count_) {
        return;
    }

    // Reset all child data for the current frame to defaults (0, 0, 0, visible=true)
    for (std::size_t child_idx = 0; child_idx < static_frames_by_child_.size(); ++child_idx) {
        auto& timeline = static_frames_by_child_[child_idx];
        if (selected_frame_index_ >= static_cast<int>(timeline.size())) {
            continue;
        }
        auto& sample = timeline[static_cast<std::size_t>(selected_frame_index_)];
        sample.child_index = static_cast<int>(child_idx);
        sample.px = 0.0f;
        sample.py = 0.0f;
        sample.pz = 0.0f;
        sample.degree = 0.0f;
        sample.visible = false;
        sample.has_data = true;
    }

    // Apply immediately to keep preview and manifest synchronized
    data_dirty_ = true;
    apply_current_frame_to_children();
    apply_live_changes();
    data_dirty_ = false;

    sync_visibility_checkbox();
    refresh_selection_state();
}

}  // namespace devmode::frame_editors

