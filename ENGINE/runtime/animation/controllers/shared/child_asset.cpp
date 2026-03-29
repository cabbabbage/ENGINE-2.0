#include "child_asset.hpp"

#include "animation/controllers/shared/anchored_child_placement.hpp"
#include "assets/asset/Asset.hpp"
#include "animation/controllers/shared/anchor_bound_asset_helper.hpp"
#include "core/AssetsManager.hpp"
#include "rendering/render/render_depth_policy.hpp"
#include "utils/log.hpp"

#include <cmath>
#include <utility>

namespace {

double desired_render_depth_bias(const AnchorPoint& anchor, int quantized_world_z) {
    return render_depth::bias_for_quantized_depth(anchor.world_depth,
                                                  static_cast<double>(quantized_world_z));
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
    if (!ensure_child_alive()) {
        return;
    }
    bound_ = true;
    bound_anchor_name_ = parent_anchor_name;
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
    if (!ensure_child_alive()) {
        return;
    }
    if (parent_anchor.name.empty()) {
        vibble::log::warn("[ChildAsset] Cannot bind '" + asset_name_ + "' to an unnamed anchor");
        return;
    }
    bound_ = true;
    bound_anchor_name_ = parent_anchor.name;
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
    const bool anchor_available = parent_anchor.is_active();
    if (!anchor_available) {
        auto_hidden_for_anchor_ = true;
        bool changed = clear_child_render_offset();
        const bool should_hide = manual_hidden_ || !has_successful_sync_ || auto_hidden_for_anchor_;
        changed = set_child_hidden_state_internal(should_hide) || changed;
        return changed;
    }

    anchored_child_placement::PlacementInput placement_input{};
    placement_input.parent.world_x = owner_ ? static_cast<float>(owner_->world_x()) : 0.0f;
    placement_input.parent.world_y = owner_ ? static_cast<float>(owner_->world_y()) : 0.0f;
    placement_input.parent.world_z = owner_ ? static_cast<float>(owner_->world_z()) : 0.0f;
    placement_input.parent.resolution_layer =
        (owner_ && owner_->grid_point()) ? owner_->grid_point()->resolution_layer() : child_->grid_resolution;
    placement_input.anchor_definition.anchor = parent_anchor;

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

    if (!parent_anchor.is_active()) {
        auto_hidden_for_anchor_ = true;
        refresh_hidden_state();
        return false;
    }

    return apply_anchor_solution(parent_anchor);
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
    constexpr float kResidualEpsilon = 1e-5f;
    if (std::fabs(child_->render_anchor_offset_x()) <= kResidualEpsilon &&
        std::fabs(child_->render_anchor_offset_y()) <= kResidualEpsilon &&
        std::fabs(child_->render_anchor_offset_z()) <= kResidualEpsilon) {
        return false;
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

bool ChildAsset::update() {
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
    if (bound_) {
        register_anchor_binding();
    }
}
