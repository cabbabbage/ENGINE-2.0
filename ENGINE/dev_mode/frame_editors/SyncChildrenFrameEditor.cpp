#include "SyncChildrenFrameEditor.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <vector>

#include "asset/Asset.hpp"
#include "asset/animation.hpp"
#include "asset/animation_frame.hpp"
#include "dev_mode/asset_sections/animation_editor_window/AnimationDocument.hpp"
#include "dev_mode/dm_styles.hpp"
#include "dev_mode/widgets.hpp"
#include "dev_mode/frame_editors/shared/SnapUtils.hpp"
#include "render/warped_screen_grid.hpp"
#include "render/composite_asset_renderer.hpp"
#include <SDL_image.h>

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
    if (point_3d_editor_) {
        point_3d_editor_->reset_axis(AdjustmentAxis::X);
        point_3d_editor_->set_grid_resolution(context_.snap_resolution);

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

            float dx_world = (snapped_world.x - static_cast<float>(anchor.x)) / scale;
            float dy_world = (snapped_world.y - static_cast<float>(anchor.y)) / scale;
            float dz_world = (snapped_world_z - (context_.target ? context_.target->world_z_offset() : 0.0f)) / scale;

            auto& sample = const_cast<std::vector<child_timelines::ChildFrameSample>&>(child_frames)[selected_frame_index_];
            sample.dx = flipped ? -dx_world : dx_world;
            sample.dy = dy_world;
            sample.dz = dz_world;
            sample.has_data = true;
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

            float dx_world = (snapped_world.x - static_cast<float>(anchor.x)) / scale;
            float dy_world = (snapped_world.y - static_cast<float>(anchor.y)) / scale;
            float dz_world = (snapped_world_z - (context_.target ? context_.target->world_z_offset() : 0.0f)) / scale;

            auto& sample = const_cast<std::vector<child_timelines::ChildFrameSample>&>(child_frames)[selected_frame_index_];
            sample.dx = flipped ? -dx_world : dx_world;
            sample.dy = dy_world;
            sample.dz = dz_world;
            sample.has_data = true;
            sample.visible = true;

            data_dirty_ = true;
        });
    }
    wants_close_ = false;
    data_dirty_ = true;
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
    frame_navigator_ = std::make_unique<FrameNavigator>();
    frame_navigator_->set_frame_count(frame_count_);
    frame_navigator_->set_current_frame(selected_frame_index_);
    frame_navigator_->set_on_frame_changed([this](int frame) {
        // Save changes before changing frames
        persist_pending_changes();
        selected_frame_index_ = frame;
        data_dirty_ = true;
        sync_visibility_checkbox();
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

    ensure_manifest_transaction();
    // Initial sync of visibility checkbox with current frame/child state
    sync_visibility_checkbox();
}

void SyncChildrenFrameEditor::end() {
    if (selection_state_) {
        selection_state_->reset();
        selection_state_ = nullptr;
    }
    point_3d_editor_ = nullptr;
    btn_back_.reset();
    frame_navigator_.reset();
    dd_child_selector_.reset();
    cb_child_visible_.reset();
    wants_close_ = false;
}

bool SyncChildrenFrameEditor::handle_event(const SDL_Event& e) {
    // Handle Point3DEditor events first
    SDL_Rect overlay_rect{0, 0, 0, 0};
    bool overlay_valid = false;
    if (point_3d_editor_) {
        int sw = 0, sh = 0;
        SDL_GetRendererOutputSize(nullptr, &sw, &sh);
        int height = point_3d_editor_->get_overlay_height(sw);
        if (height > 0) {
            overlay_rect = SDL_Rect{0, sh - height, sw, height};
            overlay_valid = true;
        }
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

    SDL_Point mouse_pos = {0, 0};
    if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
        mouse_pos = {e.button.x, e.button.y};
    } else if (e.type == SDL_MOUSEMOTION) {
        mouse_pos = {e.motion.x, e.motion.y};
    } else if (e.type == SDL_MOUSEWHEEL) {
        SDL_GetMouseState(&mouse_pos.x, &mouse_pos.y);
    }

    const bool pointer_in_overlay = overlay_valid && SDL_PointInRect(&mouse_pos, &overlay_rect);
    if (SDL_PointInRect(&mouse_pos, &back_rect_) == SDL_FALSE &&
        SDL_PointInRect(&mouse_pos, &ui_rect_) == SDL_FALSE &&
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

    if (e.type == SDL_KEYDOWN) {
        // No LEFT/RIGHT arrow keys - use frame navigator buttons only
        if (e.key.keysym.sym == SDLK_TAB) {
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

    // Render child assets - sync their positions from slot data first
    const auto& slots = context_.target->animation_children();
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const auto& slot = slots[i];
        if (!slot.visible) continue;
        Asset* child = context_.assets->find_child_timeline_asset(context_.target, static_cast<int>(i));
        if (!child) continue;
        // Explicitly sync child asset position from slot data during frame editing
        // This ensures the child renders at the correct position even without calling Asset::update()
        child->pos = slot.world_pos;
        child->set_world_z_offset(slot.world_z);
        child->mark_composite_dirty();
        CompositeAssetRenderer composite_renderer(renderer, context_.assets);
        composite_renderer.update(child, 0.0f);
    }

    // Render axis points
    SDL_Point anchor = asset_anchor_world();
    for (std::size_t idx = 0; idx < child_assets_.size(); ++idx) {
        if (idx >= static_frames_by_child_.size()) continue;
        if (idx >= child_modes_.size()) continue;
        if (child_modes_[idx] == AnimationChildMode::Async) {
            continue;
        }
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

        if (is_current_child) {
            point_3d_editor_->render_selectable_point(renderer, screen, is_selected, is_hovered);
        } else {
            point_3d_editor_->render_non_selectable_point(renderer, screen);
        }
    }
}

void SyncChildrenFrameEditor::render_overlays(SDL_Renderer* renderer) const {
    if (!renderer) return;

    // Layout the UI elements
    int sw = 0, sh = 0;
    SDL_GetRendererOutputSize(renderer, &sw, &sh);
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

    ui_rect_.h = cursor_y - y;

    // Render elements
    if (btn_back_) btn_back_->render(renderer);
    if (frame_navigator_) frame_navigator_->render(renderer);
    if (dd_child_selector_) dd_child_selector_->render(renderer);
    if (cb_child_visible_) cb_child_visible_->render(renderer);

    // Render Point3DEditor overlays at the bottom
    if (point_3d_editor_) {
        int sw = 0, sh = 0;
        SDL_GetRendererOutputSize(renderer, &sw, &sh);
        int height = point_3d_editor_->get_overlay_height(sw);
        SDL_Rect bottom_container{0, sh - height, sw, height};
        point_3d_editor_->render_overlays(renderer, bottom_container);
    }
}

void SyncChildrenFrameEditor::populate_child_data() {
    child_assets_.clear();
    child_modes_.clear();
    static_frames_by_child_.clear();
    async_timelines_by_child_.clear();
    frame_count_ = 0;
    if (!context_.target || !context_.document) {
        return;
    }
    child_assets_ = context_.document->animation_children();
    child_modes_.assign(child_assets_.size(), AnimationChildMode::Static);
    static_frames_by_child_.assign(child_assets_.size(), std::vector<child_timelines::ChildFrameSample>{});
    async_timelines_by_child_.assign(child_assets_.size(), std::vector<child_timelines::ChildFrameSample>{});

    if (!context_.target->info) {
        return;
    }
    auto it = context_.target->info->animations.find(context_.animation_id);
    if (it == context_.target->info->animations.end()) {
        return;
    }
    const Animation& animation = it->second;
    const auto& path = animation.movement_path(animation.default_movement_path_index());
    frame_count_ = static_cast<int>(path.size());
    if (frame_count_ <= 0) {
        frame_count_ = 1;
    }
    for (auto& timeline : static_frames_by_child_) {
        timeline.assign(frame_count_, child_timelines::ChildFrameSample{});
    }
    for (std::size_t child_idx = 0; child_idx < child_assets_.size(); ++child_idx) {
        for (int frame_idx = 0; frame_idx < frame_count_; ++frame_idx) {
            auto& sample = static_frames_by_child_[child_idx][frame_idx];
            sample.child_index = static_cast<int>(child_idx);
            sample.visible = false;
        }
    }

    for (int frame_idx = 0; frame_idx < frame_count_; ++frame_idx) {
        if (frame_idx >= static_cast<int>(path.size())) break;
        const AnimationFrame& src = path[frame_idx];
        for (const auto& child_src : src.children) {
            if (child_src.child_index < 0 ||
                child_src.child_index >= static_cast<int>(child_assets_.size())) {
                continue;
            }
            if (child_modes_[child_src.child_index] == AnimationChildMode::Async) {
                continue;
            }
            auto& sample = static_frames_by_child_[static_cast<std::size_t>(child_src.child_index)][frame_idx];
            sample.child_index = child_src.child_index;
            sample.dx = static_cast<float>(child_src.dx);
            sample.dy = static_cast<float>(child_src.dy);
            sample.dz = static_cast<float>(child_src.dz);
            sample.degree = child_src.degree;
            sample.visible = child_src.visible;
            sample.has_data = true;
        }
    }

    const auto& timelines = animation.child_timelines();
    for (std::size_t child_idx = 0; child_idx < timelines.size(); ++child_idx) {
        const auto& descriptor = timelines[child_idx];
        const std::size_t idx = child_idx;
        if (idx >= child_assets_.size()) continue;
        child_modes_[idx] = descriptor.mode;
        if (descriptor.mode == AnimationChildMode::Async) {
            auto& timeline = async_timelines_by_child_[idx];
            timeline.clear();
            for (const auto& frame : descriptor.frames) {
                child_timelines::ChildFrameSample sample{};
                sample.child_index = frame.child_index;
                sample.dx = static_cast<float>(frame.dx);
                sample.dy = static_cast<float>(frame.dy);
                sample.dz = static_cast<float>(frame.dz);
                sample.degree = frame.degree;
                sample.visible = frame.visible;
                sample.has_data = true;
                timeline.push_back(sample);
            }
        }
    }
    data_dirty_ = true;
}

void SyncChildrenFrameEditor::apply_current_frame_to_children() {
    if (!context_.target) return;
    auto& slots = const_cast<std::vector<Asset::AnimationChildAttachment>&>(context_.target->animation_children());
    if (slots.empty()) return;
    SDL_Point render_pos{
        static_cast<int>(std::lround(context_.target->smoothed_translation_x())),
        static_cast<int>(std::lround(context_.target->smoothed_translation_y()))
    };
    animation_update::child_attachments::ParentState parent_state{};
    parent_state.position = render_pos;
    parent_state.base_position = animation_update::detail::bottom_middle_for(*context_.target, render_pos);
    parent_state.scale = context_.target->smoothed_scale();
    parent_state.flipped = context_.target->flipped;
    parent_state.world_z = context_.target->world_z_offset();
    parent_state.animation_id = context_.animation_id;

    std::vector<AnimationChildFrameData> overrides;
    for (std::size_t idx = 0; idx < child_assets_.size(); ++idx) {
        if (idx >= static_frames_by_child_.size()) continue;
        if (idx >= child_modes_.size()) continue;
        if (child_modes_[idx] == AnimationChildMode::Async) continue;
        if (selected_frame_index_ < 0 || selected_frame_index_ >= frame_count_) continue;
        const auto& timeline = static_frames_by_child_[idx];
        if (selected_frame_index_ >= static_cast<int>(timeline.size())) continue;
        const auto& sample = timeline[selected_frame_index_];
        // Always include all children in overrides - do not skip any.
        // apply_frame_data() sets all slots to visible=false first, and only
        // slots in the override list get updated. Skipping children here would
        // leave them invisible even if they should be visible.
        AnimationChildFrameData entry{};
        entry.child_index = static_cast<int>(idx);
        entry.dx = static_cast<int>(std::lround(sample.dx));
        entry.dy = static_cast<int>(std::lround(sample.dy));
        entry.dz = static_cast<int>(std::lround(sample.dz));
        entry.degree = sample.degree;
        entry.visible = sample.visible;
        overrides.push_back(entry);
    }
    const AnimationFrame* current_frame = context_.target->current_frame;
    animation_update::child_attachments::apply_frame_data(slots,
                                                         parent_state,
                                                         current_frame,
                                                         overrides.empty() ? nullptr : &overrides);
    context_.target->mark_composite_dirty();
}

float SyncChildrenFrameEditor::attachment_scale() const {
    if (!context_.assets || !context_.target) {
        return 1.0f;
    }
    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    float perspective_scale = 1.0f;
    if (const auto* gp = cam.grid_point_for_asset(context_.target)) {
        perspective_scale = std::max(0.0001f, gp->perspective_scale);
    }
    float remainder = context_.target->current_remaining_scale_adjustment;
    if (!std::isfinite(remainder) || remainder <= 0.0f) {
        remainder = 1.0f;
    }
    float scale = remainder / std::max(0.0001f, perspective_scale);
    if (!std::isfinite(scale) || scale <= 0.0f) {
        scale = 1.0f;
    }
    return scale;
}

SDL_Point SyncChildrenFrameEditor::asset_anchor_world() const {
    if (!context_.target) {
        return SDL_Point{0, 0};
    }
    return animation_update::detail::bottom_middle_for(*context_.target, context_.target->pos);
}

SyncChildrenFrameEditor::ChildWorldPose SyncChildrenFrameEditor::child_world_pose(int child_index) const {
    ChildWorldPose pose{};
    if (child_index < 0 || child_index >= static_cast<int>(static_frames_by_child_.size())) {
        return pose;
    }
    if (frame_count_ <= 0) {
        return pose;
    }
    int frame_idx = std::clamp(selected_frame_index_, 0, frame_count_ - 1);
    const auto& timeline = static_frames_by_child_[static_cast<std::size_t>(child_index)];
    if (frame_idx >= static_cast<int>(timeline.size())) {
        return pose;
    }
    const auto& sample = timeline[static_cast<std::size_t>(frame_idx)];
    const float scale = attachment_scale();
    const float dx = sample.dx * scale;
    const float dy = sample.dy * scale;
    const float dz = sample.dz * scale;
    const float world_dx = context_.target && context_.target->flipped ? -dx : dx;
    pose.pos = SDL_FPoint{world_dx, dy};
    pose.z = (context_.target ? context_.target->world_z_offset() : 0.0f) + dz;
    return pose;
}

std::vector<int> SyncChildrenFrameEditor::static_child_point_indices() const {
    std::vector<int> indices;
    indices.reserve(child_assets_.size());
    for (std::size_t idx = 0; idx < child_assets_.size(); ++idx) {
        if (idx < child_modes_.size() && child_modes_[idx] == AnimationChildMode::Async) {
            continue;
        }
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
    manifest_txn_.begin(context_);
    manifest_txn_.set_deferred_persist(true);
    manifest_txn_.set_apply_callback([this]() -> bool {
        if (!context_.document) {
            return false;
        }
        const auto payload_opt = context_.document->animation_payload_json(context_.animation_id);
        nlohmann::json payload = payload_opt.value_or(nlohmann::json::object());
        payload["child_timelines"] = child_timelines::build_child_timelines_payload(
            payload,
            static_frames_by_child_,
            child_assets_,
            child_modes_,
            async_timelines_by_child_);
        return context_.document->update_animation_payload(context_.animation_id, payload);
    });
}

void SyncChildrenFrameEditor::apply_live_changes() {
    if (manifest_txn_.active()) {
        manifest_txn_.commit(false);
    }
    invalidate_preview();
}

void SyncChildrenFrameEditor::persist_pending_changes() {
    if (!manifest_txn_.active()) {
        return;
    }
    manifest_txn_.commit(true);
    invalidate_preview();
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
    selection_state_->child_index = selected_child_index_;
    const ChildWorldPose pose = child_world_pose(selected_child_index_);
    SDL_Point anchor = asset_anchor_world();
    const float base_z = context_.target ? context_.target->world_z_offset() : 0.0f;
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

}  // namespace devmode::frame_editors
