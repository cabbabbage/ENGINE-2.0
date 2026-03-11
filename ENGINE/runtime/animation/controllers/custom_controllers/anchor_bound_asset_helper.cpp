#include "anchor_bound_asset_helper.hpp"
#include "anchor_binding_order.hpp"

#include "assets/asset/Asset.hpp"
#include "assets/asset/anchor_point.hpp"
#include "core/AssetsManager.hpp"
#include "utils/log.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace {

bool read_binding_debug_toggle() {
    const char* raw = SDL_getenv("VIBBLE_ANCHOR_BIND_DEBUG");
    if (!raw) {
        return false;
    }
    const std::string value(raw);
    return value == "1" || value == "true" || value == "TRUE" || value == "on" || value == "ON";
}

std::string asset_label(const Asset* asset) {
    if (!asset || !asset->info) {
        return "<unknown>";
    }
    const std::string& spawn = asset->spawn_id;
    if (!spawn.empty()) {
        return asset->info->name + "#" + spawn;
    }
    return asset->info->name;
}

int asset_resolution_layer(const Asset* asset) {
    if (!asset) {
        return 0;
    }
    if (const world::GridPoint* gp = asset->grid_point()) {
        return gp->resolution_layer();
    }
    return asset->grid_resolution;
}

} // namespace

AnchorBoundAssetHelper::AnchorBoundAssetHelper(Asset* controller)
    : controller_(controller)
    , assets_(controller ? controller->get_assets() : nullptr)
    , debug_tick_logging_enabled_(read_binding_debug_toggle()) {
    if (assets_) {
        assets_->register_binding_helper(this);
    }
}

AnchorBoundAssetHelper::~AnchorBoundAssetHelper() {
    if (assets_) {
        assets_->unregister_binding_helper(this);
    }

    for (auto& [id, state] : children_) {
        (void)id;
        teardown_binding(state);
    }
    children_.clear();
}

bool AnchorBoundAssetHelper::tick_for_frame() {
    return update();
}

std::string AnchorBoundAssetHelper::asset_stable_id(const Asset* asset) const {
    if (!asset) {
        return {};
    }
    if (!asset->spawn_id.empty()) {
        return asset->spawn_id;
    }
    if (asset->info && !asset->info->name.empty()) {
        return asset->info->name;
    }
    return std::to_string(reinterpret_cast<std::uintptr_t>(asset));
}

std::uintptr_t AnchorBoundAssetHelper::binding_key_for_child(const Asset& child) const {
    return reinterpret_cast<std::uintptr_t>(&child);
}

std::optional<AnchorPoint> AnchorBoundAssetHelper::resolve_anchor(BindingRecord& record) const {
    if (!record.parent) {
        return std::nullopt;
    }

    if (record.anchor_name.empty() && record.anchor_index) {
        auto name = record.parent->anchor_name_for_index(*record.anchor_index);
        if (name.has_value()) {
            record.anchor_name = *name;
        }
    }

    if (record.anchor_name.empty()) {
        return std::nullopt;
    }

    return record.parent->anchor_state(record.anchor_name,
                                       anchor_points::GridMaterialization::None,
                                       Asset::AnchorResolveMode::ForceRecompute);
}

void AnchorBoundAssetHelper::teardown_binding(BindingRecord& state) {
    if (state.parent && state.child) {
        if (state.registered_with_parent || state.parent->has_child(state.child)) {
            state.parent->remove_child(state.child);
        }
    }
    if (state.child) {
        set_child_hidden_state(state.child, true);
        state.child->set_render_depth_bias(0.0);
    }
    state.bound = false;
    state.currently_active = false;
    state.ticks_remaining = -1;
    state.expiry_tick = std::nullopt;
    state.anchor_index = std::nullopt;
    state.anchor_name.clear();
    state.registered_with_parent = false;
}

void AnchorBoundAssetHelper::purge_bindings_for_asset(const Asset* asset) {
    if (!asset) {
        return;
    }
    const std::string stable = asset_stable_id(asset);
    const std::uintptr_t asset_key = reinterpret_cast<std::uintptr_t>(asset);
    for (auto it = children_.begin(); it != children_.end();) {
        BindingRecord& state = it->second;
        const bool has_instance_keys = (state.child_instance_key != 0) || (state.parent_instance_key != 0);
        const bool legacy_stable_match =
            !has_instance_keys && !stable.empty() && (state.child_id == stable || state.parent_id == stable);
        const bool matches =
            state.child == asset || state.parent == asset ||
            state.child_instance_key == asset_key || state.parent_instance_key == asset_key ||
            legacy_stable_match;
        if (matches) {
            teardown_binding(state);
            it = children_.erase(it);
        } else {
            ++it;
        }
    }
}

bool AnchorBoundAssetHelper::bind_child_to_anchor(Asset& parent, Asset& child, const AnchorPoint& anchor) {
    return bind_child_for_ticks(parent, child, anchor, -1);
}

bool AnchorBoundAssetHelper::bind_child_for_ticks(Asset& parent,
                                                  Asset& child,
                                                  const AnchorPoint& anchor,
                                                  int ticks) {
    const std::uintptr_t key = binding_key_for_child(child);
    BindingRecord& state = children_[key];
    state.parent = &parent;
    state.parent_id = asset_stable_id(&parent);
    state.parent_instance_key = reinterpret_cast<std::uintptr_t>(&parent);
    state.child = &child;
    state.child_id = asset_stable_id(&child);
    state.child_instance_key = key;
    state.bind_sequence = bind_sequence_counter_++;
    state.anchor_name = anchor.name;
    state.anchor_index = std::nullopt;
    state.ticks_remaining = ticks;
    state.expiry_tick =
        (ticks > 0) ? std::optional<std::uint64_t>(tick_counter_ + static_cast<std::uint64_t>(ticks))
                    : std::nullopt;
    state.bound = true;
    state.currently_active = false;
    state.last_anchor_depth_offset = anchor.depth_offset;

    parent.add_child(&child);
    state.registered_with_parent = true;
    child.set_render_depth_bias(0.0);

    const bool changed = apply_binding_tick(state);
    if (changed && assets_) {
        assets_->mark_active_assets_dirty();
    }
    return true;
}

void AnchorBoundAssetHelper::unbind_child(Asset& parent, Asset& child) {
    const std::uintptr_t key = binding_key_for_child(child);
    auto it = children_.find(key);
    if (it != children_.end()) {
        if (it->second.parent == &parent && it->second.child == &child) {
            teardown_binding(it->second);
            children_.erase(it);
            return;
        }
    }

    for (auto scan = children_.begin(); scan != children_.end(); ++scan) {
        if (scan->second.parent == &parent && scan->second.child == &child) {
            teardown_binding(scan->second);
            children_.erase(scan);
            return;
        }
    }
}

bool AnchorBoundAssetHelper::resolve_binding_entities(BindingRecord& record) {
    if (assets_) {
        if (record.parent && !assets_->contains_asset(record.parent)) {
            record.parent = nullptr;
        }
        if (record.child && !assets_->contains_asset(record.child)) {
            record.child = nullptr;
        }
    }

    if (!record.parent && record.parent_instance_key != 0 && assets_) {
        Asset* candidate = reinterpret_cast<Asset*>(record.parent_instance_key);
        if (assets_->contains_asset(candidate)) {
            record.parent = candidate;
        }
    }
    if (!record.child && record.child_instance_key != 0 && assets_) {
        Asset* candidate = reinterpret_cast<Asset*>(record.child_instance_key);
        if (assets_->contains_asset(candidate)) {
            record.child = candidate;
        }
    }

    if (!record.parent && controller_ && asset_stable_id(controller_) == record.parent_id) {
        record.parent = controller_;
    }

    if (!record.parent && assets_ && record.parent_instance_key == 0) {
        record.parent = assets_->find_asset_by_stable_id(record.parent_id);
    }

    if (!record.child && assets_ && record.child_instance_key == 0) {
        record.child = assets_->find_asset_by_stable_id(record.child_id);
    }

    if (record.parent) {
        record.parent_instance_key = reinterpret_cast<std::uintptr_t>(record.parent);
    }
    if (record.child) {
        record.child_instance_key = reinterpret_cast<std::uintptr_t>(record.child);
    }

    return record.parent != nullptr && record.child != nullptr;
}

std::vector<std::uintptr_t> AnchorBoundAssetHelper::build_binding_order(bool& has_cycle,
                                                                        std::size_t& cycle_nodes) const {
    std::vector<anchor_binding_order::Node> nodes;
    nodes.reserve(children_.size());
    for (const auto& [key, state] : children_) {
        anchor_binding_order::Node node{};
        node.id = key;
        if (state.parent_instance_key != 0 && children_.find(state.parent_instance_key) != children_.end()) {
            node.depends_on = state.parent_instance_key;
        }
        node.sort_key = state.bind_sequence;
        nodes.push_back(node);
    }

    const anchor_binding_order::Result order = anchor_binding_order::compute(nodes);
    has_cycle = order.has_cycle;
    cycle_nodes = order.cycle_nodes;
    return order.ordered_ids;
}

bool AnchorBoundAssetHelper::update() {
    ++tick_counter_;
    bool changed_any = false;

    for (auto it = children_.begin(); it != children_.end();) {
        BindingRecord& state = it->second;

        if (!state.bound) {
            teardown_binding(state);
            it = children_.erase(it);
            continue;
        }

        if (!resolve_binding_entities(state)) {
#if !defined(NDEBUG)
            vibble::log::debug("[AnchorBinder] stale binding removed (parent='" + state.parent_id +
                               "' child='" + state.child_id + "')");
#endif
            teardown_binding(state);
            it = children_.erase(it);
            continue;
        }

        if (state.expiry_tick.has_value() && tick_counter_ >= *state.expiry_tick) {
            teardown_binding(state);
            it = children_.erase(it);
            continue;
        }

        if (state.ticks_remaining > 0) {
            --state.ticks_remaining;
            if (state.ticks_remaining == 0) {
                teardown_binding(state);
                it = children_.erase(it);
                continue;
            }
        }

        ++it;
    }

    if (children_.empty()) {
        return false;
    }

    bool has_cycle = false;
    std::size_t cycle_nodes = 0;
    const std::vector<std::uintptr_t> ordered = build_binding_order(has_cycle, cycle_nodes);

    if (has_cycle) {
        log_cycle_warning(cycle_nodes);
    }

    const std::size_t max_passes = has_cycle ? std::min<std::size_t>(4, std::max<std::size_t>(1, ordered.size())) : 1;
    bool changed_last_pass = false;
    for (std::size_t pass = 0; pass < max_passes; ++pass) {
        bool changed_this_pass = false;
        for (const std::uintptr_t key : ordered) {
            auto it = children_.find(key);
            if (it == children_.end()) {
                continue;
            }
            changed_this_pass = apply_binding_tick(it->second) || changed_this_pass;
        }
        changed_any = changed_any || changed_this_pass;
        changed_last_pass = changed_this_pass;
        if (!has_cycle || !changed_this_pass) {
            break;
        }
    }

    if (has_cycle && changed_last_pass && last_cycle_warning_tick_ != tick_counter_) {
        vibble::log::warn("[AnchorBinder] cyclic anchor bindings still changing after reconciliation passes");
        last_cycle_warning_tick_ = tick_counter_;
    }

    if (changed_any && assets_) {
        assets_->mark_active_assets_dirty();
    }
    return changed_any;
}

bool AnchorBoundAssetHelper::apply_binding_tick(BindingRecord& state) {
    if (!state.parent || !state.child) {
        return false;
    }

    auto resolved = resolve_anchor(state);
    const bool anchor_available = resolved.has_value() && resolved->is_active();
    bool changed = false;

    if (!anchor_available) {
        changed = set_child_hidden_state(state.child, true) || changed;
        if (state.child->render_depth_bias() != 0.0) {
            state.child->set_render_depth_bias(0.0);
            changed = true;
        }
        if (state.currently_active) {
            changed = true;
        }
        state.currently_active = false;

        log_binding_tick(state,
                         false,
                         state.parent->world_x(),
                         state.parent->world_y(),
                         state.child->world_x(),
                         state.child->world_y());
        return changed;
    }

    const AnchorPoint& anchor = *resolved;
    state.last_anchor_depth_offset = anchor.depth_offset;
    const int anchor_world_x = static_cast<int>(std::lround(anchor.world_pos_2d.x));
    const int anchor_world_y = static_cast<int>(std::lround(anchor.world_pos_2d.y));
    const int anchor_world_z = anchor.world_z;
    const int anchor_layer = anchor.resolution_layer;

    const bool need_move =
        state.child->world_x() != anchor_world_x ||
        state.child->world_y() != anchor_world_y ||
        state.child->world_z() != anchor_world_z ||
        asset_resolution_layer(state.child) != anchor_layer;

    if (need_move) {
        state.child->move_to_world_position(anchor_world_x, anchor_world_y, anchor_world_z, anchor_layer);
        changed = true;
    }

    changed = set_child_hidden_state(state.child, false) || changed;

    if (!state.currently_active) {
        changed = true;
    }
    state.currently_active = true;

#if !defined(NDEBUG)
    if (state.child->world_x() != anchor_world_x ||
        state.child->world_y() != anchor_world_y ||
        state.child->world_z() != anchor_world_z ||
        asset_resolution_layer(state.child) != anchor_layer) {
        log_alignment_mismatch(state, anchor_world_x, anchor_world_y, anchor_world_z, anchor_layer);
    }
#endif

    log_binding_tick(state,
                     true,
                     anchor_world_x,
                     anchor_world_y,
                     state.child->world_x(),
                     state.child->world_y());
    return changed;
}

bool AnchorBoundAssetHelper::set_child_hidden_state(Asset* child, bool hidden) const {
    if (!child) {
        return false;
    }

    bool changed = false;
    if (child->is_hidden() != hidden) {
        child->set_hidden(hidden);
        changed = true;
    }
    if (child->is_anchor_hidden() != hidden) {
        child->set_anchor_hidden(hidden);
        changed = true;
    }
    const bool desired_active = !hidden;
    if (child->active != desired_active) {
        child->active = desired_active;
        changed = true;
    }
    return changed;
}

void AnchorBoundAssetHelper::log_binding_tick(const BindingRecord& state,
                                              bool anchor_available,
                                              int anchor_world_x,
                                              int anchor_world_y,
                                              int child_world_x,
                                              int child_world_y) const {
    if (!debug_tick_logging_enabled_ || !state.parent) {
        return;
    }

    const std::string parent_label = asset_label(state.parent);
    const std::string child_label = state.child ? asset_label(state.child) : state.child_id;
    vibble::log::debug("[AnchorBinderTick] parent=" + parent_label +
                       " child=" + child_label +
                       " anchor=" + state.anchor_name +
                       " anchor_depth_offset=" + std::to_string(state.last_anchor_depth_offset) +
                       " anchor_world=(" + std::to_string(anchor_world_x) + "," + std::to_string(anchor_world_y) + ")" +
                       " child_world=(" + std::to_string(child_world_x) + "," + std::to_string(child_world_y) + ")" +
                       " active=" + std::string(state.currently_active ? "true" : "false") +
                       " anchor_available=" + std::string(anchor_available ? "true" : "false"));
}

void AnchorBoundAssetHelper::log_cycle_warning(std::size_t cycle_nodes) const {
    if (cycle_nodes == 0) {
        return;
    }
#if !defined(NDEBUG)
    vibble::log::warn("[AnchorBinder] cycle detected in binding graph; falling back to reconciliation passes (" +
                      std::to_string(cycle_nodes) + " nodes)");
#endif
}

void AnchorBoundAssetHelper::log_alignment_mismatch(const BindingRecord& state,
                                                    int anchor_world_x,
                                                    int anchor_world_y,
                                                    int anchor_world_z,
                                                    int anchor_resolution_layer) const {
    if (!state.child || !state.parent) {
        return;
    }
    vibble::log::warn("[AnchorBinder] alignment mismatch after bind: parent=" + asset_label(state.parent) +
                      " parent_id=" + state.parent_id +
                      " child=" + asset_label(state.child) +
                      " child_id=" + state.child_id +
                      " anchor=" + state.anchor_name +
                      " frame=" + std::to_string(tick_counter_) +
                      " expected=(" + std::to_string(anchor_world_x) + "," +
                      std::to_string(anchor_world_y) + "," +
                      std::to_string(anchor_world_z) + "," +
                      std::to_string(anchor_resolution_layer) + ")" +
                      " actual=(" + std::to_string(state.child->world_x()) + "," +
                      std::to_string(state.child->world_y()) + "," +
                      std::to_string(state.child->world_z()) + "," +
                      std::to_string(asset_resolution_layer(state.child)) + ")");
}
