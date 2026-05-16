#include "child_asset.hpp"

#include "animation/controllers/shared/anchored_child_placement.hpp"
#include "animation/controllers/shared/oval_anchor_heading.hpp"
#include "animation/animation_update.hpp"
#include "assets/asset/Asset.hpp"
#include "animation/controllers/shared/anchor_bound_asset_helper.hpp"
#include "core/AssetsManager.hpp"
#include "rendering/render/render_depth_policy.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "utils/log.hpp"
#include "utils/string_utils.hpp"

#include <cmath>
#include <optional>
#include <unordered_set>
#include <utility>

namespace {

std::vector<std::string> normalize_tag_list(const std::vector<std::string>& values) {
    std::vector<std::string> normalized;
    normalized.reserve(values.size());
    std::unordered_set<std::string> seen;
    seen.reserve(values.size());
    for (const std::string& raw : values) {
        std::string token = vibble::strings::to_lower_copy(vibble::strings::trim_copy(raw));
        if (token.empty()) {
            continue;
        }
        if (seen.insert(token).second) {
            normalized.push_back(std::move(token));
        }
    }
    return normalized;
}

double desired_render_depth_bias(const AnchorPoint& anchor, int quantized_world_z) {
    return render_depth::bias_for_quantized_depth(anchor.world_depth,
                                                  static_cast<double>(quantized_world_z));
}

anchor_points::AnchorWorldPoint3 child_anchor_world_displacement(const AnchorPoint& parent_anchor,
                                                                  Asset* owner_asset,
                                                                  const Asset* child_asset) {
    anchor_points::AnchorWorldPoint3 displacement{};
    if (!child_asset) {
        return displacement;
    }

    const Asset::CumulativeMovementDisplacement cumulative =
        child_asset->current_frame_cumulative_movement_displacement();
    if (!cumulative.valid) {
        return displacement;
    }

    float displacement_x = cumulative.dx;
    float displacement_y = cumulative.dy;
    float displacement_z = cumulative.dz;

    // Preserve existing operation ordering: movement sampling -> flip parity -> optional oval heading
    // rotation around world Y (XZ plane only).
    if (parent_anchor.flip_horizontal) {
        displacement_x = -displacement_x;
    }
    if (parent_anchor.flip_vertical) {
        displacement_y = -displacement_y;
    }
    const std::optional<float> heading_radians =
        oval_anchor_heading::resolve_effective_oval_heading_radians(owner_asset, parent_anchor);
    if (heading_radians.has_value()) {
        oval_anchor_heading::rotate_xz_about_world_y(*heading_radians, displacement_x, displacement_z);
    }

    displacement = anchor_points::AnchorWorldPoint3{
        displacement_x,
        displacement_y,
        displacement_z,
        std::isfinite(displacement_x) &&
            std::isfinite(displacement_y) &&
            std::isfinite(displacement_z)};
    return displacement;
}

bool sample_perspective_scale_at_world_point(const WarpedScreenGrid& cam,
                                             const anchor_points::AnchorWorldPoint3& world_point,
                                             float& out_scale) {
    out_scale = 1.0f;
    if (!world_point.valid ||
        !std::isfinite(world_point.x) ||
        !std::isfinite(world_point.y) ||
        !std::isfinite(world_point.z)) {
        return false;
    }

    float sampled_scale = 1.0f;
    if (!cam.sample_perspective_scale(SDL_FPoint{world_point.x, world_point.y},
                                      world_point.z,
                                      sampled_scale) ||
        !std::isfinite(sampled_scale) ||
        sampled_scale <= 0.0f) {
        return false;
    }

    out_scale = std::max(0.0001f, sampled_scale);
    return true;
}

std::optional<float> resolve_child_perspective_override(const AnchorPoint& parent_anchor,
                                                        const anchored_child_placement::PlacementOutput& placement,
                                                        const Assets* assets_owner) {
    const auto fallback_parent_scale = [&]() -> std::optional<float> {
        if (!parent_anchor.has_flat_perspective_scale) {
            return std::nullopt;
        }
        return std::max(0.0001f, parent_anchor.flat_perspective_scale);
    };

    switch (parent_anchor.scaling_method) {
    case AnchorScalingMethod::Parent:
    case AnchorScalingMethod::Relative2DAnchorPoint:
        return fallback_parent_scale();
    case AnchorScalingMethod::Real3DPoint:
    case AnchorScalingMethod::Real3DFloorPoint:
        break;
    }

    if (!assets_owner) {
        return fallback_parent_scale();
    }

    const WarpedScreenGrid& camera = assets_owner->getView();
    anchor_points::AnchorWorldPoint3 sample_point = placement.anchor_world;
    if (parent_anchor.scaling_method == AnchorScalingMethod::Real3DFloorPoint) {
        sample_point.y = 0.0f;
        sample_point.valid = std::isfinite(sample_point.x) &&
            std::isfinite(sample_point.y) &&
            std::isfinite(sample_point.z);
    }

    float sampled_scale = 1.0f;
    if (sample_perspective_scale_at_world_point(camera, sample_point, sampled_scale)) {
        return sampled_scale;
    }
    return fallback_parent_scale();
}

struct ChildRuntimeSnapshot {
    bool has_child = false;
    int world_x = 0;
    int world_y = 0;
    int world_z = 0;
    int resolution_layer = 0;
    bool hidden = true;
    bool anchor_hidden = true;
    bool active = false;
    bool has_anchor_perspective_override = false;
    bool has_anchor_sprite_transform_override = false;
    float render_anchor_offset_x = 0.0f;
    float render_anchor_offset_y = 0.0f;
    float render_anchor_offset_z = 0.0f;
    double render_depth_bias = 0.0;
};

ChildRuntimeSnapshot make_child_runtime_snapshot(const ChildAsset& child_asset) {
    ChildRuntimeSnapshot snapshot{};
    Asset* child = child_asset.get_asset();
    if (!child) {
        return snapshot;
    }

    snapshot.has_child = true;
    snapshot.world_x = child->world_x();
    snapshot.world_y = child->world_y();
    snapshot.world_z = child->world_z();
    snapshot.resolution_layer = child->grid_resolution;
    snapshot.hidden = child->is_hidden();
    snapshot.anchor_hidden = child->is_anchor_hidden();
    snapshot.active = child->active;
    snapshot.has_anchor_perspective_override = child->has_anchor_perspective_override();
    snapshot.has_anchor_sprite_transform_override = child->has_anchor_sprite_transform_override();
    snapshot.render_anchor_offset_x = child->render_anchor_offset_x();
    snapshot.render_anchor_offset_y = child->render_anchor_offset_y();
    snapshot.render_anchor_offset_z = child->render_anchor_offset_z();
    snapshot.render_depth_bias = child->render_depth_bias();
    return snapshot;
}

} // namespace

ChildAsset::ChildAsset(Asset& owner, std::string asset_name)
    : asset_name_(std::move(asset_name))
    , owner_(&owner)
    , assets_(owner.get_assets()) {

    (void)ensure_child_alive();
}

ChildAsset::~ChildAsset() {
    destroy();
}

ChildAsset::ChildAsset(ChildAsset&& other) noexcept {
    move_from(std::move(other));
}

ChildAsset& ChildAsset::operator=(ChildAsset&& other) noexcept {
    if (this != &other) {
        destroy();
        move_from(std::move(other));
    }
    return *this;
}

void ChildAsset::destroy() {
    unregister_anchor_binding();
    (void)clear_child_render_offset();
    if (child_ && owner_ && owner_->has_child(child_)) {
        owner_->remove_child(child_);
    }
    if (child_ && assets_ && assets_->contains_asset(child_)) {
        child_->Delete();
    }
    child_ = nullptr;
    bound_ = false;
    bound_anchor_name_.clear();
    has_successful_sync_ = false;
    auto_hidden_for_anchor_ = true;
}

Asset* ChildAsset::orphan(std::optional<OrphanImpulse> impulse) {
    Asset* orphaned = get_asset();
    if (!orphaned) {
        return nullptr;
    }

    unregister_anchor_binding();
    (void)clear_child_render_offset();
    if (owner_ && owner_->has_child(orphaned)) {
        owner_->remove_child(orphaned);
    }

    orphaned->set_anchor_hidden(false);
    orphaned->set_hidden(false);
    orphaned->active = true;
    if (assets_) {
        assets_->mark_active_assets_dirty();
    }

    child_ = nullptr;
    bound_ = false;
    bound_anchor_name_.clear();
    has_successful_sync_ = false;
    auto_hidden_for_anchor_ = false;
    manual_hidden_ = false;
    last_tag_criteria_initialized_ = false;
    last_required_tags_.clear();
    last_excluded_tags_.clear();
    orphaned->notify_orphaned(owner_, impulse);
    return orphaned;
}

void ChildAsset::hide() {
    if (!ensure_child_alive()) {
        return;
    }
    manual_hidden_ = true;
    (void)clear_child_render_offset();
    refresh_hidden_state();
}

void ChildAsset::unhide() {
    if (!ensure_child_alive()) {
        return;
    }
    manual_hidden_ = false;
    refresh_hidden_state();
}

void ChildAsset::set_animation(const std::string& animation_name) {
    if (!ensure_child_alive()) {
        return;
    }
    if (!child_ || !child_->anim_) {
        return;
    }
    child_->anim_->set_animation(animation_name);
}

void ChildAsset::set_grid_point(const std::string& parent_anchor_name) {
    if (!ensure_child_alive()) {
        return;
    }
    bound_ = false;
    bound_anchor_name_.clear();
    unregister_anchor_binding();
    const auto anchor = resolve_owner_anchor(parent_anchor_name);
    if (!anchor.has_value()) {
        auto_hidden_for_anchor_ = true;
        refresh_hidden_state();
        return;
    }
    (void)place_once(*anchor, false);
}

void ChildAsset::set_grid_point(const AnchorPoint& parent_anchor) {
    if (!ensure_child_alive()) {
        return;
    }
    bound_ = false;
    bound_anchor_name_.clear();
    unregister_anchor_binding();
    (void)place_once(parent_anchor, false);
}

void ChildAsset::bind(const std::string& parent_anchor_name) {
    bound_ = true;
    bound_anchor_name_ = parent_anchor_name;
    if (!ensure_child_alive()) {
        return;
    }
    const auto anchor = resolve_owner_anchor(parent_anchor_name);
    if (anchor.has_value()) {
        (void)place_once(*anchor, true);
    } else {
        auto_hidden_for_anchor_ = true;
        refresh_hidden_state();
    }
    register_anchor_binding();
}

void ChildAsset::bind(const AnchorPoint& parent_anchor) {
    if (parent_anchor.name.empty()) {
        vibble::log::warn("[ChildAsset] Cannot bind '" + asset_name_ + "' to an unnamed anchor");
        return;
    }
    bound_ = true;
    bound_anchor_name_ = parent_anchor.name;
    if (!ensure_child_alive()) {
        return;
    }
    (void)place_once(parent_anchor, true);
    register_anchor_binding();
}

void ChildAsset::unbind() {
    if (!ensure_child_alive()) {
        return;
    }
    bound_ = false;
    bound_anchor_name_.clear();
    unregister_anchor_binding();
    (void)clear_child_render_offset();
    auto_hidden_for_anchor_ = false;
    refresh_hidden_state();
}

bool ChildAsset::ensure_child_alive() {
    if (child_ && assets_ && assets_->contains_asset(child_)) {
        return true;
    }

    unregister_anchor_binding();
    if (child_ && owner_ && owner_->has_child(child_)) {
        owner_->remove_child(child_);
    }
    child_ = nullptr;

    if (!owner_) {
        return false;
    }

    if (!spawn_child_asset()) {
        return false;
    }

    has_successful_sync_ = false;
    auto_hidden_for_anchor_ = true;

    if (bound_ && !bound_anchor_name_.empty()) {
        bind(bound_anchor_name_);
    } else {
        const bool should_hide = manual_hidden_ || !has_successful_sync_ || auto_hidden_for_anchor_;
        (void)set_child_hidden_state_internal(should_hide);
    }

    return true;
}

bool ChildAsset::apply_anchor_solution(const AnchorPoint& parent_anchor) {
    if (!ensure_child_alive()) {
        return false;
    }
    return apply_anchor_solution_internal(parent_anchor);
}

bool ChildAsset::apply_anchor_solution_internal(const AnchorPoint& parent_anchor) {
    const bool anchor_render_visible = parent_anchor.is_active() && !parent_anchor.hidden;
    if (!anchor_render_visible) {
        auto_hidden_for_anchor_ = true;
        bool changed = clear_child_render_offset();
        const bool should_hide = manual_hidden_ || !has_successful_sync_ || auto_hidden_for_anchor_;
        changed = set_child_hidden_state_internal(should_hide) || changed;
        return changed;
    }

    const std::vector<std::string> required_tags = normalize_tag_list(parent_anchor.tags);
    const std::vector<std::string> excluded_tags = normalize_tag_list(parent_anchor.anti_tags);
    const bool has_tag_criteria = !required_tags.empty() || !excluded_tags.empty();
    if (!has_tag_criteria) {
        last_tag_criteria_initialized_ = false;
        last_required_tags_.clear();
        last_excluded_tags_.clear();
    } else if (!last_tag_criteria_initialized_ ||
               required_tags != last_required_tags_ ||
               excluded_tags != last_excluded_tags_) {
        if (child_ && child_->anim_) {
            (void)child_->anim_->set_animation_by_tags(required_tags, excluded_tags);
        }
        last_required_tags_ = required_tags;
        last_excluded_tags_ = excluded_tags;
        last_tag_criteria_initialized_ = true;
    }

    anchored_child_placement::PlacementInput placement_input{};
    placement_input.parent.world_x = owner_ ? static_cast<float>(owner_->world_x()) : 0.0f;
    placement_input.parent.world_y = owner_ ? static_cast<float>(owner_->world_y()) : 0.0f;
    placement_input.parent.world_z = owner_ ? static_cast<float>(owner_->world_z()) : 0.0f;
    placement_input.parent.resolution_layer =
        (owner_ && owner_->grid_point()) ? owner_->grid_point()->resolution_layer() : child_->grid_resolution;
    placement_input.anchor_definition.anchor = parent_anchor;
    const anchor_points::AnchorWorldPoint3 anchor_displacement =
        child_anchor_world_displacement(parent_anchor, owner_, child_);
    if (anchor_displacement.valid) {
        placement_input.anchor_world_displacement.x = anchor_displacement.x;
        placement_input.anchor_world_displacement.y = anchor_displacement.y;
        placement_input.anchor_world_displacement.z = anchor_displacement.z;
    }
    placement_input.sprite_transform.mirror_x = parent_anchor.flip_horizontal;
    placement_input.sprite_transform.mirror_y = parent_anchor.flip_vertical;
    placement_input.sprite_transform.rotation_degrees = parent_anchor.rotation_degrees;

    anchored_child_placement::PlacementOutput placement{};
    if (!anchored_child_placement::resolve_child_placement(placement_input, placement) ||
        !placement.child_world.valid) {
        auto_hidden_for_anchor_ = true;
        bool changed = clear_child_render_offset();
        const bool should_hide = manual_hidden_ || !has_successful_sync_ || auto_hidden_for_anchor_;
        changed = set_child_hidden_state_internal(should_hide) || changed;
        return changed;
    }

    const float exact_world_x = placement.child_world.x;
    const float exact_world_y = placement.child_world.y;
    const float exact_world_z = placement.child_world.z;

    const int target_world_x = placement.child_world_quantized.x;
    const int target_world_y = placement.child_world_quantized.y;
    const int target_world_z = placement.child_world_quantized_z;
    int target_layer = placement.resolution_layer;
    if (target_layer < 0 && owner_ && owner_->grid_point()) {
        target_layer = owner_->grid_point()->resolution_layer();
    }
    if (target_layer < 0) {
        target_layer = child_->grid_resolution;
    }

    const float residual_x = exact_world_x - static_cast<float>(target_world_x);
    const float residual_y = exact_world_y - static_cast<float>(target_world_y);
    const float residual_z = exact_world_z - static_cast<float>(target_world_z);
    bool changed = false;

    const std::optional<float> perspective_override_scale =
        bound_ ? resolve_child_perspective_override(parent_anchor, placement, assets_) : std::nullopt;
    if (perspective_override_scale.has_value()) {
        changed = child_->set_anchor_perspective_override(*perspective_override_scale,
                                                          target_layer) || changed;
    } else {
        changed = child_->clear_anchor_perspective_override() || changed;
    }
    const SDL_FlipMode anchor_flip = static_cast<SDL_FlipMode>(
        (parent_anchor.flip_horizontal ? static_cast<int>(SDL_FLIP_HORIZONTAL) : 0) |
        (parent_anchor.flip_vertical ? static_cast<int>(SDL_FLIP_VERTICAL) : 0));
    if (child_->set_anchor_sprite_transform_override(anchor_flip, static_cast<double>(parent_anchor.rotation_degrees))) {
        changed = true;
    }

    if (child_->world_x() != target_world_x ||
        child_->world_y() != target_world_y ||
        child_->world_z() != target_world_z ||
        child_->grid_resolution != target_layer) {
        child_->move_to_world_position(target_world_x,
                                       target_world_y,
                                       target_world_z,
                                       target_layer);
        changed = true;
    }

    constexpr float kResidualEpsilon = 1e-5f;
    if (std::fabs(child_->render_anchor_offset_x() - residual_x) > kResidualEpsilon ||
        std::fabs(child_->render_anchor_offset_y() - residual_y) > kResidualEpsilon ||
        std::fabs(child_->render_anchor_offset_z() - residual_z) > kResidualEpsilon) {
        child_->set_render_anchor_offset(residual_x, residual_y, residual_z);
        changed = true;
    }

    const double desired_bias = desired_render_depth_bias(parent_anchor, target_world_z);
    if (std::fabs(child_->render_depth_bias() - desired_bias) > 1e-6) {
        child_->set_render_depth_bias(desired_bias);
        changed = true;
    }

    has_successful_sync_ = true;
    auto_hidden_for_anchor_ = false;
    const bool should_hide = manual_hidden_ || !has_successful_sync_ || auto_hidden_for_anchor_;
    changed = set_child_hidden_state_internal(should_hide) || changed;
    return changed;
}

bool ChildAsset::place_once(const AnchorPoint& parent_anchor, bool keep_bound) {
    if (!keep_bound) {
        bound_ = false;
        bound_anchor_name_.clear();
    }

    const bool anchor_render_visible = parent_anchor.is_active() && !parent_anchor.hidden;
    const bool changed = apply_anchor_solution(parent_anchor);
    if (!anchor_render_visible) {
        return false;
    }
    return changed;
}

bool ChildAsset::set_child_hidden_state(bool hidden) {
    if (!ensure_child_alive()) {
        return false;
    }
    return set_child_hidden_state_internal(hidden);
}

bool ChildAsset::set_child_hidden_state_internal(bool hidden) {
    if (!child_) {
        return false;
    }
    bool changed = false;
    if (child_->is_hidden() != hidden) {
        child_->set_hidden(hidden);
        changed = true;
    }
    if (child_->is_anchor_hidden() != hidden) {
        child_->set_anchor_hidden(hidden);
        changed = true;
    }
    const bool desired_active = !hidden;
    if (child_->active != desired_active) {
        child_->active = desired_active;
        changed = true;
    }
    if (changed && assets_) {
        assets_->mark_active_assets_dirty();
    }
    if (hidden) {
        changed = clear_child_render_offset() || changed;
    }
    return changed;
}

bool ChildAsset::clear_child_render_offset() {
    if (!child_) {
        return false;
    }
    bool changed = false;
    changed = child_->clear_anchor_perspective_override() || changed;
    changed = child_->clear_anchor_sprite_transform_override() || changed;
    constexpr float kResidualEpsilon = 1e-5f;
    if (std::fabs(child_->render_anchor_offset_x()) <= kResidualEpsilon &&
        std::fabs(child_->render_anchor_offset_y()) <= kResidualEpsilon &&
        std::fabs(child_->render_anchor_offset_z()) <= kResidualEpsilon) {
        return changed;
    }
    child_->clear_render_anchor_offset();
    return true;
}

std::optional<AnchorPoint> ChildAsset::resolve_owner_anchor(const std::string& anchor_name) const {
    if (!owner_ || anchor_name.empty()) {
        return std::nullopt;
    }
    return owner_->anchor_state(anchor_name,
                                anchor_points::GridMaterialization::None,
                                Asset::AnchorResolveMode::ForceRecompute);
}

void ChildAsset::register_anchor_binding() {
    if (!bound_ || bound_anchor_name_.empty() || !owner_) {
        return;
    }
    Asset* child_asset = get_asset();
    if (!child_asset) {
        return;
    }
    anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().register_child(
        owner_,
        this,
        child_asset,
        bound_anchor_name_);
}

void ChildAsset::unregister_anchor_binding() {
    Asset* child_asset = get_asset();
    anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().unregister_child(child_asset);
}

void ChildAsset::refresh_hidden_state() {
    if (!ensure_child_alive()) {
        return;
    }

    const bool should_hide = manual_hidden_ || !has_successful_sync_ || auto_hidden_for_anchor_;
    (void)set_child_hidden_state_internal(should_hide);
}

bool ChildAsset::spawn_child_asset() {
    if (!owner_) {
        return false;
    }
    Assets* resolved_assets = resolve_assets();
    if (!resolved_assets) {
        return false;
    }
    child_ = resolved_assets->spawn_asset(asset_name_, owner_->world_xz_point());
    if (!child_) {
        if (!spawn_warning_logged_) {
            vibble::log::warn("[ChildAsset] Cannot create child asset '" + asset_name_ +
                              "' because spawn_asset failed");
            spawn_warning_logged_ = true;
        }
        return false;
    }
    spawn_warning_logged_ = false;
    // Child attachments are driven by parent anchor binding and explicit animation requests.
    // Disable fallback controller auto-reset to "default" so set_animation() remains visible.
    child_->set_default_controller_animation_enforced(false);
    owner_->add_child(child_);
    return true;
}

Assets* ChildAsset::resolve_assets() {
    if (assets_) {
        return assets_;
    }
    if (owner_) {
        assets_ = owner_->get_assets();
    }
    return assets_;
}

bool ChildAsset::is_hidden() const {
    if (!child_ || !assets_ || !assets_->contains_asset(child_)) {
        return true;
    }
    return child_->is_hidden();
}

bool ChildAsset::is_bound() const {
    return bound_;
}

Asset* ChildAsset::get_asset() const {
    if (!child_ || !assets_ || !assets_->contains_asset(child_)) {
        return nullptr;
    }
    return child_;
}

bool ChildAsset::update_internal() {
    if (!ensure_child_alive() || !bound_) {
        return false;
    }
    const auto anchor = resolve_owner_anchor(bound_anchor_name_);
    if (!anchor.has_value()) {
        auto_hidden_for_anchor_ = true;
        const bool should_hide = manual_hidden_ || !has_successful_sync_ || auto_hidden_for_anchor_;
        bool changed = set_child_hidden_state_internal(should_hide);
        changed = clear_child_render_offset() || changed;
        return changed;
    }
    return apply_anchor_solution(*anchor);
}

ChildAsset::UpdateResult ChildAsset::update_detailed() {
    const ChildRuntimeSnapshot before = make_child_runtime_snapshot(*this);
    const bool changed = update_internal();
    const ChildRuntimeSnapshot after = make_child_runtime_snapshot(*this);

    UpdateResult result{};
    result.any_change = changed;
    if (!changed) {
        return result;
    }

    result.world_transform_changed =
        before.has_child != after.has_child ||
        before.world_x != after.world_x ||
        before.world_y != after.world_y ||
        before.world_z != after.world_z ||
        before.resolution_layer != after.resolution_layer;

    result.visibility_or_membership_changed =
        before.has_child != after.has_child ||
        before.hidden != after.hidden ||
        before.anchor_hidden != after.anchor_hidden ||
        before.active != after.active;

    result.visual_only_changed = !result.world_transform_changed &&
                                 !result.visibility_or_membership_changed &&
                                 (before.has_anchor_perspective_override != after.has_anchor_perspective_override ||
                                  before.has_anchor_sprite_transform_override != after.has_anchor_sprite_transform_override ||
                                  std::fabs(before.render_anchor_offset_x - after.render_anchor_offset_x) > 1e-6f ||
                                  std::fabs(before.render_anchor_offset_y - after.render_anchor_offset_y) > 1e-6f ||
                                  std::fabs(before.render_anchor_offset_z - after.render_anchor_offset_z) > 1e-6f ||
                                  std::fabs(before.render_depth_bias - after.render_depth_bias) > 1e-9);

    result.needs_repass = result.world_transform_changed;
    result.needs_traversal_refresh = result.world_transform_changed ||
                                     result.visibility_or_membership_changed;
    return result;
}

bool ChildAsset::update() {
    return update_internal();
}

void ChildAsset::move_from(ChildAsset&& other) noexcept {
    other.unregister_anchor_binding();
    asset_name_ = std::move(other.asset_name_);
    owner_ = other.owner_;
    assets_ = other.assets_;
    child_ = other.child_;
    bound_anchor_name_ = std::move(other.bound_anchor_name_);
    bound_ = other.bound_;
    manual_hidden_ = other.manual_hidden_;
    auto_hidden_for_anchor_ = other.auto_hidden_for_anchor_;
    has_successful_sync_ = other.has_successful_sync_;
    spawn_warning_logged_ = other.spawn_warning_logged_;
    last_tag_criteria_initialized_ = other.last_tag_criteria_initialized_;
    last_required_tags_ = std::move(other.last_required_tags_);
    last_excluded_tags_ = std::move(other.last_excluded_tags_);
    other.asset_name_.clear();
    other.owner_ = nullptr;
    other.assets_ = nullptr;
    other.child_ = nullptr;
    other.bound_anchor_name_.clear();
    other.bound_ = false;
    other.manual_hidden_ = false;
    other.auto_hidden_for_anchor_ = true;
    other.has_successful_sync_ = false;
    other.spawn_warning_logged_ = false;
    other.last_tag_criteria_initialized_ = false;
    other.last_required_tags_.clear();
    other.last_excluded_tags_.clear();
    if (bound_) {
        register_anchor_binding();
    }
}
