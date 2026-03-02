#include "anchor_bound_asset_helper.hpp"

#include "assets/Asset.hpp"
#include "assets/asset/anchor_point.hpp"
#include "core/AssetsManager.hpp"
#include "utils/log.hpp"

#include <SDL3/SDL.h>

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
    if (!assets_) {
        return;
    }

    assets_->unregister_binding_helper(this);
    for (auto& [id, state] : children_) {
        (void)id;
        if (!state.child) {
            continue;
        }
        set_child_hidden_state(state.child, true);
        state.bound = false;
        state.currently_active = false;
        state.ticks_remaining = -1;
    }
}

void AnchorBoundAssetHelper::tick_for_frame() {
    update();
}

Asset* AnchorBoundAssetHelper::create_child(const std::string& asset_id) {
    if (!assets_ || !controller_) {
        return nullptr;
    }

    Asset* child = assets_->spawn_asset(asset_id, controller_->world_point());
    if (!child) {
        vibble::log::warn("[AnchorBinder] failed to spawn child asset '" + asset_id + "'.");
        return nullptr;
    }

    set_child_hidden_state(child, true);

    ChildState state{};
    state.child = child;
    children_[asset_id] = std::move(state);
    return child;
}

bool AnchorBoundAssetHelper::bind(const std::string& child_asset_id, const std::string& anchor_name) {
    return bind_for_ticks(child_asset_id, anchor_name, -1);
}

bool AnchorBoundAssetHelper::bind_for_ticks(const std::string& child_asset_id,
                                            const std::string& anchor_name,
                                            int ticks) {
    if (!controller_ || !assets_) {
        return false;
    }

    ChildState* state = get_child_state(child_asset_id);
    if (!state) {
        Asset* created = create_child(child_asset_id);
        state = get_child_state(child_asset_id);
        if (!created || !state) {
            return false;
        }
    }

    state->anchor_name = anchor_name;
    state->ticks_remaining = ticks;
    state->bound = true;
    apply_binding_tick(child_asset_id, *state);
    return true;
}

void AnchorBoundAssetHelper::unbind(const std::string& child_asset_id) {
    ChildState* state = get_child_state(child_asset_id);
    if (!state) {
        return;
    }

    if (state->child) {
        set_child_hidden_state(state->child, true);
    }
    state->bound = false;
    state->currently_active = false;
    state->ticks_remaining = -1;
    state->anchor_name.clear();
}

void AnchorBoundAssetHelper::update() {
    if (!controller_) {
        return;
    }

    controller_->mark_anchors_dirty();

    for (auto& [id, state] : children_) {
        if (!state.bound) {
            continue;
        }
        if (state.ticks_remaining > 0) {
            --state.ticks_remaining;
            if (state.ticks_remaining == 0) {
                unbind(id);
                continue;
            }
        }
        apply_binding_tick(id, state);
    }
}

void AnchorBoundAssetHelper::apply_binding_tick(const std::string& child_id, ChildState& state) {
    if (!controller_ || !state.child) {
        return;
    }

    auto resolved = controller_->anchor_state(state.anchor_name,
                                              anchor_points::GridMaterialization::Ensure,
                                              std::nullopt);
    const bool anchor_available =
        resolved.has_value() && !resolved->missing && resolved->has_canonical_texture_source;

    if (!anchor_available) {
        set_child_hidden_state(state.child, true);
        state.currently_active = false;
        log_binding_tick(child_id,
                         state,
                         false,
                         controller_->world_x(),
                         controller_->world_y(),
                         state.child->world_x(),
                         state.child->world_y());
        return;
    }

    state.child->move_to_world_position(resolved->world_px.x,
                                        resolved->world_px.y,
                                        resolved->world_z,
                                        resolved->resolution_layer);
    set_child_hidden_state(state.child, false);
    state.currently_active = true;

    log_binding_tick(child_id,
                     state,
                     true,
                     resolved->world_px.x,
                     resolved->world_px.y,
                     state.child->world_x(),
                     state.child->world_y());
}

AnchorBoundAssetHelper::ChildState* AnchorBoundAssetHelper::get_child_state(const std::string& id) {
    auto it = children_.find(id);
    if (it == children_.end()) {
        return nullptr;
    }
    return &it->second;
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
                                              const ChildState& state,
                                              bool anchor_available,
                                              int anchor_world_x,
                                              int anchor_world_y,
                                              int child_world_x,
                                              int child_world_y) const {
    if (!debug_tick_logging_enabled_ || !controller_) {
        return;
    }

    const std::string parent_label = asset_label(controller_);
    const std::string child_label = state.child ? asset_label(state.child) : child_id;
    vibble::log::debug("[AnchorBinderTick] parent=" + parent_label +
                       " child=" + child_label +
                       " anchor=" + state.anchor_name +
                       " anchor_world=(" + std::to_string(anchor_world_x) + "," + std::to_string(anchor_world_y) + ")" +
                       " child_world=(" + std::to_string(child_world_x) + "," + std::to_string(child_world_y) + ")" +
                       " active=" + std::string(state.currently_active ? "true" : "false") +
                       " anchor_available=" + std::string(anchor_available ? "true" : "false"));
}
