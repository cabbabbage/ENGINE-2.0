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
#include "render/warped_screen_grid.hpp"

namespace devmode::frame_editors {

namespace {
SDL_Point round_point(const SDL_FPoint& pt) {
    return SDL_Point{static_cast<int>(std::lround(pt.x)), static_cast<int>(std::lround(pt.y))};
}

int resolve_wheel_steps(const SDL_MouseWheelEvent& wheel) {
    float precise = wheel.preciseY;
    int delta = wheel.y;
    if (wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
        delta = -delta;
        precise = -precise;
    }
    if (std::fabs(precise) >= 0.01f) {
        return static_cast<int>(std::lround(precise));
    }
    return delta;
}
}  // namespace

void SyncChildrenFrameEditor::begin(const FrameEditorContext& context) {
    context_ = context;
    selection_state_ = context.selection_state;
    axis_adjuster_ = context.axis_adjuster;
    if (selection_state_) {
        selection_state_->reset();
    }
    if (axis_adjuster_) {
        axis_adjuster_->reset_axis(AdjustmentAxis::X);
    }
    wants_close_ = false;
    dragging_child_ = false;
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
    ensure_manifest_transaction();
}

void SyncChildrenFrameEditor::end() {
    if (manifest_txn_.active()) {
        manifest_txn_.commit();
    }
    if (selection_state_) {
        selection_state_->reset();
        selection_state_ = nullptr;
    }
    axis_adjuster_ = nullptr;
    btn_back_.reset();
    wants_close_ = false;
}

bool SyncChildrenFrameEditor::handle_event(const SDL_Event& e) {
    if (!context_.assets || !context_.target) {
        return false;
    }
    if (btn_back_ && btn_back_->handle_event(e)) {
        wants_close_ = true;
        return true;
    }
    if (e.type == SDL_MOUSEWHEEL && selection_state_ && selection_state_->target == SelectionTarget::ChildPoint) {
        const int steps = resolve_wheel_steps(e.wheel);
        if (steps != 0) {
            apply_scroll_adjustment(steps);
            return true;
        }
    }
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        SDL_Point mouse{e.button.x, e.button.y};
        if (auto hit = hit_test_child_marker(mouse)) {
            selected_child_index_ = *hit;
            if (selection_state_) {
                selection_state_->target = SelectionTarget::ChildPoint;
                selection_state_->child_index = selected_child_index_;
                selection_state_->screen_pos = mouse;
                selection_state_->world_pos = child_world_position(selected_child_index_);
            }
            if (axis_adjuster_) {
                axis_adjuster_->cycle_axis();
            }
            dragging_child_ = true;
            drag_start_mouse_ = mouse;
            if (selected_child_index_ >= 0 && selected_child_index_ < static_frames_by_child_.size()) {
                const auto& timeline = static_frames_by_child_[selected_child_index_];
                if (selected_frame_index_ < static_cast<int>(timeline.size())) {
                    drag_start_sample_ = timeline[selected_frame_index_];
                } else {
                    drag_start_sample_ = child_timelines::ChildFrameSample{};
                }
            }
            return true;
        } else if (selection_state_) {
            selection_state_->target = SelectionTarget::None;
        }
    } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
        if (dragging_child_) {
            dragging_child_ = false;
            return true;
        }
    } else if (e.type == SDL_MOUSEMOTION) {
        if (dragging_child_ && selected_child_index_ >= 0 && selected_child_index_ < static_cast<int>(child_assets_.size())) {
            float scale = attachment_scale();
            if (scale <= 0.0f) scale = 1.0f;
            const float dx_screen = static_cast<float>(e.motion.x - drag_start_mouse_.x);
            const float dy_screen = static_cast<float>(e.motion.y - drag_start_mouse_.y);
            const bool flipped = context_.target->flipped;
            float dx_world = dx_screen / scale;
            float dy_world = dy_screen / scale;
            child_timelines::ChildFrameSample& sample = static_frames_by_child_[selected_child_index_][selected_frame_index_];
            switch (selection_state_ ? selection_state_->axis : AdjustmentAxis::X) {
                case AdjustmentAxis::X:
                    sample.dx = drag_start_sample_.dx + (flipped ? -dx_world : dx_world);
                    break;
                case AdjustmentAxis::Y:
                    sample.dy = drag_start_sample_.dy + dy_world;
                    break;
                case AdjustmentAxis::Z:
                    sample.dz = drag_start_sample_.dz + dy_world;
                    break;
            }
            sample.has_data = true;
            sample.visible = true;
            data_dirty_ = true;
            return true;
        }
    } else if (e.type == SDL_KEYDOWN) {
        if (e.key.keysym.sym == SDLK_LEFT) {
            selected_frame_index_ = std::max(0, selected_frame_index_ - 1);
            data_dirty_ = true;
            return true;
        }
        if (e.key.keysym.sym == SDLK_RIGHT) {
            selected_frame_index_ = std::min(frame_count_ - 1, selected_frame_index_ + 1);
            data_dirty_ = true;
            return true;
        }
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
        if (manifest_txn_.active()) {
            manifest_txn_.commit();
        }
        data_dirty_ = false;
    }
}

void SyncChildrenFrameEditor::render_world(SDL_Renderer* renderer) const {
    if (!renderer || !context_.target || !context_.assets) return;
    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    SDL_Point anchor = asset_anchor_world();
    const float radius = 6.0f;
    for (std::size_t idx = 0; idx < child_assets_.size(); ++idx) {
        if (idx >= static_frames_by_child_.size()) continue;
        if (idx >= child_modes_.size()) continue;
        if (child_modes_[idx] == AnimationChildMode::Async) {
            continue;
        }
        SDL_FPoint child_pos = child_world_position(static_cast<int>(idx));
        SDL_FPoint world{anchor.x + child_pos.x, anchor.y + child_pos.y};
        SDL_FPoint screen = cam.map_to_screen_f(world);
        SDL_Point center = round_point(screen);
        bool is_selected = static_cast<int>(idx) == selected_child_index_;
        SDL_Rect marker{center.x - static_cast<int>(radius), center.y - static_cast<int>(radius),
                        static_cast<int>(radius * 2), static_cast<int>(radius * 2)};
        SDL_Color color = is_selected ? DMStyles::AccentButton().bg : DMStyles::HeaderButton().bg;
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, 192);
        SDL_RenderFillRect(renderer, &marker);
        SDL_SetRenderDrawColor(renderer, DMStyles::Border().r, DMStyles::Border().g,
                               DMStyles::Border().b, 255);
        SDL_RenderDrawRect(renderer, &marker);
    }
}

void SyncChildrenFrameEditor::render_overlays(SDL_Renderer* renderer) const {
    if (!renderer || !btn_back_) return;
    SDL_Rect rect{DMSpacing::small_gap(), DMSpacing::small_gap(), 80, DMButton::height()};
    btn_back_->set_rect(rect);
    back_rect_ = rect;
    btn_back_->render(renderer);
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
        if (!sample.has_data && !sample.visible) {
            continue;
        }
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

SDL_FPoint SyncChildrenFrameEditor::child_world_position(int child_index) const {
    SDL_FPoint world{0.0f, 0.0f};
    if (child_index < 0 || child_index >= static_cast<int>(static_frames_by_child_.size())) {
        return world;
    }
    if (frame_count_ <= 0) {
        return world;
    }
    int frame_idx = std::clamp(selected_frame_index_, 0, frame_count_ - 1);
    const auto& timeline = static_frames_by_child_[static_cast<std::size_t>(child_index)];
    if (frame_idx >= static_cast<int>(timeline.size())) {
        return world;
    }
    const auto& sample = timeline[static_cast<std::size_t>(frame_idx)];
    const float scale = attachment_scale();
    const float dx = sample.dx * scale;
    const float dy = sample.dy * scale;
    const float world_dx = context_.target && context_.target->flipped ? -dx : dx;
    return SDL_FPoint{world_dx, dy};
}

std::optional<int> SyncChildrenFrameEditor::hit_test_child_marker(const SDL_Point& mouse) const {
    if (!context_.assets || !context_.target) {
        return std::nullopt;
    }
    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    SDL_Point anchor = asset_anchor_world();
    const float radius = 12.0f;
    const float radius_sq = radius * radius;
    for (std::size_t idx = 0; idx < child_assets_.size(); ++idx) {
        if (idx >= child_modes_.size()) continue;
        if (child_modes_[idx] == AnimationChildMode::Async) {
            continue;
        }
        SDL_FPoint world = child_world_position(static_cast<int>(idx));
        SDL_FPoint world_pos{anchor.x + world.x, anchor.y + world.y};
        SDL_FPoint screen = cam.map_to_screen_f(world_pos);
        const float dx = static_cast<float>(mouse.x) - screen.x;
        const float dy = static_cast<float>(mouse.y) - screen.y;
        if ((dx * dx + dy * dy) <= radius_sq) {
            return static_cast<int>(idx);
        }
    }
    return std::nullopt;
}

void SyncChildrenFrameEditor::ensure_manifest_transaction() {
    if (!context_.document) {
        return;
    }
    manifest_txn_.begin(context_);
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

void SyncChildrenFrameEditor::apply_scroll_adjustment(int steps) {
    if (steps == 0 || selected_child_index_ < 0 || selected_child_index_ >= static_cast<int>(static_frames_by_child_.size())) {
        return;
    }
    if (selected_frame_index_ < 0 || selected_frame_index_ >= frame_count_) {
        return;
    }
    auto& timeline = static_frames_by_child_[selected_child_index_];
    if (selected_frame_index_ >= static_cast<int>(timeline.size())) {
        return;
    }
    auto& sample = timeline[selected_frame_index_];
    switch (selection_state_ ? selection_state_->axis : AdjustmentAxis::X) {
        case AdjustmentAxis::X:
            sample.dx += static_cast<float>(steps);
            break;
        case AdjustmentAxis::Y:
            sample.dy += static_cast<float>(steps);
            break;
        case AdjustmentAxis::Z:
            sample.dz += static_cast<float>(steps);
            break;
    }
    sample.visible = true;
    sample.has_data = true;
    data_dirty_ = true;
}

}  // namespace devmode::frame_editors
