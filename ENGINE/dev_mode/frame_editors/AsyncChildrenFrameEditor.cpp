#include "dev_mode/frame_editors/AsyncChildrenFrameEditor.hpp"

#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

#include "asset/Asset.hpp"
#include "asset/animation.hpp"
#include "asset/animation_frame.hpp"
#include "animation_update/animation_update.hpp"
#include "dev_mode/asset_sections/animation_editor_window/AnimationDocument.hpp"
#include "dev_mode/dm_styles.hpp"
#include "dev_mode/widgets.hpp"
#include "dev_mode/frame_editors/shared/SnapUtils.hpp"
#include "dev_mode/frame_editors/shared/FramePointResolver.hpp"

#include "render/warped_screen_grid.hpp"

namespace devmode::frame_editors {

namespace {
SDL_Point round_point(const SDL_FPoint& pt) {
    return SDL_Point{static_cast<int>(std::lround(pt.x)), static_cast<int>(std::lround(pt.y))};
}

constexpr float kFrameInterval = 1.0f / static_cast<float>(kBaseAnimationFps);

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

void AsyncChildrenFrameEditor::begin(const FrameEditorContext& context) {
    context_ = context;
    selection_state_ = context.selection_state;
    point_3d_editor_ = std::make_unique<Point3DEditor>(selection_state_);
    if (selection_state_) {
        selection_state_->reset();
    }
    if (point_3d_editor_) {
        point_3d_editor_->reset_axis(AdjustmentAxis::X);
        point_3d_editor_->set_grid_resolution(context_.snap_resolution);
        // Children use Z as percentage of parent height
        point_3d_editor_->set_z_display_mode(ZDisplayMode::Percentage);
        FramePointResolver resolver(context_.target);
        point_3d_editor_->set_parent_height(resolver.parent_height_px());

        point_3d_editor_->set_on_point_selected([this](int index) {
            if (index < 0) {
                // Deselecting - persist changes before clearing selection state
                if (data_dirty_ && !async_frames_by_child_.empty()) {
                    apply_live_changes();
                }
                if (selection_state_) {
                    selection_state_->reset();
                }
                selected_child_index_ = -1;
            } else {
                // Selecting a child point - guard against invalid state
                if (async_frames_by_child_.empty() || child_assets_.empty()) {
                    return;  // Data not ready
                }
                selected_child_index_ = std::clamp(index, 0, static_cast<int>(child_assets_.size()) - 1);
                if (selected_child_index_ >= 0 &&
                    selected_child_index_ < static_cast<int>(async_has_start_.size())) {
                    async_has_start_[static_cast<std::size_t>(selected_child_index_)] = true;
                }
                selected_child_frame_index_ = std::max(0, mapped_child_frame_index(selected_child_index_));
                if (selection_state_) {
                    selection_state_->target = SelectionTarget::ChildPoint;
                    selection_state_->child_index = selected_child_index_;
                }
                refresh_selection_state();
            }
        });

        point_3d_editor_->set_on_position_changed([this](const SDL_FPoint& new_world_pos, float new_world_z) {
            // Guard: Check all necessary data structures exist and are initialized
            if (selected_child_index_ < 0 ||
                selected_child_index_ >= static_cast<int>(async_frames_by_child_.size()) ||
                async_frames_by_child_.empty()) {
                return;
            }

            float scale = attachment_scale();
            if (scale <= 0.0f) scale = 1.0f;

            SDL_FPoint snapped_world = snap_world_point_to_grid(new_world_pos, context_.snap_resolution);
            float snapped_world_z = snap_world_z_to_grid(new_world_z, context_.snap_resolution);
            SDL_Point anchor = asset_anchor_world();
            const bool flipped = context_.target && context_.target->flipped;
            FramePointResolver resolver(context_.target);

            float dx_world = (snapped_world.x - static_cast<float>(anchor.x)) / scale;
            float dy_world = (snapped_world.y - static_cast<float>(anchor.y)) / scale;
            float dz_percent = resolver.to_percent(snapped_world_z);

            ensure_async_frame_capacity(selected_child_index_, selected_child_frame_index_);

            // Re-check bounds after potential resize
            if (selected_child_index_ >= static_cast<int>(async_frames_by_child_.size())) {
                return;
            }

            auto& sample = async_frames_by_child_[selected_child_index_][selected_child_frame_index_];
            sample.has_data = true;
            sample.visible = true;

            sample.dx = flipped ? -dx_world : dx_world;
            sample.dy = dy_world;
            sample.dz = dz_percent;

            data_dirty_ = true;
            refresh_selection_state();
        });
    }
    wants_close_ = false;
    data_dirty_ = true;
    selected_child_index_ = 0;
    selected_child_frame_index_ = 0;
    selected_parent_frame_index_ = 0;
    if (context.target && context.target->current_frame) {
        selected_parent_frame_index_ = std::max(0, context.target->current_frame->frame_index);
    }
    btn_back_ = std::make_unique<DMButton>("Back", &DMStyles::HeaderButton(), 80, DMButton::height());
    back_rect_ = SDL_Rect{DMSpacing::small_gap(), DMSpacing::small_gap(), 80, DMButton::height()};
    if (btn_back_) {
        btn_back_->set_rect(back_rect_);
    }
    populate_child_data();
    ensure_manifest_transaction();
}

void AsyncChildrenFrameEditor::end() {
    if (selection_state_) {
        selection_state_->reset();
        selection_state_ = nullptr;
    }
    point_3d_editor_ = nullptr;
    btn_back_.reset();
    wants_close_ = false;
}

bool AsyncChildrenFrameEditor::handle_event(const SDL_Event& e) {
    if (!context_.assets || !context_.target) {
        return false;
    }
    SDL_Rect overlay_rect{0, 0, 0, 0};
    bool overlay_valid = false;
    if (point_3d_editor_) {
        // Use the cached container from Point3DEditor (set during render_overlays)
        // This avoids issues with SDL_GetRendererOutputSize(nullptr, ...) failing
        overlay_rect = point_3d_editor_->get_cached_container();
        overlay_valid = (overlay_rect.w > 0 && overlay_rect.h > 0);
        if (point_3d_editor_->handle_event(e, overlay_rect)) {
            return true;
        }
    }
    if (btn_back_ && btn_back_->handle_event(e)) {
        wants_close_ = true;
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

    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    SDL_Point anchor = asset_anchor_world();
    std::vector<SDL_FPoint> point_screens;
    std::vector<bool> point_selectable;

    for (std::size_t idx = 0; idx < child_assets_.size(); ++idx) {
        if (idx >= child_modes_.size() || child_modes_[idx] != AnimationChildMode::Async) {
            continue;
        }
        const ChildWorldPose pose = child_world_pose(static_cast<int>(idx));
        SDL_FPoint world{anchor.x + pose.pos.x, anchor.y + pose.pos.y};
        SDL_FPoint screen{};
        if (!cam.project_world_point(world, pose.z, screen)) {
            screen = cam.map_to_screen_f(world);
        }
        point_screens.push_back(screen);
        // Only currently selected child is selectable
        point_selectable.push_back(static_cast<int>(idx) == selected_child_index_);
    }

    const bool pointer_in_overlay = overlay_valid && SDL_PointInRect(&mouse_pos, &overlay_rect);
    // Only consume event if point editor actually handled it
    if (!pointer_in_overlay) {
        if (point_3d_editor_->handle_mouse_event(e, point_screens, point_selectable)) {
            return true;
        }
    }

    if (e.type == SDL_KEYDOWN) {
        switch (e.key.keysym.sym) {
            // No LEFT/RIGHT arrow keys - use frame navigator buttons only
            case SDLK_TAB:
                selected_child_index_ = (selected_child_index_ + 1) %
                                        std::max(1, static_cast<int>(child_assets_.size()));
                selected_child_frame_index_ = std::max(0, mapped_child_frame_index(selected_child_index_));
                data_dirty_ = true;
                refresh_selection_state();
                return true;
            case SDLK_COMMA:
                selected_child_frame_index_ = std::max(0, selected_child_frame_index_ - 1);
                ensure_async_frame_capacity(selected_child_index_, selected_child_frame_index_);
                data_dirty_ = true;
                refresh_selection_state();
                return true;
            case SDLK_PERIOD:
                selected_child_frame_index_ = std::min(selected_child_frame_index_ + 1,
                                                       std::max(0, parent_frame_count_ - 1));
                ensure_async_frame_capacity(selected_child_index_, selected_child_frame_index_);
                data_dirty_ = true;
                refresh_selection_state();
                return true;
            case SDLK_LEFTBRACKET:
                adjust_start_frame(selected_child_index_, -1);
                return true;
            case SDLK_RIGHTBRACKET:
                adjust_start_frame(selected_child_index_, 1);
                return true;
            default:
                break;
        }
    }
    return false;
}

void AsyncChildrenFrameEditor::update(const Input& /*input*/, float /*dt*/) {
    if (data_dirty_ && context_.target) {
        apply_preview();
        apply_live_changes();
        data_dirty_ = false;
    }
}

void AsyncChildrenFrameEditor::render_world(SDL_Renderer* renderer) const {
    if (!renderer || !context_.target || !context_.assets) return;
    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    SDL_Point anchor = asset_anchor_world();
    for (std::size_t idx = 0; idx < child_assets_.size(); ++idx) {
        if (idx >= child_modes_.size()) continue;
        if (child_modes_[idx] != AnimationChildMode::Async) continue;

        const ChildWorldPose pose = child_world_pose(static_cast<int>(idx));
        SDL_FPoint world{anchor.x + pose.pos.x, anchor.y + pose.pos.y};
        SDL_FPoint screen{};
        if (!cam.project_world_point(world, pose.z, screen)) {
            screen = cam.map_to_screen_f(world);
        }

        const bool is_current_child = (static_cast<int>(idx) == selected_child_index_);
        const bool is_selected = (is_current_child &&
                                 selection_state_ &&
                                 selection_state_->target == SelectionTarget::ChildPoint);
        const bool is_hovered = (static_cast<int>(idx) == point_3d_editor_->get_hovered_point_index());

        if (is_current_child) {
            point_3d_editor_->render_selectable_point(renderer, screen, is_selected, is_hovered);
        } else {
            point_3d_editor_->render_non_selectable_point(renderer, screen);
        }
    }
 }

void AsyncChildrenFrameEditor::render_overlays(SDL_Renderer* renderer) const {
    if (!renderer || !btn_back_) return;
    SDL_Rect rect{DMSpacing::small_gap(), DMSpacing::small_gap(), 80, DMButton::height()};
    btn_back_->set_rect(rect);
    back_rect_ = rect;
    btn_back_->render(renderer);
}

void AsyncChildrenFrameEditor::populate_child_data() {
    child_assets_.clear();
    child_modes_.clear();
    static_frames_by_child_.clear();
    async_frames_by_child_.clear();
    async_start_times_.clear();
    async_start_frames_.clear();
    async_has_start_.clear();
    parent_frame_count_ = 0;
    if (!context_.target || !context_.document) {
        return;
    }

    child_assets_ = context_.document->animation_children();
    child_modes_.assign(child_assets_.size(), AnimationChildMode::Static);
    static_frames_by_child_.assign(child_assets_.size(), std::vector<child_timelines::ChildFrameSample>{});
    async_frames_by_child_.assign(child_assets_.size(), std::vector<child_timelines::ChildFrameSample>{});
    async_start_times_.assign(child_assets_.size(), 0.0f);
    async_start_frames_.assign(child_assets_.size(), 0);
    async_has_start_.assign(child_assets_.size(), false);

    if (!context_.target->info) {
        return;
    }
    auto it = context_.target->info->animations.find(context_.animation_id);
    if (it == context_.target->info->animations.end()) {
        return;
    }
    const Animation& animation = it->second;
    const auto& path = animation.movement_path(animation.default_movement_path_index());
    parent_frame_count_ = static_cast<int>(path.size());
    if (parent_frame_count_ <= 0) {
        parent_frame_count_ = 1;
    }
    for (auto& timeline : static_frames_by_child_) {
        timeline.assign(parent_frame_count_, child_timelines::ChildFrameSample{});
    }
    for (std::size_t child_idx = 0; child_idx < child_assets_.size(); ++child_idx) {
        for (int frame_idx = 0; frame_idx < parent_frame_count_; ++frame_idx) {
            auto& sample = static_frames_by_child_[child_idx][frame_idx];
            sample.child_index = static_cast<int>(child_idx);
            sample.visible = false;
        }
    }

    for (int frame_idx = 0; frame_idx < parent_frame_count_; ++frame_idx) {
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
            sample.dx = child_src.offset.px;
            sample.dy = child_src.offset.py;
            sample.dz = child_src.offset.pz;
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
                auto& timeline = async_frames_by_child_[idx];
                timeline.clear();
                for (const auto& frame : descriptor.frames) {
                    child_timelines::ChildFrameSample sample{};
                sample.child_index = frame.child_index;
                sample.dx = frame.offset.px;
                sample.dy = frame.offset.py;
                sample.dz = frame.offset.pz;
                sample.degree = frame.degree;
                sample.visible = frame.visible;
                sample.has_data = true;
                timeline.push_back(sample);
            }
            if (descriptor.has_start_time || descriptor.start_frame > 0 || descriptor.start_time > 0.0f || descriptor.auto_start) {
                async_has_start_[idx] = true;
                async_start_frames_[idx] = std::max(0, descriptor.start_frame);
                float derived_start = descriptor.start_time;
                if (derived_start <= 0.0f && descriptor.start_frame > 0) {
                    derived_start = static_cast<float>(descriptor.start_frame) * kFrameInterval;
                }
                async_start_times_[idx] = derived_start;
            }
            if (timeline.empty()) {
                child_timelines::ChildFrameSample fallback{};
                fallback.child_index = static_cast<int>(idx);
                fallback.visible = false;
                timeline.push_back(fallback);
            }
        }
    }

    // Prefer explicit payload metadata for async start offsets if present.
    if (auto payload_opt = context_.document->animation_payload_json(context_.animation_id)) {
        const nlohmann::json& payload = *payload_opt;
        auto timelines_json = payload.find("child_timelines");
        if (timelines_json != payload.end() && timelines_json->is_array()) {
            std::unordered_map<std::string, int> index_by_asset;
            for (std::size_t i = 0; i < child_assets_.size(); ++i) {
                index_by_asset.emplace(child_assets_[i], static_cast<int>(i));
            }
            for (const auto& entry : *timelines_json) {
                if (!entry.is_object()) continue;
                std::string asset = entry.value("asset", std::string{});
                int idx = entry.value("child", entry.value("child_index", -1));
                if (idx < 0 && !asset.empty()) {
                    auto it_idx = index_by_asset.find(asset);
                    if (it_idx != index_by_asset.end()) {
                        idx = it_idx->second;
                    }
                }
                if (idx < 0 || idx >= static_cast<int>(child_assets_.size())) continue;
                if (child_timelines::timeline_entry_is_static(entry)) {
                    continue;
                }
                int start_frame = 0;
                float start_time = 0.0f;
                bool has_start = false;
                if (entry.contains("start_frame")) {
                    start_frame = entry["start_frame"].is_number_integer()
                                      ? entry["start_frame"].get<int>()
                                      : 0;
                    has_start = true;
                } else if (entry.contains("start")) {
                    start_frame = entry["start"].is_number_integer() ? entry["start"].get<int>() : 0;
                    has_start = true;
                }
                if (entry.contains("start_time")) {
                    try {
                        start_time = static_cast<float>(entry["start_time"].get<double>());
                        has_start = true;
                    } catch (...) {
                    }
                }
                if (has_start) {
                    async_has_start_[static_cast<std::size_t>(idx)] = true;
                    if (start_frame < 0) start_frame = 0;
                    async_start_frames_[static_cast<std::size_t>(idx)] = start_frame;
                    if (start_time <= 0.0f && start_frame > 0) {
                        start_time = static_cast<float>(start_frame) * kFrameInterval;
                    }
                    async_start_times_[static_cast<std::size_t>(idx)] = start_time;
                } else if (entry.value("auto_start", entry.value("autostart", false))) {
                    async_has_start_[static_cast<std::size_t>(idx)] = true;
                    async_start_frames_[static_cast<std::size_t>(idx)] = 0;
                    async_start_times_[static_cast<std::size_t>(idx)] = 0.0f;
                }
            }
        }
    }

    if (selected_parent_frame_index_ >= parent_frame_count_) {
        selected_parent_frame_index_ = std::max(0, parent_frame_count_ - 1);
    }
    data_dirty_ = true;
}

void AsyncChildrenFrameEditor::ensure_manifest_transaction() {
    if (!context_.document) {
        return;
    }
    manifest_txn_.begin(context_);
    manifest_txn_.set_deferred_persist(true);
    manifest_txn_.set_apply_callback([this]() -> bool {
        if (!context_.document) {
            return false;
        }
        // normalize start times from frames when missing
        std::vector<float> start_times = async_start_times_;
        if (start_times.size() < async_start_frames_.size()) {
            start_times.resize(async_start_frames_.size(), 0.0f);
        }
        for (std::size_t i = 0; i < start_times.size(); ++i) {
            if (start_times[i] <= 0.0f && i < async_start_frames_.size() && async_start_frames_[i] > 0) {
                start_times[i] = static_cast<float>(async_start_frames_[i]) * kFrameInterval;
            }
        }
        const auto payload_opt = context_.document->animation_payload_json(context_.animation_id);
        nlohmann::json payload = payload_opt.value_or(nlohmann::json::object());
        payload["child_timelines"] = child_timelines::build_child_timelines_payload(
            payload,
            static_frames_by_child_,
            child_assets_,
            child_modes_,
            async_frames_by_child_,
            start_times,
            async_has_start_);
        return context_.document->update_animation_payload(context_.animation_id, payload);
    });
}

void AsyncChildrenFrameEditor::apply_live_changes() {
    if (manifest_txn_.active()) {
        manifest_txn_.commit(false);
    }
    invalidate_preview();
}

void AsyncChildrenFrameEditor::persist_pending_changes() {
    if (!manifest_txn_.active()) {
        return;
    }
    manifest_txn_.commit(true);
    invalidate_preview();
}

void AsyncChildrenFrameEditor::invalidate_preview() const {
    if (context_.preview && !context_.animation_id.empty()) {
        context_.preview->invalidate(context_.animation_id);
    }
}

float AsyncChildrenFrameEditor::attachment_scale() const {
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

SDL_Point AsyncChildrenFrameEditor::asset_anchor_world() const {
    if (!context_.target) {
        return SDL_Point{0, 0};
    }
    return animation_update::detail::bottom_middle_for(*context_.target, context_.target->pos);
}

AsyncChildrenFrameEditor::ChildWorldPose AsyncChildrenFrameEditor::child_world_pose(int child_index) const {
    ChildWorldPose pose{};
    auto sample = sample_for_child(child_index, true);
    if (!context_.target) {
        return pose;
    }
    FramePointResolver resolver(context_.target);
    const float scale = attachment_scale();
    const float dx = sample.dx * scale;
    const float dy = sample.dy * scale;
    const float world_dx = context_.target->flipped ? -dx : dx;
    pose.pos = SDL_FPoint{world_dx, dy};
    pose.z = resolver.to_world_z(sample.dz);
    return pose;
}

std::optional<int> AsyncChildrenFrameEditor::hit_test_child_marker(const SDL_Point& mouse) const {
    if (!context_.assets || !context_.target) {
        return std::nullopt;
    }
    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    SDL_Point anchor = asset_anchor_world();
    const float radius = 12.0f;
    const float radius_sq = radius * radius;
    for (std::size_t idx = 0; idx < child_assets_.size(); ++idx) {
        if (idx >= child_modes_.size()) continue;
        if (child_modes_[idx] != AnimationChildMode::Async) continue;
        const ChildWorldPose pose = child_world_pose(static_cast<int>(idx));
        SDL_FPoint world{anchor.x + pose.pos.x, anchor.y + pose.pos.y};
        SDL_FPoint screen{};
        if (!cam.project_world_point(world, pose.z, screen)) {
            screen = cam.map_to_screen_f(world);
        }
        const float dx = static_cast<float>(mouse.x) - screen.x;
        const float dy = static_cast<float>(mouse.y) - screen.y;
        if ((dx * dx + dy * dy) <= radius_sq) {
            return static_cast<int>(idx);
        }
    }
    return std::nullopt;
}

child_timelines::ChildFrameSample AsyncChildrenFrameEditor::sample_for_child(int child_index, bool for_preview) const {
    if (child_index < 0 || child_index >= static_cast<int>(child_modes_.size())) {
        return child_timelines::ChildFrameSample{};
    }
    if (child_modes_[child_index] != AnimationChildMode::Async) {
        if (child_index < static_cast<int>(static_frames_by_child_.size())) {
            const auto& timeline = static_frames_by_child_[static_cast<std::size_t>(child_index)];
            int frame_idx = std::clamp(selected_parent_frame_index_, 0, std::max(0, parent_frame_count_ - 1));
            if (frame_idx >= 0 && frame_idx < static_cast<int>(timeline.size())) {
                return timeline[static_cast<std::size_t>(frame_idx)];
            }
        }
        return child_timelines::ChildFrameSample{};
    }

    const int mapped_idx = mapped_child_frame_index(child_index);
    if (mapped_idx < 0) {
        return default_sample(child_index);
    }
    const auto& timeline = async_frames_by_child_[static_cast<std::size_t>(child_index)];
    if (mapped_idx >= static_cast<int>(timeline.size())) {
        return for_preview ? child_timelines::ChildFrameSample{} : default_sample(child_index);
    }
    return timeline[static_cast<std::size_t>(mapped_idx)];
}

child_timelines::ChildFrameSample AsyncChildrenFrameEditor::default_sample(int child_index) const {
    child_timelines::ChildFrameSample sample{};
    sample.child_index = child_index;
    sample.visible = false;
    sample.has_data = false;
    return sample;
}

int AsyncChildrenFrameEditor::mapped_child_frame_index(int child_index) const {
    if (child_index < 0 || child_index >= static_cast<int>(child_modes_.size())) {
        return -1;
    }
    if (child_modes_[child_index] != AnimationChildMode::Async) {
        return selected_parent_frame_index_;
    }
    if (child_index >= static_cast<int>(async_has_start_.size()) ||
        !async_has_start_[static_cast<std::size_t>(child_index)]) {
        return -1;
    }
    const float start_time = start_time_for_child(child_index);
    const float parent_time = static_cast<float>(selected_parent_frame_index_) * kFrameInterval;
    const float elapsed = parent_time - start_time;
    if (elapsed < 0.0f) {
        return -1;
    }
    const int idx = static_cast<int>(std::floor(elapsed * static_cast<float>(kBaseAnimationFps) + 1e-4f));
    return std::max(0, idx);
}

void AsyncChildrenFrameEditor::ensure_async_frame_capacity(int child_index, int frame_index) {
    if (child_index < 0 || child_index >= static_cast<int>(async_frames_by_child_.size())) {
        return;
    }
    auto& timeline = async_frames_by_child_[static_cast<std::size_t>(child_index)];
    if (frame_index < 0) return;
    if (static_cast<std::size_t>(frame_index) >= timeline.size()) {
        const std::size_t desired = static_cast<std::size_t>(frame_index + 1);
        const std::size_t current = timeline.size();
        timeline.resize(desired);
        for (std::size_t i = current; i < timeline.size(); ++i) {
            timeline[i] = default_sample(child_index);
            timeline[i].child_index = child_index;
        }
    }
}

float AsyncChildrenFrameEditor::start_time_for_child(int child_index) const {
    if (child_index < 0 || child_index >= static_cast<int>(child_assets_.size())) {
        return 0.0f;
    }
    float start_time = (child_index < static_cast<int>(async_start_times_.size()))
                           ? async_start_times_[static_cast<std::size_t>(child_index)]
                           : 0.0f;
    if (start_time <= 0.0f && child_index < static_cast<int>(async_start_frames_.size()) &&
        async_start_frames_[static_cast<std::size_t>(child_index)] > 0) {
        start_time = static_cast<float>(async_start_frames_[static_cast<std::size_t>(child_index)]) * kFrameInterval;
    }
    return start_time;
}

int AsyncChildrenFrameEditor::start_frame_for_child(int child_index) const {
    if (child_index < 0 || child_index >= static_cast<int>(child_assets_.size())) {
        return 0;
    }
    int start_frame = (child_index < static_cast<int>(async_start_frames_.size()))
                          ? async_start_frames_[static_cast<std::size_t>(child_index)]
                          : 0;
    if (start_frame <= 0) {
        const float start_time = start_time_for_child(child_index);
        if (start_time > 0.0f) {
            start_frame = static_cast<int>(std::lround(start_time / kFrameInterval));
        }
    }
    return std::max(0, start_frame);
}

void AsyncChildrenFrameEditor::adjust_start_frame(int child_index, int delta_frames) {
    if (child_index < 0 || child_index >= static_cast<int>(child_assets_.size())) {
        return;
    }
    if (async_start_frames_.size() < child_assets_.size()) {
        async_start_frames_.resize(child_assets_.size(), 0);
    }
    if (async_start_times_.size() < child_assets_.size()) {
        async_start_times_.resize(child_assets_.size(), 0.0f);
    }
    async_has_start_[static_cast<std::size_t>(child_index)] = true;
    int next = std::max(0, start_frame_for_child(child_index) + delta_frames);
    async_start_frames_[static_cast<std::size_t>(child_index)] = next;
    async_start_times_[static_cast<std::size_t>(child_index)] = static_cast<float>(next) * kFrameInterval;
    data_dirty_ = true;
}

void AsyncChildrenFrameEditor::refresh_selection_state() {
    if (!selection_state_ || !context_.target || !context_.assets ||
        selection_state_->target != SelectionTarget::ChildPoint) {
        return;
    }
    FramePointResolver resolver(context_.target);
    // Update parent height for Z percent display (in case scale changed)
    if (point_3d_editor_) {
        point_3d_editor_->set_parent_height(resolver.parent_height_px());
    }
    selection_state_->child_index = selected_child_index_;
    const ChildWorldPose pose = child_world_pose(selected_child_index_);
    SDL_Point anchor = asset_anchor_world();
    const WarpedScreenGrid& cam = context_.camera ? *context_.camera : context_.assets->getView();
    SDL_FPoint world{anchor.x + pose.pos.x, anchor.y + pose.pos.y};
    const float base_z = resolver.base_world_z();
    selection_state_->world_pos = world;
    selection_state_->world_z = pose.z;
    SDL_FPoint screen{};
    if (!cam.project_world_point(world, pose.z, screen)) {
        screen = cam.map_to_screen_f(world);
    }
    selection_state_->screen_pos = round_point(screen);
    selection_state_->set_anchor_world(anchor, base_z);
}

void AsyncChildrenFrameEditor::apply_preview() {
    if (!context_.target) return;
    auto& slots = const_cast<std::vector<Asset::AnimationChildAttachment>&>(context_.target->animation_children());
    if (slots.empty()) return;
    SDL_Point render_pos{
        static_cast<int>(std::lround(context_.target->smoothed_translation_x())),
        static_cast<int>(std::lround(context_.target->smoothed_translation_y()))
    };
    FramePointResolver resolver(context_.target);
    animation_update::child_attachments::ParentState parent_state{};
    parent_state.position = render_pos;
    parent_state.base_position = animation_update::detail::bottom_middle_for(*context_.target, render_pos);
    parent_state.scale = context_.target->smoothed_scale();
    parent_state.flipped = context_.target->flipped;
    parent_state.world_z = resolver.base_world_z();
    parent_state.height = resolver.parent_height_px();
    parent_state.animation_id = context_.animation_id;
    std::vector<AnimationChildFrameData> overrides;
    for (std::size_t idx = 0; idx < child_assets_.size(); ++idx) {
        if (idx >= child_modes_.size()) continue;
        const auto sample = sample_for_child(static_cast<int>(idx), true);
        if (!sample.has_data && !sample.visible) {
            continue;
        }
        AnimationChildFrameData entry{};
        entry.child_index = static_cast<int>(idx);
        // Store percentages - runtime conversion happens in apply_frame_data
        entry.offset.px = sample.dx;
        entry.offset.py = sample.dy;
        entry.offset.pz = sample.dz;
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

}  // namespace devmode::frame_editors
