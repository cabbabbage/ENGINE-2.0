#include "child_asset.hpp"

#include "custom_asset_controller.hpp"
#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "rendering/render/render_depth_policy.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>
#include <utility>

namespace {

double desired_render_depth_bias(const AnchorPoint& anchor, int quantized_world_z) {
    return render_depth::bias_for_quantized_depth(anchor.world_depth,
                                                  static_cast<double>(quantized_world_z));
}

using ChildRegistry = std::unordered_map<Asset*, std::vector<ChildAsset*>>;

ChildRegistry& child_registry() {
    static ChildRegistry registry;
    return registry;
}

std::vector<Asset*>& owner_sync_stack() {
    static thread_local std::vector<Asset*> sync_stack;
    return sync_stack;
}

void register_child_wrapper(Asset* owner, ChildAsset* child_asset) {
    if (!owner || !child_asset) {
        return;
    }
    auto& registered = child_registry()[owner];
    if (std::find(registered.begin(), registered.end(), child_asset) == registered.end()) {
        registered.push_back(child_asset);
    }
}

void unregister_child_wrapper(Asset* owner, ChildAsset* child_asset) {
    if (!owner || !child_asset) {
        return;
    }

    auto registry_it = child_registry().find(owner);
    if (registry_it == child_registry().end()) {
        return;
    }

    auto& registered = registry_it->second;
    registered.erase(std::remove(registered.begin(), registered.end(), child_asset),
                     registered.end());
    if (registered.empty()) {
        child_registry().erase(registry_it);
    }
}

bool push_owner_sync(Asset* owner) {
    auto& sync_stack = owner_sync_stack();
    if (!owner) {
        return false;
    }
    if (std::find(sync_stack.begin(), sync_stack.end(), owner) != sync_stack.end()) {
        return false;
    }
    sync_stack.push_back(owner);
    return true;
}

void pop_owner_sync(Asset* owner) {
    auto& sync_stack = owner_sync_stack();
    if (!owner || sync_stack.empty()) {
        return;
    }
    if (sync_stack.back() == owner) {
        sync_stack.pop_back();
        return;
    }

    auto it = std::find(sync_stack.begin(), sync_stack.end(), owner);
    if (it != sync_stack.end()) {
        sync_stack.erase(it);
    }
}

} // namespace

ChildAsset::ChildAsset(std::string asset_name)
    : asset_name_(std::move(asset_name)) {
    owner_ = CustomAssetController::active_owner_asset();
    assets_ = CustomAssetController::active_assets();
    if (!owner_ || !assets_) {
        vibble::log::warn("[ChildAsset] Cannot create child asset '" + asset_name_ +
                          "' outside an active custom controller context");
        return;
    }

    child_ = assets_->spawn_asset(asset_name_, owner_->world_xz_point());
    if (!child_) {
        vibble::log::warn("[ChildAsset] Cannot create child asset '" + asset_name_ +
                          "' because spawn_asset failed");
        return;
    }
    owner_->add_child(child_);
    register_with_owner();
    refresh_hidden_state();
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
    if (!ensure_child_alive()) {
        child_ = nullptr;
        bound_ = false;
        bound_anchor_name_.clear();
        has_successful_sync_ = false;
        auto_hidden_for_anchor_ = true;
        return;
    }

    if (owner_ && owner_->has_child(child_)) {
        owner_->remove_child(child_);
    }
    unregister_from_owner();
    child_->Delete();
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
}

void ChildAsset::unbind() {
    if (!ensure_child_alive()) {
        return;
    }
    bound_ = false;
    bound_anchor_name_.clear();
    auto_hidden_for_anchor_ = false;
    refresh_hidden_state();
}

bool ChildAsset::ensure_child_alive() {
    if (!child_) {
        return false;
    }
    if (!assets_ || !assets_->contains_asset(child_)) {
        unregister_from_owner();
        child_ = nullptr;
        return false;
    }
    return true;
}

bool ChildAsset::apply_anchor_solution(const AnchorPoint& parent_anchor) {
    if (!ensure_child_alive()) {
        return false;
    }
    const bool anchor_available = parent_anchor.is_active();
    if (!anchor_available) {
        auto_hidden_for_anchor_ = true;
        refresh_hidden_state();
        return false;
    }

    const int target_world_x = static_cast<int>(std::lround(parent_anchor.world_pos_2d.x));
    const int target_world_y = static_cast<int>(std::lround(parent_anchor.world_pos_2d.y));
    const int target_world_z = parent_anchor.world_z;
    const int target_layer = parent_anchor.resolution_layer;

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

    const double desired_bias = desired_render_depth_bias(parent_anchor, target_world_z);
    if (std::fabs(child_->render_depth_bias() - desired_bias) > 1e-6) {
        child_->set_render_depth_bias(desired_bias);
        changed = true;
    }

    has_successful_sync_ = true;
    auto_hidden_for_anchor_ = false;
    refresh_hidden_state();
    sync_registered_for_owner(child_);
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
    return changed;
}

std::optional<AnchorPoint> ChildAsset::resolve_owner_anchor(const std::string& anchor_name) const {
    if (!owner_ || anchor_name.empty()) {
        return std::nullopt;
    }
    return owner_->anchor_state(anchor_name,
                                anchor_points::GridMaterialization::None,
                                Asset::AnchorResolveMode::ForceRecompute);
}

void ChildAsset::refresh_hidden_state() {
    if (!ensure_child_alive()) {
        return;
    }

    const bool should_hide = manual_hidden_ || !has_successful_sync_ || auto_hidden_for_anchor_;
    (void)set_child_hidden_state(should_hide);
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

void ChildAsset::update() {
    if (!ensure_child_alive() || !bound_) {
        return;
    }
    const auto anchor = resolve_owner_anchor(bound_anchor_name_);
    if (!anchor.has_value()) {
        auto_hidden_for_anchor_ = true;
        refresh_hidden_state();
        return;
    }
    (void)apply_anchor_solution(*anchor);
}

void ChildAsset::move_from(ChildAsset&& other) noexcept {
    asset_name_ = std::move(other.asset_name_);
    owner_ = other.owner_;
    assets_ = other.assets_;
    child_ = other.child_;
    bound_anchor_name_ = std::move(other.bound_anchor_name_);
    bound_ = other.bound_;
    manual_hidden_ = other.manual_hidden_;
    auto_hidden_for_anchor_ = other.auto_hidden_for_anchor_;
    has_successful_sync_ = other.has_successful_sync_;
    unregister_child_wrapper(owner_, &other);
    register_with_owner();
    other.asset_name_.clear();
    other.owner_ = nullptr;
    other.assets_ = nullptr;
    other.child_ = nullptr;
    other.bound_anchor_name_.clear();
    other.bound_ = false;
    other.manual_hidden_ = false;
    other.auto_hidden_for_anchor_ = true;
    other.has_successful_sync_ = false;
}

void ChildAsset::sync_registered_for_owner(Asset* owner) {
    if (!push_owner_sync(owner)) {
        return;
    }

    auto registry_it = child_registry().find(owner);
    if (registry_it != child_registry().end()) {
        const std::vector<ChildAsset*> registered = registry_it->second;
        for (ChildAsset* child_asset : registered) {
            if (child_asset) {
                child_asset->update();
            }
        }
    }

    pop_owner_sync(owner);
}

void ChildAsset::register_with_owner() {
    if (!owner_ || !child_) {
        return;
    }
    register_child_wrapper(owner_, this);
}

void ChildAsset::unregister_from_owner() {
    if (!owner_) {
        return;
    }
    unregister_child_wrapper(owner_, this);
}
