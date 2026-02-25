#include "anchor_bound_asset_helper.hpp"

#include "assets/Asset.hpp"
#include "core/AssetsManager.hpp"
#include "utils/log.hpp"
#include "assets/asset/anchor_point.hpp"
#include <SDL3/SDL.h>

namespace {

constexpr int kDormantWorldZ = 0;
constexpr int kDormantResolutionLayer = 0;

} // namespace

AnchorBoundAssetHelper::AnchorBoundAssetHelper(Asset* controller)
    : controller_(controller)
    , assets_(controller ? controller->get_assets() : nullptr) {
    if (assets_) {
        assets_->register_binding_helper(this);
    }
}

AnchorBoundAssetHelper::~AnchorBoundAssetHelper() {
    if (!assets_) return;

    assets_->unregister_binding_helper(this);

    for (auto& [id, state] : children_) {
        if (state.active) {
            detach_child(state);
        }
        if (state.dormant) {
            ensure_hidden(state.dormant.get());
            // Keep assets alive by re-attaching them (hidden) so raw pointers in Assets::all remain valid.
            const int base_layer = controller_ ? controller_->grid_resolution : kDormantResolutionLayer;
            assets_->attach_asset(std::move(state.dormant), kDormantWorldZ, base_layer);
        }
    }
}

Asset* AnchorBoundAssetHelper::create_child(const std::string& asset_id) {
    if (!assets_ || !controller_) {
        vibble::log::warn("[AnchorBinder] create_child skipped: controller or assets missing.");
        return nullptr;
    }

    SDL_Point spawn_at = controller_->world_point();
    std::unique_ptr<Asset> dormant = assets_->create_unattached_asset(asset_id, spawn_at);
    if (!dormant) {
        vibble::log::warn("[AnchorBinder] create_child failed: asset '" + asset_id + "' missing or could not be created.");
        return nullptr;
    }

    ensure_hidden(dormant.get());

    Asset* raw = dormant.get();
    ChildState state{};
    state.dormant = std::move(dormant);
    state.live = nullptr;
    state.active = false;
    children_[asset_id] = std::move(state);
    return raw;
}

bool AnchorBoundAssetHelper::bind(const std::string& child_asset_id, const std::string& anchor_name) {
    return bind_for_ticks(child_asset_id, anchor_name, -1);
}

bool AnchorBoundAssetHelper::bind_for_ticks(const std::string& child_asset_id, const std::string& anchor_name, int ticks) {
    ChildState* state = get_child_state(child_asset_id);
    if (!state) {
        Asset* created = create_child(child_asset_id);
        state = get_child_state(child_asset_id);
        if (!created || !state) {
            return false;
        }
    }
    if (!controller_ || !assets_) {
        return false;
    }
    if (!attach_child(child_asset_id, *state, anchor_name)) {
        return false;
    }
    state->ticks_remaining = ticks;
    state->anchor_name = anchor_name;
    state->active = true;
    return true;
}

void AnchorBoundAssetHelper::unbind(const std::string& child_asset_id) {
    ChildState* state = get_child_state(child_asset_id);
    if (!state || !state->active) {
        return;
    }
    detach_child(*state);
}

void AnchorBoundAssetHelper::update() {
    if (!controller_) return;
    for (auto& [id, state] : children_) {
        if (!state.active) continue;
        if (state.ticks_remaining > 0) {
            --state.ticks_remaining;
            if (state.ticks_remaining == 0) {
                detach_child(state);
                continue;
            }
        }
        // Refresh position each tick.
        attach_child(id, state, state.anchor_name);
    }
}

AnchorBoundAssetHelper::ChildState* AnchorBoundAssetHelper::get_child_state(const std::string& id) {
    auto it = children_.find(id);
    if (it != children_.end()) {
        return &it->second;
    }
    return nullptr;
}

void AnchorBoundAssetHelper::ensure_hidden(Asset* child) {
    if (!child) return;
    child->set_hidden(true);
    child->set_anchor_hidden(true);
    child->active = false;
}

bool AnchorBoundAssetHelper::attach_child(const std::string& id, ChildState& state, const std::string& anchor_name) {
    if (!controller_ || !assets_) {
        return false;
    }

    // If dormant, move back into the grid.
    if (state.dormant) {
        // Place at controller position before registering.
        SDL_Point base_pos = controller_->world_point();
        state.dormant->move_to_world_position(base_pos.x, base_pos.y, kDormantWorldZ);

        const int base_layer = controller_->grid_resolution;
        Asset* live = assets_->attach_asset(std::move(state.dormant), kDormantWorldZ, base_layer);
        if (!live) {
            vibble::log::warn("[AnchorBinder] attach_child failed: could not register dormant asset.");
            return false;
        }
        state.live = live;
    }

    Asset* child = state.live;
    if (!child) {
        vibble::log::warn("[AnchorBinder] attach_child failed: missing live asset pointer for '" + id + "'.");
        return false;
    }

    ensure_hidden(child);

    // Resolve anchor position manually (no Asset follow graph).
    auto resolved = controller_->anchor_state(anchor_name,
                                              anchor_points::GridMaterialization::Ensure,
                                              anchor_points::AnchorDepthPolicy::MatchOwner);
    if (!resolved.has_value() || resolved->missing || !resolved->has_canonical_texture_source) {
        // Keep hidden until anchor exists.
        return true;
    }

    child->move_to_world_position(resolved->world_px.x, resolved->world_px.y, resolved->world_z);
    child->grid_resolution = resolved->resolution_layer;

    child->set_hidden(false);
    child->set_anchor_hidden(false);
    child->active = true;
    return true;
}

void AnchorBoundAssetHelper::detach_child(ChildState& state) {
    if (!assets_) return;

    Asset* child = state.live;
    if (child) {
        ensure_hidden(child);
        state.dormant = assets_->extract_asset(child);
        state.live = nullptr;
    }
    state.active = false;
    state.ticks_remaining = -1;
    state.anchor_name.clear();
}
