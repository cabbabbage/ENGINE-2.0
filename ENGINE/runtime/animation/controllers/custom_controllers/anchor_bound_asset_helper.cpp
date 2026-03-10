#include "anchor_bound_asset_helper.hpp"

#include "assets/asset/Asset.hpp"
#include "assets/asset/anchor_point.hpp"
#include "core/AssetsManager.hpp"
#include "utils/log.hpp"

#include <SDL3/SDL.h>
#include <cmath>

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

void AnchorBoundAssetHelper::tick_for_frame() {
    update();
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

std::string AnchorBoundAssetHelper::binding_key_for_child(const Asset& child) const {
    return asset_stable_id(&child);
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
                                       anchor_points::GridMaterialization::None);
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
    for (auto it = children_.begin(); it != children_.end(); ) {
        BindingRecord& state = it->second;
        const bool matches =
            state.child == asset || state.parent == asset ||
            (!stable.empty() && (state.child_id == stable || state.parent_id == stable));
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
    const std::string key = binding_key_for_child(child);
    BindingRecord& state = children_[key];
    state.parent = &parent;
    state.parent_id = asset_stable_id(&parent);
    state.child = &child;
    state.child_id = asset_stable_id(&child);
    state.anchor_name = anchor.name;
    state.anchor_index = std::nullopt;
    state.ticks_remaining = ticks;
    state.expiry_tick = (ticks > 0) ? std::optional<std::uint64_t>(tick_counter_ + static_cast<std::uint64_t>(ticks)) : std::nullopt;
    state.bound = true;
    state.currently_active = false;
    state.last_anchor_depth_offset = anchor.depth_offset;

    parent.add_child(&child);
    state.registered_with_parent = true;
    child.set_render_depth_bias(0.0);
    apply_binding_tick(key, state);
    return true;
}

void AnchorBoundAssetHelper::unbind_child(Asset& parent, Asset& child) {
    const std::string key = binding_key_for_child(child);
    auto it = children_.find(key);
    if (it == children_.end()) {
        return;
    }
    if (it->second.parent == &parent && it->second.child == &child) {
        teardown_binding(it->second);
        children_.erase(it);
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

    if (!record.parent && controller_ && asset_stable_id(controller_) == record.parent_id) {
        record.parent = controller_;
    }

    if (!record.parent && assets_) {
        record.parent = assets_->find_asset_by_stable_id(record.parent_id);
    }

    if (!record.child && assets_) {
        record.child = assets_->find_asset_by_stable_id(record.child_id);
    }

    return record.parent != nullptr && record.child != nullptr;
}

void AnchorBoundAssetHelper::update() {
    ++tick_counter_;

    for (auto it = children_.begin(); it != children_.end(); ) {
        BindingRecord& state = it->second;
        const std::string& id = it->first;

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
        apply_binding_tick(id, state);
        ++it;
    }
}

void AnchorBoundAssetHelper::apply_binding_tick(const std::string& child_id, BindingRecord& state) {
    if (!state.parent || !state.child) {
        return;
    }

    auto resolved = resolve_anchor(state);
    const bool anchor_available = resolved.has_value() && resolved->is_active();

    if (!anchor_available) {
        set_child_hidden_state(state.child, true);
        state.child->set_render_depth_bias(0.0);
        state.currently_active = false;
        log_binding_tick(child_id,
                         state,
                         false,
                         state.parent->world_x(),
                         state.parent->world_y(),
                         state.child->world_x(),
                         state.child->world_y());
        return;
    }

    const AnchorPoint& anchor = *resolved;
    state.last_anchor_depth_offset = anchor.depth_offset;
    const int anchor_world_x = static_cast<int>(std::lround(anchor.world_pos_2d.x));
    const int anchor_world_y = static_cast<int>(std::lround(anchor.world_pos_2d.y));
    state.child->move_to_world_position(anchor_world_x,
                                        anchor_world_y,
                                        anchor.world_z,
                                        anchor.resolution_layer);
    set_child_hidden_state(state.child, false);
    state.currently_active = true;

    log_binding_tick(child_id,
                     state,
                     true,
                     anchor_world_x,
                     anchor_world_y,
                     state.child->world_x(),
                     state.child->world_y());
}

void AnchorBoundAssetHelper::set_child_hidden_state(Asset* child, bool hidden) const {
    if (!child) {
        return;
    }
    child->set_hidden(hidden);
    child->set_anchor_hidden(hidden);
    child->active = !hidden;
}

void AnchorBoundAssetHelper::log_binding_tick(const std::string& child_id,
                                              const BindingRecord& state,
                                              bool anchor_available,
                                              int anchor_world_x,
                                              int anchor_world_y,
                                              int child_world_x,
                                              int child_world_y) const {
    if (!debug_tick_logging_enabled_ || !state.parent) {
        return;
    }

    const std::string parent_label = asset_label(state.parent);
    const std::string child_label = state.child ? asset_label(state.child) : child_id;
    vibble::log::debug("[AnchorBinderTick] parent=" + parent_label +
                       " child=" + child_label +
                       " anchor=" + state.anchor_name +
                       " anchor_depth_offset=" + std::to_string(state.last_anchor_depth_offset) +
                       " anchor_world=(" + std::to_string(anchor_world_x) + "," + std::to_string(anchor_world_y) + ")" +
                       " child_world=(" + std::to_string(child_world_x) + "," + std::to_string(child_world_y) + ")" +
                       " active=" + std::string(state.currently_active ? "true" : "false") +
                       " anchor_available=" + std::string(anchor_available ? "true" : "false"));
}
